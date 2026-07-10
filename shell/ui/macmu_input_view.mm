// SPDX-License-Identifier: MIT

#import "macmu_input_view.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstdint>

#include "guest_input_sender.h"
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

}  // namespace

@interface MacMuInputView : MTKView
- (instancetype)initWithFrame:(NSRect)frame
                       device:(id<MTLDevice>)device
                  inputSender:(InputSender*)inputSender
              guestInputSender:(GuestInputSender*)guestInputSender;
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
@end

@implementation MacMuInputView {
    InputSender* _inputSender;  // not owned
    GuestInputSender* _guestInputSender;  // not owned
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
              guestInputSender:(GuestInputSender*)guestInputSender {
    self = [super initWithFrame:frame device:device];
    if (!self) {
        return nil;
    }
    _inputSender = inputSender;
    _guestInputSender = guestInputSender;
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
                                 GuestInputSender* guest_input_sender) {
    return [[MacMuInputView alloc] initWithFrame:frame
                                         device:device
                                    inputSender:input_sender
                                guestInputSender:guest_input_sender];
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
