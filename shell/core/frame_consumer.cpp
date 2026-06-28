// SPDX-License-Identifier: MIT
//
// Frame channel consumer: shm-backed seqlock payload + Mach doorbell. Moved
// verbatim from the original single-file MacMu.mm. Pure C++.

#include "frame_consumer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_init.h>
#include <mach/message.h>
#include <servers/bootstrap.h>
#include <string>
#include <sys/mman.h>
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

struct DoorbellMessage {
    mach_msg_header_t header;
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
        return false;
    }
    mapped_ = mmap(nullptr, totalSize_, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd_, 0);
    if (mapped_ == MAP_FAILED) {
        mapped_ = nullptr;
        return false;
    }
    ShmHeader* header = static_cast<ShmHeader*>(mapped_);
    header->magic = kShmMagic;
    header->version = kShmVersion;
    header->payloadOffset = sizeof(ShmHeader);
    payload_ = static_cast<char*>(mapped_) + sizeof(ShmHeader);
    std::memset(payload_, 0, sizeof(ShmPayload));

    kern_return_t kr =
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &machPort_);
    if (kr != KERN_SUCCESS) {
        teardown();  // release the half-initialized shm so producer falls back
        return false;
    }
    mach_port_limits_t limits = {0};
    limits.mpl_qlimit = MACH_PORT_QLIMIT_MAX;
    mach_port_set_attributes(mach_task_self(), machPort_, MACH_PORT_LIMITS_INFO,
                             reinterpret_cast<mach_port_info_t>(&limits),
                             MACH_PORT_LIMITS_INFO_COUNT);
    // bootstrap_register expects a name_t (char[128]); copy into a buffer.
    name_t serviceName;
    std::memset(serviceName, 0, sizeof(serviceName));
    std::strncpy(serviceName, name_.c_str(), sizeof(serviceName) - 1);
    kr = bootstrap_register(bootstrap_port, serviceName, machPort_);
    if (kr != KERN_SUCCESS) {
        teardown();  // ditto: avoid a half-open channel the producer would bind to
        return false;
    }
    ownsRight_ = true;
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

        DoorbellMessage msg;
        std::memset(&msg, 0, sizeof(msg));
        const uint64_t now = steady_now_ms();
        if (now >= deadline_ms) break;
        const mach_msg_timeout_t remaining =
            static_cast<mach_msg_timeout_t>(deadline_ms - now);
        kern_return_t kr =
            mach_msg(&msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(msg),
                     machPort_, remaining, MACH_PORT_NULL);
        if (kr == KERN_SUCCESS || kr == MACH_RCV_TOO_LARGE) {
            if (read(out) && out->frame > last_frame) return true;
        } else if (kr == MACH_RCV_TIMED_OUT) {
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
        DoorbellMessage msg;
        std::memset(&msg, 0, sizeof(msg));
        kern_return_t kr = mach_msg(&msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                                    sizeof(msg), machPort_, 0, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS && kr != MACH_RCV_TOO_LARGE) break;
    }
}

void FrameConsumer::teardown() {
    // Note: we intentionally do not unregister the bootstrap name here
    // (bootstrap_register is deprecated and its name_t signature differs
    // by SDK). Deallocating the receive right is sufficient; the producer
    // will fail its bootstrap_look_up and fall back to shm polling.
    if (machPort_ != 0) {
        mach_port_deallocate(mach_task_self(), machPort_);
        machPort_ = 0;
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
    }
    valid_ = false;
}
