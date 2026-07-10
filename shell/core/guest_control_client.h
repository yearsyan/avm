// SPDX-License-Identifier: MIT
//
// GuestControlClient: host side of the MacMu guest agent's control RPC
// connection. The agent connects out through qemu's pipe:unix transport to a
// dedicated host Unix socket (separate from the input socket so bulky
// responses never head-of-line block input events).
//
// Wire protocol (ASCII lines):
//   host -> agent:  "v\n" handshake, then "<id> <command> [args...]\n"
//   agent -> host:  "ok\n" handshake reply, then "<id> ok [payload]\n"
//                   or "<id> err [message]\n"
// Payloads are single-line (the agent uses JSON for structured data).

#ifndef MACMU_SHELL_GUEST_CONTROL_CLIENT_H
#define MACMU_SHELL_GUEST_CONTROL_CLIENT_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "pending_request_table.h"

class GuestControlClient {
   public:
    // ok, payload (or error message when !ok). Fired on the reader thread.
    using ResponseCallback = std::function<void(bool, std::string)>;

    GuestControlClient() = default;
    ~GuestControlClient();
    GuestControlClient(const GuestControlClient&) = delete;
    GuestControlClient& operator=(const GuestControlClient&) = delete;

    bool start(const std::string& socket_path);
    void stop();

    bool ready() const { return clientFd_.load(std::memory_order_acquire) >= 0; }

    // Send "<id> <command>\n" and register |callback| for the response.
    // |command| must be a single line without '\n'. Times out after
    // |timeout_ms| (approximate; enforced by the reader thread's sweep).
    void request(const std::string& command, uint64_t timeout_ms, ResponseCallback callback);

   private:
    void accept_thread();
    void serve_connection(int fd);
    void handle_line(const std::string& line);
    void fail_all_pending(const char* message);
    void sweep_timeouts();

    std::string socketPath_;
    std::atomic<int> listenFd_{-1};
    std::atomic<int> clientFd_{-1};
    std::atomic<bool> stopRequested_{false};
    std::thread acceptThread_;
    std::mutex writeMutex_;
    macmu::shell::PendingRequestTable<uint64_t, ResponseCallback> pending_;
    std::atomic<uint64_t> nextRequestId_{1};
};

#endif  // MACMU_SHELL_GUEST_CONTROL_CLIENT_H
