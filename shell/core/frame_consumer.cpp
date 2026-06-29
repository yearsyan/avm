// SPDX-License-Identifier: MIT
//
// Frame channel consumer: shm-backed seqlock payload + Unix socket doorbell.

#include "frame_consumer.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr uint64_t kShmMagic = 0x4d41434d5546524dull;  // 'MACMUFRM'
constexpr uint32_t kShmVersion = 1;

struct ShmHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t payloadOffset;
};

struct ShmPayload {
    uint32_t iosurfaceId;
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
    uint64_t frame;
    uint64_t timestampNs;
};

std::string frame_channel_name(uint32_t wrapper_pid) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "macmu.frame.%u", static_cast<unsigned>(wrapper_pid));
    return std::string(buf);
}

std::string shm_path(const std::string& name) {
    return name.find('/') == std::string::npos ? std::string("/") + name : name;
}

std::string doorbell_path(const std::string& name) {
    return "/tmp/" + name + ".doorbell";
}

bool fill_unix_addr(const std::string& path, sockaddr_un* addr) {
    if (path.size() >= sizeof(addr->sun_path)) {
        return false;
    }
    std::memset(addr, 0, sizeof(*addr));
#ifdef __APPLE__
    addr->sun_len = sizeof(*addr);
#endif
    addr->sun_family = AF_UNIX;
    std::strncpy(addr->sun_path, path.c_str(), sizeof(addr->sun_path) - 1);
    return true;
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
    totalSize_ = sizeof(ShmHeader) + sizeof(ShmPayload);
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
    ShmHeader* header = static_cast<ShmHeader*>(mapped_);
    header->magic = kShmMagic;
    header->version = kShmVersion;
    header->payloadOffset = sizeof(ShmHeader);
    payload_ = static_cast<char*>(mapped_) + sizeof(ShmHeader);
    std::memset(payload_, 0, sizeof(ShmPayload));

    const std::string pathDoorbell = doorbell_path(name_);
    sockaddr_un addr = {};
    if (!fill_unix_addr(pathDoorbell, &addr)) {
        teardown();
        return false;
    }
    doorbellFd_ = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (doorbellFd_ < 0) {
        teardown();
        return false;
    }
    unlink(pathDoorbell.c_str());
    if (bind(doorbellFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        teardown();
        return false;
    }
    valid_ = true;
    return true;
}

bool FrameConsumer::read(SurfaceMetadata* out) {
    if (!valid_ || out == nullptr) return false;
    ShmPayload* p = static_cast<ShmPayload*>(payload_);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t f0 = p->frame;
        std::atomic_thread_fence(std::memory_order_acquire);
        const MacmuIOSurfaceID iosurfaceId = static_cast<MacmuIOSurfaceID>(p->iosurfaceId);
        const uint32_t width = p->width;
        const uint32_t height = p->height;
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t f1 = p->frame;
        if (f0 == f1) {
            out->iosurfaceId = iosurfaceId;
            out->width = width;
            out->height = height;
            out->frame = f0;
            return out->frame != 0;
        }
    }
    return false;
}

bool FrameConsumer::wait_for_frame(uint64_t last_frame, uint64_t timeout_ms, SurfaceMetadata* out) {
    if (!valid_) return false;
    if (read(out) && out->frame > last_frame) return true;
    const uint64_t deadline_ms = steady_now_ms() + timeout_ms;
    do {
        // Drain any queued notifications without blocking first.
        drain_notifications();
        if (read(out) && out->frame > last_frame) return true;

        const uint64_t now = steady_now_ms();
        if (now >= deadline_ms) break;
        pollfd pfd = {};
        pfd.fd = doorbellFd_;
        pfd.events = POLLIN;
        const int pollResult =
            poll(&pfd, 1, static_cast<int>(deadline_ms - now));
        if (pollResult > 0 && (pfd.revents & POLLIN)) {
            drain_notifications();
            if (read(out) && out->frame > last_frame) return true;
        } else if (pollResult == 0) {
            continue;
        } else if (pollResult < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    } while (steady_now_ms() < deadline_ms);
    return read(out) && out->frame > last_frame;
}

uint64_t FrameConsumer::steady_now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void FrameConsumer::drain_notifications() {
    while (true) {
        uint64_t frame = 0;
        const ssize_t bytes = recv(doorbellFd_, &frame, sizeof(frame), MSG_DONTWAIT);
        if (bytes >= 0) {
            continue;
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
        unlink(doorbell_path(name_).c_str());
    }
    valid_ = false;
}
