// SPDX-License-Identifier: MIT
//
// The Metal shader source used by the surface renderer to blit the guest
// IOSurface into the CAMetalDrawable. Kept as a plain C string so it lives in a
// header-only, framework-free form; the renderer wraps it in NSString before
// handing it to the device compiler.

#ifndef MACMU_SHELL_METAL_SHADER_H
#define MACMU_SHELL_METAL_SHADER_H

// Fullscreen-triangle vertex + bilinear-sampled texture fragment shader. UVs
// are flipped vertically so the guest surface appears right-side up.
inline constexpr const char* kSurfaceShaderSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 position [[position]];
    float2 uv;
};

vertex VSOut vertexMain(uint vertexId [[vertex_id]]) {
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0)
    };
    VSOut out;
    out.position = float4(positions[vertexId], 0.0, 1.0);
    out.uv = float2((positions[vertexId].x + 1.0) * 0.5,
                    1.0 - (positions[vertexId].y + 1.0) * 0.5);
    return out;
}

fragment half4 fragmentMain(VSOut in [[stage_in]],
                            texture2d<half> surfaceTexture [[texture(0)]]) {
    constexpr sampler textureSampler(address::clamp_to_edge, filter::linear);
    return surfaceTexture.sample(textureSampler, in.uv);
}
)METAL";

#endif  // MACMU_SHELL_METAL_SHADER_H
