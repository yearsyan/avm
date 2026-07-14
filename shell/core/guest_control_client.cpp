// SPDX-License-Identifier: MIT

#include "guest_control_client.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "posix_util.h"
#include "unix_listener.h"

namespace {

void close_fd(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

}  // namespace

GuestControlClient::~GuestControlClient() {
    stop();
}

bool GuestControlClient::start(const std::string& socket_path,
                               ConnectionCallback on_connection_changed) {
    stop();
    const int fd = macmu::shell::create_unix_listener(socket_path, [](const std::string& message) {
        std::fprintf(stderr, "MacMu guest control: %s\n", message.c_str());
    });
    if (fd < 0) {
        return false;
    }
    socketPath_ = socket_path;
    connectionCallback_ = std::move(on_connection_changed);
    stopRequested_.store(false, std::memory_order_release);
    listenFd_.store(fd, std::memory_order_release);
    acceptThread_ = std::thread([this] { accept_thread(); });
    return true;
}

void GuestControlClient::stop() {
    stopRequested_.store(true, std::memory_order_release);
    const int listenFd = listenFd_.exchange(-1, std::memory_order_acq_rel);
    if (listenFd >= 0) {
        shutdown(listenFd, SHUT_RDWR);
        close(listenFd);
    }
    macmu::shell::wake_unix_listener(socketPath_);
    {
        // The accept thread owns both publishing and clearing clientFd_. Leave
        // the state published until it performs the connection cleanup so its
        // true/false notifications remain ordered. shutdown() wakes its reader.
        std::lock_guard<std::mutex> lock(writeMutex_);
        const int clientFd = clientFd_.load(std::memory_order_acquire);
        if (clientFd >= 0) {
            shutdown(clientFd, SHUT_RDWR);
        }
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    if (!socketPath_.empty()) {
        unlink(socketPath_.c_str());
        socketPath_.clear();
    }
    fail_all_pending("guest control client stopped");
    connectionCallback_ = {};
}

void GuestControlClient::request(const std::string& command, uint64_t timeout_ms,
                                 ResponseCallback callback) {
    if (clientFd_.load(std::memory_order_acquire) < 0) {
        if (callback) {
            callback(false, "guest agent not connected");
        }
        return;
    }
    const uint64_t requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
    if (callback) {
        pending_.add(requestId, std::move(callback), timeout_ms);
    }

    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "%llu ", static_cast<unsigned long long>(requestId));
    const std::string line = std::string(prefix) + command + "\n";

    bool writeFailed = false;
    {
        // Re-load the fd under writeMutex_: the accept thread closes a dead
        // client's fd under the same mutex, so the fd sampled here cannot be
        // closed (or reused by the kernel) while we are sending on it.
        std::lock_guard<std::mutex> lock(writeMutex_);
        const int fd = clientFd_.load(std::memory_order_acquire);
        if (fd < 0) {
            writeFailed = true;
        }
        size_t done = 0;
        while (!writeFailed && done < line.size()) {
            const ssize_t n = send(fd, line.data() + done, line.size() - done, 0);
            if (n > 0) {
                done += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            writeFailed = true;
        }
    }
    if (writeFailed) {
        ResponseCallback failed = pending_.take(requestId);
        if (failed) {
            failed(false, "guest control write failed");
        }
    }
}

void GuestControlClient::accept_thread() {
    while (!stopRequested_.load(std::memory_order_acquire)) {
        const int listenFd = listenFd_.load(std::memory_order_acquire);
        if (listenFd < 0) {
            break;
        }
        sockaddr_un peer = {};
        socklen_t peerLen = sizeof(peer);
        const int fd = accept(listenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!stopRequested_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        if (!macmu::shell::set_close_on_exec(fd)) {
            close(fd);
            continue;
        }
        if (!macmu::shell::perform_unix_pipe_handshake(fd)) {
            close(fd);
            continue;
        }

        bool connected = false;
        {
            // Synchronize publication with stop(): once stopRequested_ is set,
            // a handshake that was already in flight must not make ready()
            // become true again.
            std::lock_guard<std::mutex> lock(writeMutex_);
            if (!stopRequested_.load(std::memory_order_acquire)) {
                clientFd_.store(fd, std::memory_order_release);
                connected = true;
            }
        }
        if (!connected) {
            close(fd);
            break;
        }
        std::fprintf(stderr, "MacMu guest control agent connected.\n");
        if (connectionCallback_) {
            connectionCallback_(true);
        }
        serve_connection(fd);
        bool disconnected = false;
        {
            // Publish the disconnect and close under writeMutex_ so an
            // in-flight request() cannot send on the closed/reused fd.
            std::lock_guard<std::mutex> lock(writeMutex_);
            int expected = fd;
            disconnected = clientFd_.compare_exchange_strong(
                expected, -1, std::memory_order_acq_rel);
            close_fd(fd);
        }
        fail_all_pending("guest agent disconnected");
        if (disconnected && connectionCallback_) {
            connectionCallback_(false);
        }
    }
}

void GuestControlClient::serve_connection(int fd) {
    std::string buffer;
    std::vector<char> chunk(16 * 1024);
    while (!stopRequested_.load(std::memory_order_acquire)) {
        pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pollResult = poll(&pfd, 1, 1000);
        if (pollResult == 0) {
            sweep_timeouts();
            continue;
        }
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        const ssize_t bytes = read(fd, chunk.data(), chunk.size());
        if (bytes <= 0) {
            if (bytes < 0 && errno == EINTR) {
                continue;
            }
            return;  // agent disconnected
        }
        buffer.append(chunk.data(), static_cast<size_t>(bytes));
        size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                handle_line(line);
            }
        }
        // Guard against a runaway agent flooding an unterminated line.
        if (buffer.size() > (4u << 20)) {
            std::fprintf(stderr, "MacMu guest control: oversized response line; dropping.\n");
            return;
        }
    }
}

void GuestControlClient::handle_line(const std::string& line) {
    // "<id> ok [payload]" | "<id> err [message]"
    char* end = nullptr;
    errno = 0;
    const unsigned long long id = std::strtoull(line.c_str(), &end, 10);
    if (errno != 0 || end == line.c_str() || *end != ' ') {
        return;  // not a response line (e.g. stray handshake output); ignore
    }
    const char* rest = end + 1;
    bool ok = false;
    if (std::strncmp(rest, "ok", 2) == 0 && (rest[2] == '\0' || rest[2] == ' ')) {
        ok = true;
        rest += 2;
    } else if (std::strncmp(rest, "err", 3) == 0 && (rest[3] == '\0' || rest[3] == ' ')) {
        rest += 3;
    } else {
        return;
    }
    if (*rest == ' ') {
        ++rest;
    }

    ResponseCallback callback = pending_.take(static_cast<uint64_t>(id));
    if (callback) {
        callback(ok, std::string(rest));
    }
}

void GuestControlClient::fail_all_pending(const char* message) {
    pending_.fail_all([&](ResponseCallback callback) { callback(false, message); });
}

void GuestControlClient::sweep_timeouts() {
    pending_.sweep_timeouts(
        [](ResponseCallback callback) { callback(false, "guest request timed out"); });
}
