// SPDX-License-Identifier: MIT
//
// MacMu shell <-> qemu control plane ('MMCP').
//
// Transport: an AF_UNIX SOCK_STREAM socketpair created by the shell; the qemu
// end is dup2()'d into kControlChildFd and advertised through kControlFdEnv
// (the same inherited-fd pattern as the frame doorbell fd 198 and the input
// fd 199). Framing is a fixed 16-byte header followed by a packed
// little-endian payload. Requests carry a sender-unique requestId which the
// response echoes; events use requestId 0.
//
// Rules (see docs/FRAME_CHANNEL_V2_CONTROL_PLANE.md):
//  * Bad magic/version or an oversized length is a protocol failure: close
//    the channel. There is no resync on a corrupt stream.
//  * Unknown request type -> ERROR response with kControlErrUnsupported.
//  * Unknown event type -> ignored by the receiver.
//
// This header is shared source between the MIT shell and the qemu glue (the
// glue's include path points at shell/protocol), so it must stay
// dependency-free.

#ifndef MACMU_CONTROL_PROTOCOL_H
#define MACMU_CONTROL_PROTOCOL_H

#include <stdint.h>

namespace macmu {

inline constexpr uint32_t kControlProtocolMagic = 0x4d4d4350;  // "MMCP"
inline constexpr uint16_t kControlProtocolVersion = 1;
inline constexpr uint32_t kControlMaxPayload = 64 * 1024;

inline constexpr const char* kControlFdEnv = "MACMU_CONTROL_FD";
inline constexpr int kControlChildFd = 197;

// Message type ranges: requests 0x0001-0x00FF (shell -> qemu), responses
// 0x0101-0x01FF (response type == request type | 0x0100), events
// 0x0201-0x02FF (qemu -> shell, requestId == 0).
enum class ControlMessageType : uint16_t {
    kHello = 0x0001,
    kPing = 0x0002,
    kDisplayAdd = 0x0010,
    kDisplayRemove = 0x0011,
    kDisplayList = 0x0012,
    kDisplayStream = 0x0013,

    kHelloAck = 0x0101,
    kPong = 0x0102,
    kDisplayAddOk = 0x0110,
    kDisplayRemoveOk = 0x0111,
    kDisplayListOk = 0x0112,
    kDisplayStreamOk = 0x0113,
    kError = 0x01FF,

    kEventDisplay = 0x0201,
    kEventBoot = 0x0202,
};

inline constexpr uint16_t control_response_type(uint16_t requestType) {
    return requestType | 0x0100;
}

// ERROR codes.
inline constexpr int32_t kControlErrUnsupported = 1;
inline constexpr int32_t kControlErrInvalidArgument = 2;
inline constexpr int32_t kControlErrRejected = 3;   // host-side add/remove failed
inline constexpr int32_t kControlErrNotReady = 4;   // guest service not connected yet
inline constexpr int32_t kControlErrInternal = 5;

// EVENT_DISPLAY states.
inline constexpr uint32_t kControlDisplayAdded = 1;
inline constexpr uint32_t kControlDisplayChanged = 2;
inline constexpr uint32_t kControlDisplayRemoved = 3;

// DISPLAY_ADD with displayId == kControlDisplayIdAuto asks qemu to pick a
// free user display id.
inline constexpr uint32_t kControlDisplayIdAuto = 0xFFFFFFFFu;

#pragma pack(push, 1)

struct ControlFrameHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t type;       // ControlMessageType
    uint32_t requestId;  // 0 for events
    uint32_t length;     // payload bytes following this header
};
static_assert(sizeof(ControlFrameHeader) == 16, "MMCP header ABI changed");

struct ControlHello {
    uint32_t protoVersion;
};

struct ControlHelloAck {
    uint32_t protoVersion;
    uint32_t capabilities;  // reserved, 0
    uint32_t maxDisplays;   // frame-channel slot count on the qemu side
    uint32_t frameShmVersion;
};

struct ControlPong {
    uint64_t timestampNs;
};

struct ControlDisplayAdd {
    uint32_t displayId;  // kControlDisplayIdAuto to auto-allocate
    uint32_t width;
    uint32_t height;
    uint32_t dpi;
    uint32_t flags;  // guest VIRTUAL_DISPLAY_* flags; 0 = qemu defaults
};

struct ControlDisplayAddOk {
    uint32_t displayId;
};

struct ControlDisplayRemove {
    uint32_t displayId;
};

// Enable/disable host-paced frame export streaming for one secondary
// display. VirtualDisplay-backed displays update their bound ColorBuffer in
// place with no per-frame host signal, so the shell turns streaming on while
// it has a window sampling that display and off when the window closes.
struct ControlDisplayStream {
    uint32_t displayId;
    uint32_t enabled;  // 0 or 1
};

struct ControlDisplayInfo {
    uint32_t displayId;
    uint32_t width;
    uint32_t height;
    uint32_t dpi;
    uint32_t flags;
    uint32_t enabled;
};

struct ControlDisplayListOk {
    uint32_t count;
    // Followed by count * ControlDisplayInfo.
};

struct ControlError {
    int32_t code;
    uint32_t msgLen;
    // Followed by msgLen bytes of UTF-8 message (not NUL terminated).
};

struct ControlEventDisplay {
    uint32_t displayId;
    uint32_t state;  // kControlDisplay{Added,Changed,Removed}
    uint32_t width;
    uint32_t height;
    uint32_t dpi;
    uint32_t flags;
};

struct ControlEventBoot {
    uint32_t stage;  // reserved for boot progress reporting
};

#pragma pack(pop)

}  // namespace macmu

#endif  // MACMU_CONTROL_PROTOCOL_H
