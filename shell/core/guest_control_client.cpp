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

namespace {

void close_fd(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

int create_listener(const std::string& path) {
    sockaddr_un addr = {};
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, "MacMu guest control: invalid socket path %s\n", path.c_str());
        return -1;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    unlink(path.c_str());
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void wake_listener(const std::string& path) {
    if (path.empty()) {
        return;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(fd);
}

bool perform_handshake(int fd) {
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    const char ping[] = "v\n";
    if (send(fd, ping, sizeof(ping) - 1, 0) != static_cast<ssize_t>(sizeof(ping) - 1)) {
        return false;
    }
    pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 2000) <= 0 || !(pfd.revents & POLLIN)) {
        return false;
    }
    char response[16] = {};
    const ssize_t bytes = read(fd, response, sizeof(response) - 1);
    return bytes >= 2 && std::strstr(response, "ok") != nullptr;
}

}  // namespace

GuestControlClient::~GuestControlClient() {
    stop();
}

bool GuestControlClient::start(const std::string& socket_path) {
    stop();
    const int fd = create_listener(socket_path);
    if (fd < 0) {
        return false;
    }
    socketPath_ = socket_path;
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
    wake_listener(socketPath_);
    const int clientFd = clientFd_.exchange(-1, std::memory_order_acq_rel);
    if (clientFd >= 0) {
        shutdown(clientFd, SHUT_RDWR);
        // serve_connection owns the close.
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    if (!socketPath_.empty()) {
        unlink(socketPath_.c_str());
        socketPath_.clear();
    }
    fail_all_pending("guest control client stopped");
}

void GuestControlClient::request(const std::string& command, uint64_t timeout_ms,
                                 ResponseCallback callback) {
    const int fd = clientFd_.load(std::memory_order_acquire);
    if (fd < 0) {
        if (callback) {
            callback(false, "guest agent not connected");
        }
        return;
    }
    const uint64_t requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
    if (callback) {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_[requestId] = Pending{std::move(callback), steady_now_ms() + timeout_ms};
    }

    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "%llu ", static_cast<unsigned long long>(requestId));
    const std::string line = std::string(prefix) + command + "\n";

    bool writeFailed = false;
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        size_t done = 0;
        while (done < line.size()) {
            const ssize_t n = send(fd, line.data() + done, line.size() - done, 0);
            if (n > 0) {
                done += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            writeFailed = true;
            break;
        }
    }
    if (writeFailed) {
        ResponseCallback failed;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            auto it = pending_.find(requestId);
            if (it != pending_.end()) {
                failed = std::move(it->second.callback);
                pending_.erase(it);
            }
        }
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
        if (!perform_handshake(fd)) {
            close(fd);
            continue;
        }
        clientFd_.store(fd, std::memory_order_release);
        std::fprintf(stderr, "MacMu guest control agent connected.\n");
        serve_connection(fd);
        int expected = fd;
        clientFd_.compare_exchange_strong(expected, -1, std::memory_order_acq_rel);
        close_fd(fd);
        fail_all_pending("guest agent disconnected");
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

    ResponseCallback callback;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        auto it = pending_.find(static_cast<uint64_t>(id));
        if (it != pending_.end()) {
            callback = std::move(it->second.callback);
            pending_.erase(it);
        }
    }
    if (callback) {
        callback(ok, std::string(rest));
    }
}

void GuestControlClient::fail_all_pending(const char* message) {
    std::map<uint64_t, Pending> failed;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        failed.swap(pending_);
    }
    for (auto& entry : failed) {
        if (entry.second.callback) {
            entry.second.callback(false, message);
        }
    }
}

void GuestControlClient::sweep_timeouts() {
    const uint64_t now = steady_now_ms();
    std::vector<ResponseCallback> expired;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.deadlineMs <= now) {
                expired.push_back(std::move(it->second.callback));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& callback : expired) {
        if (callback) {
            callback(false, "guest request timed out");
        }
    }
}

uint64_t GuestControlClient::steady_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
