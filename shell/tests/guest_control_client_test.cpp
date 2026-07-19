// SPDX-License-Identifier: MIT

#include "guest_control_client.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void send_all(int fd, const std::string& bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t result = send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (result > 0) {
            sent += static_cast<size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error("socket write failed");
        }
    }
}

std::string read_line(int fd) {
    std::string line;
    while (true) {
        char byte = 0;
        const ssize_t result = read(fd, &byte, 1);
        if (result == 1) {
            if (byte == '\n') {
                return line;
            }
            line.push_back(byte);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error("socket closed before line terminator");
        }
    }
}

std::vector<char> read_exact(int fd, size_t size) {
    std::vector<char> bytes(size);
    size_t received = 0;
    while (received < size) {
        const ssize_t result = read(fd, bytes.data() + received, size - received);
        if (result > 0) {
            received += static_cast<size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error("socket closed during binary payload");
        }
    }
    return bytes;
}

int connect_with_retry(const std::string& socket_path) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error("socket creation failed");
        }
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path.c_str());
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("could not connect fake guest agent");
}

struct TemporaryFiles {
    explicit TemporaryFiles(std::string suffix)
        : socketPath((fs::current_path() / ("macmu-control-test-" + suffix + ".sock")).string()),
          apkPath((fs::current_path() / ("macmu-control-test-" + suffix + ".apk")).string()) {}

    ~TemporaryFiles() {
        std::error_code error;
        fs::remove(socketPath, error);
        fs::remove(apkPath, error);
    }

    std::string socketPath;
    std::string apkPath;
};

}  // namespace

int main() {
    TemporaryFiles files(std::to_string(static_cast<unsigned>(getpid())));
    GuestControlClient client;
    std::thread guest;
    try {
        const char rawApkBytes[] = {'P', 'K', '\x03', '\x04', '\0', 'b', 'i', 'n', 'a', 'r', 'y',
                                    '\n', 'l', 'i', 'n',  'e',  '\r', '\n', 'e', 'n', 'd'};
        const std::string apkBytes(rawApkBytes, sizeof(rawApkBytes));
        {
            std::ofstream apk(files.apkPath, std::ios::binary | std::ios::trunc);
            apk.write(apkBytes.data(), static_cast<std::streamsize>(apkBytes.size()));
            require(static_cast<bool>(apk), "failed to create test APK");
        }
        std::mutex stateMutex;
        std::condition_variable stateChanged;
        bool connected = false;
        require(client.start(files.socketPath, [&](bool value) {
                    {
                        std::lock_guard<std::mutex> lock(stateMutex);
                        connected = value;
                    }
                    stateChanged.notify_all();
                }),
                "failed to start guest control client");

        std::exception_ptr guestFailure;
        guest = std::thread([&] {
            int fd = -1;
            try {
                fd = connect_with_retry(files.socketPath);
                require(read_line(fd) == "v", "unexpected handshake request");
                send_all(fd, "ok\n");

                const std::string installLine = read_line(fd);
                std::istringstream install(installLine);
                uint64_t installId = 0;
                std::string command;
                size_t byteCount = 0;
                install >> installId >> command >> byteCount;
                require(command == "install", "missing install command");
                require(byteCount == apkBytes.size(), "wrong APK byte count");
                const std::vector<char> payload = read_exact(fd, byteCount);
                require(std::string(payload.data(), payload.size()) == apkBytes,
                        "APK payload changed in transit");
                send_all(fd, std::to_string(installId) + " ok\n");

                const std::string appsLine = read_line(fd);
                std::istringstream apps(appsLine);
                uint64_t appsId = 0;
                apps >> appsId >> command;
                require(command == "apps", "line protocol did not resume after APK payload");
                send_all(fd, std::to_string(appsId) + " ok []\n");

                const std::string uninstallLine = read_line(fd);
                std::istringstream uninstall(uninstallLine);
                uint64_t uninstallId = 0;
                std::string packageName;
                uninstall >> uninstallId >> command >> packageName;
                require(command == "uninstall", "missing uninstall command");
                require(packageName == "com.example.systemapp", "wrong uninstall package");
                send_all(fd, std::to_string(uninstallId) +
                                 " err System applications cannot be uninstalled\n");
                close(fd);
            } catch (...) {
                if (fd >= 0) {
                    close(fd);
                }
                guestFailure = std::current_exception();
            }
        });

        {
            std::unique_lock<std::mutex> lock(stateMutex);
            require(stateChanged.wait_for(lock, std::chrono::seconds(3), [&] { return connected; }),
                    "guest control handshake timed out");
        }

        std::mutex responseMutex;
        std::condition_variable responseChanged;
        bool installDone = false;
        bool installOk = false;
        client.install_apk(files.apkPath, 3000, [&](bool ok, std::string) {
            {
                std::lock_guard<std::mutex> lock(responseMutex);
                installDone = true;
                installOk = ok;
            }
            responseChanged.notify_all();
        });
        {
            std::unique_lock<std::mutex> lock(responseMutex);
            require(responseChanged.wait_for(lock, std::chrono::seconds(3),
                                             [&] { return installDone; }),
                    "install response timed out");
            require(installOk, "install response reported failure");
        }

        bool appsDone = false;
        bool appsOk = false;
        std::string appsPayload;
        client.request("apps", 3000, [&](bool ok, std::string payload) {
            {
                std::lock_guard<std::mutex> lock(responseMutex);
                appsDone = true;
                appsOk = ok;
                appsPayload = std::move(payload);
            }
            responseChanged.notify_all();
        });
        {
            std::unique_lock<std::mutex> lock(responseMutex);
            require(responseChanged.wait_for(lock, std::chrono::seconds(3),
                                             [&] { return appsDone; }),
                    "apps response timed out");
            require(appsOk && appsPayload == "[]", "apps response was malformed");
        }

        bool uninstallDone = false;
        bool uninstallOk = true;
        std::string uninstallPayload;
        client.request("uninstall com.example.systemapp", 3000,
                       [&](bool ok, std::string payload) {
                           {
                               std::lock_guard<std::mutex> lock(responseMutex);
                               uninstallDone = true;
                               uninstallOk = ok;
                               uninstallPayload = std::move(payload);
                           }
                           responseChanged.notify_all();
                       });
        {
            std::unique_lock<std::mutex> lock(responseMutex);
            require(responseChanged.wait_for(lock, std::chrono::seconds(3),
                                             [&] { return uninstallDone; }),
                    "uninstall response timed out");
            require(!uninstallOk &&
                            uninstallPayload == "System applications cannot be uninstalled",
                    "uninstall error response was malformed");
        }

        client.stop();
        guest.join();
        if (guestFailure) {
            std::rethrow_exception(guestFailure);
        }
        std::cout << "guest_control_client_test: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        client.stop();
        if (guest.joinable()) {
            guest.join();
        }
        std::cerr << "guest_control_client_test: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
