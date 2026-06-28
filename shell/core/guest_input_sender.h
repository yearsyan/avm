// SPDX-License-Identifier: MIT

#ifndef MACMU_SHELL_GUEST_INPUT_SENDER_H
#define MACMU_SHELL_GUEST_INPUT_SENDER_H

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

struct ShellOptions;

class GuestInputSender {
public:
    GuestInputSender() = default;
    ~GuestInputSender();

    GuestInputSender(const GuestInputSender&) = delete;
    GuestInputSender& operator=(const GuestInputSender&) = delete;

    void start(const ShellOptions& options);
    void stop();
    bool ready() const { return m_writeFd.load(std::memory_order_acquire) >= 0; }

    void send_hover(uint32_t display_id, int x, int y);
    void send_scroll(uint32_t display_id, int x, int y, float hscroll, float vscroll);

private:
    void setup_thread();

    std::string m_adbPath;
    std::string m_serverJarPath;
    std::atomic<int> m_writeFd{-1};
    std::atomic<int> m_forwardPort{0};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_setupThread;
};

#endif  // MACMU_SHELL_GUEST_INPUT_SENDER_H
