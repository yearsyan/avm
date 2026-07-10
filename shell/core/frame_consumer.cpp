// SPDX-License-Identifier: MIT
//
// Frame channel consumer (v2): shm slot table + inherited FD doorbell.

#include "frame_consumer.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "posix_util.h"

namespace {

constexpr uint64_t kShmMagic = 0x4d41434d5546524dull;  // 'MACMUFRM'
constexpr uint32_t kShmVersion = 2;
constexpr int kDoorbellSocketBufferBytes = 1 << 20;

// Shell-side copy of the cross-process wire layout. Keep this byte-for-byte in
// sync with the producer copy in
// hardware/google/gfxstream/host/common/iosurface_export.cpp. Update both files
// together and bump kShmVersion for incompatible layout changes. See
// docs/FRAME_CHANNEL_V2_CONTROL_PLANE.md.
struct ShmHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t payloadOffset;
    uint32_t slotCount;
    uint32_t slotStride;
    uint64_t reserved[2];
};

struct ShmDisplaySlot {
    uint64_t seq;          // seqlock: odd while the producer is writing
    uint64_t frame;        // monotonic per slot; 0 = never published
    uint32_t iosurfaceId;
    uint32_t width;
    uint32_t height;
    uint32_t dpi;
    uint32_t pixelFormat;  // fourcc, currently always 'BGRA'
    uint32_t flags;
    uint64_t timestampNs;
    uint8_t pad[16];
};
static_assert(sizeof(ShmDisplaySlot) == 64, "slot must stay one cache line");

// Advisory doorbell payload; contents must not be trusted (datagrams can be
// coalesced or dropped), readers re-scan the slot table on any wake.
struct FrameDoorbellMsg {
    uint32_t displayId;
    uint32_t reserved;
    uint64_t frame;
};

std::string frame_channel_name(uint32_t wrapper_pid) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "macmu.frame.%u", static_cast<unsigned>(wrapper_pid));
    return std::string(buf);
}

std::string shm_path(const std::string& name) {
    return name.find('/') == std::string::npos ? std::string("/") + name : name;
}

void set_socket_buffer_size_best_effort(int fd, int option) {
    int size = kDoorbellSocketBufferBytes;
    setsockopt(fd, SOL_SOCKET, option, &size, sizeof(size));
}

}  // namespace

FrameConsumer::~FrameConsumer() { teardown(); }

bool FrameConsumer::create(uint32_t wrapper_pid) {
    name_ = frame_channel_name(wrapper_pid);
    const std::string path = shm_path(name_);
    shmFd_ = shm_open(path.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (shmFd_ < 0) {
        return false;
    }
    totalSize_ = sizeof(ShmHeader) + kMacmuFrameSlotCount * sizeof(ShmDisplaySlot);
    if (ftruncate(shmFd_, static_cast<off_t>(totalSize_)) != 0) {
        teardown();
        return false;
    }
    mapped_ = mmap(nullptr, totalSize_, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd_, 0);
    if (mapped_ == MAP_FAILED) {
        mapped_ = nullptr;
        teardown();
        return false;
    }
    std::memset(mapped_, 0, totalSize_);
    ShmHeader* header = static_cast<ShmHeader*>(mapped_);
    header->magic = kShmMagic;
    header->version = kShmVersion;
    header->payloadOffset = sizeof(ShmHeader);
    header->slotCount = kMacmuFrameSlotCount;
    header->slotStride = sizeof(ShmDisplaySlot);
    slots_ = static_cast<char*>(mapped_) + sizeof(ShmHeader);

    int doorbellPair[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, doorbellPair) != 0) {
        teardown();
        return false;
    }
    doorbellFd_ = doorbellPair[0];
    producerDoorbellFd_ = doorbellPair[1];
    if (!macmu::shell::set_close_on_exec(doorbellFd_) ||
        !macmu::shell::set_close_on_exec(producerDoorbellFd_)) {
        teardown();
        return false;
    }
    set_socket_buffer_size_best_effort(doorbellFd_, SO_RCVBUF);
    set_socket_buffer_size_best_effort(producerDoorbellFd_, SO_SNDBUF);

    valid_ = true;
    return true;
}

void FrameConsumer::close_producer_doorbell_fd() {
    if (producerDoorbellFd_ >= 0) {
        close(producerDoorbellFd_);
        producerDoorbellFd_ = -1;
    }
}

void FrameConsumer::reset_slots() {
    std::lock_guard<std::mutex> lock(slotsMutex_);
    if (!valid_ || !slots_) {
        return;
    }
    std::memset(slots_, 0, kMacmuFrameSlotCount * sizeof(ShmDisplaySlot));
    drain_notifications();
}

bool FrameConsumer::read(uint32_t display_id, SurfaceMetadata* out) {
    std::lock_guard<std::mutex> lock(slotsMutex_);
    if (!valid_ || out == nullptr || display_id >= kMacmuFrameSlotCount) return false;
    ShmDisplaySlot* slot = static_cast<ShmDisplaySlot*>(slots_) + display_id;
    for (int attempt = 0; attempt < 8; ++attempt) {
        // The seq words are shared with another process; they must be read
        // with atomic ops so the compiler cannot hoist or fold the loads
        // across retries. The fences order the payload reads between them.
        const uint64_t s0 = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        if (s0 & 1) {
            continue;
        }
        const MacmuIOSurfaceID iosurfaceId = static_cast<MacmuIOSurfaceID>(slot->iosurfaceId);
        const uint32_t width = slot->width;
        const uint32_t height = slot->height;
        const uint64_t frame = slot->frame;
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t s1 = __atomic_load_n(&slot->seq, __ATOMIC_RELAXED);
        if (s0 == s1) {
            out->displayId = display_id;
            out->iosurfaceId = iosurfaceId;
            out->width = width;
            out->height = height;
            out->frame = frame;
            return frame != 0;
        }
    }
    return false;
}

bool FrameConsumer::scan_slots(const uint64_t* last_frames, uint32_t* out_display_id) {
    const uint32_t start = nextScanStart_ % kMacmuFrameSlotCount;
    for (uint32_t offset = 0; offset < kMacmuFrameSlotCount; ++offset) {
        const uint32_t id = (start + offset) % kMacmuFrameSlotCount;
        SurfaceMetadata meta = {};
        if (read(id, &meta) && meta.frame != 0 && meta.frame != last_frames[id]) {
            nextScanStart_ = (id + 1) % kMacmuFrameSlotCount;
            if (out_display_id) {
                *out_display_id = id;
            }
            return true;
        }
    }
    return false;
}

bool FrameConsumer::wait_for_any_frame(const uint64_t* last_frames, uint64_t timeout_ms,
                                       uint32_t* out_display_id) {
    if (!valid_ || last_frames == nullptr) return false;
    if (scan_slots(last_frames, out_display_id)) return true;
    const uint64_t deadline_ms = steady_now_ms() + timeout_ms;
    do {
        const uint64_t now = steady_now_ms();
        if (now >= deadline_ms) break;
        pollfd pfd = {};
        pfd.fd = doorbellFd_;
        pfd.events = POLLIN;
        const int pollResult = poll(&pfd, 1, static_cast<int>(deadline_ms - now));
        if (pollResult > 0 && (pfd.revents & POLLIN)) {
            drain_notifications();
            if (scan_slots(last_frames, out_display_id)) return true;
        } else if (pollResult == 0) {
            break;
        } else if (pollResult < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    } while (steady_now_ms() < deadline_ms);
    return scan_slots(last_frames, out_display_id);
}

uint64_t FrameConsumer::steady_now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void FrameConsumer::drain_notifications() {
    while (true) {
        FrameDoorbellMsg msg = {};
        const ssize_t bytes = recv(doorbellFd_, &msg, sizeof(msg), MSG_DONTWAIT);
        if (bytes > 0) {
            continue;
        }
        if (bytes == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

void FrameConsumer::teardown() {
    if (doorbellFd_ >= 0) {
        close(doorbellFd_);
        doorbellFd_ = -1;
    }
    close_producer_doorbell_fd();
    if (mapped_) {
        munmap(mapped_, totalSize_);
        mapped_ = nullptr;
    }
    if (shmFd_ >= 0) {
        close(shmFd_);
        shmFd_ = -1;
    }
    if (!name_.empty()) {
        shm_unlink(shm_path(name_).c_str());
    }
    valid_ = false;
}
