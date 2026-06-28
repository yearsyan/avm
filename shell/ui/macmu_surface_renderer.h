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

// Create the renderer delegate for |view|. Returns nil-equivalent (nullptr) on
// Metal pipeline failure. The caller owns the returned object and is expected
// to release it via -release (it is a plain NSObject under ARC).
MacMuSurfaceRendererRef macmu_surface_renderer_create(MTKView* view,
                                                      FrameConsumer* frame_consumer);

#ifdef __OBJC__
// Convert an AppKit point in |view| coordinates into guest framebuffer pixels.
// Returns false when no guest frame is known yet or the point is outside the
// rendered viewport and |clamp| is false. The current IOSurface export only
// carries display 0, but the out parameter keeps the caller protocol-ready for
// multi-display metadata.
bool macmu_surface_renderer_map_view_point(MacMuSurfaceRendererRef renderer,
                                           MTKView* view,
                                           double point_x,
                                           double point_y,
                                           bool clamp,
                                           int* out_x,
                                           int* out_y,
                                           uint32_t* out_display_id);
#endif

#endif  // MACMU_SHELL_SURFACE_RENDERER_H
