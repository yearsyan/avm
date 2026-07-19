// SPDX-License-Identifier: MIT

#include "hid_keyboard.h"

#include <algorithm>

namespace macmu::shell {

uint8_t mac_virtual_key_to_hid_usage(uint16_t key_code) {
    // Apple virtual key codes are stable hardware-position identifiers. The
    // numeric values below are the Carbon kVK_* constants, kept local so this
    // pure C++ mapping remains unit-testable without AppKit/Carbon.
    switch (key_code) {
        case 0x00: return 0x04;  // A
        case 0x0b: return 0x05;  // B
        case 0x08: return 0x06;  // C
        case 0x02: return 0x07;  // D
        case 0x0e: return 0x08;  // E
        case 0x03: return 0x09;  // F
        case 0x05: return 0x0a;  // G
        case 0x04: return 0x0b;  // H
        case 0x22: return 0x0c;  // I
        case 0x26: return 0x0d;  // J
        case 0x28: return 0x0e;  // K
        case 0x25: return 0x0f;  // L
        case 0x2e: return 0x10;  // M
        case 0x2d: return 0x11;  // N
        case 0x1f: return 0x12;  // O
        case 0x23: return 0x13;  // P
        case 0x0c: return 0x14;  // Q
        case 0x0f: return 0x15;  // R
        case 0x01: return 0x16;  // S
        case 0x11: return 0x17;  // T
        case 0x20: return 0x18;  // U
        case 0x09: return 0x19;  // V
        case 0x0d: return 0x1a;  // W
        case 0x07: return 0x1b;  // X
        case 0x10: return 0x1c;  // Y
        case 0x06: return 0x1d;  // Z

        case 0x12: return 0x1e;  // 1
        case 0x13: return 0x1f;  // 2
        case 0x14: return 0x20;  // 3
        case 0x15: return 0x21;  // 4
        case 0x17: return 0x22;  // 5
        case 0x16: return 0x23;  // 6
        case 0x1a: return 0x24;  // 7
        case 0x1c: return 0x25;  // 8
        case 0x19: return 0x26;  // 9
        case 0x1d: return 0x27;  // 0

        case 0x24: return 0x28;  // Return
        case 0x35: return 0x29;  // Escape
        case 0x33: return 0x2a;  // Delete (Backspace)
        case 0x30: return 0x2b;  // Tab
        case 0x31: return 0x2c;  // Space
        case 0x1b: return 0x2d;  // Minus
        case 0x18: return 0x2e;  // Equal
        case 0x21: return 0x2f;  // Left bracket
        case 0x1e: return 0x30;  // Right bracket
        case 0x2a: return 0x31;  // Backslash
        case 0x29: return 0x33;  // Semicolon
        case 0x27: return 0x34;  // Apostrophe
        case 0x32: return 0x35;  // Grave
        case 0x2b: return 0x36;  // Comma
        case 0x2f: return 0x37;  // Period
        case 0x2c: return 0x38;  // Slash
        case 0x39: return 0x39;  // Caps Lock

        case 0x7a: return 0x3a;  // F1
        case 0x78: return 0x3b;  // F2
        case 0x63: return 0x3c;  // F3
        case 0x76: return 0x3d;  // F4
        case 0x60: return 0x3e;  // F5
        case 0x61: return 0x3f;  // F6
        case 0x62: return 0x40;  // F7
        case 0x64: return 0x41;  // F8
        case 0x65: return 0x42;  // F9
        case 0x6d: return 0x43;  // F10
        case 0x67: return 0x44;  // F11
        case 0x6f: return 0x45;  // F12

        case 0x72: return 0x49;  // Help / Insert
        case 0x73: return 0x4a;  // Home
        case 0x74: return 0x4b;  // Page Up
        case 0x75: return 0x4c;  // Forward Delete
        case 0x77: return 0x4d;  // End
        case 0x79: return 0x4e;  // Page Down
        case 0x7c: return 0x4f;  // Right arrow
        case 0x7b: return 0x50;  // Left arrow
        case 0x7d: return 0x51;  // Down arrow
        case 0x7e: return 0x52;  // Up arrow

        case 0x47: return 0x53;  // Keypad Clear / Num Lock
        case 0x4b: return 0x54;  // Keypad Divide
        case 0x43: return 0x55;  // Keypad Multiply
        case 0x4e: return 0x56;  // Keypad Minus
        case 0x45: return 0x57;  // Keypad Plus
        case 0x4c: return 0x58;  // Keypad Enter
        case 0x53: return 0x59;  // Keypad 1
        case 0x54: return 0x5a;  // Keypad 2
        case 0x55: return 0x5b;  // Keypad 3
        case 0x56: return 0x5c;  // Keypad 4
        case 0x57: return 0x5d;  // Keypad 5
        case 0x58: return 0x5e;  // Keypad 6
        case 0x59: return 0x5f;  // Keypad 7
        case 0x5b: return 0x60;  // Keypad 8
        case 0x5c: return 0x61;  // Keypad 9
        case 0x52: return 0x62;  // Keypad 0
        case 0x41: return 0x63;  // Keypad Decimal
        case 0x0a: return 0x64;  // ISO Section / Non-US Backslash
        case 0x51: return 0;     // Keypad Equal is outside the boot descriptor

        case 0x3b: return 0xe0;  // Left Control
        case 0x38: return 0xe1;  // Left Shift
        case 0x3a: return 0xe2;  // Left Option / Alt
        case 0x37: return 0xe3;  // Left Command / GUI
        case 0x3e: return 0xe4;  // Right Control
        case 0x3c: return 0xe5;  // Right Shift
        case 0x3d: return 0xe6;  // Right Option / Alt
        case 0x36: return 0xe7;  // Right Command / GUI
        default: return 0;
    }
}

bool HidKeyboardState::set_key(uint8_t usage, bool pressed) {
    if (usage >= kHidKeyboardFirstModifier && usage <= kHidKeyboardLastModifier) {
        const uint8_t mask = static_cast<uint8_t>(1u << (usage - kHidKeyboardFirstModifier));
        const bool wasPressed = (report_[0] & mask) != 0;
        if (wasPressed == pressed) {
            return false;
        }
        if (pressed) {
            report_[0] |= mask;
        } else {
            report_[0] &= static_cast<uint8_t>(~mask);
        }
        return true;
    }
    if (usage < 0x04 || usage >= keys_.size() || keys_[usage] == pressed) {
        return false;
    }
    keys_[usage] = pressed;
    rebuild_keys();
    return true;
}

bool HidKeyboardState::is_pressed(uint8_t usage) const {
    if (usage >= kHidKeyboardFirstModifier && usage <= kHidKeyboardLastModifier) {
        const uint8_t mask = static_cast<uint8_t>(1u << (usage - kHidKeyboardFirstModifier));
        return (report_[0] & mask) != 0;
    }
    return usage < keys_.size() && keys_[usage];
}

bool HidKeyboardState::release_all() {
    const bool changed = std::any_of(report_.begin(), report_.end(), [](uint8_t byte) {
        return byte != 0;
    });
    keys_.fill(false);
    report_.fill(0);
    return changed;
}

void HidKeyboardState::rebuild_keys() {
    std::fill(report_.begin() + 2, report_.end(), 0);
    size_t count = 0;
    for (size_t usage = 0x04; usage < keys_.size(); ++usage) {
        if (!keys_[usage]) {
            continue;
        }
        if (count >= 6) {
            std::fill(report_.begin() + 2, report_.end(), 0x01);  // ErrorRollOver
            return;
        }
        report_[2 + count] = static_cast<uint8_t>(usage);
        ++count;
    }
}

}  // namespace macmu::shell
