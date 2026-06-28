// SPDX-License-Identifier: MIT
//
// Aspect-fit viewport math, kept pure C++ so it can be tested / reasoned about
// without pulling in Metal. The Viewport struct is laid out identically to
// Metal's MTLViewport (6 x double: originX, originY, width, height, znear,
// zfar) so the renderer can reinterpret_cast it directly when handing it to
// [encoder setViewport:].

#ifndef MACMU_SHELL_VIEWPORT_H
#define MACMU_SHELL_VIEWPORT_H

#include <cstdint>

struct Viewport {
    double originX = 0.0;
    double originY = 0.0;
    double width = 0.0;
    double height = 0.0;
    double znear = 0.0;
    double zfar = 1.0;
};

// Compute an aspect-fit viewport so the |source_width|x|source_height| surface
// is centered inside the |drawable_width|x|drawable_height| render target with
// no stretching. When the source dimensions are unknown/zero (or the drawable
// is degenerate) the whole drawable is returned. This matches the arithmetic
// that used to live inline in MacMuSurfaceRenderer::recomputeViewport.
Viewport aspect_fit_viewport(uint32_t source_width, uint32_t source_height,
                             double drawable_width, double drawable_height);

#endif  // MACMU_SHELL_VIEWPORT_H
