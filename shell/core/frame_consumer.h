// SPDX-License-Identifier: MIT
//
// FrameConsumer: the shell-side half of the cross-process frame channel.
//
// This mirrors the layout and naming convention used by the gfxstream producer
// (host/common/iosurface_export.cpp) but is deliberately self-contained so the
// MIT-licensed shell does not depend on any gfxstream headers. The shared
// memory object is named "macmu.frame.<wrapperPid>" (the shell's own pid,
// which it exports to qemu via ANDROID_EMULATOR_WRAPPER_PID), and an inherited
// FD doorbell carries a notification for each frame. The shell keeps the
// consumer end and passes the producer end to qemu by fd inheritance.
//
// When the channel cannot be established, valid() returns false and callers
// should avoid launching a frame-driven display path.

#ifndef MACMU_SHELL_FRAME_CONSUMER_H
#define MACMU_SHELL_FRAME_CONSUMER_H

#include <cstdint>
#include <string>

#include "surface_metadata.h"

class FrameConsumer {
   public:
    FrameConsumer() = default;
    ~FrameConsumer();
    FrameConsumer(const FrameConsumer&) = delete;
    FrameConsumer& operator=(const FrameConsumer&) = delete;

    // Create the shm object + doorbell socketpair. The consumer must be set up
    // BEFORE qemu is launched so the producer can find it on first publish.
    bool create(uint32_t wrapper_pid);

    bool valid() const { return valid_; }

    // Producer end of the doorbell socketpair. Keep this fd open while the app
    // may restart qemu; teardown closes it with the rest of the channel.
    int producer_doorbell_fd() const { return producerDoorbellFd_; }
    void close_producer_doorbell_fd();

    // Seqlock-style read: sample the frame counter, read the payload, then
    // re-read the counter. Only accept the snapshot if the two counter reads
    // match, which guarantees we did not observe a producer update mid-snapshot
    // (e.g. a new surface id paired with the previous frame number, which would
    // make the consumer think it has already consumed the new surface).
    bool read(SurfaceMetadata* out);

    // Block until a frame different from |last_frame| is available or
    // |timeout_ms| elapses. Drains coalesced doorbell notifications first.
    bool wait_for_frame(uint64_t last_frame, uint64_t timeout_ms, SurfaceMetadata* out);

   private:
    static uint64_t steady_now_ms();
    void drain_notifications();
    void teardown();

    std::string name_;
    int shmFd_ = -1;
    void* mapped_ = nullptr;
    size_t totalSize_ = 0;
    void* payload_ = nullptr;  // ShmPayload* (defined locally in the .cpp)
    int doorbellFd_ = -1;
    int producerDoorbellFd_ = -1;
    bool valid_ = false;
};

#endif  // MACMU_SHELL_FRAME_CONSUMER_H
