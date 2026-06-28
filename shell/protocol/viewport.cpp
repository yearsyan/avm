// SPDX-License-Identifier: MIT
//
// Aspect-fit viewport math. Moved verbatim from the original
// MacMuSurfaceRenderer::recomputeViewport; pure C++.

#include "viewport.h"

#include <algorithm>

Viewport aspect_fit_viewport(uint32_t source_width, uint32_t source_height,
                             double drawable_width, double drawable_height) {
    if (source_width == 0 || source_height == 0 || drawable_width <= 0 ||
        drawable_height <= 0) {
        return Viewport{0.0, 0.0, drawable_width, drawable_height, 0.0, 1.0};
    }
    const double source_aspect =
        static_cast<double>(source_width) / static_cast<double>(source_height);
    const double drawable_aspect = drawable_width / drawable_height;
    double width = drawable_width;
    double height = drawable_height;
    if (drawable_aspect > source_aspect) {
        width = drawable_height * source_aspect;
    } else {
        height = drawable_width / source_aspect;
    }
    return Viewport{(drawable_width - width) * 0.5, (drawable_height - height) * 0.5, width,
                    height, 0.0, 1.0};
}
