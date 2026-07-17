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
#include <cstdlib>
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

bool renderer_debug_logs_enabled() {
    static const bool enabled = std::getenv("MACMU_RENDERER_LOG") != nullptr;
    return enabled;
}

NSMutableDictionary<NSString*, id<MTLRenderPipelineState>>* surface_pipeline_cache() {
    static NSMutableDictionary<NSString*, id<MTLRenderPipelineState>>* cache =
        [[NSMutableDictionary alloc] init];
    return cache;
}

id<MTLRenderPipelineState> cached_surface_pipeline(id<MTLDevice> device,
                                                   MTLPixelFormat pixelFormat,
                                                   NSError** error) {
    NSMutableDictionary<NSString*, id<MTLRenderPipelineState>>* cache = surface_pipeline_cache();
    NSString* key = [NSString stringWithFormat:@"%llu:%lu",
                                               static_cast<unsigned long long>(device.registryID),
                                               static_cast<unsigned long>(pixelFormat)];
    @synchronized(cache) {
        id<MTLRenderPipelineState> cached = [cache objectForKey:key];
        if (cached) {
            if (error) {
                *error = nil;
            }
            return cached;
        }

        NSError* localError = nil;
        id<MTLLibrary> library =
            [device newLibraryWithSource:[NSString stringWithUTF8String:kSurfaceShaderSource]
                                 options:nil
                                   error:&localError];
        if (!library) {
            if (error) {
                *error = localError;
            }
            return nil;
        }

        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"vertexMain"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"fragmentMain"];
        descriptor.colorAttachments[0].pixelFormat = pixelFormat;
        id<MTLRenderPipelineState> pipeline =
            [device newRenderPipelineStateWithDescriptor:descriptor error:&localError];
        if (!pipeline) {
            if (error) {
                *error = localError;
            }
            return nil;
        }

        [cache setObject:pipeline forKey:key];
        if (error) {
            *error = nil;
        }
        return pipeline;
    }
}

}  // namespace

@interface MacMuSurfaceRenderer : NSObject <MTKViewDelegate>
- (instancetype)initWithView:(MTKView*)view
               frameConsumer:(FrameConsumer*)frameConsumer
                   displayId:(uint32_t)displayId;
@end

@implementation MacMuSurfaceRenderer {
    MTKView* _view;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    SurfaceMetadata _metadata;
    id<MTLTexture> _surfaceTexture;
    NSMutableDictionary<NSNumber*, id>* _surfaceCache;
    NSMutableDictionary<NSNumber*, id<MTLTexture>>* _textureCache;
    FrameConsumer* _frameConsumer;  // not owned; nil when unavailable
    uint32_t _displayId;            // frame-channel slot this renderer samples
    bool _useChannel;               // true when FrameConsumer is valid
    uint64_t _lastDrawnFrame;       // last frame number actually rendered
    uint64_t _lastLoggedSubmitFrame;
    MacmuIOSurfaceID _lastFailedSurfaceId;
    bool _loggedWaitingForFrame;
    bool _loggedMissingTexture;
    bool _loggedNoDrawable;
    Viewport _cachedViewport;       // recomputed only when drawable size changes
    bool _viewportValid;
}

- (instancetype)initWithView:(MTKView*)view
               frameConsumer:(FrameConsumer*)frameConsumer
                   displayId:(uint32_t)displayId {
    self = [super init];
    if (!self) {
        return nil;
    }

    _view = view;
    _device = view.device;
    _queue = [_device newCommandQueue];
    _surfaceCache = [[NSMutableDictionary alloc] init];
    _textureCache = [[NSMutableDictionary alloc] init];
    _frameConsumer = frameConsumer;
    _displayId = displayId;
    _useChannel = frameConsumer != nullptr && frameConsumer->valid();
    _lastDrawnFrame = 0;
    _lastLoggedSubmitFrame = 0;
    _lastFailedSurfaceId = 0;
    _loggedWaitingForFrame = false;
    _loggedMissingTexture = false;
    _loggedNoDrawable = false;
    _viewportValid = false;
    _cachedViewport = Viewport{};

    NSError* error = nil;
    _pipeline = cached_surface_pipeline(_device, view.colorPixelFormat, &error);
    if (!_pipeline) {
        NSLog(@"Failed to create Metal pipeline: %@", error);
        return nil;
    }

    return self;
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
        *outDisplayId = _displayId;
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
        gotFrame = _frameConsumer->read(_displayId, &next);
    }
    if (!gotFrame) {
        if (renderer_debug_logs_enabled() && !_surfaceTexture && !_loggedWaitingForFrame) {
            NSLog(@"MacMu renderer waiting for first IOSurface frame.");
            _loggedWaitingForFrame = true;
        }
        return NO;
    }
    _loggedWaitingForFrame = false;
    // A qemu restart resets the producer's frame counter. Treat a lower frame
    // number as a fresh stream; only an equal frame is a true duplicate.
    const BOOL frameCounterReset = next.frame < _lastDrawnFrame;
    if (next.frame == _lastDrawnFrame && _surfaceTexture != nil) {
        return NO;
    }
    const BOOL sizeChanged = frameCounterReset || next.width != _metadata.width ||
                             next.height != _metadata.height;
    const BOOL surfaceChanged =
        sizeChanged || next.iosurfaceId != _metadata.iosurfaceId;
    if (surfaceChanged) {
        if (renderer_debug_logs_enabled()) {
            NSLog(@"MacMu renderer mapping IOSurface %u (%ux%u), frame %llu%@.",
                  next.iosurfaceId, next.width, next.height,
                  static_cast<unsigned long long>(next.frame),
                  frameCounterReset ? @" after producer restart" : @"");
        }
        if (sizeChanged) {
            [_surfaceCache removeAllObjects];
            [_textureCache removeAllObjects];
        }

        NSNumber* cacheKey = @(next.iosurfaceId);
        id<MTLTexture> texture = [_textureCache objectForKey:cacheKey];
        if (!texture) {
            IOSurfaceRef surface = IOSurfaceLookup(next.iosurfaceId);
            if (!surface) {
                if (_lastFailedSurfaceId != next.iosurfaceId) {
                    NSLog(@"MacMu renderer IOSurfaceLookup failed for id %u.", next.iosurfaceId);
                    _lastFailedSurfaceId = next.iosurfaceId;
                }
                return NO;
            }
            MTLTextureDescriptor* descriptor =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                  width:next.width
                                                                 height:next.height
                                                              mipmapped:NO];
            descriptor.usage = MTLTextureUsageShaderRead;
            texture = [_device newTextureWithDescriptor:descriptor
                                              iosurface:surface
                                                  plane:0];
            if (!texture) {
                NSLog(@"MacMu renderer Metal texture creation failed for IOSurface %u.",
                      next.iosurfaceId);
                CFRelease(surface);
                return NO;
            }
            id surfaceObject = CFBridgingRelease(surface);
            [_surfaceCache setObject:surfaceObject forKey:cacheKey];
            [_textureCache setObject:texture forKey:cacheKey];
        }
        _surfaceTexture = texture;
        if (sizeChanged) {
            const NSSize contentSize = fitted_window_content_size(next.width, next.height);
            [_view.window setContentAspectRatio:NSMakeSize(next.width, next.height)];
            [_view.window setContentSize:contentSize];
            _viewportValid = false;
        }
        if (renderer_debug_logs_enabled()) {
            NSLog(@"MacMu renderer mapped IOSurface %u into Metal texture.", next.iosurfaceId);
        }
        _lastFailedSurfaceId = 0;
    }
    _metadata = next;
    return YES;
}

- (void)drawInMTKView:(MTKView*)view {
    // The doorbell wakes us for new guest frames, but AppKit can also ask the
    // view to redraw after expose/resize. In that case, reuse the last mapped
    // IOSurface instead of leaving the drawable at the window background.
    const BOOL hasNewFrame = [self reloadSurfaceIfNeeded];
    const BOOL canDrawSurface = _surfaceTexture != nil && _pipeline != nil;
    if (!canDrawSurface) {
        if (renderer_debug_logs_enabled() && (hasNewFrame || !_loggedMissingTexture)) {
            NSLog(@"MacMu renderer cannot draw yet (texture=%@ pipeline=%@).",
                  _surfaceTexture ? @"yes" : @"no", _pipeline ? @"yes" : @"no");
            _loggedMissingTexture = true;
        }
    } else {
        _loggedMissingTexture = false;
    }

    id<CAMetalDrawable> drawable = view.currentDrawable;
    MTLRenderPassDescriptor* pass = view.currentRenderPassDescriptor;
    if (!drawable || !pass) {
        if (!_loggedNoDrawable) {
            NSLog(@"MacMu renderer has no drawable/pass yet (drawable=%@ pass=%@).",
                  drawable ? @"yes" : @"no", pass ? @"yes" : @"no");
            _loggedNoDrawable = true;
        }
        return;
    }
    _loggedNoDrawable = false;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = view.clearColor;

    id<MTLCommandBuffer> commandBuffer = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (canDrawSurface) {
        if (!_viewportValid) {
            [self recomputeViewport:view];
        }
        // Viewport is layout-compatible with MTLViewport (6 x double).
        MTLViewport metalViewport;
        std::memcpy(&metalViewport, &_cachedViewport, sizeof(metalViewport));
        [encoder setViewport:metalViewport];
        [encoder setRenderPipelineState:_pipeline];
        [encoder setFragmentTexture:_surfaceTexture atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];

    if (!canDrawSurface) {
        return;
    }
    if (hasNewFrame) {
        _lastDrawnFrame = _metadata.frame;
    }
    if (renderer_debug_logs_enabled() && _metadata.frame != 0 &&
        _metadata.frame != _lastLoggedSubmitFrame) {
        NSLog(@"MacMu renderer submitted frame %llu from IOSurface %u.",
              static_cast<unsigned long long>(_metadata.frame), _metadata.iosurfaceId);
        _lastLoggedSubmitFrame = _metadata.frame;
    }
}

@end

MacMuSurfaceRendererRef macmu_surface_renderer_create(MTKView* view,
                                                      FrameConsumer* frame_consumer,
                                                      uint32_t display_id) {
    return [[MacMuSurfaceRenderer alloc] initWithView:view
                                        frameConsumer:frame_consumer
                                            displayId:display_id];
}

NSSize macmu_fitted_window_content_size(uint32_t pixel_width, uint32_t pixel_height) {
    return fitted_window_content_size(pixel_width, pixel_height);
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
