// SPDX-License-Identifier: MIT

#include "hid_keyboard.h"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    using macmu::shell::HidKeyboardState;
    using macmu::shell::mac_virtual_key_to_hid_usage;

    try {
        require(mac_virtual_key_to_hid_usage(0x00) == 0x04, "A mapping failed");
        require(mac_virtual_key_to_hid_usage(0x06) == 0x1d, "Z mapping failed");
        require(mac_virtual_key_to_hid_usage(0x12) == 0x1e, "1 mapping failed");
        require(mac_virtual_key_to_hid_usage(0x1d) == 0x27, "0 mapping failed");
        require(mac_virtual_key_to_hid_usage(0x24) == 0x28, "Return mapping failed");
        require(mac_virtual_key_to_hid_usage(0x7b) == 0x50, "Left mapping failed");
        require(mac_virtual_key_to_hid_usage(0x38) == 0xe1, "Shift mapping failed");
        require(mac_virtual_key_to_hid_usage(0x36) == 0xe7, "right Command mapping failed");
        require(mac_virtual_key_to_hid_usage(0xffff) == 0, "unknown mapping failed");

        HidKeyboardState state;
        require(state.set_key(0xe1, true), "Shift down was ignored");
        require(state.set_key(0x04, true), "A down was ignored");
        require(!state.set_key(0x04, true), "repeat should not change the report");
        require(state.report()[0] == 0x02 && state.report()[2] == 0x04,
                "Shift+A report is malformed");

        for (uint8_t usage = 0x05; usage <= 0x0a; ++usage) {
            require(state.set_key(usage, true), "ordinary key down was ignored");
        }
        for (size_t i = 2; i < state.report().size(); ++i) {
            require(state.report()[i] == 0x01, "seven-key rollover was not reported");
        }
        require(state.set_key(0x0a, false), "ordinary key up was ignored");
        require(state.report()[2] == 0x04 && state.report()[7] == 0x09,
                "six-key report did not recover after rollover");

        require(state.release_all(), "release_all did not report a change");
        for (uint8_t byte : state.report()) {
            require(byte == 0, "release_all left a key pressed");
        }
        require(!state.release_all(), "empty release_all should be a no-op");

        std::cout << "hid_keyboard_test: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "hid_keyboard_test: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
