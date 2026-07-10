// SPDX-License-Identifier: MIT

#include "control_channel.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "posix_util.h"

ControlChannel::~ControlChannel() {
    stop();
}

bool ControlChannel::create() {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return false;
    }
    localFd_ = fds[0];
    remoteFd_ = fds[1];
    if (!macmu::shell::set_close_on_exec(localFd_) ||
        !macmu::shell::set_close_on_exec(remoteFd_)) {
        stop();
        return false;
    }
    return true;
}

void ControlChannel::start(EventCallback on_event, ClosedCallback on_closed,
                           ReadyCallback on_ready) {
    if (localFd_ < 0 || readerThread_.joinable()) {
        return;
    }
    onEvent_ = std::move(on_event);
    onClosed_ = std::move(on_closed);
    onReady_ = std::move(on_ready);
    alive_.store(true, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
    stopRequested_.store(false, std::memory_order_release);
    readerThread_ = std::thread([this] { reader_thread(); });

    // Open the conversation. The HELLO_ACK is delivered like any response.
    macmu::ControlHello hello = {macmu::kControlProtocolVersion};
    request(macmu::ControlMessageType::kHello, &hello, sizeof(hello), 30000,
            [this](Response response) {
                if (response.ok && response.payload.size() >= sizeof(macmu::ControlHelloAck)) {
                    macmu::ControlHelloAck ack;
                    std::memcpy(&ack, response.payload.data(), sizeof(ack));
                    std::fprintf(stderr,
                                 "MacMu control channel ready (proto=%u maxDisplays=%u "
                                 "frameShm=v%u).\n",
                                 ack.protoVersion, ack.maxDisplays, ack.frameShmVersion);
                    ready_.store(true, std::memory_order_release);
                    if (onReady_) {
                        onReady_();
                    }
                }
            });
}

void ControlChannel::close_remote_fd() {
    if (remoteFd_ >= 0) {
        close(remoteFd_);
        remoteFd_ = -1;
    }
}

void ControlChannel::stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (localFd_ >= 0) {
        shutdown(localFd_, SHUT_RDWR);
    }
    if (readerThread_.joinable()) {
        readerThread_.join();
    }
    {
        // Close under writeMutex_ so a concurrent write_frame can never send
        // on a closed (and possibly kernel-reused) fd number.
        std::lock_guard<std::mutex> lock(writeMutex_);
        if (localFd_ >= 0) {
            close(localFd_);
            localFd_ = -1;
        }
    }
    close_remote_fd();
    fail_all_pending(macmu::kControlErrInternal, "channel stopped");
    ready_.store(false, std::memory_order_release);
    alive_.store(false, std::memory_order_release);
}

void ControlChannel::request(macmu::ControlMessageType type, const void* payload,
                             uint32_t length, uint64_t timeout_ms, ResponseCallback callback) {
    if (!alive()) {
        if (callback) {
            Response response;
            response.ok = false;
            response.errorCode = macmu::kControlErrInternal;
            response.errorMessage = "control channel not connected";
            callback(std::move(response));
        }
        return;
    }
    const uint32_t requestId = nextRequestId_.fetch_add(1, std::memory_order_relaxed);
    if (callback) {
        pending_.add(requestId, std::move(callback), timeout_ms);
    }
    if (!write_frame(static_cast<uint16_t>(type), requestId, payload, length)) {
        ResponseCallback failed = pending_.take(requestId);
        if (failed) {
            Response response;
            response.ok = false;
            response.errorCode = macmu::kControlErrInternal;
            response.errorMessage = "control channel write failed";
            failed(std::move(response));
        }
    }
}

bool ControlChannel::write_frame(uint16_t type, uint32_t request_id, const void* payload,
                                 uint32_t length) {
    macmu::ControlFrameHeader header = {};
    header.magic = macmu::kControlProtocolMagic;
    header.version = macmu::kControlProtocolVersion;
    header.type = type;
    header.requestId = request_id;
    header.length = length;

    std::lock_guard<std::mutex> lock(writeMutex_);
    if (localFd_ < 0 || !alive()) {
        return false;
    }
    struct Chunk {
        const uint8_t* data;
        size_t size;
    };
    const Chunk chunks[2] = {
        {reinterpret_cast<const uint8_t*>(&header), sizeof(header)},
        {static_cast<const uint8_t*>(payload), length},
    };
    for (const Chunk& chunk : chunks) {
        size_t done = 0;
        while (done < chunk.size) {
            const ssize_t n = ::send(localFd_, chunk.data + done, chunk.size - done,
#ifdef MSG_NOSIGNAL
                                     MSG_NOSIGNAL
#else
                                     0
#endif
            );
            if (n > 0) {
                done += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
    }
    return true;
}

void ControlChannel::reader_thread() {
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    setsockopt(localFd_, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    while (!stopRequested_.load(std::memory_order_acquire)) {
        pollfd pfd = {};
        pfd.fd = localFd_;
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
            break;
        }
        if (!(pfd.revents & (POLLIN | POLLHUP))) {
            continue;
        }

        macmu::ControlFrameHeader header = {};
        if (!read_exact(&header, sizeof(header))) {
            break;
        }
        if (header.magic != macmu::kControlProtocolMagic ||
            header.version != macmu::kControlProtocolVersion ||
            header.length > macmu::kControlMaxPayload) {
            std::fprintf(stderr, "MacMu control channel: bad frame; closing.\n");
            break;
        }
        std::vector<uint8_t> payload(header.length);
        if (header.length > 0 && !read_exact(payload.data(), payload.size())) {
            break;
        }

        if (header.requestId == 0) {
            if (onEvent_) {
                onEvent_(header.type, std::move(payload));
            }
            continue;
        }

        ResponseCallback callback = pending_.take(header.requestId);
        if (!callback) {
            continue;  // late response after timeout: drop
        }

        Response response;
        response.type = header.type;
        if (header.type == static_cast<uint16_t>(macmu::ControlMessageType::kError)) {
            response.ok = false;
            if (payload.size() >= sizeof(macmu::ControlError)) {
                macmu::ControlError error;
                std::memcpy(&error, payload.data(), sizeof(error));
                response.errorCode = error.code;
                const size_t msgLen =
                    std::min(static_cast<size_t>(error.msgLen),
                             payload.size() - sizeof(macmu::ControlError));
                response.errorMessage.assign(
                    reinterpret_cast<const char*>(payload.data()) + sizeof(macmu::ControlError),
                    msgLen);
            } else {
                response.errorCode = macmu::kControlErrInternal;
                response.errorMessage = "malformed error frame";
            }
        } else {
            response.ok = true;
            response.payload = std::move(payload);
        }
        callback(std::move(response));
        sweep_timeouts();
    }

    alive_.store(false, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
    fail_all_pending(macmu::kControlErrInternal, "control channel closed");
    if (onClosed_ && !stopRequested_.load(std::memory_order_acquire)) {
        onClosed_();
    }
}

bool ControlChannel::read_exact(void* buffer, size_t size) {
    uint8_t* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::read(localFd_, out + done, size - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;  // EOF (qemu exited) or hard error
    }
    return true;
}

void ControlChannel::fail_all_pending(int32_t code, const char* message) {
    pending_.fail_all([&](ResponseCallback callback) {
        Response response;
        response.ok = false;
        response.errorCode = code;
        response.errorMessage = message;
        callback(std::move(response));
    });
}

void ControlChannel::sweep_timeouts() {
    pending_.sweep_timeouts([](ResponseCallback callback) {
        Response response;
        response.ok = false;
        response.errorCode = macmu::kControlErrInternal;
        response.errorMessage = "request timed out";
        callback(std::move(response));
    });
}
