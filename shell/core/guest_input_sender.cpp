// SPDX-License-Identifier: MIT

#include "guest_input_sender.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

void log_message(const char* format, ...) {
    FILE* file = std::fopen("/tmp/macmu-rpc-agent-host.log", "a");
    if (!file) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fclose(file);
}

int milliscroll(float value) {
    constexpr float kScale = 1000.0f;
    constexpr float kMaxAxisValue = 1000.0f;
    const float clamped = std::clamp(value, -kMaxAxisValue, kMaxAxisValue);
    return static_cast<int>(std::lround(clamped * kScale));
}

void close_fd(int fd) {
    if (fd >= 0) {
        close(fd);
    }
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

int create_listener(const std::string& path) {
    sockaddr_un addr = {};
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
        log_message("invalid Unix socket path: %s", path.c_str());
        return -1;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_message("socket(listener) failed: %s", std::strerror(errno));
        return -1;
    }

    unlink(path.c_str());

    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log_message("bind(listener:%s) failed: %s", path.c_str(), std::strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0) {
        log_message("listen failed: %s", std::strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
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
    const int pollResult = poll(&pfd, 1, 2000);
    if (pollResult <= 0 || !(pfd.revents & POLLIN)) {
        return false;
    }

    char response[16] = {};
    const ssize_t readBytes = read(fd, response, sizeof(response) - 1);
    return readBytes >= 2 && std::strstr(response, "ok") != nullptr;
}

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

}  // namespace

GuestInputSender::~GuestInputSender() {
    stop();
}

bool GuestInputSender::start(const std::string& socket_path) {
    stop();

    const int fd = create_listener(socket_path);
    if (fd < 0) {
        return false;
    }

    m_stopRequested.store(false, std::memory_order_release);
    m_socketPath = socket_path;
    m_listenFd.store(fd, std::memory_order_release);
    log_message("MacMu RPC host listener ready on %s", m_socketPath.c_str());
    m_acceptThread = std::thread([this] { accept_thread(); });
    return true;
}

void GuestInputSender::stop() {
    m_stopRequested.store(true, std::memory_order_release);

    const int listenFd = m_listenFd.exchange(-1, std::memory_order_acq_rel);
    if (listenFd >= 0) {
        shutdown(listenFd, SHUT_RDWR);
        close(listenFd);
    }
    wake_listener(m_socketPath);

    {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        close_client_locked();
    }

    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }

    if (!m_socketPath.empty()) {
        unlink(m_socketPath.c_str());
        m_socketPath.clear();
    }
}

void GuestInputSender::send_hover(uint32_t display_id, int x, int y) {
    char line[96];
    const int len = std::snprintf(line, sizeof(line), "h %u %d %d\n", display_id, x, y);
    if (len <= 0 || len >= static_cast<int>(sizeof(line))) {
        return;
    }
    send_line(line, len);
}

void GuestInputSender::send_hover_exit() {
    static constexpr char kLine[] = "e\n";
    send_line(kLine, static_cast<int>(sizeof(kLine) - 1));
}

void GuestInputSender::send_scroll(uint32_t display_id,
                                   int x,
                                   int y,
                                   float hscroll,
                                   float vscroll) {
    const int hscrollMilli = milliscroll(hscroll);
    const int vscrollMilli = milliscroll(vscroll);
    if (hscrollMilli == 0 && vscrollMilli == 0) {
        return;
    }

    char line[128];
    const int len = std::snprintf(line, sizeof(line), "s %u %d %d %d %d\n", display_id, x, y,
                                  hscrollMilli, vscrollMilli);
    if (len <= 0 || len >= static_cast<int>(sizeof(line))) {
        return;
    }
    send_line(line, len);
}

void GuestInputSender::send_touch(macmu::InputEventKind kind,
                                  uint32_t display_id,
                                  int pointer_id,
                                  int x,
                                  int y) {
    char phase = 0;
    switch (kind) {
        case macmu::InputEventKind::kTouchBegin:
            phase = 'b';
            break;
        case macmu::InputEventKind::kTouchUpdate:
            phase = 'm';
            break;
        case macmu::InputEventKind::kTouchEnd:
            phase = 'e';
            break;
        default:
            return;
    }

    char line[128];
    const int len = std::snprintf(line, sizeof(line), "t %u %d %c %d %d\n", display_id,
                                  pointer_id, phase, x, y);
    if (len <= 0 || len >= static_cast<int>(sizeof(line))) {
        return;
    }
    send_line(line, len);
}

void GuestInputSender::send_mouse_move(uint32_t display_id, int x, int y, uint32_t buttons) {
    char line[128];
    const int len = std::snprintf(line, sizeof(line), "m %u %d %d %u\n", display_id, x, y,
                                  buttons);
    if (len <= 0 || len >= static_cast<int>(sizeof(line))) {
        return;
    }
    send_line(line, len);
}

void GuestInputSender::send_mouse_button(uint32_t display_id, int x, int y, uint32_t buttons) {
    char line[128];
    const int len = std::snprintf(line, sizeof(line), "b %u %d %d %u\n", display_id, x, y,
                                  buttons);
    if (len <= 0 || len >= static_cast<int>(sizeof(line))) {
        return;
    }
    send_line(line, len);
}

void GuestInputSender::accept_thread() {
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        const int listenFd = m_listenFd.load(std::memory_order_acquire);
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
            if (!m_stopRequested.load(std::memory_order_acquire)) {
                log_message("accept failed: %s", std::strerror(errno));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        if (!perform_handshake(fd)) {
            log_message("guest RPC handshake failed");
            close(fd);
            continue;
        }

        set_nonblocking(fd);
        {
            std::lock_guard<std::mutex> lock(m_socketMutex);
            close_client_locked();
            m_writeFd.store(fd, std::memory_order_release);
        }

        log_message("MacMu RPC guest agent connected");
        std::fprintf(stderr, "MacMu RPC guest agent connected.\n");
    }
}

void GuestInputSender::close_client_locked() {
    const int fd = m_writeFd.exchange(-1, std::memory_order_acq_rel);
    close_fd(fd);
}

bool GuestInputSender::send_line(const char* line, int len) {
    std::lock_guard<std::mutex> lock(m_socketMutex);
    const int fd = m_writeFd.load(std::memory_order_acquire);
    if (fd < 0) {
        return false;
    }

    const ssize_t written = send(fd, line, static_cast<size_t>(len), 0);
    if (written == static_cast<ssize_t>(len)) {
        return true;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return false;
    }

    if (written < 0) {
        log_message("MacMu RPC write failed: %s", std::strerror(errno));
    } else {
        log_message("MacMu RPC short write: %zd/%d", written, len);
    }
    close_client_locked();
    return false;
}
