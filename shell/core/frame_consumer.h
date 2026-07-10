// SPDX-License-Identifier: MIT
//
// FrameConsumer: the shell-side half of the cross-process frame channel (v2).
//
// This mirrors the layout and naming convention used by the gfxstream producer
// (host/common/iosurface_export.cpp) but is deliberately self-contained so the
// MIT-licensed shell does not depend on any gfxstream headers. The shared
// memory object is named "macmu.frame.<wrapperPid>" (the shell's own pid,
// which it exports to qemu via ANDROID_EMULATOR_WRAPPER_PID), and an inherited
// FD doorbell carries a notification for each frame. The shell keeps the
// consumer end and passes the producer end to qemu by fd inheritance.
//
// Protocol v2: the shm object holds a fixed table of per-display slots
// (slot index == Android display id), each an independent odd/even seqlock.
// One doorbell serves all displays; its payload is advisory and readers
// re-scan the slot table on wake. See docs/FRAME_CHANNEL_V2_CONTROL_PLANE.md.
//
// When the channel cannot be established, valid() returns false and callers
// should avoid launching a frame-driven display path.

#ifndef MACMU_SHELL_FRAME_CONSUMER_H
#define MACMU_SHELL_FRAME_CONSUMER_H

#include <cstdint>
#include <mutex>
#include <string>

#include "surface_metadata.h"

inline constexpr uint32_t kMacmuFrameSlotCount = 16;

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

    // Zero the whole slot table. Call between qemu generations so a restarted
    // producer starts from a clean table (its frame counters reset too).
    void reset_slots();

    // Seqlock read of one display slot: sample the sequence (reject odd =
    // writer active), read the payload, re-read the sequence, accept only if
    // unchanged. Returns false if the slot has never been published.
    bool read(uint32_t display_id, SurfaceMetadata* out);

    // Block until ANY display has a frame newer than its entry in
    // |last_frames| (array of kMacmuFrameSlotCount cursors) or |timeout_ms|
    // elapses. On success returns true and stores the ready display id in
    // |out_display_id|; the caller re-reads the slot and updates its cursor.
    // Drains coalesced doorbell notifications before scanning.
    bool wait_for_any_frame(const uint64_t* last_frames, uint64_t timeout_ms,
                            uint32_t* out_display_id);

   private:
    static uint64_t steady_now_ms();
    void drain_notifications();
    bool scan_slots(const uint64_t* last_frames, uint32_t* out_display_id);
    void teardown();

    std::string name_;
    int shmFd_ = -1;
    void* mapped_ = nullptr;
    size_t totalSize_ = 0;
    void* slots_ = nullptr;  // ShmDisplaySlot* (defined locally in the .cpp)
    int doorbellFd_ = -1;
    int producerDoorbellFd_ = -1;
    uint32_t nextScanStart_ = 0;
    mutable std::mutex slotsMutex_;
    bool valid_ = false;
};

#endif  // MACMU_SHELL_FRAME_CONSUMER_H
