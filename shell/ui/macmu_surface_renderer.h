// SPDX-License-Identifier: MIT
//
// Interface to the IOSurface -> Metal blit renderer. The implementation is an
// Obj-C MTKViewDelegate (needs MetalKit/AppKit), so the public surface here is
// intentionally opaque: callers get an `id` they hand to MTKView.delegate.

#ifndef MACMU_SHELL_SURFACE_RENDERER_H
#define MACMU_SHELL_SURFACE_RENDERER_H

#include <cstdint>
#include <string>

#ifdef __OBJC__
#import <MetalKit/MetalKit.h>

// The renderer is an MTKViewDelegate; callers only need it as such, so the
// public handle is typed as id<MTKViewDelegate> and the concrete class stays
// internal to macmu_surface_renderer.mm.
using MacMuSurfaceRendererRef = id<MTKViewDelegate>;
#else
class MacMuSurfaceRenderer;
using MacMuSurfaceRendererRef = void*;
#endif

class FrameConsumer;

// Create the renderer delegate for |view|, sampling frame-channel slot
// |display_id|. Returns nil-equivalent (nullptr) on Metal pipeline failure.
// The caller owns the returned object and is expected to release it via
// -release (it is a plain NSObject under ARC).
MacMuSurfaceRendererRef macmu_surface_renderer_create(MTKView* view,
                                                      FrameConsumer* frame_consumer,
                                                      uint32_t display_id);

#ifdef __OBJC__
// Convert an AppKit point in |view| coordinates into guest framebuffer pixels.
// Returns false when no guest frame is known yet or the point is outside the
// rendered viewport and |clamp| is false. |out_display_id| receives the
// renderer's display id so input events route to the right guest display.
bool macmu_surface_renderer_map_view_point(MacMuSurfaceRendererRef renderer,
                                           MTKView* view,
                                           double point_x,
                                           double point_y,
                                           bool clamp,
                                           int* out_x,
                                           int* out_y,
                                           uint32_t* out_display_id);

// Fit a guest framebuffer of |pixel_width|x|pixel_height| into the main
// screen's visible frame (85% cap, never upscaled below 1:1), returning the
// suggested window content size. Exposed so the shell can size a secondary
// display window to the requested aspect ratio before the first frame
// arrives, avoiding a first-frame geometry jump.
NSSize macmu_fitted_window_content_size(uint32_t pixel_width, uint32_t pixel_height);
#endif

#endif  // MACMU_SHELL_SURFACE_RENDERER_H
