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
#include <map>
#include <mutex>
#ifdef __APPLE__
#include <pthread.h>
#endif
#include <condition_variable>
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
constexpr uint32_t kFrameSlotCount = 16;
constexpr uint32_t kDefaultDisplayStreamFps = 60;
// Defensive protocol limit only; current macOS displays are far below this.
constexpr uint32_t kMaximumDisplayStreamFps = 1000;

uint32_t normalize_stream_fps(uint32_t requestedFps) {
    if (requestedFps == 0) {
        return kDefaultDisplayStreamFps;
    }
    return requestedFps > kMaximumDisplayStreamFps ? kMaximumDisplayStreamFps : requestedFps;
}

std::chrono::nanoseconds stream_interval(uint32_t framesPerSecond) {
    return std::chrono::nanoseconds(1000000000ull / normalize_stream_fps(framesPerSecond));
}

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
        mStopRequested.store(false, std::memory_order_release);
        mRunning.store(true, std::memory_order_relaxed);
        mThread = std::thread([this] { thread_loop(); });
        dinfo("MacMu control receiver listening on inherited fd %d", mFd);
    }

    void stop() {
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(mStateMutex);
            mStopRequested.store(true, std::memory_order_release);
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
        android_resetDisplayExportSubscriptions();

        if (!mStopRequested.load(std::memory_order_acquire) && android_getMainLooper()) {
            dwarning("MacMu control: shell lease ended; requesting QEMU shutdown.");
            android::base::ThreadLooper::runOnMainLooper([] {
                const auto* agents = getConsoleAgents();
                if (agents && agents->vm && agents->vm->system_shutdown_request) {
                    agents->vm->system_shutdown_request(QEMU_SHUTDOWN_CAUSE_HOST_UI);
                }
            });
        }
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
                // maximumFramesPerSecond was appended to the original 8-byte
                // request. Accept both layouts so mixed shell/backend builds
                // fall back to 60 Hz instead of breaking the control channel.
                constexpr size_t kLegacyDisplayStreamSize = sizeof(uint32_t) * 2;
                if (payload.size() < kLegacyDisplayStreamSize) {
                    send_error(header.requestId, macmu::kControlErrInvalidArgument,
                               "short DISPLAY_STREAM payload");
                    return;
                }
                macmu::ControlDisplayStream request = {};
                const size_t copySize = payload.size() < sizeof(request) ? payload.size()
                                                                         : sizeof(request);
                std::memcpy(&request, payload.data(), copySize);
                if (request.displayId >= kFrameSlotCount) {
                    send_error(header.requestId, macmu::kControlErrInvalidArgument,
                               "display id exceeds frame slot table");
                    return;
                }
                set_display_streaming(request.displayId, request.enabled != 0,
                                      request.maximumFramesPerSecond);
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
        // Invalidate the per-display frame slot before the ordered removal
        // response/event makes this id reusable in the shell. Generation-aware
        // publication also drops any old GPU export that finishes late.
        android_clearDisplayExportFrame(request.displayId);
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
    // update their bound ColorBuffer in place with no reliable per-frame host
    // signal. The shell reports the maximum refresh rate of the NSScreen that
    // owns each application window, and this loop independently paces that
    // display to the same ceiling. The map is tiny (user displays 1..5).
    // ------------------------------------------------------------------

    struct DisplayStreamState {
        uint32_t maximumFramesPerSecond = kDefaultDisplayStreamFps;
        std::chrono::steady_clock::time_point nextExport;
    };

    void set_display_streaming(uint32_t displayId, bool enabled,
                               uint32_t requestedFramesPerSecond = 0) {
        android_setDisplayExportEnabled(displayId, enabled);
        if (displayId == 0) {
            return;
        }
        const uint32_t maximumFramesPerSecond = normalize_stream_fps(requestedFramesPerSecond);
        bool exportImmediately = false;
        bool rateChanged = false;
        {
            std::lock_guard<std::mutex> lock(mStreamMutex);
            if (enabled) {
                const auto nextExport =
                    std::chrono::steady_clock::now() + stream_interval(maximumFramesPerSecond);
                auto stream = mStreamingDisplays.find(displayId);
                if (stream == mStreamingDisplays.end()) {
                    mStreamingDisplays[displayId] = {maximumFramesPerSecond, nextExport};
                    exportImmediately = true;
                    rateChanged = true;
                } else if (stream->second.maximumFramesPerSecond != maximumFramesPerSecond) {
                    stream->second.maximumFramesPerSecond = maximumFramesPerSecond;
                    stream->second.nextExport = nextExport;
                    rateChanged = true;
                }
                if (!mStreamThread.joinable()) {
                    mStreamThread = std::thread([this] { stream_loop(); });
                }
            } else {
                mStreamingDisplays.erase(displayId);
            }
        }
        if (exportImmediately) {
            android_exportDisplayFrame(displayId);
        }
        if (enabled && rateChanged) {
            dinfo("MacMu display %u export pacing set to %u Hz", displayId,
                  maximumFramesPerSecond);
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
                mStreamCondition.wait(lock, [this] {
                    return !mRunning.load(std::memory_order_relaxed) ||
                           !mStreamingDisplays.empty();
                });
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            auto nextWake = std::chrono::steady_clock::time_point::max();
            std::vector<uint32_t> displaysToExport;
            for (auto& [displayId, state] : mStreamingDisplays) {
                if (state.nextExport <= now) {
                    displaysToExport.push_back(displayId);
                    // Skip missed deadlines instead of bursting several stale
                    // exports; this is a maximum cadence, not a frame queue.
                    state.nextExport = now + stream_interval(state.maximumFramesPerSecond);
                }
                if (state.nextExport < nextWake) {
                    nextWake = state.nextExport;
                }
            }
            if (displaysToExport.empty()) {
                // No predicate: a rate update or display removal must wake us
                // immediately so the next deadline can be recalculated.
                mStreamCondition.wait_until(lock, nextWake);
                continue;
            }

            lock.unlock();
            for (const uint32_t displayId : displaysToExport) {
                android_exportDisplayFrame(displayId);
            }
            lock.lock();
        }
    }

    void stop_stream_thread() {
        android_resetDisplayExportSubscriptions();
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
    std::atomic<bool> mStopRequested{false};
    int mFd = -1;
    std::thread mThread;
    std::mutex mStreamMutex;
    std::condition_variable mStreamCondition;
    std::map<uint32_t, DisplayStreamState> mStreamingDisplays;
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
