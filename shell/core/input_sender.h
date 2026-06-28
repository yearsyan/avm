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

    bool open(const std::string& socket_path);
    bool valid() const { return m_fd >= 0; }

    void send_touch(macmu::InputEventKind kind,
                    uint32_t display_id,
                    int pointer_id,
                    int x,
                    int y);
    void send_mouse_move(uint32_t display_id, int x, int y, uint32_t buttons);
    void send_mouse_button(uint32_t display_id, int x, int y, uint32_t buttons);

private:
    void send_packet(const macmu::InputEventPacket& packet);

    int m_fd = -1;
    std::string m_socketPath;
};

#endif  // MACMU_SHELL_INPUT_SENDER_H
