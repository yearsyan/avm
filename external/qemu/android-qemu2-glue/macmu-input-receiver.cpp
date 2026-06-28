// SPDX-License-Identifier: Apache-2.0

#include "android-qemu2-glue/macmu-input-receiver.h"

#include "aemu/base/async/ThreadLooper.h"
#include "android/skin/event.h"
#include "android/utils/debug.h"
#include "android/utils/looper.h"
#include "macmu_input_protocol.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#ifdef __APPLE__
#include <pthread.h>
#endif
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace {

class MacMuInputReceiver {
public:
    void start(const QAndroidUserEventAgent* agent) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mRunning.load(std::memory_order_relaxed)) {
            return;
        }
        const char* path = std::getenv(macmu::kInputSocketEnv);
        if (!path || !path[0] || !agent) {
            return;
        }
        sockaddr_un addr = {};
        if (!fill_sockaddr(path, &addr)) {
            dwarning("MacMu input socket path is too long: %s", path);
            return;
        }

        const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) {
            dwarning("Failed to create MacMu input socket: %s", std::strerror(errno));
            return;
        }
        unlink(path);
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            dwarning("Failed to bind MacMu input socket %s: %s", path, std::strerror(errno));
            close(fd);
            return;
        }

        mAgent = agent;
        mPath = path;
        mFd = fd;
        mRunning.store(true, std::memory_order_relaxed);
        mThread = std::thread([this] { thread_loop(); });
        dinfo("MacMu input receiver listening on %s", mPath.c_str());
    }

    void stop() {
        std::thread thread;
        std::string path;
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!mRunning.load(std::memory_order_relaxed)) {
                return;
            }
            mRunning.store(false, std::memory_order_relaxed);
            fd = mFd;
            mFd = -1;
            path = mPath;
            thread = std::move(mThread);
        }
        if (fd >= 0) {
            close(fd);
        }
        if (thread.joinable()) {
            thread.join();
        }
        if (!path.empty()) {
            unlink(path.c_str());
        }
    }

private:
    static bool fill_sockaddr(const char* path, sockaddr_un* addr) {
        const size_t len = std::strlen(path);
        if (len == 0 || len >= sizeof(addr->sun_path)) {
            return false;
        }
        std::memset(addr, 0, sizeof(*addr));
        addr->sun_family = AF_UNIX;
        std::strncpy(addr->sun_path, path, sizeof(addr->sun_path) - 1);
        return true;
    }

    void thread_loop() {
#ifdef __APPLE__
        pthread_setname_np("macmu-input");
#endif
        while (mRunning.load(std::memory_order_relaxed)) {
            pollfd pfd = {};
            pfd.fd = mFd;
            pfd.events = POLLIN;
            const int ready = poll(&pfd, 1, 100);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (ready == 0 || !(pfd.revents & POLLIN)) {
                continue;
            }

            macmu::InputEventPacket packet = {};
            const ssize_t size = recv(pfd.fd, &packet, sizeof(packet), 0);
            if (size != sizeof(packet)) {
                continue;
            }
            if (!valid_packet(packet)) {
                continue;
            }
            post_packet(packet);
        }
    }

    bool valid_packet(const macmu::InputEventPacket& packet) const {
        if (packet.magic != macmu::kInputProtocolMagic ||
            packet.version != macmu::kInputProtocolVersion) {
            return false;
        }
        return packet.kind >= static_cast<uint16_t>(macmu::InputEventKind::kTouchBegin) &&
               packet.kind <= static_cast<uint16_t>(macmu::InputEventKind::kMouseButton);
    }

    void post_packet(macmu::InputEventPacket packet) {
        if (!android_getMainLooper()) {
            return;
        }
        const QAndroidUserEventAgent* agent = mAgent;
        android::base::ThreadLooper::runOnMainLooper([this, agent, packet] {
            deliver_packet(agent, packet);
        });
    }

    void deliver_packet(const QAndroidUserEventAgent* agent,
                        const macmu::InputEventPacket& packet) {
        if (!agent) {
            return;
        }
        const int displayId = static_cast<int>(packet.displayId);
        const auto kind = static_cast<macmu::InputEventKind>(packet.kind);
        switch (kind) {
            case macmu::InputEventKind::kTouchBegin:
            case macmu::InputEventKind::kTouchUpdate:
            case macmu::InputEventKind::kTouchEnd: {
                SkinEvent event = {};
                event.type = kind == macmu::InputEventKind::kTouchBegin
                                     ? kEventTouchBegin
                                     : kind == macmu::InputEventKind::kTouchUpdate
                                           ? kEventTouchUpdate
                                           : kEventTouchEnd;
                event.u.multi_touch_point.display_id = packet.displayId;
                event.u.multi_touch_point.id = packet.pointerId;
                event.u.multi_touch_point.pressure =
                        kind == macmu::InputEventKind::kTouchEnd ? 0
                                                                 : macmu::kInputTouchPressure;
                event.u.multi_touch_point.orientation = 0;
                event.u.multi_touch_point.x = packet.x;
                event.u.multi_touch_point.y = packet.y;
                event.u.multi_touch_point.x_global = packet.xGlobal;
                event.u.multi_touch_point.y_global = packet.yGlobal;
                event.u.multi_touch_point.touch_major = macmu::kInputTouchMajor;
                event.u.multi_touch_point.touch_minor = macmu::kInputTouchMinor;
                agent->sendTouchEvents(&event, displayId);
                break;
            }
            case macmu::InputEventKind::kMouseMove:
            case macmu::InputEventKind::kMouseButton: {
                auto& state = mMouseStates[packet.displayId];
                int dx = 0;
                int dy = 0;
                if (state.hasPosition) {
                    dx = packet.x - state.x;
                    dy = packet.y - state.y;
                }
                state.x = packet.x;
                state.y = packet.y;
                state.hasPosition = true;
                agent->sendMouseEvent(dx, dy, 0, packet.buttons, displayId,
                                      MOUSE_EVENT_MODE_REL);
                break;
            }
        }
    }

    struct MouseState {
        bool hasPosition = false;
        int x = 0;
        int y = 0;
    };

    std::mutex mMutex;
    std::atomic<bool> mRunning{false};
    const QAndroidUserEventAgent* mAgent = nullptr;
    int mFd = -1;
    std::string mPath;
    std::thread mThread;
    std::unordered_map<uint32_t, MouseState> mMouseStates;
};

MacMuInputReceiver* receiver() {
    static MacMuInputReceiver sReceiver;
    return &sReceiver;
}

}  // namespace

void macmu_input_receiver_start(const QAndroidUserEventAgent* user_event_agent) {
    receiver()->start(user_event_agent);
}

void macmu_input_receiver_stop() {
    receiver()->stop();
}
