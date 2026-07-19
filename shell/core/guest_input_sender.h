// SPDX-License-Identifier: MIT

#ifndef MACMU_SHELL_GUEST_INPUT_SENDER_H
#define MACMU_SHELL_GUEST_INPUT_SENDER_H

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "hid_keyboard.h"
#include "macmu_input_protocol.h"

class GuestInputSender {
public:
    GuestInputSender() = default;
    ~GuestInputSender();

    GuestInputSender(const GuestInputSender&) = delete;
    GuestInputSender& operator=(const GuestInputSender&) = delete;

    bool start(const std::string& socket_path, const std::string& app_data_dir);
    void stop();
    std::string socket_path() const { return m_socketPath; }
    bool ready() const { return m_writeFd.load(std::memory_order_acquire) >= 0; }

    bool send_hover(uint32_t display_id, int x, int y);
    bool send_hover_exit();
    bool send_scroll(uint32_t display_id, int x, int y, float hscroll, float vscroll);
    bool send_touch(macmu::InputEventKind kind,
                    uint32_t display_id,
                    int pointer_id,
                    int x,
                    int y);
    bool send_mouse_move(uint32_t display_id, int x, int y, uint32_t buttons);
    bool send_mouse_button(uint32_t display_id, int x, int y, uint32_t buttons);
    bool send_keyboard_report(
        uint32_t display_id,
        const std::array<uint8_t, macmu::shell::kHidKeyboardReportSize>& report);

private:
    void accept_thread();
    void close_client_locked();
    bool send_line(const char* line, int len, bool reliable);

    std::mutex m_socketMutex;
    std::string m_socketPath;
    std::string m_logPath;
    std::atomic<int> m_listenFd{-1};
    std::atomic<int> m_writeFd{-1};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_acceptThread;
};

#endif  // MACMU_SHELL_GUEST_INPUT_SENDER_H
