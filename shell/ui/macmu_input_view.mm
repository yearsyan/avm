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
@end

@implementation MacMuInputView {
    InputSender* _inputSender;  // not owned
    GuestInputSender* _guestInputSender;  // not owned
    MacMuSurfaceRendererRef _renderer;
    NSTrackingArea* _trackingArea;
    BOOL _leftTouchActive;
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
    _lastTouchX = 0;
    _lastTouchY = 0;
    return self;
}

- (void)setSurfaceRenderer:(MacMuSurfaceRendererRef)renderer {
    _renderer = renderer;
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

- (void)mouseDown:(NSEvent*)event {
    [[self window] makeFirstResponder:self];
    int x = 0;
    int y = 0;
    uint32_t displayId = 0;
    if (![self mapEvent:event clamp:NO x:&x y:&y displayId:&displayId]) {
        return;
    }
    _leftTouchActive = YES;
    _lastTouchX = x;
    _lastTouchY = y;
    _inputSender->send_touch(macmu::InputEventKind::kTouchBegin, displayId, 0, x, y);
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
        _inputSender->send_touch(macmu::InputEventKind::kTouchUpdate, displayId, 0, x, y);
    }
}

- (void)mouseUp:(NSEvent*)event {
    if (!_leftTouchActive) {
        return;
    }
    int x = _lastTouchX;
    int y = _lastTouchY;
    uint32_t displayId = 0;
    [self mapEvent:event clamp:YES x:&x y:&y displayId:&displayId];
    _leftTouchActive = NO;
    _inputSender->send_touch(macmu::InputEventKind::kTouchEnd, displayId, 0, x, y);
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
        _inputSender->send_mouse_move(displayId, x, y, buttons_for_event(event));
    }
}

- (void)sendMouseButtonEvent:(NSEvent*)event {
    int x = 0;
    int y = 0;
    uint32_t displayId = 0;
    if ([self mapEvent:event clamp:YES x:&x y:&y displayId:&displayId]) {
        _inputSender->send_mouse_button(displayId, x, y, buttons_for_event(event));
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
