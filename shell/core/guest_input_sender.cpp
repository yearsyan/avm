// SPDX-License-Identifier: MIT

#include "guest_input_sender.h"

#include "shell_options.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <cmath>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

void log_message(const char* format, ...) {
    FILE* file = std::fopen("/tmp/macmu-input-injector.log", "a");
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

std::string path_join(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs.back() == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

bool file_exists(const std::string& path) {
    return !path.empty() && access(path.c_str(), F_OK) == 0;
}

int milliscroll(float value) {
    constexpr float kScale = 1000.0f;
    constexpr float kMaxAxisValue = 1000.0f;
    const float clamped = std::clamp(value, -kMaxAxisValue, kMaxAxisValue);
    return static_cast<int>(std::lround(clamped * kScale));
}

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] ? std::string(value) : std::string();
}

std::string sdk_root_from_system_path(const std::string& system_path) {
    const std::string marker = "/system-images/";
    const size_t pos = system_path.find(marker);
    if (pos == std::string::npos) {
        return {};
    }
    return system_path.substr(0, pos);
}

std::string default_adb_path(const ShellOptions& options) {
    if (std::string path = env_value("MACMU_ADB_PATH"); !path.empty()) {
        return path;
    }

    std::vector<std::string> roots;
    if (std::string root = env_value("ANDROID_SDK_ROOT"); !root.empty()) {
        roots.push_back(root);
    }
    if (std::string root = env_value("ANDROID_HOME"); !root.empty()) {
        roots.push_back(root);
    }
    if (std::string root = sdk_root_from_system_path(options.systemPath); !root.empty()) {
        roots.push_back(root);
    }

    for (const std::string& root : roots) {
        const std::string path = path_join(root, "platform-tools/adb");
        if (file_exists(path)) {
            return path;
        }
    }
    return "adb";
}

std::string default_server_jar_path(const ShellOptions& options) {
    if (std::string path = env_value("MACMU_INPUT_SERVER_JAR"); !path.empty()) {
        return path;
    }
    return path_join(options.launcherDir, "lib/macmu-input-server.jar");
}

int run_process(const std::vector<std::string>& args, bool log_exit = true) {
    if (args.empty()) {
        return -1;
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, args[0].c_str(), nullptr, nullptr, argv.data(), environ);
    if (rc != 0) {
        log_message("failed to start %s: %s", args[0].c_str(), std::strerror(rc));
        return -1;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        log_message("waitpid failed for %s: %s", args[0].c_str(), std::strerror(errno));
        return -1;
    }
    if (log_exit) {
        log_message("process %s exited with status %d", args[0].c_str(), status);
    }
    return status;
}

int allocate_local_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_message("socket(port probe) failed: %s", std::strerror(errno));
        return 0;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log_message("bind(port probe) failed: %s", std::strerror(errno));
        close(fd);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        log_message("getsockname(port probe) failed: %s", std::strerror(errno));
        close(fd);
        return 0;
    }
    const int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

bool perform_handshake(int fd) {
    const char ping[] = "v\n";
    if (write(fd, ping, sizeof(ping) - 1) != static_cast<ssize_t>(sizeof(ping) - 1)) {
        return false;
    }

    pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int pollResult = poll(&pfd, 1, 1000);
    if (pollResult <= 0 || !(pfd.revents & POLLIN)) {
        return false;
    }

    char response[16] = {};
    const ssize_t readBytes = read(fd, response, sizeof(response) - 1);
    return readBytes >= 2 && std::strstr(response, "ok") != nullptr;
}

int connect_forwarded_port(int port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            if (perform_handshake(fd)) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                return fd;
            }
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    log_message("connect to forwarded port %d failed: %s", port, std::strerror(errno));
    return -1;
}

bool spawn_server(const std::string& adb_path) {
    const int status = run_process({
            adb_path,
            "-e",
            "shell",
            "setsid sh -c \"export CLASSPATH=/data/local/tmp/"
            "macmu-input-server.jar; exec app_process / "
            "dev.macmu.input.MacMuInputServer\" "
            ">/data/local/tmp/macmu-input-server.log 2>&1 < /dev/null &",
    });
    log_message("daemon input server start status %d", status);
    return status == 0;
}

bool wait_for_boot_completed(const std::string& adb_path,
                             const std::atomic<bool>& stop_requested) {
    for (int attempt = 0; attempt < 180; ++attempt) {
        if (stop_requested.load(std::memory_order_acquire)) {
            return false;
        }
        const int status = run_process({
                adb_path,
                "-e",
                "shell",
                "test \"$(getprop sys.boot_completed)\" = \"1\"",
        }, false);
        if (status == 0) {
            log_message("guest boot completed");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    log_message("guest boot wait timed out");
    return false;
}

}  // namespace

GuestInputSender::~GuestInputSender() {
    stop();
}

void GuestInputSender::start(const ShellOptions& options) {
    m_adbPath = default_adb_path(options);
    m_serverJarPath = default_server_jar_path(options);
    log_message("start adb=%s jar=%s", m_adbPath.c_str(), m_serverJarPath.c_str());
    m_stopRequested.store(false, std::memory_order_release);
    m_setupThread = std::thread([this] { setup_thread(); });
}

void GuestInputSender::stop() {
    m_stopRequested.store(true, std::memory_order_release);

    const int fd = m_writeFd.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
        close(fd);
    }

    if (m_setupThread.joinable()) {
        m_setupThread.join();
    }

    if (!m_adbPath.empty()) {
        run_process({m_adbPath, "-e", "shell", "pkill", "-f",
                     "dev.macmu.input.MacMuInputServer"});
    }

    const int port = m_forwardPort.exchange(0, std::memory_order_acq_rel);
    if (port > 0 && !m_adbPath.empty()) {
        run_process({m_adbPath, "-e", "forward", "--remove", "tcp:" + std::to_string(port)});
    }
}

void GuestInputSender::send_hover(uint32_t display_id, int x, int y) {
    const int fd = m_writeFd.load(std::memory_order_acquire);
    if (fd < 0) {
        return;
    }

    char line[96];
    const int len = std::snprintf(line, sizeof(line), "h %u %d %d\n", display_id, x, y);
    if (len <= 0 || len >= static_cast<int>(sizeof(line))) {
        return;
    }
    const ssize_t written = write(fd, line, static_cast<size_t>(len));
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EPIPE) {
        std::fprintf(stderr, "MacMu input injector write failed: %s\n", std::strerror(errno));
    }
}

void GuestInputSender::send_scroll(uint32_t display_id,
                                   int x,
                                   int y,
                                   float hscroll,
                                   float vscroll) {
    const int fd = m_writeFd.load(std::memory_order_acquire);
    if (fd < 0) {
        return;
    }

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
    const ssize_t written = write(fd, line, static_cast<size_t>(len));
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EPIPE) {
        std::fprintf(stderr, "MacMu input injector write failed: %s\n", std::strerror(errno));
    }
}

void GuestInputSender::setup_thread() {
    if (!file_exists(m_serverJarPath)) {
        log_message("jar missing: %s", m_serverJarPath.c_str());
        return;
    }

    log_message("waiting for adb device");
    run_process({m_adbPath, "-e", "wait-for-device"});
    if (m_stopRequested.load(std::memory_order_acquire)) {
        log_message("stop requested after wait-for-device");
        return;
    }

    log_message("waiting for guest boot");
    if (!wait_for_boot_completed(m_adbPath, m_stopRequested)) {
        return;
    }
    if (m_stopRequested.load(std::memory_order_acquire)) {
        log_message("stop requested after guest boot");
        return;
    }

    log_message("pushing input server jar");
    if (run_process({m_adbPath, "-e", "push", m_serverJarPath,
                     "/data/local/tmp/macmu-input-server.jar"}) != 0) {
        log_message("push failed");
        return;
    }
    if (m_stopRequested.load(std::memory_order_acquire)) {
        log_message("stop requested after push");
        return;
    }

    const int port = allocate_local_port();
    if (port <= 0) {
        return;
    }
    m_forwardPort.store(port, std::memory_order_release);
    const std::string forwardSpec = "tcp:" + std::to_string(port);
    log_message("forwarding %s to localabstract:macmu_input", forwardSpec.c_str());
    if (run_process({m_adbPath, "-e", "forward", forwardSpec, "localabstract:macmu_input"}) !=
        0) {
        log_message("adb forward failed");
        return;
    }

    run_process({m_adbPath, "-e", "shell", "pkill", "-f",
                 "dev.macmu.input.MacMuInputServer"});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!spawn_server(m_adbPath)) {
        return;
    }

    const int socketFd = connect_forwarded_port(port);
    if (socketFd < 0) {
        run_process({m_adbPath, "-e", "shell", "pkill", "-f",
                     "dev.macmu.input.MacMuInputServer"});
        return;
    }

    m_writeFd.store(socketFd, std::memory_order_release);
    log_message("ready");
    std::fprintf(stderr, "MacMu input injector ready.\n");
}
