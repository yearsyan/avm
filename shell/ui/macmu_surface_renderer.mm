// SPDX-License-Identifier: MIT
//
// IOSurface -> Metal blit renderer (the MTKViewDelegate). Moved out of
// macmu_shell.mm so the app shell stays small; viewport math and the shader
// source live in viewport.* / metal_shader.h respectively. Needs AppKit/Metal.

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "frame_consumer.h"
#include "macmu_surface_renderer.h"
#include "metal_shader.h"
#include "surface_metadata.h"
#include "viewport.h"

namespace {

NSSize fitted_window_content_size(uint32_t pixel_width, uint32_t pixel_height) {
    if (pixel_width == 0 || pixel_height == 0) {
        return NSMakeSize(420, 720);
    }

    CGFloat width = static_cast<CGFloat>(pixel_width);
    CGFloat height = static_cast<CGFloat>(pixel_height);
    if (NSScreen* screen = [NSScreen mainScreen]) {
        const NSRect visibleFrame = [screen visibleFrame];
        const CGFloat maxWidth = visibleFrame.size.width * 0.85;
        const CGFloat maxHeight = visibleFrame.size.height * 0.85;
        const CGFloat scale = std::max<CGFloat>(1.0, std::max(width / maxWidth, height / maxHeight));
        width /= scale;
        height /= scale;
    }

    return NSMakeSize(std::max<CGFloat>(1.0, width), std::max<CGFloat>(1.0, height));
}

}  // namespace

@interface MacMuSurfaceRenderer : NSObject <MTKViewDelegate>
- (instancetype)initWithView:(MTKView*)view frameConsumer:(FrameConsumer*)frameConsumer;
@end

@implementation MacMuSurfaceRenderer {
    MTKView* _view;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    SurfaceMetadata _metadata;
    IOSurfaceRef _surface;
    id<MTLTexture> _surfaceTexture;
    FrameConsumer* _frameConsumer;  // not owned; nil when unavailable
    bool _useChannel;               // true when FrameConsumer is valid
    uint64_t _lastDrawnFrame;       // last frame number actually rendered
    Viewport _cachedViewport;       // recomputed only when drawable size changes
    bool _viewportValid;
}

- (instancetype)initWithView:(MTKView*)view frameConsumer:(FrameConsumer*)frameConsumer {
    self = [super init];
    if (!self) {
        return nil;
    }

    _view = view;
    _device = view.device;
    _queue = [_device newCommandQueue];
    _surface = nullptr;
    _frameConsumer = frameConsumer;
    _useChannel = frameConsumer != nullptr && frameConsumer->valid();
    _lastDrawnFrame = 0;
    _viewportValid = false;
    _cachedViewport = Viewport{};

    NSError* error = nil;
    id<MTLLibrary> library = [_device newLibraryWithSource:[NSString stringWithUTF8String:kSurfaceShaderSource]
                                                   options:nil
                                                     error:&error];
    if (!library) {
        NSLog(@"Failed to build Metal library: %@", error);
        return nil;
    }

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [library newFunctionWithName:@"vertexMain"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"fragmentMain"];
    descriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat;
    _pipeline = [_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!_pipeline) {
        NSLog(@"Failed to create Metal pipeline: %@", error);
        return nil;
    }

    return self;
}

- (void)dealloc {
    if (_surface) {
        CFRelease(_surface);
        _surface = nullptr;
    }
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    // Recompute and cache the aspect-fit viewport; this avoids redoing the
    // arithmetic on every draw call.
    [self recomputeViewport:view];
}

- (void)recomputeViewport:(MTKView*)view {
    const CGSize drawableSize = view.drawableSize;
    _cachedViewport = aspect_fit_viewport(_metadata.width, _metadata.height,
                                          static_cast<double>(drawableSize.width),
                                          static_cast<double>(drawableSize.height));
    _viewportValid = true;
}

- (BOOL)mapViewPoint:(NSPoint)point
                view:(MTKView*)view
               clamp:(BOOL)clamp
                   x:(int*)outX
                   y:(int*)outY
           displayId:(uint32_t*)outDisplayId {
    if (_metadata.width == 0 || _metadata.height == 0) {
        return NO;
    }
    if (!_viewportValid) {
        [self recomputeViewport:view];
    }

    const NSRect bounds = view.bounds;
    const CGSize drawableSize = view.drawableSize;
    if (bounds.size.width <= 0 || bounds.size.height <= 0 || drawableSize.width <= 0 ||
        drawableSize.height <= 0 || _cachedViewport.width <= 0 || _cachedViewport.height <= 0) {
        return NO;
    }

    const double drawableX = point.x * drawableSize.width / bounds.size.width;
    const double drawableY =
        drawableSize.height - (point.y * drawableSize.height / bounds.size.height);
    const double minX = _cachedViewport.originX;
    const double minY = _cachedViewport.originY;
    const double maxX = minX + _cachedViewport.width;
    const double maxY = minY + _cachedViewport.height;

    if (!clamp && (drawableX < minX || drawableX > maxX || drawableY < minY || drawableY > maxY)) {
        return NO;
    }

    const double clampedX = std::clamp(drawableX, minX, std::nextafter(maxX, minX));
    const double clampedY = std::clamp(drawableY, minY, std::nextafter(maxY, minY));
    const double sourceX = (clampedX - minX) * static_cast<double>(_metadata.width) /
                           _cachedViewport.width;
    const double sourceY = (clampedY - minY) * static_cast<double>(_metadata.height) /
                           _cachedViewport.height;

    if (outX) {
        *outX = std::clamp(static_cast<int>(sourceX), 0,
                           static_cast<int>(_metadata.width) - 1);
    }
    if (outY) {
        *outY = std::clamp(static_cast<int>(sourceY), 0,
                           static_cast<int>(_metadata.height) - 1);
    }
    if (outDisplayId) {
        *outDisplayId = 0;
    }
    return YES;
}

// Returns true if there is a new frame to render (frame number advanced past
// the last drawn frame), false otherwise. On a true return the IOSurface is
// refreshed when its id/size changed.
- (BOOL)reloadSurfaceIfNeeded {
    SurfaceMetadata next = {};
    bool gotFrame = false;
    if (_useChannel) {
        gotFrame = _frameConsumer->read(&next);
    }
    if (!gotFrame) {
        return NO;
    }
    // Only a frame-number advance counts as a new frame worth rendering.
    if (next.frame <= _lastDrawnFrame && _surfaceTexture != nil) {
        return NO;
    }
    const BOOL surfaceChanged = next.iosurfaceId != _metadata.iosurfaceId ||
                               next.width != _metadata.width ||
                               next.height != _metadata.height;
    if (surfaceChanged) {
        IOSurfaceRef surface = IOSurfaceLookup(next.iosurfaceId);
        if (!surface) {
            return NO;
        }
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                              width:next.width
                                                             height:next.height
                                                          mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [_device newTextureWithDescriptor:descriptor
                                                         iosurface:surface
                                                             plane:0];
        if (!texture) {
            CFRelease(surface);
            return NO;
        }
        if (_surface) {
            CFRelease(_surface);
        }
        _surface = surface;
        _surfaceTexture = texture;
        const NSSize contentSize = fitted_window_content_size(next.width, next.height);
        [_view.window setContentAspectRatio:NSMakeSize(next.width, next.height)];
        [_view.window setContentSize:contentSize];
        [_view.window setTitle:[NSString stringWithFormat:@"MacMu IOSurface %u", next.iosurfaceId]];
        _viewportValid = false;
    }
    _metadata = next;
    return YES;
}

- (void)drawInMTKView:(MTKView*)view {
    // Skip the Metal submit entirely when there is no new frame to show. This
    // is the key power/throughput win: a static guest screen (lock screen,
    // paused app) no longer drives a 60 Hz blit loop.
    const BOOL hasNewFrame = [self reloadSurfaceIfNeeded];
    if (!hasNewFrame) {
        return;
    }
    if (!_surfaceTexture || !_pipeline) {
        return;
    }

    id<CAMetalDrawable> drawable = view.currentDrawable;
    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    if (!drawable || !pass) {
        return;
    }
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = view.clearColor;

    if (!_viewportValid) {
        [self recomputeViewport:view];
    }
    // Viewport is layout-compatible with MTLViewport (6 x double).
    MTLViewport metalViewport;
    std::memcpy(&metalViewport, &_cachedViewport, sizeof(metalViewport));
    id<MTLCommandBuffer> commandBuffer = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setViewport:metalViewport];
    [encoder setRenderPipelineState:_pipeline];
    [encoder setFragmentTexture:_surfaceTexture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];

    _lastDrawnFrame = _metadata.frame;
}

@end

MacMuSurfaceRendererRef macmu_surface_renderer_create(MTKView* view,
                                                      FrameConsumer* frame_consumer) {
    return [[MacMuSurfaceRenderer alloc] initWithView:view frameConsumer:frame_consumer];
}

bool macmu_surface_renderer_map_view_point(MacMuSurfaceRendererRef renderer,
                                           MTKView* view,
                                           double point_x,
                                           double point_y,
                                           bool clamp,
                                           int* out_x,
                                           int* out_y,
                                           uint32_t* out_display_id) {
    MacMuSurfaceRenderer* concrete = (MacMuSurfaceRenderer*)renderer;
    if (!concrete || !view) {
        return false;
    }
    return [concrete mapViewPoint:NSMakePoint(point_x, point_y)
                             view:view
                            clamp:clamp ? YES : NO
                                x:out_x
                                y:out_y
                        displayId:out_display_id];
}
