// SPDX-License-Identifier: MIT

#import "macmu_input_view.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstdint>

#include "guest_input_sender.h"
#include "hid_keyboard.h"
#include "input_sender.h"
#include "macmu_input_protocol.h"
#include "macmu_surface_renderer.h"

namespace {

enum class InputTransport : uint8_t {
    kNone,
    kGuestAgent,
    kDirectQemu,
};

uint32_t buttons_for_event(NSEvent* event) {
    uint32_t buttons = 0;
    const NSUInteger pressed = [NSEvent pressedMouseButtons];
    if (pressed & (1u << 0)) {
        buttons |= macmu::kInputMouseButtonLeft;
    }
    if (pressed & (1u << 1)) {
        buttons |= macmu::kInputMouseButtonRight;
    }
    if (pressed & (1u << 2)) {
        buttons |= macmu::kInputMouseButtonMiddle;
    }

    switch ([event type]) {
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseDragged:
            buttons |= macmu::kInputMouseButtonLeft;
            break;
        case NSEventTypeLeftMouseUp:
            buttons &= ~macmu::kInputMouseButtonLeft;
            break;
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseDragged:
            buttons |= macmu::kInputMouseButtonRight;
            break;
        case NSEventTypeRightMouseUp:
            buttons &= ~macmu::kInputMouseButtonRight;
            break;
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseDragged:
            if ([event buttonNumber] == 2) {
                buttons |= macmu::kInputMouseButtonMiddle;
            }
            break;
        case NSEventTypeOtherMouseUp:
            if ([event buttonNumber] == 2) {
                buttons &= ~macmu::kInputMouseButtonMiddle;
            }
            break;
        default:
            break;
    }
    return buttons;
}

float scroll_axis_value(NSEvent* event, CGFloat delta) {
    if (delta == 0.0) {
        return 0.0f;
    }
    if ([event hasPreciseScrollingDeltas]) {
        return static_cast<float>(delta / 10.0);
    }
    return static_cast<float>(delta);
}

NSEventModifierFlags modifier_flag_for_hid_usage(uint8_t usage) {
    switch (usage) {
        case 0xe0:
        case 0xe4:
            return NSEventModifierFlagControl;
        case 0xe1:
        case 0xe5:
            return NSEventModifierFlagShift;
        case 0xe2:
        case 0xe6:
            return NSEventModifierFlagOption;
        case 0xe3:
        case 0xe7:
            return NSEventModifierFlagCommand;
        default:
            return 0;
    }
}

}  // namespace

@interface MacMuInputView : MTKView
- (instancetype)initWithFrame:(NSRect)frame
                       device:(id<MTLDevice>)device
                  inputSender:(InputSender*)inputSender
              guestInputSender:(GuestInputSender*)guestInputSender
                     displayId:(uint32_t)displayId;
- (void)resetInputState;
- (BOOL)sendTouchKind:(macmu::InputEventKind)kind
             display:(uint32_t)displayId
                   x:(int)x
                   y:(int)y
           transport:(InputTransport)transport;
- (BOOL)sendMouseButtonForDisplay:(uint32_t)displayId
                                x:(int)x
                                y:(int)y
                          buttons:(uint32_t)buttons
                        transport:(InputTransport)transport;
- (BOOL)sendKeyboardReport;
- (BOOL)synchronizeKeyboardModifiers:(NSEventModifierFlags)flags;
@end

@implementation MacMuInputView {
    InputSender* _inputSender;  // not owned
    GuestInputSender* _guestInputSender;  // not owned
    uint32_t _displayId;
    macmu::shell::HidKeyboardState _keyboardState;
    MacMuSurfaceRendererRef _renderer;
    NSTrackingArea* _trackingArea;
    BOOL _leftTouchActive;
    InputTransport _leftTouchTransport;
    uint32_t _leftTouchDisplayId;
    InputTransport _mouseTransport;
    uint32_t _mouseDisplayId;
    int _lastMouseX;
    int _lastMouseY;
    int _lastTouchX;
    int _lastTouchY;
}

- (instancetype)initWithFrame:(NSRect)frame
                       device:(id<MTLDevice>)device
                  inputSender:(InputSender*)inputSender
              guestInputSender:(GuestInputSender*)guestInputSender
                     displayId:(uint32_t)displayId {
    self = [super initWithFrame:frame device:device];
    if (!self) {
        return nil;
    }
    _inputSender = inputSender;
    _guestInputSender = guestInputSender;
    _displayId = displayId;
    _renderer = nil;
    _leftTouchActive = NO;
    _leftTouchTransport = InputTransport::kNone;
    _leftTouchDisplayId = 0;
    _mouseTransport = InputTransport::kNone;
    _mouseDisplayId = 0;
    _lastMouseX = 0;
    _lastMouseY = 0;
    _lastTouchX = 0;
    _lastTouchY = 0;
    return self;
}

- (void)setSurfaceRenderer:(MacMuSurfaceRendererRef)renderer {
    _renderer = renderer;
}

- (void)resetInputState {
    if (_keyboardState.release_all()) {
        [self sendKeyboardReport];
    }
    _leftTouchActive = NO;
    _leftTouchTransport = InputTransport::kNone;
    _mouseTransport = InputTransport::kNone;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}

- (BOOL)sendKeyboardReport {
    return _guestInputSender &&
           _guestInputSender->send_keyboard_report(_displayId, _keyboardState.report());
}

- (BOOL)synchronizeKeyboardModifiers:(NSEventModifierFlags)flags {
    struct ModifierGroup {
        NSEventModifierFlags flag;
        uint8_t leftUsage;
        uint8_t rightUsage;
    };
    static constexpr ModifierGroup kGroups[] = {
        {NSEventModifierFlagControl, 0xe0, 0xe4},
        {NSEventModifierFlagShift, 0xe1, 0xe5},
        {NSEventModifierFlagOption, 0xe2, 0xe6},
        {NSEventModifierFlagCommand, 0xe3, 0xe7},
    };

    BOOL changed = NO;
    for (const ModifierGroup& group : kGroups) {
        const bool leftPressed = _keyboardState.is_pressed(group.leftUsage);
        const bool rightPressed = _keyboardState.is_pressed(group.rightUsage);
        if ((flags & group.flag) == 0) {
            changed |= _keyboardState.set_key(group.leftUsage, false);
            changed |= _keyboardState.set_key(group.rightUsage, false);
        } else if (!leftPressed && !rightPressed) {
            // The view may become first responder while a modifier is already
            // held. Preserve the generic AppKit state as the left-side key;
            // later flagsChanged events refine it to the physical side.
            changed |= _keyboardState.set_key(group.leftUsage, true);
        }
    }
    return changed;
}

- (void)keyDown:(NSEvent*)event {
    const uint8_t usage = macmu::shell::mac_virtual_key_to_hid_usage(event.keyCode);
    if (usage == 0) {
        [super keyDown:event];
        return;
    }
    if (event.isARepeat) {
        // UHID represents current key state; Android generates repeats while
        // the usage remains present in subsequent keyboard scans.
        return;
    }
    BOOL changed = [self synchronizeKeyboardModifiers:event.modifierFlags];
    changed |= _keyboardState.set_key(usage, true);
    if (changed) {
        [self sendKeyboardReport];
    }
}

- (void)keyUp:(NSEvent*)event {
    const uint8_t usage = macmu::shell::mac_virtual_key_to_hid_usage(event.keyCode);
    if (usage == 0) {
        [super keyUp:event];
        return;
    }
    BOOL changed = [self synchronizeKeyboardModifiers:event.modifierFlags];
    changed |= _keyboardState.set_key(usage, false);
    if (changed) {
        [self sendKeyboardReport];
    }
}

- (void)flagsChanged:(NSEvent*)event {
    const uint8_t usage = macmu::shell::mac_virtual_key_to_hid_usage(event.keyCode);
    if (usage == 0x39) {
        // macOS exposes Caps Lock as a toggled flagsChanged event rather than
        // a conventional down/up pair. Pulse the HID usage so Android toggles
        // its own physical-keyboard lock state without leaving the key stuck.
        _keyboardState.set_key(usage, true);
        [self sendKeyboardReport];
        _keyboardState.set_key(usage, false);
        [self sendKeyboardReport];
        return;
    }
    const NSEventModifierFlags flag = modifier_flag_for_hid_usage(usage);
    if (flag == 0) {
        [super flagsChanged:event];
        return;
    }

    const bool wasPressed = _keyboardState.is_pressed(usage);
    const bool pressed = !wasPressed && (event.modifierFlags & flag) != 0;
    if (_keyboardState.set_key(usage, pressed)) {
        [self sendKeyboardReport];
    }
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self.window setAcceptsMouseMovedEvents:YES];
}

- (void)viewWillMoveToWindow:(NSWindow*)newWindow {
    if (!newWindow) {
        if (_leftTouchActive) {
            [self sendTouchKind:macmu::InputEventKind::kTouchEnd
                        display:_leftTouchDisplayId
                              x:_lastTouchX
                              y:_lastTouchY
                      transport:_leftTouchTransport];
            _leftTouchActive = NO;
            _leftTouchTransport = InputTransport::kNone;
        }
        if (_mouseTransport != InputTransport::kNone) {
            [self sendMouseButtonForDisplay:_mouseDisplayId
                                          x:_lastMouseX
                                          y:_lastMouseY
                                    buttons:0
                                  transport:_mouseTransport];
            _mouseTransport = InputTransport::kNone;
        }
        if (_guestInputSender) {
            _guestInputSender->send_hover_exit();
        }
    }
    [super viewWillMoveToWindow:newWindow];
}

- (void)updateTrackingAreas {
    if (_trackingArea) {
        [self removeTrackingArea:_trackingArea];
    }
    NSTrackingAreaOptions options =
        NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow |
        NSTrackingInVisibleRect;
    _trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                 options:options
                                                   owner:self
                                                userInfo:nil];
    [self addTrackingArea:_trackingArea];
    [super updateTrackingAreas];
}

- (BOOL)mapEvent:(NSEvent*)event clamp:(BOOL)clamp x:(int*)x y:(int*)y displayId:(uint32_t*)displayId {
    const NSPoint point = [self convertPoint:[event locationInWindow] fromView:nil];
    return macmu_surface_renderer_map_view_point(_renderer, self, point.x, point.y, clamp, x, y,
                                                 displayId);
}

- (void)mouseMoved:(NSEvent*)event {
    int x = 0;
    int y = 0;
    uint32_t displayId = 0;
    if ([self mapEvent:event clamp:NO x:&x y:&y displayId:&displayId]) {
        if (_guestInputSender) {
            _guestInputSender->send_hover(displayId, x, y);
        }
    }
}

- (void)mouseExited:(NSEvent*)event {
    if (_guestInputSender) {
        _guestInputSender->send_hover_exit();
    }
}

- (InputTransport)preferredInputTransport {
    return _guestInputSender && _guestInputSender->ready()
               ? InputTransport::kGuestAgent
               : InputTransport::kDirectQemu;
}

- (BOOL)sendTouchKind:(macmu::InputEventKind)kind
             display:(uint32_t)displayId
                   x:(int)x
                   y:(int)y
           transport:(InputTransport)transport {
    if (transport == InputTransport::kGuestAgent) {
        return _guestInputSender &&
               _guestInputSender->send_touch(kind, displayId, 0, x, y);
    }
    if (transport == InputTransport::kDirectQemu) {
        return _inputSender && _inputSender->send_touch(kind, displayId, 0, x, y);
    }
    return NO;
}

- (BOOL)sendMouseMoveForDisplay:(uint32_t)displayId
                              x:(int)x
                              y:(int)y
                        buttons:(uint32_t)buttons
                      transport:(InputTransport)transport {
    if (transport == InputTransport::kGuestAgent) {
        return _guestInputSender &&
               _guestInputSender->send_mouse_move(displayId, x, y, buttons);
    }
    if (transport == InputTransport::kDirectQemu) {
        return _inputSender && _inputSender->send_mouse_move(displayId, x, y, buttons);
    }
    return NO;
}

- (BOOL)sendMouseButtonForDisplay:(uint32_t)displayId
                                x:(int)x
                                y:(int)y
                          buttons:(uint32_t)buttons
                        transport:(InputTransport)transport {
    if (transport == InputTransport::kGuestAgent) {
        return _guestInputSender &&
               _guestInputSender->send_mouse_button(displayId, x, y, buttons);
    }
    if (transport == InputTransport::kDirectQemu) {
        return _inputSender && _inputSender->send_mouse_button(displayId, x, y, buttons);
    }
    return NO;
}

- (void)mouseDown:(NSEvent*)event {
    [[self window] makeFirstResponder:self];
    int x = 0;
    int y = 0;
    uint32_t displayId = 0;
    if (![self mapEvent:event clamp:NO x:&x y:&y displayId:&displayId]) {
        return;
    }
    _lastTouchX = x;
    _lastTouchY = y;
    _leftTouchDisplayId = displayId;
    _leftTouchTransport = [self preferredInputTransport];
    BOOL sent = [self sendTouchKind:macmu::InputEventKind::kTouchBegin
                           display:displayId
                                 x:x
                                 y:y
                         transport:_leftTouchTransport];
    // It is safe to fall back only before a gesture has started. Once BEGIN
    // succeeds, every MOVE/END stays pinned to that transport.
    if (!sent && _leftTouchTransport == InputTransport::kGuestAgent) {
        _leftTouchTransport = InputTransport::kDirectQemu;
        sent = [self sendTouchKind:macmu::InputEventKind::kTouchBegin
                           display:displayId
                                 x:x
                                 y:y
                         transport:_leftTouchTransport];
    }
    _leftTouchActive = sent;
    if (!sent) {
        _leftTouchTransport = InputTransport::kNone;
    }
}

- (void)mouseDragged:(NSEvent*)event {
    if (!_leftTouchActive) {
        return;
    }
    int x = 0;
    int y = 0;
    uint32_t displayId = 0;
    if ([self mapEvent:event clamp:YES x:&x y:&y displayId:&displayId]) {
        _lastTouchX = x;
        _lastTouchY = y;
        [self sendTouchKind:macmu::InputEventKind::kTouchUpdate
                    display:_leftTouchDisplayId
                          x:x
                          y:y
                  transport:_leftTouchTransport];
    }
}

- (void)mouseUp:(NSEvent*)event {
    if (!_leftTouchActive) {
        return;
    }
    int x = _lastTouchX;
    int y = _lastTouchY;
    uint32_t ignoredDisplayId = _leftTouchDisplayId;
    [self mapEvent:event clamp:YES x:&x y:&y displayId:&ignoredDisplayId];
    _leftTouchActive = NO;
    [self sendTouchKind:macmu::InputEventKind::kTouchEnd
                display:_leftTouchDisplayId
                      x:x
                      y:y
              transport:_leftTouchTransport];
    _leftTouchTransport = InputTransport::kNone;
}

- (void)scrollWheel:(NSEvent*)event {
    int x = 0;
    int y = 0;
    uint32_t displayId = 0;
    if (![self mapEvent:event clamp:NO x:&x y:&y displayId:&displayId]) {
        return;
    }

    const float hscroll = scroll_axis_value(event, [event scrollingDeltaX]);
    const float vscroll = scroll_axis_value(event, [event scrollingDeltaY]);
    if (_guestInputSender) {
        _guestInputSender->send_scroll(displayId, x, y, hscroll, vscroll);
    }
}

- (void)rightMouseDown:(NSEvent*)event {
    [self sendMouseButtonEvent:event];
}

- (void)rightMouseDragged:(NSEvent*)event {
    [self sendMouseMoveEvent:event clamp:YES];
}

- (void)rightMouseUp:(NSEvent*)event {
    [self sendMouseButtonEvent:event];
}

- (void)otherMouseDown:(NSEvent*)event {
    [self sendMouseButtonEvent:event];
}

- (void)otherMouseDragged:(NSEvent*)event {
    [self sendMouseMoveEvent:event clamp:YES];
}

- (void)otherMouseUp:(NSEvent*)event {
    [self sendMouseButtonEvent:event];
}

- (void)sendMouseMoveEvent:(NSEvent*)event clamp:(BOOL)clamp {
    int x = 0;
    int y = 0;
    uint32_t displayId = 0;
    if ([self mapEvent:event clamp:clamp x:&x y:&y displayId:&displayId]) {
        const uint32_t buttons = buttons_for_event(event);
        _mouseDisplayId = displayId;
        _lastMouseX = x;
        _lastMouseY = y;
        InputTransport transport = _mouseTransport;
        if (transport == InputTransport::kNone) {
            transport = [self preferredInputTransport];
        }
        [self sendMouseMoveForDisplay:displayId
                                    x:x
                                    y:y
                              buttons:buttons
                            transport:transport];
    }
}

- (void)sendMouseButtonEvent:(NSEvent*)event {
    int x = 0;
    int y = 0;
    uint32_t displayId = 0;
    if ([self mapEvent:event clamp:YES x:&x y:&y displayId:&displayId]) {
        const uint32_t buttons = buttons_for_event(event);
        _mouseDisplayId = displayId;
        _lastMouseX = x;
        _lastMouseY = y;
        const NSEventType type = event.type;
        const BOOL isDown = type == NSEventTypeRightMouseDown ||
                            type == NSEventTypeOtherMouseDown;
        if (isDown && _mouseTransport == InputTransport::kNone) {
            _mouseTransport = [self preferredInputTransport];
            BOOL sent = [self sendMouseButtonForDisplay:displayId
                                                      x:x
                                                      y:y
                                                buttons:buttons
                                              transport:_mouseTransport];
            if (!sent && _mouseTransport == InputTransport::kGuestAgent) {
                _mouseTransport = InputTransport::kDirectQemu;
                sent = [self sendMouseButtonForDisplay:displayId
                                                     x:x
                                                     y:y
                                               buttons:buttons
                                             transport:_mouseTransport];
            }
            if (!sent) {
                _mouseTransport = InputTransport::kNone;
            }
        } else {
            [self sendMouseButtonForDisplay:displayId
                                          x:x
                                          y:y
                                    buttons:buttons
                                  transport:_mouseTransport];
        }
        if (buttons == 0) {
            _mouseTransport = InputTransport::kNone;
        }
    }
}

@end

MTKView* macmu_input_view_create(NSRect frame,
                                 id<MTLDevice> device,
                                 InputSender* input_sender,
                                 GuestInputSender* guest_input_sender,
                                 uint32_t display_id) {
    return [[MacMuInputView alloc] initWithFrame:frame
                                         device:device
                                    inputSender:input_sender
                                guestInputSender:guest_input_sender
                                       displayId:display_id];
}

void macmu_input_view_set_renderer(MTKView* view, MacMuSurfaceRendererRef renderer) {
    MacMuInputView* inputView = (MacMuInputView*)view;
    if ([inputView isKindOfClass:[MacMuInputView class]]) {
        [inputView setSurfaceRenderer:renderer];
    }
}

void macmu_input_view_reset_state(MTKView* view) {
    MacMuInputView* inputView = (MacMuInputView*)view;
    if ([inputView isKindOfClass:[MacMuInputView class]]) {
        [inputView resetInputState];
    }
}
