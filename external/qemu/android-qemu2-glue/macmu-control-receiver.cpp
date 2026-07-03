// SPDX-License-Identifier: Apache-2.0

#include "android-qemu2-glue/macmu-control-receiver.h"

#include "aemu/base/async/ThreadLooper.h"
#include "android/console.h"
#include "android/macmu-opengles-hooks.h"
#include "android/utils/debug.h"
#include "android/utils/looper.h"
#include "macmu_control_protocol.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <mutex>
#ifdef __APPLE__
#include <pthread.h>
#endif
#include <condition_variable>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using macmu::ControlFrameHeader;
using macmu::ControlMessageType;

// User-creatable display ids are 1..kMaxUserDisplayId; ids above that are the
// emulator-internal range used for guest-initiated displays.
constexpr uint32_t kMaxUserDisplayId = 5;

uint64_t steady_now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

class MacMuControlReceiver {
public:
    void start() {
        std::lock_guard<std::mutex> lock(mStateMutex);
        if (mRunning.load(std::memory_order_relaxed)) {
            return;
        }
        const char* fdEnv = std::getenv(macmu::kControlFdEnv);
        if (!fdEnv || !fdEnv[0]) {
            return;
        }
        char* end = nullptr;
        const long parsed = std::strtol(fdEnv, &end, 10);
        if (end == fdEnv || parsed < 0 || parsed >= std::numeric_limits<int>::max()) {
            dwarning("MacMu control fd env %s invalid: %s", macmu::kControlFdEnv, fdEnv);
            return;
        }
        const int fd = static_cast<int>(parsed);
        const int flags = fcntl(fd, F_GETFD);
        if (flags < 0) {
            dwarning("MacMu control fd %d not usable: %s", fd, std::strerror(errno));
            return;
        }
        mFd = fd;
        mRunning.store(true, std::memory_order_relaxed);
        mThread = std::thread([this] { thread_loop(); });
        dinfo("MacMu control receiver listening on inherited fd %d", mFd);
    }

    void stop() {
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(mStateMutex);
            if (!mRunning.load(std::memory_order_relaxed)) {
                return;
            }
            mRunning.store(false, std::memory_order_relaxed);
            if (mFd >= 0) {
                // Shut down reads so the reader thread unblocks; the write
                // side is shared with in-flight responses guarded by
                // mWriteMutex.
                shutdown(mFd, SHUT_RDWR);
            }
            thread = std::move(mThread);
        }
        if (thread.joinable()) {
            thread.join();
        }
        stop_stream_thread();
        std::lock_guard<std::mutex> writeLock(mWriteMutex);
        if (mFd >= 0) {
            close(mFd);
            mFd = -1;
        }
    }

private:
    void thread_loop() {
#ifdef __APPLE__
        pthread_setname_np("macmu-control");
#endif
        while (mRunning.load(std::memory_order_relaxed)) {
            ControlFrameHeader header = {};
            if (!read_exact(&header, sizeof(header))) {
                break;
            }
            if (header.magic != macmu::kControlProtocolMagic ||
                header.version != macmu::kControlProtocolVersion ||
                header.length > macmu::kControlMaxPayload) {
                dwarning("MacMu control: bad frame (magic=0x%x version=%u length=%u); closing.",
                         header.magic, header.version, header.length);
                break;
            }
            std::vector<uint8_t> payload(header.length);
            if (header.length > 0 && !read_exact(payload.data(), payload.size())) {
                break;
            }
            handle_frame(header, std::move(payload));
        }
        mRunning.store(false, std::memory_order_relaxed);
    }

    bool read_exact(void* buffer, size_t size) {
        uint8_t* out = static_cast<uint8_t*>(buffer);
        size_t done = 0;
        while (done < size) {
            const ssize_t n = ::read(mFd, out + done, size - done);
            if (n > 0) {
                done += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EINTR)) {
                continue;
            }
            return false;  // EOF or hard error: shell went away
        }
        return true;
    }

    void send_frame(uint16_t type, uint32_t requestId, const void* payload, uint32_t length) {
        ControlFrameHeader header = {};
        header.magic = macmu::kControlProtocolMagic;
        header.version = macmu::kControlProtocolVersion;
        header.type = type;
        header.requestId = requestId;
        header.length = length;

        std::lock_guard<std::mutex> lock(mWriteMutex);
        if (mFd < 0) {
            return;
        }
        if (!write_all(&header, sizeof(header)) ||
            (length > 0 && !write_all(payload, length))) {
            dwarning("MacMu control: write failed (%s); channel dead.", std::strerror(errno));
        }
    }

    bool write_all(const void* buffer, size_t size) {
        const uint8_t* in = static_cast<const uint8_t*>(buffer);
        size_t done = 0;
        while (done < size) {
            const ssize_t n = ::write(mFd, in + done, size - done);
            if (n > 0) {
                done += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        return true;
    }

    template <typename T>
    void send_response(uint32_t requestId, ControlMessageType type, const T& payload) {
        send_frame(static_cast<uint16_t>(type), requestId, &payload, sizeof(payload));
    }

    void send_error(uint32_t requestId, int32_t code, const std::string& message) {
        std::vector<uint8_t> payload(sizeof(macmu::ControlError) + message.size());
        auto* error = reinterpret_cast<macmu::ControlError*>(payload.data());
        error->code = code;
        error->msgLen = static_cast<uint32_t>(message.size());
        std::memcpy(payload.data() + sizeof(macmu::ControlError), message.data(),
                    message.size());
        send_frame(static_cast<uint16_t>(ControlMessageType::kError), requestId, payload.data(),
                   static_cast<uint32_t>(payload.size()));
    }

    void handle_frame(const ControlFrameHeader& header, std::vector<uint8_t> payload) {
        const auto type = static_cast<ControlMessageType>(header.type);
        switch (type) {
            case ControlMessageType::kHello:
                handle_hello(header.requestId);
                return;
            case ControlMessageType::kPing: {
                macmu::ControlPong pong = {steady_now_ns()};
                send_response(header.requestId, ControlMessageType::kPong, pong);
                return;
            }
            case ControlMessageType::kDisplayAdd:
                if (payload.size() < sizeof(macmu::ControlDisplayAdd)) {
                    send_error(header.requestId, macmu::kControlErrInvalidArgument,
                               "short DISPLAY_ADD payload");
                    return;
                }
                run_on_main_looper(header.requestId, [this, header, payload] {
                    macmu::ControlDisplayAdd request;
                    std::memcpy(&request, payload.data(), sizeof(request));
                    handle_display_add(header.requestId, request);
                });
                return;
            case ControlMessageType::kDisplayRemove:
                if (payload.size() < sizeof(macmu::ControlDisplayRemove)) {
                    send_error(header.requestId, macmu::kControlErrInvalidArgument,
                               "short DISPLAY_REMOVE payload");
                    return;
                }
                run_on_main_looper(header.requestId, [this, header, payload] {
                    macmu::ControlDisplayRemove request;
                    std::memcpy(&request, payload.data(), sizeof(request));
                    handle_display_remove(header.requestId, request);
                });
                return;
            case ControlMessageType::kDisplayStream: {
                if (payload.size() < sizeof(macmu::ControlDisplayStream)) {
                    send_error(header.requestId, macmu::kControlErrInvalidArgument,
                               "short DISPLAY_STREAM payload");
                    return;
                }
                macmu::ControlDisplayStream request;
                std::memcpy(&request, payload.data(), sizeof(request));
                if (request.displayId == 0) {
                    send_error(header.requestId, macmu::kControlErrInvalidArgument,
                               "display 0 streams via its own present path");
                    return;
                }
                set_display_streaming(request.displayId, request.enabled != 0);
                send_frame(static_cast<uint16_t>(ControlMessageType::kDisplayStreamOk),
                           header.requestId, nullptr, 0);
                return;
            }
            case ControlMessageType::kDisplayList:
                run_on_main_looper(header.requestId, [this, header] {
                    handle_display_list(header.requestId);
                });
                return;
            default:
                send_error(header.requestId, macmu::kControlErrUnsupported,
                           "unsupported request type");
                return;
        }
    }

    void run_on_main_looper(uint32_t requestId, std::function<void()> task) {
        if (!android_getMainLooper()) {
            send_error(requestId, macmu::kControlErrNotReady, "qemu main looper not ready");
            return;
        }
        android::base::ThreadLooper::runOnMainLooper(std::move(task));
    }

    void handle_hello(uint32_t requestId) {
        macmu::ControlHelloAck ack = {};
        ack.protoVersion = macmu::kControlProtocolVersion;
        ack.capabilities = 0;
        ack.maxDisplays = kMaxUserDisplayId;
        ack.frameShmVersion = 2;
        send_response(requestId, ControlMessageType::kHelloAck, ack);
    }

    const QAndroidMultiDisplayAgent* multi_display_agent() {
        return getConsoleAgents()->multi_display;
    }

    void handle_display_add(uint32_t requestId, const macmu::ControlDisplayAdd& request) {
        const auto* agent = multi_display_agent();
        if (!agent) {
            send_error(requestId, macmu::kControlErrInternal, "multi-display agent unavailable");
            return;
        }

        uint32_t displayId = request.displayId;
        if (displayId == macmu::kControlDisplayIdAuto) {
            displayId = 0;
            for (uint32_t candidate = 1; candidate <= kMaxUserDisplayId; ++candidate) {
                uint32_t w = 0, h = 0, dpi = 0, flags = 0;
                bool enabled = false;
                const bool exists = agent->getMultiDisplay(candidate, nullptr, nullptr, &w, &h,
                                                           &dpi, &flags, &enabled);
                if (!exists || !enabled) {
                    displayId = candidate;
                    break;
                }
            }
            if (displayId == 0) {
                send_error(requestId, macmu::kControlErrRejected, "no free display id");
                return;
            }
        }
        if (displayId == 0 || displayId > kMaxUserDisplayId) {
            send_error(requestId, macmu::kControlErrInvalidArgument,
                       "display id out of user range (1..5)");
            return;
        }

        const int result = agent->setMultiDisplay(displayId, -1, -1, request.width,
                                                  request.height, request.dpi, request.flags,
                                                  /*add=*/true);
        if (result < 0) {
            send_error(requestId,
                       result == -EPIPE ? macmu::kControlErrNotReady
                                        : macmu::kControlErrRejected,
                       std::string("setMultiDisplay failed: ") + std::to_string(result));
            return;
        }

        macmu::ControlDisplayAddOk ok = {displayId};
        send_response(requestId, ControlMessageType::kDisplayAddOk, ok);
        send_display_event(displayId, macmu::kControlDisplayAdded, request.width, request.height,
                           request.dpi, request.flags);
    }

    void handle_display_remove(uint32_t requestId, const macmu::ControlDisplayRemove& request) {
        const auto* agent = multi_display_agent();
        if (!agent) {
            send_error(requestId, macmu::kControlErrInternal, "multi-display agent unavailable");
            return;
        }
        if (request.displayId == 0 || request.displayId > kMaxUserDisplayId) {
            send_error(requestId, macmu::kControlErrInvalidArgument,
                       "display id out of user range (1..5)");
            return;
        }
        const int result = agent->setMultiDisplay(request.displayId, -1, -1, 0, 0, 0, 0,
                                                  /*add=*/false);
        if (result < 0) {
            send_error(requestId, macmu::kControlErrRejected,
                       std::string("setMultiDisplay(del) failed: ") + std::to_string(result));
            return;
        }
        set_display_streaming(request.displayId, false);
        send_frame(static_cast<uint16_t>(ControlMessageType::kDisplayRemoveOk), requestId,
                   nullptr, 0);
        send_display_event(request.displayId, macmu::kControlDisplayRemoved, 0, 0, 0, 0);
    }

    void handle_display_list(uint32_t requestId) {
        const auto* agent = multi_display_agent();
        if (!agent) {
            send_error(requestId, macmu::kControlErrInternal, "multi-display agent unavailable");
            return;
        }
        std::vector<macmu::ControlDisplayInfo> entries;
        int32_t previousId = -1;
        uint32_t id = 0, w = 0, h = 0, dpi = 0, flags = 0, cb = 0;
        int32_t x = 0, y = 0;
        while (agent->getNextMultiDisplay(previousId, &id, &x, &y, &w, &h, &dpi, &flags, &cb)) {
            previousId = static_cast<int32_t>(id);
            macmu::ControlDisplayInfo info = {};
            info.displayId = id;
            info.width = w;
            info.height = h;
            info.dpi = dpi;
            info.flags = flags;
            info.enabled = 1;
            entries.push_back(info);
        }

        std::vector<uint8_t> payload(sizeof(macmu::ControlDisplayListOk) +
                                     entries.size() * sizeof(macmu::ControlDisplayInfo));
        auto* list = reinterpret_cast<macmu::ControlDisplayListOk*>(payload.data());
        list->count = static_cast<uint32_t>(entries.size());
        if (!entries.empty()) {
            std::memcpy(payload.data() + sizeof(macmu::ControlDisplayListOk), entries.data(),
                        entries.size() * sizeof(macmu::ControlDisplayInfo));
        }
        send_frame(static_cast<uint16_t>(ControlMessageType::kDisplayListOk), requestId,
                   payload.data(), static_cast<uint32_t>(payload.size()));
    }

    void send_display_event(uint32_t displayId, uint32_t state, uint32_t w, uint32_t h,
                            uint32_t dpi, uint32_t flags) {
        macmu::ControlEventDisplay event = {};
        event.displayId = displayId;
        event.state = state;
        event.width = w;
        event.height = h;
        event.dpi = dpi;
        event.flags = flags;
        send_frame(static_cast<uint16_t>(ControlMessageType::kEventDisplay), /*requestId=*/0,
                   &event, sizeof(event));
    }

    // ------------------------------------------------------------------
    // Shell-paced display streaming: VirtualDisplay-backed secondary displays
    // update their bound ColorBuffer in place with no per-frame host signal,
    // so while the shell has a window on a display it asks us to pump exports
    // at ~60 Hz. The set is tiny (user displays 1..5).
    // ------------------------------------------------------------------

    void set_display_streaming(uint32_t displayId, bool enabled) {
        std::lock_guard<std::mutex> lock(mStreamMutex);
        if (enabled) {
            mStreamingDisplays.insert(displayId);
            if (!mStreamThread.joinable()) {
                mStreamThread = std::thread([this] { stream_loop(); });
            }
        } else {
            mStreamingDisplays.erase(displayId);
        }
        mStreamCondition.notify_all();
    }

    void stream_loop() {
#ifdef __APPLE__
        pthread_setname_np("macmu-dispstream");
#endif
        std::unique_lock<std::mutex> lock(mStreamMutex);
        while (mRunning.load(std::memory_order_relaxed)) {
            if (mStreamingDisplays.empty()) {
                mStreamCondition.wait_for(lock, std::chrono::milliseconds(250));
                continue;
            }
            const std::set<uint32_t> displays = mStreamingDisplays;
            lock.unlock();
            for (const uint32_t displayId : displays) {
                android_exportDisplayFrame(displayId);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            lock.lock();
        }
    }

    void stop_stream_thread() {
        {
            std::lock_guard<std::mutex> lock(mStreamMutex);
            mStreamingDisplays.clear();
        }
        mStreamCondition.notify_all();
        if (mStreamThread.joinable()) {
            mStreamThread.join();
        }
    }

    std::mutex mStateMutex;
    std::mutex mWriteMutex;
    std::atomic<bool> mRunning{false};
    int mFd = -1;
    std::thread mThread;
    std::mutex mStreamMutex;
    std::condition_variable mStreamCondition;
    std::set<uint32_t> mStreamingDisplays;
    std::thread mStreamThread;
};

MacMuControlReceiver* receiver() {
    static MacMuControlReceiver sReceiver;
    return &sReceiver;
}

}  // namespace

void macmu_control_receiver_start() {
    receiver()->start();
}

void macmu_control_receiver_stop() {
    receiver()->stop();
}
