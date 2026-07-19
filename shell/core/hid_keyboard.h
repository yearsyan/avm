// SPDX-License-Identifier: MIT

#ifndef MACMU_SHELL_HID_KEYBOARD_H
#define MACMU_SHELL_HID_KEYBOARD_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace macmu::shell {

inline constexpr size_t kHidKeyboardReportSize = 8;
inline constexpr uint8_t kHidKeyboardFirstModifier = 0xe0;
inline constexpr uint8_t kHidKeyboardLastModifier = 0xe7;

// Translate an Apple virtual key code (the hardware-position code exposed by
// NSEvent.keyCode) to a USB HID keyboard-page usage. Returns 0 for keys that
// are not represented by scrcpy's standard boot-keyboard descriptor.
uint8_t mac_virtual_key_to_hid_usage(uint16_t key_code);

// Maintains the complete boot-keyboard report sent through Android UHID.
// Reports contain one modifier byte, one reserved byte and up to six ordinary
// keys. Android owns key repeat; setting an already-pressed key is a no-op.
class HidKeyboardState {
public:
    bool set_key(uint8_t usage, bool pressed);
    bool is_pressed(uint8_t usage) const;
    bool release_all();

    const std::array<uint8_t, kHidKeyboardReportSize>& report() const {
        return report_;
    }

private:
    void rebuild_keys();

    // The descriptor exposes usages 0..0x65 (Application). Usages 0 and 1
    // remain reserved; keeping the full range makes rebuilding deterministic.
    std::array<bool, 0x66> keys_{};
    std::array<uint8_t, kHidKeyboardReportSize> report_{};
};

}  // namespace macmu::shell

#endif  // MACMU_SHELL_HID_KEYBOARD_H
