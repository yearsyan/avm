// SPDX-License-Identifier: MIT

#ifndef MACMU_SHELL_INPUT_SENDER_H
#define MACMU_SHELL_INPUT_SENDER_H

#include <cstdint>
#include <string>

#include "macmu_input_protocol.h"

class InputSender {
public:
    InputSender() = default;
    ~InputSender();

    InputSender(const InputSender&) = delete;
    InputSender& operator=(const InputSender&) = delete;

    // Preferred transport: builds an AF_UNIX SOCK_DGRAM socketpair, keeps the
    // local end for sending, and exposes the remote end through
    // remote_fd() so the qemu launcher can dup2() it into a fixed child fd.
    // No filesystem socket path is used. Returns false on socketpair failure.
    bool create();
    bool valid() const { return m_fd >= 0; }

    // Remote socketpair end to hand to qemu via fd inheritance. Stays open
    // (with FD_CLOEXEC set) while the sender is alive; -1 when not created.
    int remote_fd() const { return m_remoteFd; }

    void send_touch(macmu::InputEventKind kind,
                    uint32_t display_id,
                    int pointer_id,
                    int x,
                    int y);
    void send_mouse_move(uint32_t display_id, int x, int y, uint32_t buttons);
    void send_mouse_button(uint32_t display_id, int x, int y, uint32_t buttons);

private:
    void send_packet(const macmu::InputEventPacket& packet);

    int m_fd = -1;       // local end (we sendto()/send() to m_remoteFd's peer)
    int m_remoteFd = -1;  // remote end handed to qemu via fd inheritance
};

#endif  // MACMU_SHELL_INPUT_SENDER_H
