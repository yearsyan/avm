// MacMu macOS shell for IOSurface display export.
// SPDX-License-Identifier: MIT
//
// The app shell owns the AppKit lifecycle, the status window, the per-display
// IOSurface windows, the apps window, the menu-bar status item, and the qemu
// supervisor. One window == one guest display: display 0 is the built-in
// panel; secondary displays are created over the MMCP control channel and
// their frames arrive through the display-indexed frame channel slots.

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "control_channel.h"
#include "frame_consumer.h"
#include "guest_control_client.h"
#include "guest_input_sender.h"
#include "input_sender.h"
#include "machine_manager.h"
#include "macmu_control_protocol.h"
#include "macmu_input_view.h"
#include "macmu_surface_renderer.h"
#include "qemu_launcher.h"
#include "shell_options.h"
#include "surface_metadata.h"

namespace {

NSString* ns_string(const std::string& value) {
    return [NSString stringWithUTF8String:value.c_str()];
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

// Defaults for user-created secondary displays.
constexpr uint32_t kNewDisplayWidth = 1280;
constexpr uint32_t kNewDisplayHeight = 720;
constexpr uint32_t kNewDisplayDpi = 240;

struct DisplayWindow {
    NSWindow* __strong window = nil;
    MTKView* __strong view = nil;
    MacMuSurfaceRendererRef __strong renderer = nil;
};

}  // namespace

@interface MacMuAppDelegate
    : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTableViewDataSource,
                NSTableViewDelegate>
- (instancetype)initWithOptions:(const ShellOptions&)options;
@end

@implementation MacMuAppDelegate {
    ShellOptions _options;

    FrameConsumer* _frameConsumer;
    InputSender* _inputSender;
    GuestInputSender* _guestInputSender;
    GuestControlClient* _guestControlClient;
    id<MTLDevice> _metalDevice;

    NSWindow* _statusWindow;
    NSTextField* _qemuStatusValue;
    NSTextField* _appDataPathValue;
    NSTextField* _avdPathValue;
    NSTextField* _systemPathValue;
    NSButton* _createMachineButton;
    NSButton* _startButton;

    // displayId -> window/view/renderer. Main thread only.
    std::map<uint32_t, DisplayWindow> _displayWindows;
    // Display ids being closed because the guest/event said so; suppresses the
    // windowWillClose -> DISPLAY_REMOVE echo.
    std::map<uint32_t, bool> _suppressRemoveOnClose;

    NSWindow* _appsWindow;
    NSTableView* _appsTable;
    NSTextField* _appsStatusValue;
    NSMutableArray<NSDictionary*>* _apps;

    NSStatusItem* _statusItem;

    std::shared_ptr<ControlChannel> _controlChannel;  // guarded by _controlMutex
    std::mutex _controlMutex;

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
    _guestControlClient = nullptr;
    _metalDevice = nil;
    _statusWindow = nil;
    _qemuStatusValue = nil;
    _appDataPathValue = nil;
    _avdPathValue = nil;
    _systemPathValue = nil;
    _createMachineButton = nil;
    _startButton = nil;
    _appsWindow = nil;
    _appsTable = nil;
    _appsStatusValue = nil;
    _apps = [[NSMutableArray alloc] init];
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
            [self openDisplayWindowForDisplay:0];
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
    NSWindow* window = [notification object];
    if (window == _appsWindow) {
        return;
    }
    for (auto it = _displayWindows.begin(); it != _displayWindows.end(); ++it) {
        if (it->second.window != window) {
            continue;
        }
        const uint32_t displayId = it->first;
        [self teardownDisplayWindowEntry:it->second];
        _displayWindows.erase(it);

        const auto suppress = _suppressRemoveOnClose.find(displayId);
        const bool suppressed = suppress != _suppressRemoveOnClose.end();
        if (suppressed) {
            _suppressRemoveOnClose.erase(suppress);
        }
        // Closing a secondary display's window removes the guest display too
        // (which also stops its export streaming). Display 0 is the built-in
        // panel: its window closes, the display stays.
        if (displayId != 0) {
            [self setDisplayStreaming:displayId enabled:NO];
            if (!suppressed) {
                [self requestDisplayRemove:displayId];
            }
        }
        break;
    }
}

- (void)teardownDisplayWindowEntry:(DisplayWindow&)entry {
    if (entry.view) {
        entry.view.paused = YES;
        entry.view.delegate = nil;
        macmu_input_view_set_renderer(entry.view, nil);
    }
    entry.renderer = nil;
    entry.view = nil;
    entry.window = nil;
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
                                                 action:@selector(openPrimaryDisplayWindow:)
                                          keyEquivalent:@"1"];
    displayItem.target = self;
    NSMenuItem* newDisplayItem = [appMenu addItemWithTitle:@"New Display"
                                                    action:@selector(newDisplay:)
                                             keyEquivalent:@"n"];
    newDisplayItem.target = self;
    NSMenuItem* appsItem = [appMenu addItemWithTitle:@"Apps…"
                                              action:@selector(showAppsWindow:)
                                       keyEquivalent:@"l"];
    appsItem.target = self;
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
                                                         action:@selector(openPrimaryDisplayWindow:)
                                                  keyEquivalent:@""];
    displayItem.target = self;
    [menu addItem:displayItem];

    NSMenuItem* newDisplayItem = [[NSMenuItem alloc] initWithTitle:@"New Display"
                                                            action:@selector(newDisplay:)
                                                     keyEquivalent:@""];
    newDisplayItem.target = self;
    [menu addItem:newDisplayItem];

    NSMenuItem* appsItem = [[NSMenuItem alloc] initWithTitle:@"Apps…"
                                                      action:@selector(showAppsWindow:)
                                               keyEquivalent:@""];
    appsItem.target = self;
    [menu addItem:appsItem];
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
        NSLog(@"MacMu frame channel ready (pid=%u, %u display slots).",
              static_cast<unsigned>(getpid()), kMacmuFrameSlotCount);
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
    if (_guestInputSender->start(_options.guestRpcSocketPath, _options.appDataDir)) {
        NSLog(@"MacMu RPC agent listener ready at %s.", _options.guestRpcSocketPath.c_str());
    } else {
        NSLog(@"MacMu RPC agent listener unavailable; guest RPC will be disabled.");
    }

    _guestControlClient = new GuestControlClient();
    if (_guestControlClient->start(_options.guestCtrlSocketPath)) {
        NSLog(@"MacMu guest control listener ready at %s.", _options.guestCtrlSocketPath.c_str());
    } else {
        NSLog(@"MacMu guest control listener unavailable; app management disabled.");
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

    const NSRect frame = NSMakeRect(0, 0, 760, 620);
    _statusWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _statusWindow.title = @"MacMu";
    _statusWindow.releasedWhenClosed = NO;
    _statusWindow.delegate = self;
    _statusWindow.minSize = NSMakeSize(680, 500);

    NSView* content = [[NSView alloc] initWithFrame:frame];
    _statusWindow.contentView = content;

    NSTextField* title = make_label(@"MacMu", NSMakeRect(28, 566, 360, 26));
    title.font = [NSFont systemFontOfSize:22.0 weight:NSFontWeightSemibold];
    title.textColor = [NSColor labelColor];
    [content addSubview:title];

    NSTextField* subtitle =
        make_label(@"Android emulator core status", NSMakeRect(30, 542, 360, 18));
    subtitle.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightRegular];
    [content addSubview:subtitle];

    [content addSubview:make_label(@"QEMU", NSMakeRect(30, 496, 130, 20))];
    _qemuStatusValue = make_value(@"Starting", NSMakeRect(170, 496, 550, 20));
    [content addSubview:_qemuStatusValue];

    [content addSubview:make_label(@"Data Root", NSMakeRect(30, 456, 130, 20))];
    _appDataPathValue = make_value(ns_string(_options.appDataDir), NSMakeRect(170, 456, 550, 20));
    [content addSubview:_appDataPathValue];

    [content addSubview:make_label(@"Machine", NSMakeRect(30, 416, 130, 20))];
    _avdPathValue = make_value(ns_string(macmu_machine_path(_options)),
                               NSMakeRect(170, 416, 550, 20));
    [content addSubview:_avdPathValue];

    [content addSubview:make_label(@"System Image", NSMakeRect(30, 376, 130, 20))];
    _systemPathValue = make_value(ns_string(_options.systemPath), NSMakeRect(170, 376, 550, 20));
    [content addSubview:_systemPathValue];

    [content addSubview:make_label(@"Apps", NSMakeRect(30, 328, 130, 20))];

    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(30, 78, 700, 240)];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;

    _appsTable = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn* packageColumn = [[NSTableColumn alloc] initWithIdentifier:@"package"];
    packageColumn.title = @"Package";
    packageColumn.width = 320;
    [_appsTable addTableColumn:packageColumn];
    NSTableColumn* activityColumn = [[NSTableColumn alloc] initWithIdentifier:@"activity"];
    activityColumn.title = @"Launcher Activity";
    activityColumn.width = 360;
    [_appsTable addTableColumn:activityColumn];
    _appsTable.dataSource = self;
    _appsTable.delegate = self;
    _appsTable.usesAlternatingRowBackgroundColors = YES;
    _appsTable.doubleAction = @selector(launchAppOnNewDisplay:);
    _appsTable.target = self;
    scroll.documentView = _appsTable;
    [content addSubview:scroll];

    NSButton* refresh = [NSButton buttonWithTitle:@"Refresh Apps"
                                           target:self
                                           action:@selector(refreshApps:)];
    refresh.frame = NSMakeRect(30, 28, 118, 34);
    refresh.bezelStyle = NSBezelStyleRounded;
    refresh.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
    [content addSubview:refresh];

    _appsStatusValue = make_value(@"", NSMakeRect(160, 35, 172, 20));
    _appsStatusValue.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
    [content addSubview:_appsStatusValue];

    _createMachineButton = [NSButton buttonWithTitle:@"Create Machine"
                                              target:self
                                              action:@selector(createMachine:)];
    _createMachineButton.frame = NSMakeRect(350, 28, 136, 34);
    _createMachineButton.bezelStyle = NSBezelStyleRounded;
    _createMachineButton.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    [content addSubview:_createMachineButton];

    _startButton = [NSButton buttonWithTitle:@"Display"
                                      target:self
                                      action:@selector(openPrimaryDisplayWindow:)];
    _startButton.frame = NSMakeRect(500, 28, 92, 34);
    _startButton.bezelStyle = NSBezelStyleRounded;
    _startButton.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    _startButton.enabled = _channelReady && _metalDevice != nil;
    [content addSubview:_startButton];

    NSButton* launchHere = [NSButton buttonWithTitle:@"Launch"
                                              target:self
                                              action:@selector(launchAppOnPrimary:)];
    launchHere.frame = NSMakeRect(606, 28, 92, 34);
    launchHere.bezelStyle = NSBezelStyleRounded;
    launchHere.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    [content addSubview:launchHere];

    NSButton* launchNew = [NSButton buttonWithTitle:@"Launch in New Display"
                                             target:self
                                             action:@selector(launchAppOnNewDisplay:)];
    launchNew.frame = NSMakeRect(500, 328, 198, 32);
    launchNew.bezelStyle = NSBezelStyleRounded;
    launchNew.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    [content addSubview:launchNew];

    [_statusWindow center];
}

- (void)showStatusWindow:(id)sender {
    [self createStatusWindow];
    [_statusWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)openPrimaryDisplayWindow:(id)sender {
    [self openDisplayWindowForDisplay:0];
}

// Idempotent: brings the display's window to front, creating it if needed.
// Main thread only.
- (void)openDisplayWindowForDisplay:(uint32_t)displayId {
    if (!_channelReady || !_frameConsumer || !_frameConsumer->valid() || !_metalDevice) {
        NSBeep();
        return;
    }
    auto existing = _displayWindows.find(displayId);
    if (existing != _displayWindows.end()) {
        [existing->second.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        return;
    }

    NSRect frame = displayId == 0 ? NSMakeRect(0, 0, 420, 720) : NSMakeRect(0, 0, 640, 360);
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.title = displayId == 0
                       ? @"MacMu Display"
                       : [NSString stringWithFormat:@"MacMu Display %u", displayId];
    window.releasedWhenClosed = NO;
    window.delegate = self;
    [window center];

    MTKView* view =
        macmu_input_view_create(frame, _metalDevice, _inputSender, _guestInputSender);
    view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    view.clearColor = MTLClearColorMake(0.03, 0.03, 0.035, 1.0);
    view.preferredFramesPerSecond = 60;
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    MacMuSurfaceRendererRef renderer =
        macmu_surface_renderer_create(view, _frameConsumer, displayId);
    macmu_input_view_set_renderer(view, renderer);
    view.delegate = renderer;
    window.contentView = view;
    [window makeFirstResponder:view];
    [window makeKeyAndOrderFront:nil];

    view.paused = YES;
    view.enableSetNeedsDisplay = YES;
    [view setNeedsDisplay:YES];

    DisplayWindow entry;
    entry.window = window;
    entry.view = view;
    entry.renderer = renderer;
    _displayWindows[displayId] = entry;

    // VirtualDisplay-backed secondary displays have no per-frame host signal;
    // ask qemu to pump exports while this window is sampling the display.
    if (displayId != 0) {
        [self setDisplayStreaming:displayId enabled:YES];
    }

    [self startDoorbellThreadIfNeeded];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)setDisplayStreaming:(uint32_t)displayId enabled:(BOOL)enabled {
    auto channel = [self controlChannel];
    if (!channel || !channel->alive()) {
        return;
    }
    macmu::ControlDisplayStream request = {displayId, enabled ? 1u : 0u};
    channel->request(macmu::ControlMessageType::kDisplayStream, &request, sizeof(request), 5000,
                     [displayId](ControlChannel::Response response) {
                         if (!response.ok) {
                             NSLog(@"MacMu display %u streaming toggle failed: %s", displayId,
                                   response.errorMessage.c_str());
                         }
                     });
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

#pragma mark - Control channel

- (std::shared_ptr<ControlChannel>)controlChannel {
    std::lock_guard<std::mutex> lock(_controlMutex);
    return _controlChannel;
}

- (void)handleControlEvent:(uint16_t)type payload:(std::vector<uint8_t>)payload {
    if (type != static_cast<uint16_t>(macmu::ControlMessageType::kEventDisplay) ||
        payload.size() < sizeof(macmu::ControlEventDisplay)) {
        return;
    }
    macmu::ControlEventDisplay event;
    std::memcpy(&event, payload.data(), sizeof(event));
    if (event.state == macmu::kControlDisplayRemoved && event.displayId != 0) {
        auto it = _displayWindows.find(event.displayId);
        if (it != _displayWindows.end()) {
            _suppressRemoveOnClose[event.displayId] = true;
            [it->second.window close];
        }
    }
}

- (void)requestDisplayRemove:(uint32_t)displayId {
    auto channel = [self controlChannel];
    if (!channel || !channel->alive()) {
        return;
    }
    macmu::ControlDisplayRemove request = {displayId};
    channel->request(macmu::ControlMessageType::kDisplayRemove, &request, sizeof(request), 5000,
                     [displayId](ControlChannel::Response response) {
                         if (!response.ok) {
                             NSLog(@"MacMu display %u remove failed: %s", displayId,
                                   response.errorMessage.c_str());
                         }
                     });
}

- (void)newDisplay:(id)sender {
    auto channel = [self controlChannel];
    if (!channel || !channel->alive()) {
        [self publishQemuStatus:@"Control channel not connected"];
        NSBeep();
        return;
    }
    macmu::ControlDisplayAdd request = {};
    request.displayId = macmu::kControlDisplayIdAuto;
    request.width = kNewDisplayWidth;
    request.height = kNewDisplayHeight;
    request.dpi = kNewDisplayDpi;
    request.flags = 0;
    MacMuAppDelegate* delegate = self;
    channel->request(
        macmu::ControlMessageType::kDisplayAdd, &request, sizeof(request), 10000,
        [delegate](ControlChannel::Response response) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!response.ok ||
                    response.payload.size() < sizeof(macmu::ControlDisplayAddOk)) {
                    NSString* message = [NSString
                        stringWithFormat:@"New display failed: %s",
                                         response.errorMessage.empty()
                                             ? "malformed response"
                                             : response.errorMessage.c_str()];
                    [delegate publishQemuStatus:message];
                    NSLog(@"%@", message);
                    return;
                }
                macmu::ControlDisplayAddOk ok;
                std::memcpy(&ok, response.payload.data(), sizeof(ok));
                [delegate openDisplayWindowForDisplay:ok.displayId];
            });
        });
}

#pragma mark - Apps window

- (void)showAppsWindow:(id)sender {
    [self showStatusWindow:sender];
    if (_apps.count == 0) {
        [self refreshApps:nil];
    }
}

- (void)setAppsStatus:(NSString*)status {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_appsStatusValue) {
            self->_appsStatusValue.stringValue = status;
        }
    });
}

- (void)refreshApps:(id)sender {
    if (!_guestControlClient || !_guestControlClient->ready()) {
        [self setAppsStatus:@"Agent not connected"];
        return;
    }
    [self setAppsStatus:@"Loading…"];
    MacMuAppDelegate* delegate = self;
    _guestControlClient->request("apps", 15000, [delegate](bool ok, std::string payload) {
        if (!ok) {
            [delegate setAppsStatus:ns_string("Failed: " + payload)];
            return;
        }
        NSData* data = [NSData dataWithBytes:payload.data() length:payload.size()];
        NSError* error = nil;
        id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
        if (![parsed isKindOfClass:[NSArray class]]) {
            [delegate setAppsStatus:@"Malformed app list"];
            return;
        }
        NSArray* entries = (NSArray*)parsed;
        dispatch_async(dispatch_get_main_queue(), ^{
            [delegate applyAppList:entries];
        });
    });
}

- (void)applyAppList:(NSArray*)entries {
    [_apps removeAllObjects];
    for (id entry in entries) {
        if (![entry isKindOfClass:[NSDictionary class]]) {
            continue;
        }
        NSDictionary* dict = (NSDictionary*)entry;
        if (![dict[@"pkg"] isKindOfClass:[NSString class]] ||
            ![dict[@"activity"] isKindOfClass:[NSString class]]) {
            continue;
        }
        [_apps addObject:dict];
    }
    [_apps sortUsingComparator:^NSComparisonResult(NSDictionary* a, NSDictionary* b) {
        return [a[@"pkg"] compare:b[@"pkg"]];
    }];
    [_appsTable reloadData];
    [self setAppsStatus:[NSString stringWithFormat:@"%lu apps",
                                                   static_cast<unsigned long>(_apps.count)]];
}

- (NSString*)selectedAppComponent {
    const NSInteger row = _appsTable.selectedRow;
    if (row < 0 || row >= static_cast<NSInteger>(_apps.count)) {
        return nil;
    }
    NSDictionary* app = _apps[static_cast<NSUInteger>(row)];
    return [NSString stringWithFormat:@"%@/%@", app[@"pkg"], app[@"activity"]];
}

- (void)launchAppOnPrimary:(id)sender {
    NSString* component = [self selectedAppComponent];
    if (!component) {
        NSBeep();
        return;
    }
    [self launchComponent:component onDisplay:0];
    [self openDisplayWindowForDisplay:0];
}

- (void)launchAppOnNewDisplay:(id)sender {
    NSString* component = [self selectedAppComponent];
    if (!component) {
        NSBeep();
        return;
    }
    auto channel = [self controlChannel];
    if (!channel || !channel->alive()) {
        [self setAppsStatus:@"Control channel not connected"];
        return;
    }
    macmu::ControlDisplayAdd request = {};
    request.displayId = macmu::kControlDisplayIdAuto;
    request.width = kNewDisplayWidth;
    request.height = kNewDisplayHeight;
    request.dpi = kNewDisplayDpi;
    request.flags = 0;
    MacMuAppDelegate* delegate = self;
    channel->request(
        macmu::ControlMessageType::kDisplayAdd, &request, sizeof(request), 10000,
        [delegate, component](ControlChannel::Response response) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!response.ok ||
                    response.payload.size() < sizeof(macmu::ControlDisplayAddOk)) {
                    [delegate setAppsStatus:ns_string("Display add failed: " +
                                                      response.errorMessage)];
                    return;
                }
                macmu::ControlDisplayAddOk ok;
                std::memcpy(&ok, response.payload.data(), sizeof(ok));
                [delegate openDisplayWindowForDisplay:ok.displayId];
                // Give the guest a moment to bring the new display up before
                // targeting it; am start on a not-yet-ready display falls back
                // to display 0.
                dispatch_after(
                    dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(1.5 * NSEC_PER_SEC)),
                    dispatch_get_main_queue(), ^{
                        [delegate launchComponent:component onDisplay:ok.displayId];
                    });
            });
        });
}

- (void)launchComponent:(NSString*)component onDisplay:(uint32_t)displayId {
    if (!_guestControlClient || !_guestControlClient->ready()) {
        [self setAppsStatus:@"Agent not connected"];
        return;
    }
    std::string command = "launch ";
    command += [component UTF8String];
    command += " " + std::to_string(displayId);
    MacMuAppDelegate* delegate = self;
    _guestControlClient->request(command, 10000, [delegate](bool ok, std::string payload) {
        if (!ok) {
            [delegate setAppsStatus:ns_string("Launch failed: " + payload)];
        } else {
            [delegate setAppsStatus:@"Launched"];
        }
    });
}

#pragma mark - Apps table data source

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    return static_cast<NSInteger>(_apps.count);
}

- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)tableColumn
                   row:(NSInteger)row {
    if (row < 0 || row >= static_cast<NSInteger>(_apps.count)) {
        return nil;
    }
    NSString* identifier = tableColumn.identifier;
    NSTextField* text = [tableView makeViewWithIdentifier:identifier owner:self];
    if (!text) {
        text = make_value(@"", NSMakeRect(0, 0, tableColumn.width, 18));
        text.identifier = identifier;
    }
    NSDictionary* app = _apps[static_cast<NSUInteger>(row)];
    text.stringValue = [identifier isEqualToString:@"package"] ? app[@"pkg"] : app[@"activity"];
    return text;
}

#pragma mark - qemu supervisor

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
        if (_channelReady && _frameConsumer) {
            _frameConsumer->reset_slots();
        }
        const int doorbellFd =
            (_channelReady && _frameConsumer) ? _frameConsumer->producer_doorbell_fd() : -1;
        const int inputFd = (_inputSender && _inputSender->valid()) ? _inputSender->remote_fd() : -1;

        auto controlChannel = std::make_shared<ControlChannel>();
        const int controlFd = controlChannel->create() ? controlChannel->remote_fd() : -1;

        const pid_t pid = launch_qemu(_options, doorbellFd, inputFd, controlFd);
        controlChannel->close_remote_fd();
        if (pid <= 0) {
            [self publishQemuStatus:@"Launch failed; retrying"];
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        MacMuAppDelegate* delegate = self;
        controlChannel->start(
            [delegate](uint16_t type, std::vector<uint8_t> payload) {
                // Reader thread -> main queue.
                auto shared = std::make_shared<std::vector<uint8_t>>(std::move(payload));
                dispatch_async(dispatch_get_main_queue(), ^{
                    [delegate handleControlEvent:type payload:std::move(*shared)];
                });
            },
            [] {});
        {
            std::lock_guard<std::mutex> lock(_controlMutex);
            _controlChannel = controlChannel;
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
        {
            std::lock_guard<std::mutex> lock(_controlMutex);
            _controlChannel.reset();
        }
        controlChannel->stop();
        controlChannel.reset();

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

#pragma mark - Frame doorbell

- (void)startDoorbellThreadIfNeeded {
    if (!_doorbellShutdown.load(std::memory_order_acquire)) {
        return;  // already running
    }
    _doorbellShutdown.store(false, std::memory_order_release);
    MacMuAppDelegate* delegate = self;
    _doorbellThread = std::thread([delegate]() { [delegate doorbellLoop]; });
}

- (void)doorbellLoop {
    uint64_t lastFrames[kMacmuFrameSlotCount] = {};
    uint64_t seenGeneration = _qemuGeneration.load(std::memory_order_acquire);
    while (!_doorbellShutdown.load(std::memory_order_acquire)) {
        const uint64_t currentGeneration = _qemuGeneration.load(std::memory_order_acquire);
        if (currentGeneration != seenGeneration) {
            seenGeneration = currentGeneration;
            std::memset(lastFrames, 0, sizeof(lastFrames));
        }

        uint32_t readyDisplayId = 0;
        if (!_frameConsumer ||
            !_frameConsumer->wait_for_any_frame(lastFrames, 100, &readyDisplayId)) {
            continue;
        }
        SurfaceMetadata meta = {};
        if (!_frameConsumer->read(readyDisplayId, &meta)) {
            continue;
        }
        lastFrames[readyDisplayId] = meta.frame;

        MacMuAppDelegate* delegate = self;
        const uint32_t displayId = readyDisplayId;
        dispatch_async(dispatch_get_main_queue(), ^{
            [delegate presentFrameForDisplay:displayId];
        });
    }
}

// Main thread. Marks the display's view dirty; auto-opens a window when a
// secondary display starts producing frames without one (shell restart,
// guest-initiated display, or add-before-window races).
- (void)presentFrameForDisplay:(uint32_t)displayId {
    if (_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    auto it = _displayWindows.find(displayId);
    if (it == _displayWindows.end()) {
        if (displayId == 0) {
            return;  // primary window is opened explicitly
        }
        [self openDisplayWindowForDisplay:displayId];
        it = _displayWindows.find(displayId);
        if (it == _displayWindows.end()) {
            return;
        }
    }
    [it->second.view setNeedsDisplay:YES];
}

- (void)stopDoorbellThread {
    _doorbellShutdown.store(true, std::memory_order_release);
    if (_doorbellThread.joinable()) {
        _doorbellThread.join();
    }
}

#pragma mark - Shutdown

- (void)hideWindowsForTermination {
    for (auto& entry : _displayWindows) {
        [self teardownDisplayWindowEntry:entry.second];
        [entry.second.window orderOut:nil];
    }
    _displayWindows.clear();
    [_appsWindow orderOut:nil];
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
    if (_guestControlClient) {
        _guestControlClient->stop();
    }

    const pid_t pid = [self currentQemuPid];
    if (pid > 0) {
        terminate_qemu(pid);
    }
    if (_qemuMonitorThread.joinable()) {
        _qemuMonitorThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(_controlMutex);
        _controlChannel.reset();
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
    if (_guestControlClient) {
        delete _guestControlClient;
        _guestControlClient = nullptr;
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
