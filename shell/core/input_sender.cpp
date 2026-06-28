// SPDX-License-Identifier: MIT

#include "input_sender.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

bool fill_sockaddr(const std::string& path, sockaddr_un* addr, socklen_t* len) {
    if (path.empty() || path.size() >= sizeof(addr->sun_path)) {
        return false;
    }
    std::memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    std::strncpy(addr->sun_path, path.c_str(), sizeof(addr->sun_path) - 1);
    *len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    return true;
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
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

bool InputSender::open(const std::string& socket_path) {
    sockaddr_un addr = {};
    socklen_t len = 0;
    if (!fill_sockaddr(socket_path, &addr, &len)) {
        std::fprintf(stderr, "MacMu input socket path is too long: %s\n", socket_path.c_str());
        return false;
    }

    const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "Failed to create MacMu input socket: %s\n", std::strerror(errno));
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    m_fd = fd;
    m_socketPath = socket_path;
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
    sockaddr_un addr = {};
    socklen_t len = 0;
    if (!fill_sockaddr(m_socketPath, &addr, &len)) {
        return;
    }
    const ssize_t sent = sendto(m_fd, &packet, sizeof(packet), 0,
                                reinterpret_cast<const sockaddr*>(&addr), len);
    if (sent < 0 && errno != ENOENT && errno != ECONNREFUSED && errno != EAGAIN &&
        errno != EWOULDBLOCK) {
        std::fprintf(stderr, "Failed to send MacMu input packet: %s\n", std::strerror(errno));
    }
}
