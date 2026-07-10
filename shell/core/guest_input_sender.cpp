// SPDX-License-Identifier: MIT

#include "guest_input_sender.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "posix_util.h"
#include "unix_listener.h"

namespace {

namespace fs = std::filesystem;
using macmu::shell::path_join;

void ensure_directory_best_effort(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
}

std::string log_path_for(const std::string& app_data_dir) {
    const std::string logDir = path_join(app_data_dir, "logs");
    ensure_directory_best_effort(logDir);
    return path_join(logDir, "macmu-rpc-agent-host." +
                                std::to_string(static_cast<unsigned>(getpid())) + ".log");
}

void log_message(const std::string& path, const char* format, ...) {
    if (path.empty()) {
        return;
    }
    FILE* file = std::fopen(path.c_str(), "a");
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

bool GuestInputSender::start(const std::string& socket_path, const std::string& app_data_dir) {
    stop();

    m_logPath = log_path_for(app_data_dir);
    const int fd = macmu::shell::create_unix_listener(
        socket_path, [this](const std::string& message) {
            log_message(m_logPath, "%s", message.c_str());
        });
    if (fd < 0) {
        return false;
    }

    m_stopRequested.store(false, std::memory_order_release);
    m_socketPath = socket_path;
    m_listenFd.store(fd, std::memory_order_release);
    log_message(m_logPath, "MacMu RPC host listener ready on %s", m_socketPath.c_str());
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
    macmu::shell::wake_unix_listener(m_socketPath);

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

bool GuestInputSender::send_hover(uint32_t display_id, int x, int y) {
    char line[96];
    const int len = std::snprintf(line, sizeof(line), "h %u %d %d\n", display_id, x, y);
    if (len <= 0 || len >= static_cast<int>(sizeof(line))) {
        return false;
    }
    return send_line(line, len, false);
}

bool GuestInputSender::send_hover_exit() {
    static constexpr char kLine[] = "e\n";
    return send_line(kLine, static_cast<int>(sizeof(kLine) - 1), true);
}

bool GuestInputSender::send_scroll(uint32_t display_id,
                                   int x,
                                   int y,
                                   float hscroll,
                                   float vscroll) {
    const int hscrollMilli = milliscroll(hscroll);
    const int vscrollMilli = milliscroll(vscroll);
    if (hscrollMilli == 0 && vscrollMilli == 0) {
        return true;
    }

    char line[128];
    const int len = std::snprintf(line, sizeof(line), "s %u %d %d %d %d\n", display_id, x, y,
                                  hscrollMilli, vscrollMilli);
    if (len <= 0 || len >= static_cast<int>(sizeof(line))) {
        return false;
    }
    return send_line(line, len, false);
}

bool GuestInputSender::send_touch(macmu::InputEventKind kind,
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
            return false;
    }

    char line[128];
    const int len = std::snprintf(line, sizeof(line), "t %u %d %c %d %d\n", display_id,
                                  pointer_id, phase, x, y);
    if (len <= 0 || len >= static_cast<int>(sizeof(line))) {
        return false;
    }
    return send_line(line, len, phase != 'm');
}

bool GuestInputSender::send_mouse_move(uint32_t display_id, int x, int y, uint32_t buttons) {
    char line[128];
    const int len = std::snprintf(line, sizeof(line), "m %u %d %d %u\n", display_id, x, y,
                                  buttons);
    if (len <= 0 || len >= static_cast<int>(sizeof(line))) {
        return false;
    }
    return send_line(line, len, false);
}

bool GuestInputSender::send_mouse_button(uint32_t display_id, int x, int y, uint32_t buttons) {
    char line[128];
    const int len = std::snprintf(line, sizeof(line), "b %u %d %d %u\n", display_id, x, y,
                                  buttons);
    if (len <= 0 || len >= static_cast<int>(sizeof(line))) {
        return false;
    }
    return send_line(line, len, true);
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
                log_message(m_logPath, "accept failed: %s", std::strerror(errno));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        if (!macmu::shell::set_close_on_exec(fd)) {
            log_message(m_logPath, "failed to set FD_CLOEXEC on guest RPC connection");
            close(fd);
            continue;
        }

        if (!macmu::shell::perform_unix_pipe_handshake(fd)) {
            log_message(m_logPath, "guest RPC handshake failed");
            close(fd);
            continue;
        }

        set_nonblocking(fd);
        {
            std::lock_guard<std::mutex> lock(m_socketMutex);
            close_client_locked();
            m_writeFd.store(fd, std::memory_order_release);
        }

        log_message(m_logPath, "MacMu RPC guest agent connected");
        std::fprintf(stderr, "MacMu RPC guest agent connected.\n");
    }
}

void GuestInputSender::close_client_locked() {
    const int fd = m_writeFd.exchange(-1, std::memory_order_acq_rel);
    close_fd(fd);
}

bool GuestInputSender::send_line(const char* line, int len, bool reliable) {
    std::lock_guard<std::mutex> lock(m_socketMutex);
    const int fd = m_writeFd.load(std::memory_order_acquire);
    if (fd < 0) {
        return false;
    }

    ssize_t written = send(fd, line, static_cast<size_t>(len), 0);
    if (written < 0 && reliable && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        pollfd pfd = {fd, POLLOUT, 0};
        int pollResult;
        do {
            pollResult = poll(&pfd, 1, 5);
        } while (pollResult < 0 && errno == EINTR);
        if (pollResult > 0 && (pfd.revents & POLLOUT)) {
            written = send(fd, line, static_cast<size_t>(len), 0);
        }
    }
    if (written == static_cast<ssize_t>(len)) {
        return true;
    }
    if (!reliable && written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return false;
    }

    if (written < 0) {
        log_message(m_logPath, "MacMu RPC write failed: %s", std::strerror(errno));
    } else {
        log_message(m_logPath, "MacMu RPC short write: %zd/%d", written, len);
    }
    close_client_locked();
    return false;
}
