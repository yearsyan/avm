// SPDX-License-Identifier: MIT
//
// Small fixed-size datagram protocol used by the MacMu shell to forward host
// pointer input into the headless qemu backend.

#ifndef MACMU_INPUT_PROTOCOL_H
#define MACMU_INPUT_PROTOCOL_H

#include <stdint.h>

namespace macmu {

inline constexpr uint32_t kInputProtocolMagic = 0x4d4d494e;  // "MMIN"
inline constexpr uint16_t kInputProtocolVersion = 1;
inline constexpr const char* kInputSocketEnv = "MACMU_INPUT_SOCKET_PATH";
inline constexpr int kInputTouchPressure = 0x400;
inline constexpr int kInputTouchMajor = 0x400;
inline constexpr int kInputTouchMinor = 0x400;

enum class InputEventKind : uint16_t {
    kTouchBegin = 1,
    kTouchUpdate = 2,
    kTouchEnd = 3,
    kMouseMove = 4,
    kMouseButton = 5,
};

enum InputMouseButtons : uint32_t {
    kInputMouseButtonLeft = 1u << 0,
    kInputMouseButtonRight = 1u << 1,
    kInputMouseButtonMiddle = 1u << 2,
};

#pragma pack(push, 1)
struct InputEventPacket {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t displayId;
    int32_t pointerId;
    int32_t x;
    int32_t y;
    int32_t xGlobal;
    int32_t yGlobal;
    uint32_t buttons;
    uint32_t flags;
};
#pragma pack(pop)

static_assert(sizeof(InputEventPacket) == 40, "MacMu input packet ABI changed");

}  // namespace macmu

#endif  // MACMU_INPUT_PROTOCOL_H
