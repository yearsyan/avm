// SPDX-License-Identifier: MIT
//
// ControlChannel: shell side of the MMCP control plane (see
// shell/protocol/macmu_control_protocol.h). One instance lives for exactly one
// qemu generation: create() before posix_spawn (the remote end is dup2()'d
// into child fd 197), close_remote_fd() after the spawn, destroy after the
// child is reaped. qemu exiting closes its end, the reader thread sees EOF and
// the channel reports dead().
//
// Requests are asynchronous: request() writes the frame on the caller thread
// and the completion callback fires on the reader thread (or inline on write
// failure). Callers that need main-thread delivery bounce in the callback.

#ifndef MACMU_SHELL_CONTROL_CHANNEL_H
#define MACMU_SHELL_CONTROL_CHANNEL_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "macmu_control_protocol.h"

class ControlChannel {
   public:
    struct Response {
        bool ok = false;          // false: error response, timeout, or channel death
        uint16_t type = 0;        // ControlMessageType of the response frame
        int32_t errorCode = 0;    // macmu::kControlErr* when !ok
        std::string errorMessage;
        std::vector<uint8_t> payload;
    };
    using ResponseCallback = std::function<void(Response)>;
    // type, payload. Fired on the reader thread.
    using EventCallback = std::function<void(uint16_t, std::vector<uint8_t>)>;
    using ClosedCallback = std::function<void()>;

    ControlChannel() = default;
    ~ControlChannel();
    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;

    // Create the socketpair. Call before spawning qemu.
    bool create();

    // Set callbacks, then start the reader thread and send HELLO. Call after
    // create(); safe to call before or after the child spawn.
    void start(EventCallback on_event, ClosedCallback on_closed);

    // The end inherited by qemu (dup2 target: macmu::kControlChildFd). Close
    // it in the parent right after posix_spawn so EOF detection works.
    int remote_fd() const { return remoteFd_; }
    void close_remote_fd();

    bool alive() const { return alive_.load(std::memory_order_acquire); }

    // Send a request; |callback| fires with the response, an ERROR, or a
    // timeout/channel-death failure. |timeout_ms| is enforced by the reader
    // thread's sweep, so it is approximate (+<1s).
    void request(macmu::ControlMessageType type, const void* payload, uint32_t length,
                 uint64_t timeout_ms, ResponseCallback callback);

    void stop();

   private:
    struct Pending {
        ResponseCallback callback;
        uint64_t deadlineMs = 0;
    };

    void reader_thread();
    bool read_exact(void* buffer, size_t size);
    bool write_frame(uint16_t type, uint32_t request_id, const void* payload, uint32_t length);
    void fail_all_pending(int32_t code, const char* message);
    void sweep_timeouts();
    static uint64_t steady_now_ms();

    int localFd_ = -1;
    int remoteFd_ = -1;
    std::atomic<bool> alive_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<uint32_t> nextRequestId_{1};
    std::thread readerThread_;
    std::mutex writeMutex_;
    std::mutex pendingMutex_;
    std::map<uint32_t, Pending> pending_;
    EventCallback onEvent_;
    ClosedCallback onClosed_;
};

#endif  // MACMU_SHELL_CONTROL_CHANNEL_H
