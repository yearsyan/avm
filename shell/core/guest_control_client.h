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
// Commands include app discovery/lifecycle/uninstall plus "display-state
// <id>", which returns the Android logical width, height, and rotation for
// orientation sync.
// APK installation is the one binary extension: "<id> install <size>\n" is
// followed immediately by exactly |size| APK bytes. The response remains an
// ordinary line, so it shares the existing pending-request machinery.

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
    // Connected state after the protocol handshake. Fired on the accept/reader
    // thread; callers that touch UI state must bounce to their UI thread.
    using ConnectionCallback = std::function<void(bool)>;

    GuestControlClient() = default;
    ~GuestControlClient();
    GuestControlClient(const GuestControlClient&) = delete;
    GuestControlClient& operator=(const GuestControlClient&) = delete;

    bool start(const std::string& socket_path,
               ConnectionCallback on_connection_changed = {});
    void stop();

    bool ready() const { return clientFd_.load(std::memory_order_acquire) >= 0; }

    // Send "<id> <command>\n" and register |callback| for the response.
    // |command| must be a single line without '\n'. Times out after
    // |timeout_ms| (approximate; enforced by the reader thread's sweep).
    void request(const std::string& command, uint64_t timeout_ms, ResponseCallback callback);

    // Stream one host APK into the guest and request a replace/test install.
    // File transfer is synchronous, so callers must invoke this from a worker
    // thread. Completion remains asynchronous and fires on the reader thread.
    // At most one APK transfer may be active at a time; normal line requests
    // fail fast while its binary payload owns the stream.
    void install_apk(const std::string& apk_path, uint64_t timeout_ms,
                     ResponseCallback callback);

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
    std::atomic<bool> fileTransferActive_{false};
    ConnectionCallback connectionCallback_;
    macmu::shell::PendingRequestTable<uint64_t, ResponseCallback> pending_;
    std::atomic<uint64_t> nextRequestId_{1};
};

#endif  // MACMU_SHELL_GUEST_CONTROL_CLIENT_H
