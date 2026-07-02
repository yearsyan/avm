// MacMu macOS shell for IOSurface display export.
// SPDX-License-Identifier: MIT
//
// The app shell owns the AppKit lifecycle, the status window, the optional
// IOSurface display window, the menu-bar status item, and the qemu supervisor.

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "frame_consumer.h"
#include "guest_input_sender.h"
#include "input_sender.h"
#include "machine_manager.h"
#include "macmu_input_view.h"
#include "macmu_surface_renderer.h"
#include "qemu_launcher.h"
#include "shell_options.h"
#include "surface_metadata.h"

namespace {

NSString* ns_string(const std::string& value) {
    return [NSString stringWithUTF8String:value.c_str()];
}

std::string path_join(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs.back() == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

NSTextField* make_label(NSString* text, NSRect frame) {
    NSTextField* label = [[NSTextField alloc] initWithFrame:frame];
    label.stringValue = text;
    label.bezeled = NO;
    label.drawsBackground = NO;
    label.editable = NO;
    label.selectable = NO;
    label.textColor = [NSColor secondaryLabelColor];
    label.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightSemibold];
    return label;
}

NSTextField* make_value(NSString* text, NSRect frame) {
    NSTextField* value = [[NSTextField alloc] initWithFrame:frame];
    value.stringValue = text;
    value.bezeled = NO;
    value.drawsBackground = NO;
    value.editable = NO;
    value.selectable = YES;
    value.lineBreakMode = NSLineBreakByTruncatingMiddle;
    value.font = [NSFont monospacedSystemFontOfSize:12.0 weight:NSFontWeightRegular];
    return value;
}

}  // namespace

@interface MacMuAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (instancetype)initWithOptions:(const ShellOptions&)options;
@end

@implementation MacMuAppDelegate {
    ShellOptions _options;

    FrameConsumer* _frameConsumer;
    InputSender* _inputSender;
    GuestInputSender* _guestInputSender;
    id<MTLDevice> _metalDevice;

    NSWindow* _statusWindow;
    NSTextField* _qemuStatusValue;
    NSTextField* _appDataPathValue;
    NSTextField* _avdPathValue;
    NSTextField* _systemPathValue;
    NSButton* _createMachineButton;
    NSButton* _startButton;

    NSWindow* _displayWindow;
    MTKView* _displayView;
    MacMuSurfaceRendererRef _displayRenderer;

    NSStatusItem* _statusItem;

    std::atomic<bool> _shuttingDown;
    std::atomic<bool> _runtimeShutdownComplete;
    std::atomic<bool> _doorbellShutdown;
    std::atomic<uint64_t> _qemuGeneration;
    std::thread _qemuMonitorThread;
    std::thread _doorbellThread;
    std::mutex _qemuMutex;
    std::mutex _guestInputMutex;
    pid_t _qemuPid;
    bool _channelReady;
}

- (instancetype)initWithOptions:(const ShellOptions&)options {
    self = [super init];
    if (!self) {
        return nil;
    }
    _options = options;
    _frameConsumer = nullptr;
    _inputSender = nullptr;
    _guestInputSender = nullptr;
    _metalDevice = nil;
    _statusWindow = nil;
    _qemuStatusValue = nil;
    _appDataPathValue = nil;
    _avdPathValue = nil;
    _systemPathValue = nil;
    _createMachineButton = nil;
    _startButton = nil;
    _displayWindow = nil;
    _displayView = nil;
    _displayRenderer = nil;
    _statusItem = nil;
    _shuttingDown.store(false, std::memory_order_relaxed);
    _runtimeShutdownComplete.store(false, std::memory_order_relaxed);
    _doorbellShutdown.store(true, std::memory_order_relaxed);
    _qemuGeneration.store(0, std::memory_order_relaxed);
    _qemuPid = -1;
    _channelReady = false;
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [self installMainMenu];
    [self installStatusItem];
    std::string directoryError;
    if (!macmu_ensure_runtime_directories(_options, &directoryError)) {
        NSLog(@"MacMu data directory setup failed: %s", directoryError.c_str());
    }
    [self createRuntimeChannels];
    [self createStatusWindow];
    [self updateMachineControls];
    [self showStatusWindow:nil];
    [self startQemuSupervisor];
    if (_options.openDisplay) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self openDisplayWindow:nil];
        });
    }
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    if (!_runtimeShutdownComplete.load(std::memory_order_acquire)) {
        [self shutdownRuntime];
    }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    if (_runtimeShutdownComplete.load(std::memory_order_acquire)) {
        return NSTerminateNow;
    }
    [self beginAsyncTermination];
    return NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return NO;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)flag {
    [self showStatusWindow:nil];
    return YES;
}

- (void)windowWillClose:(NSNotification*)notification {
    if ([notification object] == _displayWindow) {
        [self stopDoorbellThread];
        _displayWindow = nil;
        _displayView = nil;
        _displayRenderer = nil;
    }
}

- (void)installMainMenu {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"MacMu"];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [menu addItem:appItem];

    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"MacMu"];
    NSMenuItem* showItem = [appMenu addItemWithTitle:@"Show MacMu"
                                              action:@selector(showStatusWindow:)
                                       keyEquivalent:@"0"];
    showItem.target = self;
    NSMenuItem* displayItem = [appMenu addItemWithTitle:@"Open Display"
                                                 action:@selector(openDisplayWindow:)
                                          keyEquivalent:@"1"];
    displayItem.target = self;
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* quitItem = [appMenu addItemWithTitle:@"Quit MacMu"
                                              action:@selector(terminate:)
                                       keyEquivalent:@"q"];
    quitItem.target = NSApp;
    [appItem setSubmenu:appMenu];

    [NSApp setMainMenu:menu];
}

- (void)installStatusItem {
    _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    _statusItem.button.title = @"MacMu";
    _statusItem.button.toolTip = @"MacMu";

    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"MacMu"];
    NSMenuItem* showItem = [[NSMenuItem alloc] initWithTitle:@"Show MacMu"
                                                      action:@selector(showStatusWindow:)
                                               keyEquivalent:@""];
    showItem.target = self;
    [menu addItem:showItem];

    NSMenuItem* displayItem = [[NSMenuItem alloc] initWithTitle:@"Open Display"
                                                         action:@selector(openDisplayWindow:)
                                                  keyEquivalent:@""];
    displayItem.target = self;
    [menu addItem:displayItem];
    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit MacMu"
                                                      action:@selector(quitFromStatusItem:)
                                               keyEquivalent:@""];
    quitItem.target = self;
    [menu addItem:quitItem];
    _statusItem.menu = menu;
}

- (void)createRuntimeChannels {
    _frameConsumer = new FrameConsumer();
    _channelReady = _frameConsumer->create(static_cast<uint32_t>(getpid()));
    if (_channelReady) {
        NSLog(@"MacMu frame channel ready (pid=%u).", static_cast<unsigned>(getpid()));
    } else {
        NSLog(@"MacMu frame channel unavailable; no frames will be displayed.");
    }

    _inputSender = new InputSender();
    if (_inputSender->create()) {
        NSLog(@"MacMu input channel ready (socketpair fd, remote=%d).",
              _inputSender->remote_fd());
    } else {
        NSLog(@"MacMu input channel unavailable; pointer input will be disabled.");
    }

    _guestInputSender = new GuestInputSender();
    if (_guestInputSender->start(_options.guestRpcSocketPath)) {
        NSLog(@"MacMu RPC agent listener ready at %s.", _options.guestRpcSocketPath.c_str());
    } else {
        NSLog(@"MacMu RPC agent listener unavailable; guest RPC will be disabled.");
    }
    _metalDevice = MTLCreateSystemDefaultDevice();
    if (!_metalDevice) {
        NSLog(@"Metal is not available; display window will be disabled.");
    }
}

- (void)createStatusWindow {
    if (_statusWindow) {
        return;
    }

    const NSRect frame = NSMakeRect(0, 0, 680, 360);
    _statusWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _statusWindow.title = @"MacMu";
    _statusWindow.releasedWhenClosed = NO;
    _statusWindow.delegate = self;

    NSView* content = [[NSView alloc] initWithFrame:frame];
    _statusWindow.contentView = content;

    NSTextField* title = make_label(@"MacMu", NSMakeRect(28, 306, 360, 26));
    title.font = [NSFont systemFontOfSize:22.0 weight:NSFontWeightSemibold];
    title.textColor = [NSColor labelColor];
    [content addSubview:title];

    NSTextField* subtitle =
        make_label(@"Android emulator core status", NSMakeRect(30, 282, 360, 18));
    subtitle.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightRegular];
    [content addSubview:subtitle];

    [content addSubview:make_label(@"QEMU", NSMakeRect(30, 236, 130, 20))];
    _qemuStatusValue = make_value(@"Starting", NSMakeRect(170, 236, 470, 20));
    [content addSubview:_qemuStatusValue];

    [content addSubview:make_label(@"Data Root", NSMakeRect(30, 196, 130, 20))];
    _appDataPathValue = make_value(ns_string(_options.appDataDir), NSMakeRect(170, 196, 470, 20));
    [content addSubview:_appDataPathValue];

    [content addSubview:make_label(@"Machine", NSMakeRect(30, 156, 130, 20))];
    _avdPathValue = make_value(ns_string(macmu_machine_path(_options)),
                               NSMakeRect(170, 156, 470, 20));
    [content addSubview:_avdPathValue];

    [content addSubview:make_label(@"System Image", NSMakeRect(30, 116, 130, 20))];
    _systemPathValue = make_value(ns_string(_options.systemPath), NSMakeRect(170, 116, 470, 20));
    [content addSubview:_systemPathValue];

    _createMachineButton = [NSButton buttonWithTitle:@"Create Machine"
                                              target:self
                                              action:@selector(createMachine:)];
    _createMachineButton.frame = NSMakeRect(364, 28, 136, 34);
    _createMachineButton.bezelStyle = NSBezelStyleRounded;
    [content addSubview:_createMachineButton];

    _startButton = [NSButton buttonWithTitle:@"Display"
                                      target:self
                                      action:@selector(openDisplayWindow:)];
    _startButton.frame = NSMakeRect(516, 28, 112, 34);
    _startButton.bezelStyle = NSBezelStyleRounded;
    _startButton.enabled = _channelReady && _metalDevice != nil;
    [content addSubview:_startButton];

    NSButton* quitButton = [NSButton buttonWithTitle:@"Quit"
                                              target:NSApp
                                              action:@selector(terminate:)];
    quitButton.frame = NSMakeRect(256, 28, 92, 34);
    quitButton.bezelStyle = NSBezelStyleRounded;
    [content addSubview:quitButton];

    [_statusWindow center];
}

- (void)showStatusWindow:(id)sender {
    [self createStatusWindow];
    [_statusWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)openDisplayWindow:(id)sender {
    if (!_channelReady || !_frameConsumer || !_frameConsumer->valid() || !_metalDevice) {
        NSBeep();
        return;
    }
    if (_displayWindow) {
        [_displayWindow makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        return;
    }

    NSRect frame = NSMakeRect(0, 0, 420, 720);
    _displayWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _displayWindow.title = @"MacMu Display";
    _displayWindow.releasedWhenClosed = NO;
    _displayWindow.delegate = self;
    [_displayWindow center];

    _displayView = macmu_input_view_create(frame, _metalDevice, _inputSender, _guestInputSender);
    _displayView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    _displayView.clearColor = MTLClearColorMake(0.03, 0.03, 0.035, 1.0);
    _displayView.preferredFramesPerSecond = 60;
    _displayView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    _displayRenderer = macmu_surface_renderer_create(_displayView, _frameConsumer);
    macmu_input_view_set_renderer(_displayView, _displayRenderer);
    _displayView.delegate = _displayRenderer;
    _displayWindow.contentView = _displayView;
    [_displayWindow makeFirstResponder:_displayView];
    [_displayWindow makeKeyAndOrderFront:nil];

    if (_frameConsumer->valid()) {
        _displayView.paused = YES;
        _displayView.enableSetNeedsDisplay = YES;
        [self startDoorbellThreadForView:_displayView];
        [_displayView setNeedsDisplay:YES];
    } else {
        _displayView.paused = NO;
        _displayView.enableSetNeedsDisplay = NO;
    }
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)quitFromStatusItem:(id)sender {
    [NSApp terminate:nil];
}

- (void)updateMachineControls {
    const bool hasSystemImage = macmu_system_image_exists(_options);
    const bool hasMachine = macmu_machine_exists(_options);
    if (_createMachineButton) {
        _createMachineButton.enabled = hasSystemImage && !hasMachine;
        _createMachineButton.title = hasMachine ? @"Machine Ready" : @"Create Machine";
    }
    if (_appDataPathValue) {
        _appDataPathValue.stringValue = ns_string(_options.appDataDir);
    }
    if (_avdPathValue) {
        _avdPathValue.stringValue = ns_string(macmu_machine_path(_options));
    }
    if (_systemPathValue) {
        _systemPathValue.stringValue = ns_string(_options.systemPath);
    }
}

- (void)createMachine:(id)sender {
    std::string error;
    if (macmu_create_default_machine(_options, &error)) {
        [self updateMachineControls];
        [self publishQemuStatus:@"Machine ready"];
        return;
    }
    [self publishQemuStatus:ns_string(error)];
    NSLog(@"MacMu machine creation failed: %s", error.c_str());
    [self updateMachineControls];
}

- (void)setQemuStatusText:(NSString*)text {
    _qemuStatusValue.stringValue = text;
    if (_statusItem.button) {
        _statusItem.button.title = [text hasPrefix:@"Running"] ? @"MacMu: Running" : @"MacMu";
    }
}

- (void)publishQemuStatus:(NSString*)text {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self setQemuStatusText:text];
    });
}

- (void)startQemuSupervisor {
    MacMuAppDelegate* delegate = self;
    _qemuMonitorThread = std::thread([delegate] { [delegate qemuMonitorLoop]; });
}

- (void)qemuMonitorLoop {
    while (!_shuttingDown.load(std::memory_order_acquire)) {
        if (!macmu_system_image_exists(_options)) {
            [self publishQemuStatus:@"System image missing"];
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        if (!macmu_machine_exists(_options)) {
            [self publishQemuStatus:@"Machine missing"];
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        [self publishQemuStatus:@"Starting"];
        const int doorbellFd =
            (_channelReady && _frameConsumer) ? _frameConsumer->producer_doorbell_fd() : -1;
        const int inputFd = (_inputSender && _inputSender->valid()) ? _inputSender->remote_fd() : -1;
        const pid_t pid = launch_qemu(_options, doorbellFd, inputFd);
        if (pid <= 0) {
            [self publishQemuStatus:@"Launch failed; retrying"];
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(_qemuMutex);
            _qemuPid = pid;
        }
        _qemuGeneration.fetch_add(1, std::memory_order_acq_rel);
        if (_shuttingDown.load(std::memory_order_acquire)) {
            terminate_qemu(pid);
        } else {
            [self publishQemuStatus:[NSString stringWithFormat:@"Running (pid %d)", pid]];
        }

        int status = 0;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        {
            std::lock_guard<std::mutex> lock(_qemuMutex);
            if (_qemuPid == pid) {
                _qemuPid = -1;
            }
        }

        if (_shuttingDown.load(std::memory_order_acquire)) {
            break;
        }
        [self publishQemuStatus:@"Exited; restarting"];
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

- (void)stopGuestInputSender {
    std::lock_guard<std::mutex> lock(_guestInputMutex);
    if (_guestInputSender) {
        _guestInputSender->stop();
    }
}

- (pid_t)currentQemuPid {
    std::lock_guard<std::mutex> lock(_qemuMutex);
    return _qemuPid;
}

- (void)startDoorbellThreadForView:(MTKView*)view {
    [self stopDoorbellThread];
    _doorbellShutdown.store(false, std::memory_order_release);
    MacMuAppDelegate* delegate = self;
    _doorbellThread = std::thread([delegate, view]() {
        uint64_t lastFrame = 0;
        uint64_t seenGeneration = delegate->_qemuGeneration.load(std::memory_order_acquire);
        while (!delegate->_doorbellShutdown.load(std::memory_order_acquire)) {
            const uint64_t currentGeneration =
                delegate->_qemuGeneration.load(std::memory_order_acquire);
            if (currentGeneration != seenGeneration) {
                seenGeneration = currentGeneration;
                lastFrame = 0;
            }

            SurfaceMetadata meta = {};
            if (delegate->_frameConsumer &&
                delegate->_frameConsumer->wait_for_frame(lastFrame, 100, &meta)) {
                lastFrame = meta.frame;
                dispatch_async(dispatch_get_main_queue(), ^{
                    [view setNeedsDisplay:YES];
                });
            }
        }
    });
}

- (void)stopDoorbellThread {
    _doorbellShutdown.store(true, std::memory_order_release);
    if (_doorbellThread.joinable()) {
        _doorbellThread.join();
    }
}

- (void)hideWindowsForTermination {
    if (_displayView) {
        _displayView.paused = YES;
        _displayView.delegate = nil;
        macmu_input_view_set_renderer(_displayView, nil);
    }
    [_displayWindow orderOut:nil];
    [_statusWindow orderOut:nil];
}

- (void)beginAsyncTermination {
    if (_shuttingDown.exchange(true, std::memory_order_acq_rel)) {
        [self hideWindowsForTermination];
        return;
    }

    [self hideWindowsForTermination];
    [self publishQemuStatus:@"Stopping"];

    MacMuAppDelegate* delegate = self;
    std::thread([delegate] {
        [delegate performRuntimeShutdown];
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
        });
    }).detach();
}

- (void)shutdownRuntime {
    if (_shuttingDown.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    [self performRuntimeShutdown];
}

- (void)performRuntimeShutdown {
    [self stopDoorbellThread];
    [self stopGuestInputSender];

    const pid_t pid = [self currentQemuPid];
    if (pid > 0) {
        terminate_qemu(pid);
    }
    if (_qemuMonitorThread.joinable()) {
        _qemuMonitorThread.join();
    }

    if (_frameConsumer) {
        delete _frameConsumer;
        _frameConsumer = nullptr;
    }
    if (_inputSender) {
        delete _inputSender;
        _inputSender = nullptr;
    }
    if (_guestInputSender) {
        delete _guestInputSender;
        _guestInputSender = nullptr;
    }
    _runtimeShutdownComplete.store(true, std::memory_order_release);
}

@end

int main(int argc, char** argv) {
    @autoreleasepool {
        ShellOptions options = parse_options(argc, argv);
        NSApplication* app = [NSApplication sharedApplication];
        MacMuAppDelegate* delegate = [[MacMuAppDelegate alloc] initWithOptions:options];
        [app setDelegate:delegate];
        [app run];
    }
    return 0;
}
