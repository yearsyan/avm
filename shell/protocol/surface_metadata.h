// SPDX-License-Identifier: MIT
//
// Shared surface metadata exchanged between the frame channel, the JSON
// metadata fallback reader, and the AppKit/Metal renderer. Kept as a plain
// C++ header (no Cocoa/Metal dependency) so the .cpp backend units can include
// it without pulling in IOSurface.

#ifndef MACMU_SHELL_SURFACE_METADATA_H
#define MACMU_SHELL_SURFACE_METADATA_H

#include <cstdint>

// IOSurfaceID is a uint32_t on Darwin. We avoid #include <IOSurface/IOSurface.h>
// here so pure-C++ translation units don't need the framework; the renderer
// (which does include the framework) casts freely between the two.
using MacmuIOSurfaceID = uint32_t;

struct SurfaceMetadata {
    MacmuIOSurfaceID iosurfaceId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frame = 0;
};

#endif  // MACMU_SHELL_SURFACE_METADATA_H
