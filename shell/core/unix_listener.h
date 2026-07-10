// SPDX-License-Identifier: MIT

#ifndef MACMU_SHELL_UNIX_LISTENER_H
#define MACMU_SHELL_UNIX_LISTENER_H

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "posix_util.h"

namespace macmu::shell {

using UnixListenerLog = std::function<void(const std::string&)>;

inline void log_unix_listener_message(const UnixListenerLog& log, const std::string& message) {
    if (log) {
        log(message);
    }
}

inline int create_unix_listener(const std::string& path, UnixListenerLog log = {}) {
    sockaddr_un addr = {};
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
        log_unix_listener_message(log, "invalid Unix socket path: " + path);
        return -1;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_unix_listener_message(log,
                                  std::string("socket(listener) failed: ") + std::strerror(errno));
        return -1;
    }
    if (!set_close_on_exec(fd)) {
        log_unix_listener_message(log, "failed to set FD_CLOEXEC on Unix listener");
        close(fd);
        return -1;
    }

    unlink(path.c_str());

    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log_unix_listener_message(log, "bind(listener:" + path +
                                           ") failed: " + std::strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0) {
        log_unix_listener_message(log, std::string("listen failed: ") + std::strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

inline void wake_unix_listener(const std::string& path) {
    if (path.empty()) {
        return;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    set_close_on_exec(fd);

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(fd);
}

inline bool perform_unix_pipe_handshake(int fd) {
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
    const int pollResult = poll(&pfd, 1, 2000);
    if (pollResult <= 0 || !(pfd.revents & POLLIN)) {
        return false;
    }

    char response[16] = {};
    const ssize_t readBytes = read(fd, response, sizeof(response) - 1);
    return readBytes >= 2 && std::strstr(response, "ok") != nullptr;
}

}  // namespace macmu::shell

#endif  // MACMU_SHELL_UNIX_LISTENER_H
