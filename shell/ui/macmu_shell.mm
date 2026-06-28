// MacMu macOS shell for IOSurface display export.
// SPDX-License-Identifier: MIT
//
// Build:
//   cmake -S shell -B build/shell
//   cmake --build build/shell --target macmu_shell
//
// By default this shell launches qemu-system-aarch64-headless with IOSurface
// export enabled. This file is now just the app shell: NSApplication setup,
// the menu, the window/MTKView, the doorbell thread and the AppDelegate. The
// Metal renderer lives in macmu_surface_renderer.mm; option parsing, the frame
// channel, the metadata fallback reader and the qemu launcher live in the .cpp
// companions.

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <atomic>
#include <thread>

#include "frame_consumer.h"
#include "guest_input_sender.h"
#include "input_sender.h"
#include "macmu_input_view.h"
#include "macmu_surface_renderer.h"
#include "qemu_launcher.h"
#include "shell_options.h"
#include "surface_metadata.h"

@interface MacMuAppDelegate : NSObject <NSApplicationDelegate>
- (instancetype)initWithQemuPid:(pid_t)qemuPid;
@end

@implementation MacMuAppDelegate {
    pid_t _qemuPid;
}

- (instancetype)initWithQemuPid:(pid_t)qemuPid {
    self = [super init];
    if (!self) {
        return nil;
    }
    _qemuPid = qemuPid;
    return self;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    terminate_qemu(_qemuPid);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

@end

static MacMuAppDelegate* gAppDelegate;
static FrameConsumer* gFrameConsumer;
static InputSender* gInputSender;
static GuestInputSender* gGuestInputSender;
static std::atomic<bool> gShutdown{false};
static std::thread gDoorbellThread;

int main(int argc, char** argv) {
    @autoreleasepool {
        ShellOptions options = parse_options(argc, argv);

        // The frame channel consumer must be created BEFORE qemu is launched,
        // so the producer (gfxstream) can discover the shm object + Mach
        // receive right on its first publish.
        gFrameConsumer = new FrameConsumer();
        const bool channelReady = gFrameConsumer->create(static_cast<uint32_t>(getpid()));
        if (channelReady) {
            NSLog(@"MacMu frame channel ready (pid=%u).", static_cast<unsigned>(getpid()));
        } else {
            NSLog(@"MacMu frame channel unavailable; no frames will be displayed.");
        }

        gInputSender = new InputSender();
        if (gInputSender->open(options.inputSocketPath)) {
            NSLog(@"MacMu input channel ready at %s.", options.inputSocketPath.c_str());
        } else {
            NSLog(@"MacMu input channel unavailable; pointer input will be disabled.");
        }

        const pid_t qemuPid = options.launchQemu ? launch_qemu(options) : -1;
        if (options.launchQemu && qemuPid <= 0) {
            return 1;
        }
        gGuestInputSender = new GuestInputSender();
        gGuestInputSender->start(options);

        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        gAppDelegate = [[MacMuAppDelegate alloc] initWithQemuPid:qemuPid];
        [app setDelegate:gAppDelegate];

        NSMenu* menu = [[NSMenu alloc] initWithTitle:@"MacMu"];
        NSMenuItem* appItem = [[NSMenuItem alloc] init];
        [menu addItem:appItem];
        NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"MacMu"];
        NSString* quitTitle = @"Quit MacMu";
        [appMenu addItemWithTitle:quitTitle action:@selector(terminate:) keyEquivalent:@"q"];
        [appItem setSubmenu:appMenu];
        [app setMainMenu:menu];

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            NSLog(@"Metal is not available.");
            return 1;
        }

        NSRect frame = NSMakeRect(0, 0, 420, 720);
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [window center];

        MTKView* view = macmu_input_view_create(frame, device, gInputSender, gGuestInputSender);
        view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        view.clearColor = MTLClearColorMake(0.03, 0.03, 0.035, 1.0);
        view.preferredFramesPerSecond = 60;
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        MacMuSurfaceRendererRef renderer = macmu_surface_renderer_create(view, gFrameConsumer);
        macmu_input_view_set_renderer(view, renderer);
        // MacMuSurfaceRendererRef resolves to MacMuSurfaceRenderer* under Obj-C;
        // it conforms to MTKViewDelegate, so a plain assignment is all ARC needs.
        view.delegate = renderer;
        window.contentView = view;
        [window makeFirstResponder:view];
        [window makeKeyAndOrderFront:nil];

        const bool useChannel = channelReady && gFrameConsumer->valid();
        if (useChannel) {
            // Event-driven rendering: the view only redraws when a new guest
            // frame arrives, instead of spinning a 60 Hz timer. A background
            // thread blocks on the Mach doorbell and kicks the main thread.
            view.paused = YES;
            view.enableSetNeedsDisplay = YES;
            gDoorbellThread = std::thread([view]() {
                uint64_t lastFrame = 0;
                while (!gShutdown.load(std::memory_order_relaxed)) {
                    SurfaceMetadata meta = {};
                    if (gFrameConsumer->wait_for_frame(lastFrame, 1000, &meta)) {
                        lastFrame = meta.frame;
                        // Hop to the main thread to trigger a display cycle.
                        dispatch_async(dispatch_get_main_queue(), ^{
                            [view setNeedsDisplay:YES];
                        });
                    }
                }
            });
        } else {
            // Legacy fallback: drive the draw loop at a fixed cadence.
            view.paused = NO;
            view.enableSetNeedsDisplay = NO;
        }

        [app activateIgnoringOtherApps:YES];
        [app run];

        // Signal the doorbell thread to exit, then join it BEFORE destroying
        // gFrameConsumer. The thread's wait_for_frame() has a bounded 1s timeout
        // so the join is guaranteed to return; joining first avoids a
        // use-after-free where the detached thread would touch gFrameConsumer
        // after we delete it below.
        gShutdown.store(true, std::memory_order_relaxed);
        if (gDoorbellThread.joinable()) {
            gDoorbellThread.join();
        }
    }
    if (gFrameConsumer) {
        delete gFrameConsumer;
        gFrameConsumer = nullptr;
    }
    if (gInputSender) {
        delete gInputSender;
        gInputSender = nullptr;
    }
    if (gGuestInputSender) {
        delete gGuestInputSender;
        gGuestInputSender = nullptr;
    }
    return 0;
}
