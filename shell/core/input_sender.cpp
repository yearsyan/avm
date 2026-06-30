// SPDX-License-Identifier: MIT

#include "input_sender.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool set_close_on_exec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

macmu::InputEventPacket make_packet(macmu::InputEventKind kind,
                                    uint32_t display_id,
                                    int pointer_id,
                                    int x,
                                    int y,
                                    uint32_t buttons) {
    macmu::InputEventPacket packet = {};
    packet.magic = macmu::kInputProtocolMagic;
    packet.version = macmu::kInputProtocolVersion;
    packet.kind = static_cast<uint16_t>(kind);
    packet.displayId = display_id;
    packet.pointerId = pointer_id;
    packet.x = x;
    packet.y = y;
    packet.xGlobal = x;
    packet.yGlobal = y;
    packet.buttons = buttons;
    return packet;
}

}  // namespace

InputSender::~InputSender() {
    if (m_remoteFd >= 0) {
        close(m_remoteFd);
        m_remoteFd = -1;
    }
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

bool InputSender::create() {
    int pair[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0) {
        std::fprintf(stderr, "Failed to create MacMu input socketpair: %s\n",
                     std::strerror(errno));
        return false;
    }
    if (!set_close_on_exec(pair[0]) || !set_close_on_exec(pair[1])) {
        close(pair[0]);
        close(pair[1]);
        return false;
    }
    // Non-blocking on the send end so a saturated queue (qemu slower than the
    // host input rate) drops packets instead of blocking the UI thread.
    fcntl(pair[0], F_SETFL, fcntl(pair[0], F_GETFL, 0) | O_NONBLOCK);

    m_fd = pair[0];
    m_remoteFd = pair[1];
    return true;
}

void InputSender::send_touch(macmu::InputEventKind kind,
                             uint32_t display_id,
                             int pointer_id,
                             int x,
                             int y) {
    send_packet(make_packet(kind, display_id, pointer_id, x, y, 0));
}

void InputSender::send_mouse_move(uint32_t display_id, int x, int y, uint32_t buttons) {
    send_packet(make_packet(macmu::InputEventKind::kMouseMove, display_id, 0, x, y, buttons));
}

void InputSender::send_mouse_button(uint32_t display_id, int x, int y, uint32_t buttons) {
    send_packet(make_packet(macmu::InputEventKind::kMouseButton, display_id, 0, x, y, buttons));
}

void InputSender::send_packet(const macmu::InputEventPacket& packet) {
    if (m_fd < 0) {
        return;
    }
    const ssize_t sent = send(m_fd, &packet, sizeof(packet), 0);
    if (sent < 0 && errno != ENOENT && errno != ECONNREFUSED && errno != EAGAIN &&
        errno != EWOULDBLOCK) {
        std::fprintf(stderr, "Failed to send MacMu input packet: %s\n", std::strerror(errno));
    }
}
