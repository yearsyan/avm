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
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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

// A rounded "card" container with a subtle background fill. Children are added
// as subviews in caller-supplied (card-local) coordinates.
NSView* make_card(NSRect frame) {
    NSView* card = [[NSView alloc] initWithFrame:frame];
    card.wantsLayer = YES;
    card.layer.cornerRadius = 10.0;
    card.layer.masksToBounds = YES;
    card.layer.backgroundColor = [NSColor controlBackgroundColor].CGColor;
    card.layer.borderColor = [NSColor separatorColor].CGColor;
    card.layer.borderWidth = 1.0;
    return card;
}

NSButton* make_button(NSString* title, SEL action, id target, NSRect frame) {
    NSButton* button = [NSButton buttonWithTitle:title target:target action:action];
    button.frame = frame;
    button.bezelStyle = NSBezelStyleRounded;
    button.controlSize = NSControlSizeRegular;
    return button;
}

// Returns a placeholder icon: a rounded-rect filled with a neutral tint and
// the first code unit of |label| centered. Used when a launcher entry has no
// decodable icon or while icons are still loading.
NSImage* placeholder_icon(NSString* label, NSSize size) {
    NSImage* image = [[NSImage alloc] initWithSize:size];
    [image lockFocus];
    NSBezierPath* path =
        [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(0, 0, size.width, size.height)
                                      xRadius:size.width * 0.22
                                      yRadius:size.height * 0.22];
    [[NSColor secondarySystemFillColor] setFill];
    [path fill];
    NSString* initial = label.length > 0
                            ? [[label substringToIndex:1] uppercaseString]
                            : @"?";
    NSDictionary* attrs = @{
        NSFontAttributeName : [NSFont systemFontOfSize:size.width * 0.46
                                              weight:NSFontWeightSemibold],
        NSForegroundColorAttributeName : [NSColor secondaryLabelColor]
    };
    NSRect textRect = [initial boundingRectWithSize:size
                                            options:0
                                         attributes:attrs];
    NSPoint origin = NSMakePoint((size.width - textRect.size.width) * 0.5 - textRect.origin.x,
                                 (size.height - textRect.size.height) * 0.5 - textRect.origin.y);
    [initial drawAtPoint:origin withAttributes:attrs];
    [image unlockFocus];
    return image;
}

struct DisplayLaunchProfile {
    const char* title;
    uint32_t width;
    uint32_t height;
    uint32_t dpi;
};

constexpr uint32_t kNewDisplayDpi = 240;

// User-creatable display ids on the qemu side are 1..kMaxUserDisplayId; ids
// above that are the emulator-internal range. Must match
// kMaxUserDisplayId in macmu-control-receiver.cpp.
constexpr uint32_t kMaxUserDisplayId = 5;

// Common app-window aspect ratios. 16:9 remains the default button path.
constexpr DisplayLaunchProfile kDisplayLaunchProfiles[] = {
    {"16:9  1280 x 720", 1280, 720, kNewDisplayDpi},
    {"9:16  720 x 1280", 720, 1280, kNewDisplayDpi},
    {"16:10 1280 x 800", 1280, 800, kNewDisplayDpi},
    {"10:16 800 x 1280", 800, 1280, kNewDisplayDpi},
    {"18.5:9 1480 x 720", 1480, 720, kNewDisplayDpi},
    {"9:18.5 720 x 1480", 720, 1480, kNewDisplayDpi},
    {"18:9  1440 x 720", 1440, 720, kNewDisplayDpi},
    {"9:18  720 x 1440", 720, 1440, kNewDisplayDpi},
    {"19:9  1520 x 720", 1520, 720, kNewDisplayDpi},
    {"9:19  720 x 1520", 720, 1520, kNewDisplayDpi},
    {"19.5:9 1560 x 720", 1560, 720, kNewDisplayDpi},
    {"9:19.5 720 x 1560", 720, 1560, kNewDisplayDpi},
    {"20:9  1600 x 720", 1600, 720, kNewDisplayDpi},
    {"9:20  720 x 1600", 720, 1600, kNewDisplayDpi},
    {"21:9  1680 x 720", 1680, 720, kNewDisplayDpi},
    {"32:9  1920 x 540", 1920, 540, kNewDisplayDpi},
    {"4:3   1024 x 768", 1024, 768, kNewDisplayDpi},
    {"3:4   768 x 1024", 768, 1024, kNewDisplayDpi},
    {"3:2   1200 x 800", 1200, 800, kNewDisplayDpi},
    {"2:3   800 x 1200", 800, 1200, kNewDisplayDpi},
    {"5:4   1000 x 800", 1000, 800, kNewDisplayDpi},
    {"4:5   800 x 1000", 800, 1000, kNewDisplayDpi},
    {"1:1   900 x 900", 900, 900, kNewDisplayDpi},
};
constexpr size_t kDisplayLaunchProfileCount =
    sizeof(kDisplayLaunchProfiles) / sizeof(kDisplayLaunchProfiles[0]);
constexpr size_t kDefaultDisplayLaunchProfile = 0;

// DisplayManager virtual-display flags, copied here so MacMu can request
// scrcpy-like secondary displays without depending on Android framework
// headers. Deliberately excludes SHOULD_SHOW_SYSTEM_DECORATIONS.
constexpr uint32_t kVirtualDisplayFlagPublic = 1u << 0;
constexpr uint32_t kVirtualDisplayFlagPresentation = 1u << 1;
constexpr uint32_t kVirtualDisplayFlagOwnContentOnly = 1u << 3;
constexpr uint32_t kVirtualDisplayFlagSupportsTouch = 1u << 6;
constexpr uint32_t kVirtualDisplayFlagRotatesWithContent = 1u << 7;
constexpr uint32_t kVirtualDisplayFlagTrusted = 1u << 10;
constexpr uint32_t kNewDisplayFlags =
    kVirtualDisplayFlagPublic | kVirtualDisplayFlagPresentation |
    kVirtualDisplayFlagOwnContentOnly | kVirtualDisplayFlagSupportsTouch |
    kVirtualDisplayFlagRotatesWithContent | kVirtualDisplayFlagTrusted;

struct DisplayWindow {
    NSWindow* __strong window = nil;
    MTKView* __strong view = nil;
    MacMuSurfaceRendererRef __strong renderer = nil;
    // Requested geometry captured at DISPLAY_ADD time, used for the
    // "requested vs actual" window title suffix on secondary displays.
    // Zero on display 0 and on auto-opened secondary displays.
    uint32_t requestedWidth = 0;
    uint32_t requestedHeight = 0;
    uint32_t requestedDpi = 0;
    bool reportedActual = false;  // true once the actual-size title suffix is set
};

}  // namespace

@interface MacMuAppsTableView : NSTableView
@end

@implementation MacMuAppsTableView
- (NSMenu*)menuForEvent:(NSEvent*)event {
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSInteger row = [self rowAtPoint:point];
    if (row >= 0) {
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
        return self.menu;
    }
    return [super menuForEvent:event];
}
@end

@interface MacMuAppDelegate
    : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTableViewDataSource,
                NSTableViewDelegate>
- (instancetype)initWithOptions:(const ShellOptions&)options;
// Opens (or focuses) the window bound to guest display |displayId|. The aspect
// variant seeds a secondary display's initial geometry from the requested
// dimensions; pass 0/0 to use the legacy default geometry.
- (void)openDisplayWindowForDisplay:(uint32_t)displayId
                        aspectWidth:(uint32_t)aspectWidth
                       aspectHeight:(uint32_t)aspectHeight
                               dpi:(uint32_t)dpi;
// Picks the smallest user display id (1..kMaxUserDisplayId) not currently in
// _activeUserDisplayIds, marks it active, and returns it. Returns 0 if all are
// in use. Pair with releaseUserDisplayId: when the window closes / is removed.
- (uint32_t)allocateUserDisplayId;
- (void)releaseUserDisplayId:(uint32_t)displayId;
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
    // Secondary displays created by "Launch in New Display" are bound to the
    // launched app. Closing the host window stops that app before removing the
    // guest display so Android does not reparent the task to display 0.
    std::map<uint32_t, std::string> _displayAppBindings;
    // User display ids (1..kMaxUserDisplayId) we have an open window + binding
    // for. The qemu control plane's auto-allocate (displayId == AUTO) does NOT
    // reliably skip displays we still have open: a second "launch in new
    // display" can be handed the same id as the first (the underlying
    // multi_display agent's enabled-tracking is incomplete in this build),
    // which makes the new app land on the existing display. We avoid that by
    // allocating an explicit id here — the smallest id in 1..5 not in this set
    // — and releasing it when the window closes / is removed.
    std::set<uint32_t> _activeUserDisplayIds;

    NSWindow* _appsWindow;
    NSTableView* _appsTable;
    NSTextField* _appsStatusValue;
    NSMutableArray<NSDictionary*>* _apps;
    // pkg -> friendly label and decoded icon, populated in applyAppList:.
    // Used by the icon table cell; missing entries fall back to a placeholder.
    NSMutableDictionary<NSString*, NSString*>* _appNames;
    NSMutableDictionary<NSString*, NSImage*>* _appIcons;

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
    _appNames = [[NSMutableDictionary alloc] init];
    _appIcons = [[NSMutableDictionary alloc] init];
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
        NSLog(@"MacMu [diag] windowWillClose displayId=%u suppressed=%d binding=%s", displayId,
              _suppressRemoveOnClose.count(displayId) > 0,
              (_displayAppBindings.count(displayId) ? _displayAppBindings[displayId].c_str()
                                                    : "<none>"));
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
                [self closeBoundAppAndRemoveDisplay:displayId];
            } else {
                _displayAppBindings.erase(displayId);
            }
            // Release the id whether or not we issued DISPLAY_REMOVE, so the
            // next "launch in new display" can reuse the slot. The guest-remove
            // path calls releaseUserDisplayId: after the ack to avoid a race
            // with a re-add at the same id while qemu is still tearing down.
            if (suppressed) {
                [self releaseUserDisplayId:displayId];
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

    const NSRect frame = NSMakeRect(0, 0, 760, 640);
    _statusWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _statusWindow.title = @"MacMu";
    _statusWindow.releasedWhenClosed = NO;
    _statusWindow.delegate = self;
    _statusWindow.minSize = NSMakeSize(700, 560);
    _statusWindow.backgroundColor = [NSColor windowBackgroundColor];

    NSView* content = [[NSView alloc] initWithFrame:frame];
    _statusWindow.contentView = content;

    // --- Title region ----------------------------------------------------
    NSTextField* title = make_label(@"MacMu", NSMakeRect(24, 596, 360, 30));
    title.font = [NSFont systemFontOfSize:26.0 weight:NSFontWeightBold];
    title.textColor = [NSColor labelColor];
    [content addSubview:title];

    NSTextField* subtitle =
        make_label(@"Android emulator core", NSMakeRect(26, 572, 360, 18));
    subtitle.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightRegular];
    subtitle.textColor = [NSColor secondaryLabelColor];
    [content addSubview:subtitle];

    // --- System card -----------------------------------------------------
    // Card spans the full content width minus side margins; its subviews are
    // positioned in card-local coordinates (origin at card bottom-left).
    const CGFloat cardX = 20.0;
    const CGFloat cardW = frame.size.width - cardX * 2.0;
    NSView* systemCard = make_card(NSMakeRect(cardX, 388, cardW, 168));
    systemCard.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    [content addSubview:systemCard];

    NSTextField* systemHeader = make_label(@"SYSTEM", NSMakeRect(16, 140, 200, 16));
    systemHeader.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightSemibold];
    systemHeader.textColor = [NSColor tertiaryLabelColor];
    [systemCard addSubview:systemHeader];

    const CGFloat labelX = 16.0;
    const CGFloat valueX = 130.0;
    const CGFloat valueW = cardW - valueX - 16.0;
    auto add_system_row = ^(NSString* label, NSString* value, CGFloat y) {
        NSTextField* l = make_label(label, NSMakeRect(labelX, y, valueX - labelX - 8, 18));
        l.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightMedium];
        l.textColor = [NSColor tertiaryLabelColor];
        [systemCard addSubview:l];
        NSTextField* v = make_value(value, NSMakeRect(valueX, y, valueW, 18));
        v.textColor = [NSColor labelColor];
        [systemCard addSubview:v];
        return v;
    };
    _qemuStatusValue = add_system_row(@"QEMU", @"Starting", 108);
    _appDataPathValue = add_system_row(@"Storage", @"Managed by MacMu", 84);
    _avdPathValue = add_system_row(@"Device", @"Checking", 60);
    _systemPathValue = add_system_row(@"System Image", @"Checking", 36);

    // --- Apps card -------------------------------------------------------
    // The card's scroll view grows with the window; the button row below it
    // is pinned to the bottom.
    const CGFloat appsCardY = 84.0;
    const CGFloat appsCardH = 292.0;
    NSView* appsCard = make_card(NSMakeRect(cardX, appsCardY, cardW, appsCardH));
    appsCard.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:appsCard];

    NSTextField* appsHeader = make_label(@"APPS", NSMakeRect(16, appsCardH - 26, 120, 16));
    appsHeader.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightSemibold];
    appsHeader.textColor = [NSColor tertiaryLabelColor];
    appsHeader.autoresizingMask = NSViewMaxYMargin;
    [appsCard addSubview:appsHeader];

    _appsStatusValue = make_value(@"", NSMakeRect(cardW - 280, appsCardH - 26, 264, 16));
    _appsStatusValue.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightRegular];
    _appsStatusValue.textColor = [NSColor tertiaryLabelColor];
    _appsStatusValue.alignment = NSTextAlignmentRight;
    _appsStatusValue.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    [appsCard addSubview:_appsStatusValue];

    NSScrollView* scroll =
        [[NSScrollView alloc] initWithFrame:NSMakeRect(8, 8, cardW - 16, appsCardH - 44)];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;

    _appsTable = [[MacMuAppsTableView alloc] initWithFrame:scroll.bounds];
    // Single column carrying the icon + name + package cell. The identifier is
    // arbitrary but must match what tableView:viewForTableColumn: returns.
    NSTableColumn* appColumn = [[NSTableColumn alloc] initWithIdentifier:@"app"];
    appColumn.width = scroll.bounds.size.width;
    appColumn.resizingMask = NSTableColumnAutoresizingMask;
    [_appsTable addTableColumn:appColumn];
    _appsTable.dataSource = self;
    _appsTable.delegate = self;
    _appsTable.usesAlternatingRowBackgroundColors = NO;
    _appsTable.backgroundColor = [NSColor clearColor];
    _appsTable.style = NSTableViewStylePlain;
    _appsTable.rowSizeStyle = NSTableViewRowSizeStyleDefault;
    _appsTable.intercellSpacing = NSMakeSize(0, 0);
    _appsTable.doubleAction = @selector(launchAppOnNewDisplay:);
    _appsTable.target = self;
    NSMenu* appsMenu = [[NSMenu alloc] initWithTitle:@"Apps"];
    NSMenuItem* launchPrimary = [appsMenu addItemWithTitle:@"Launch on Primary Display"
                                                    action:@selector(launchAppOnPrimary:)
                                             keyEquivalent:@""];
    launchPrimary.target = self;
    NSMenuItem* launchInDisplay =
        [appsMenu addItemWithTitle:@"Launch in New Display"
                             action:nil
                      keyEquivalent:@""];
    NSMenu* ratioMenu = [[NSMenu alloc] initWithTitle:@"Launch in New Display"];
    for (size_t i = 0; i < kDisplayLaunchProfileCount; ++i) {
        NSMenuItem* item =
            [ratioMenu addItemWithTitle:[NSString stringWithUTF8String:kDisplayLaunchProfiles[i].title]
                                 action:@selector(launchAppWithProfile:)
                          keyEquivalent:@""];
        item.target = self;
        item.representedObject = @(i);
    }
    [appsMenu setSubmenu:ratioMenu forItem:launchInDisplay];
    _appsTable.menu = appsMenu;
    scroll.documentView = _appsTable;
    [appsCard addSubview:scroll];

    // --- Bottom button row ----------------------------------------------
    // Two groups: Refresh / Import on the left, Display / Launch / New Display
    // on the right, separated by a thin separator that spans the card width.
    NSBox* separator = [[NSBox alloc] initWithFrame:NSMakeRect(cardX, 70, cardW, 1)];
    separator.boxType = NSBoxSeparator;
    separator.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    [content addSubview:separator];

    NSButton* refresh = make_button(@"Refresh", @selector(refreshApps:), self,
                                    NSMakeRect(cardX, 24, 110, 32));
    refresh.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
    [content addSubview:refresh];

    _createMachineButton = make_button(@"Import Image…", @selector(prepareDevice:), self,
                                       NSMakeRect(cardX + 120, 24, 140, 32));
    _createMachineButton.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
    [content addSubview:_createMachineButton];

    // Right-anchored action group, laid out right-to-left.
    const CGFloat btnH = 32.0;
    const CGFloat btnY = 24.0;
    NSButton* launchNew =
        make_button(@"New Display", @selector(launchAppOnNewDisplay:), self,
                    NSMakeRect(cardX + cardW - 130, btnY, 130, btnH));
    launchNew.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    [content addSubview:launchNew];

    NSButton* launchPrimaryBtn =
        make_button(@"Launch", @selector(launchAppOnPrimary:), self,
                    NSMakeRect(launchNew.frame.origin.x - 8 - 90, btnY, 90, btnH));
    launchPrimaryBtn.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    [content addSubview:launchPrimaryBtn];

    _startButton = make_button(@"Display", @selector(openPrimaryDisplayWindow:), self,
                               NSMakeRect(launchPrimaryBtn.frame.origin.x - 8 - 96, btnY, 96, btnH));
    _startButton.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    _startButton.enabled = _channelReady && _metalDevice != nil;
    [content addSubview:_startButton];

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
    [self openDisplayWindowForDisplay:displayId aspectWidth:0 aspectHeight:0 dpi:0];
}

// Same as above but seeds a secondary display's initial content size and aspect
// ratio from the requested dimensions, so the window opens already shaped like
// the chosen profile instead of jumping from a 16:9 default on the first frame.
// aspectWidth/aspectHeight of 0 fall back to the legacy default geometry.
- (void)openDisplayWindowForDisplay:(uint32_t)displayId
                        aspectWidth:(uint32_t)aspectWidth
                       aspectHeight:(uint32_t)aspectHeight
                               dpi:(uint32_t)dpi {
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

    NSSize initialSize;
    if (displayId == 0) {
        initialSize = NSMakeSize(420, 720);
    } else if (aspectWidth > 0 && aspectHeight > 0) {
        // Reuse the renderer's fit-to-screen math so the window opens at the
        // same size the first frame will resize it to (no jump).
        initialSize = macmu_fitted_window_content_size(aspectWidth, aspectHeight);
    } else {
        initialSize = NSMakeSize(640, 360);
    }
    NSRect frame = NSMakeRect(0, 0, initialSize.width, initialSize.height);
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
    // Lock the window's content aspect ratio to the request immediately; the
    // renderer also reasserts this on the first frame, but setting it now
    // keeps the resize handle honest before pixels arrive.
    if (displayId != 0 && aspectWidth > 0 && aspectHeight > 0) {
        [window setContentAspectRatio:NSMakeSize(aspectWidth, aspectHeight)];
    }
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
    if (displayId != 0 && aspectWidth > 0 && aspectHeight > 0) {
        entry.requestedWidth = aspectWidth;
        entry.requestedHeight = aspectHeight;
        entry.requestedDpi = dpi;
    }
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

- (uint32_t)allocateUserDisplayId {
    for (uint32_t candidate = 1; candidate <= kMaxUserDisplayId; ++candidate) {
        if (_activeUserDisplayIds.find(candidate) == _activeUserDisplayIds.end()) {
            _activeUserDisplayIds.insert(candidate);
            NSLog(@"MacMu [diag] allocateUserDisplayId -> %u (active now: {%s})", candidate,
                  [self describeActiveIds].UTF8String);
            return candidate;
        }
    }
    NSLog(@"MacMu [diag] allocateUserDisplayId -> 0 (none free, active: {%s})",
          [self describeActiveIds].UTF8String);
    return 0;
}

- (void)releaseUserDisplayId:(uint32_t)displayId {
    if (displayId != 0) {
        const auto erased = _activeUserDisplayIds.erase(displayId);
        NSLog(@"MacMu [diag] releaseUserDisplayId(%u) %s (active now: {%s})", displayId,
              erased ? "released" : "was-not-active", [self describeActiveIds].UTF8String);
    }
}

- (NSString*)describeActiveIds {
    NSMutableArray* parts = [NSMutableArray array];
    for (uint32_t id : _activeUserDisplayIds) {
        [parts addObject:[NSString stringWithFormat:@"%u", id]];
    }
    return [parts componentsJoinedByString:@", "];
}

- (void)updateMachineControls {
    const bool hasSystemImage = macmu_system_image_exists(_options);
    const bool hasMachine = macmu_machine_exists(_options);
    const bool qemuRunning = [self currentQemuPid] > 0;
    if (_createMachineButton) {
        _createMachineButton.enabled = !qemuRunning && (!hasSystemImage || !hasMachine);
        if (!hasSystemImage) {
            _createMachineButton.title = @"Import Image…";
        } else if (!hasMachine) {
            _createMachineButton.title = @"Prepare Device";
        } else {
            _createMachineButton.title = @"Device Ready";
        }
    }
    if (_appDataPathValue) {
        _appDataPathValue.stringValue = qemuRunning ? @"Managed by MacMu - running"
                                                    : @"Managed by MacMu";
    }
    if (_avdPathValue) {
        _avdPathValue.stringValue = hasMachine ? @"Ready"
                                               : (hasSystemImage ? @"Needs preparation"
                                                                 : @"Waiting for image");
    }
    if (_systemPathValue) {
        _systemPathValue.stringValue = hasSystemImage ? @"Ready" : @"Import required";
    }
}

- (void)prepareDevice:(id)sender {
    if (!macmu_system_image_exists(_options)) {
        [self importSystemImage:sender];
        return;
    }
    [self createMachine:sender];
}

- (void)importSystemImage:(id)sender {
    if ([self currentQemuPid] > 0) {
        [self publishQemuStatus:@"Quit MacMu before importing an image"];
        NSBeep();
        return;
    }

    [self showStatusWindow:nil];
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Import MacMu System Image";
    panel.message = @"Choose a MacMu AOSP16 arm64 system image zip.";
    panel.prompt = @"Import";
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.allowedContentTypes = @[ [UTType typeWithIdentifier:@"public.zip-archive"] ];

    MacMuAppDelegate* delegate = self;
    [panel beginSheetModalForWindow:_statusWindow
                  completionHandler:^(NSModalResponse result) {
                      if (result != NSModalResponseOK || panel.URL == nil) {
                          return;
                      }
                      [delegate importSystemImageArchive:panel.URL];
                  }];
}

- (void)importSystemImageArchive:(NSURL*)archiveURL {
    ShellOptions options = _options;
    NSString* archivePath = archiveURL.path;
    if (archivePath.length == 0) {
        [self publishQemuStatus:@"Import failed: empty path"];
        return;
    }

    [self publishQemuStatus:@"Importing system image"];
    if (_createMachineButton) {
        _createMachineButton.enabled = NO;
    }

    MacMuAppDelegate* delegate = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            std::string error;
            BOOL ok = YES;

            NSString* tempTemplate =
                [NSTemporaryDirectory() stringByAppendingPathComponent:@"macmu-image-import.XXXXXX"];
            std::vector<char> tempBuffer(strlen(tempTemplate.fileSystemRepresentation) + 1);
            std::strcpy(tempBuffer.data(), tempTemplate.fileSystemRepresentation);
            char* tempPath = mkdtemp(tempBuffer.data());
            if (!tempPath) {
                ok = NO;
                error = "failed to create temporary import directory: ";
                error += std::strerror(errno);
            }

            NSString* tempRoot = tempPath ? [NSString stringWithUTF8String:tempPath] : nil;
            if (ok) {
                NSTask* task = [[NSTask alloc] init];
                task.launchPath = @"/usr/bin/ditto";
                task.arguments = @[ @"-x", @"-k", archivePath, tempRoot ];
                NSError* launchError = nil;
                if (![task launchAndReturnError:&launchError]) {
                    ok = NO;
                    error = "failed to launch ditto: ";
                    error += launchError.localizedDescription.UTF8String ?: "unknown error";
                } else {
                    [task waitUntilExit];
                    if (task.terminationStatus != 0) {
                        ok = NO;
                        error = "failed to extract zip";
                    }
                }
            }

            std::string extractedImageDir;
            if (ok) {
                ok = macmu_find_system_image_directory(tempRoot.UTF8String, &extractedImageDir,
                                                       &error);
            }
            if (ok) {
                ok = macmu_replace_system_image_from_directory(options, extractedImageDir, &error);
            }
            if (ok) {
                ok = macmu_create_default_machine(options, &error);
            }

            if (tempRoot) {
                [[NSFileManager defaultManager] removeItemAtPath:tempRoot error:nil];
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                if (ok) {
                    [delegate publishQemuStatus:@"System image imported"];
                } else {
                    [delegate publishQemuStatus:ns_string("Import failed: " + error)];
                    NSLog(@"MacMu image import failed: %s", error.c_str());
                }
                [delegate updateMachineControls];
            });
        }
    });
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
        _displayAppBindings.erase(event.displayId);
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
    MacMuAppDelegate* delegate = self;
    channel->request(macmu::ControlMessageType::kDisplayRemove, &request, sizeof(request), 5000,
                     [delegate, displayId](ControlChannel::Response response) {
                         if (!response.ok) {
                             NSLog(@"MacMu display %u remove failed: %s", displayId,
                                   response.errorMessage.c_str());
                         }
                         // Release the id now that qemu has torn the display
                         // down (or we gave up); this lets a subsequent
                         // "launch in new display" reuse the slot.
                         dispatch_async(dispatch_get_main_queue(), ^{
                             [delegate releaseUserDisplayId:displayId];
                         });
                     });
}

- (void)closeBoundAppAndRemoveDisplay:(uint32_t)displayId {
    auto binding = _displayAppBindings.find(displayId);
    if (binding == _displayAppBindings.end()) {
        [self requestDisplayRemove:displayId];
        return;
    }

    const std::string component = binding->second;
    _displayAppBindings.erase(binding);
    if (!_guestControlClient || !_guestControlClient->ready()) {
        NSLog(@"MacMu display %u app close skipped; guest control is not connected.",
              displayId);
        [self requestDisplayRemove:displayId];
        return;
    }

    std::string command = "close ";
    command += component;
    command += " " + std::to_string(displayId);
    MacMuAppDelegate* delegate = self;
    _guestControlClient->request(command, 5000,
                                 [delegate, displayId](bool ok, std::string payload) {
                                     if (!ok) {
                                         NSLog(@"MacMu display %u bound app close failed: %s",
                                               displayId, payload.c_str());
                                     }
                                     dispatch_async(dispatch_get_main_queue(), ^{
                                         [delegate requestDisplayRemove:displayId];
                                     });
                                 });
}

- (void)newDisplay:(id)sender {
    auto channel = [self controlChannel];
    if (!channel || !channel->alive()) {
        [self publishQemuStatus:@"Control channel not connected"];
        NSBeep();
        return;
    }
    // Allocate an explicit id up front (not AUTO): the qemu control plane's
    // auto-allocate does not reliably skip displays we still have open, so a
    // second new-display request could be handed the same id as the first.
    const uint32_t displayId = [self allocateUserDisplayId];
    if (displayId == 0) {
        [self publishQemuStatus:@"No free display id (1..5 in use)"];
        NSBeep();
        return;
    }
    const DisplayLaunchProfile& profile = kDisplayLaunchProfiles[kDefaultDisplayLaunchProfile];
    macmu::ControlDisplayAdd request = {};
    request.displayId = displayId;
    request.width = profile.width;
    request.height = profile.height;
    request.dpi = profile.dpi;
    request.flags = kNewDisplayFlags;
    const uint32_t aspectWidth = profile.width;
    const uint32_t aspectHeight = profile.height;
    const uint32_t aspectDpi = profile.dpi;
    MacMuAppDelegate* delegate = self;
    channel->request(
        macmu::ControlMessageType::kDisplayAdd, &request, sizeof(request), 10000,
        [delegate, displayId, aspectWidth, aspectHeight, aspectDpi](
            ControlChannel::Response response) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!response.ok ||
                    response.payload.size() < sizeof(macmu::ControlDisplayAddOk)) {
                    NSString* message =
                        [NSString stringWithFormat:@"New display failed: %s",
                                                   response.errorMessage.empty()
                                                       ? "malformed response"
                                                       : response.errorMessage.c_str()];
                    [delegate publishQemuStatus:message];
                    NSLog(@"%@", message);
                    [delegate releaseUserDisplayId:displayId];
                    return;
                }
                macmu::ControlDisplayAddOk ok;
                std::memcpy(&ok, response.payload.data(), sizeof(ok));
                [delegate openDisplayWindowForDisplay:ok.displayId
                                          aspectWidth:aspectWidth
                                         aspectHeight:aspectHeight
                                                 dpi:aspectDpi];
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
    [_appNames removeAllObjects];
    [_appIcons removeAllObjects];
    for (id entry in entries) {
        if (![entry isKindOfClass:[NSDictionary class]]) {
            continue;
        }
        NSDictionary* dict = (NSDictionary*)entry;
        if (![dict[@"pkg"] isKindOfClass:[NSString class]] ||
            ![dict[@"activity"] isKindOfClass:[NSString class]]) {
            continue;
        }
        NSString* pkg = dict[@"pkg"];
        [_apps addObject:dict];
        // Cache friendly name (falls back to package name in the cell).
        NSString* name = dict[@"name"];
        if ([name isKindOfClass:[NSString class]] && name.length > 0) {
            _appNames[pkg] = name;
        }
        // Decode base64 PNG icon if present; failures fall back to a
        // placeholder rendered lazily by the cell.
        NSString* iconB64 = dict[@"icon"];
        if ([iconB64 isKindOfClass:[NSString class]] && iconB64.length > 0) {
            NSData* pngData =
                [[NSData alloc] initWithBase64EncodedString:iconB64
                                                    options:NSDataBase64DecodingIgnoreUnknownCharacters];
            NSImage* icon = pngData ? [[NSImage alloc] initWithData:pngData] : nil;
            if (icon) {
                _appIcons[pkg] = icon;
            }
        }
    }
    // Sort by friendly name when available, else by package — names make the
    // list more scannable than raw package ids.
    [_apps sortUsingComparator:^NSComparisonResult(NSDictionary* a, NSDictionary* b) {
        NSString* nameA = _appNames[a[@"pkg"]] ?: a[@"pkg"];
        NSString* nameB = _appNames[b[@"pkg"]] ?: b[@"pkg"];
        return [nameA localizedCaseInsensitiveCompare:nameB];
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
    [self launchSelectedAppInNewDisplayWithProfile:
              kDisplayLaunchProfiles[kDefaultDisplayLaunchProfile]];
}

- (void)launchAppWithProfile:(id)sender {
    if (![sender respondsToSelector:@selector(representedObject)]) {
        NSBeep();
        return;
    }
    NSNumber* profileIndex = [sender representedObject];
    if (![profileIndex isKindOfClass:[NSNumber class]]) {
        NSBeep();
        return;
    }
    const NSUInteger index = profileIndex.unsignedIntegerValue;
    if (index >= kDisplayLaunchProfileCount) {
        NSBeep();
        return;
    }
    [self launchSelectedAppInNewDisplayWithProfile:kDisplayLaunchProfiles[index]];
}

- (void)launchSelectedAppInNewDisplayWithProfile:(DisplayLaunchProfile)profile {
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
    // Allocate an explicit id up front (not AUTO). The qemu control plane's
    // auto-allocate can hand back the same id as an already-open display, which
    // makes the new app land on the existing display with the old aspect ratio.
    const uint32_t displayId = [self allocateUserDisplayId];
    if (displayId == 0) {
        [self setAppsStatus:@"No free display id (1..5 in use)"];
        NSBeep();
        return;
    }
    macmu::ControlDisplayAdd request = {};
    request.displayId = displayId;
    request.width = profile.width;
    request.height = profile.height;
    request.dpi = profile.dpi;
    request.flags = kNewDisplayFlags;
    const uint32_t aspectWidth = profile.width;
    const uint32_t aspectHeight = profile.height;
    const uint32_t aspectDpi = profile.dpi;
    MacMuAppDelegate* delegate = self;
    channel->request(
        macmu::ControlMessageType::kDisplayAdd, &request, sizeof(request), 10000,
        [delegate, component, displayId, aspectWidth, aspectHeight, aspectDpi](
            ControlChannel::Response response) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!response.ok ||
                    response.payload.size() < sizeof(macmu::ControlDisplayAddOk)) {
                    [delegate setAppsStatus:ns_string("Display add failed: " +
                                                      response.errorMessage)];
                    [delegate releaseUserDisplayId:displayId];
                    return;
                }
                macmu::ControlDisplayAddOk ok;
                std::memcpy(&ok, response.payload.data(), sizeof(ok));
                NSLog(@"MacMu: launched %@ on new display %u (%ux%u)", component, ok.displayId,
                      aspectWidth, aspectHeight);
                [delegate openDisplayWindowForDisplay:ok.displayId
                                          aspectWidth:aspectWidth
                                         aspectHeight:aspectHeight
                                                 dpi:aspectDpi];
                delegate->_displayAppBindings[ok.displayId] = [component UTF8String];
                // Give the guest a brief moment to register the new
                // VirtualDisplay, then launch. The agent also waits on the
                // display internally, so a short delay here is enough; the
                // previous 1.5 s blind wait made launches feel sluggish.
                dispatch_after(
                    dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.2 * NSEC_PER_SEC)),
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
    _guestControlClient->request(command, 15000, [delegate, displayId, component](
                                                     bool ok, std::string payload) {
        if (!ok) {
            [delegate setAppsStatus:ns_string("Launch failed: " + payload)];
        } else {
            [delegate setAppsStatus:[NSString
                                        stringWithFormat:@"Launched %@ on display %u",
                                                         component.lastPathComponent, displayId]];
        }
    });
}

#pragma mark - Apps table data source

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    return static_cast<NSInteger>(_apps.count);
}

- (CGFloat)tableView:(NSTableView*)tableView heightOfRow:(NSInteger)row {
    return 56.0;
}

- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)tableColumn
                   row:(NSInteger)row {
    if (row < 0 || row >= static_cast<NSInteger>(_apps.count)) {
        return nil;
    }
    NSDictionary* app = _apps[static_cast<NSUInteger>(row)];
    NSString* pkg = app[@"pkg"];
    NSString* activity = app[@"activity"];
    NSString* name = _appNames[pkg] ?: pkg;

    static NSString* const kCellIdentifier = @"MacMuAppCell";
    NSView* cell = [tableView makeViewWithIdentifier:kCellIdentifier owner:self];
    if (!cell) {
        const CGFloat cellH = 56.0;
        cell = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, tableColumn.width, cellH)];
        cell.identifier = kCellIdentifier;

        NSImageView* iconView = [[NSImageView alloc] initWithFrame:NSMakeRect(14, 8, 40, 40)];
        iconView.identifier = @"icon";
        iconView.imageScaling = NSImageScaleProportionallyDown;
        iconView.imageAlignment = NSImageAlignCenter;
        iconView.wantsLayer = YES;
        iconView.layer.cornerRadius = 8.0;
        iconView.layer.masksToBounds = YES;
        [cell addSubview:iconView];

        NSTextField* nameField =
            [[NSTextField alloc] initWithFrame:NSMakeRect(64, 30, tableColumn.width - 78, 18)];
        nameField.identifier = @"name";
        nameField.bezeled = NO;
        nameField.drawsBackground = NO;
        nameField.editable = NO;
        nameField.selectable = NO;
        nameField.lineBreakMode = NSLineBreakByTruncatingTail;
        nameField.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightSemibold];
        nameField.textColor = [NSColor labelColor];
        nameField.autoresizingMask = NSViewWidthSizable;
        [cell addSubview:nameField];

        NSTextField* subField =
            [[NSTextField alloc] initWithFrame:NSMakeRect(64, 11, tableColumn.width - 78, 14)];
        subField.identifier = @"sub";
        subField.bezeled = NO;
        subField.drawsBackground = NO;
        subField.editable = NO;
        subField.selectable = NO;
        subField.lineBreakMode = NSLineBreakByTruncatingMiddle;
        subField.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightRegular];
        subField.textColor = [NSColor tertiaryLabelColor];
        subField.autoresizingMask = NSViewWidthSizable;
        [cell addSubview:subField];
    }

    NSImageView* iconView = nil;
    NSTextField* nameField = nil;
    NSTextField* subField = nil;
    for (NSView* sub in cell.subviews) {
        if ([sub.identifier isEqualToString:@"icon"]) {
            iconView = (NSImageView*)sub;
        } else if ([sub.identifier isEqualToString:@"name"]) {
            nameField = (NSTextField*)sub;
        } else if ([sub.identifier isEqualToString:@"sub"]) {
            subField = (NSTextField*)sub;
        }
    }
    NSImage* icon = _appIcons[pkg];
    iconView.image = icon ?: placeholder_icon(name, NSMakeSize(40, 40));
    nameField.stringValue = name;
    // Sub line shows the package (and the launcher activity class when short
    // enough), so the row stays useful even when the icon/name are missing.
    NSString* activityClass = activity.lastPathComponent;
    subField.stringValue = [NSString stringWithFormat:@"%@ · %@", pkg, activityClass];
    return cell;
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
            [self publishQemuStatus:@"Preparing device"];
            std::string error;
            if (!macmu_create_default_machine(_options, &error)) {
                [self publishQemuStatus:ns_string("Device preparation failed: " + error)];
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            MacMuAppDelegate* delegate = self;
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate updateMachineControls];
            });
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
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate updateMachineControls];
            });
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

        dispatch_async(dispatch_get_main_queue(), ^{
            [delegate updateMachineControls];
        });

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
        const uint32_t actualWidth = meta.width;
        const uint32_t actualHeight = meta.height;
        dispatch_async(dispatch_get_main_queue(), ^{
            [delegate presentFrameForDisplay:displayId
                                 actualWidth:actualWidth
                                actualHeight:actualHeight];
        });
    }
}

// Main thread. Marks the display's view dirty; auto-opens a window when a
// secondary display starts producing frames without one (shell restart,
// guest-initiated display, or add-before-window races). On the first real
// frame for a secondary display, updates the window title with the
// requested-vs-actual resolution so mismatched guest-side sizing is visible.
- (void)presentFrameForDisplay:(uint32_t)displayId
                   actualWidth:(uint32_t)actualWidth
                  actualHeight:(uint32_t)actualHeight {
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
    if (displayId != 0 && !it->second.reportedActual && actualWidth > 0 && actualHeight > 0) {
        it->second.reportedActual = true;
        [self updateDisplayWindowTitle:it->first entry:it->second
                            actualWidth:actualWidth actualHeight:actualHeight];
    }
}

// Main thread. Renders the secondary display title as
// "MacMu Display N · req WxH · actual WxH" so a guest that allocated a
// different resolution than requested is obvious. Primary display keeps its
// plain title.
- (void)updateDisplayWindowTitle:(uint32_t)displayId
                            entry:(DisplayWindow&)entry
                     actualWidth:(uint32_t)actualWidth
                    actualHeight:(uint32_t)actualHeight {
    if (displayId == 0 || !entry.window) {
        return;
    }
    NSString* prefix = [NSString stringWithFormat:@"MacMu Display %u", displayId];
    if (entry.requestedWidth > 0 && entry.requestedHeight > 0) {
        entry.window.title =
            [NSString stringWithFormat:@"%@ · req %ux%u · actual %ux%u", prefix,
                                       entry.requestedWidth, entry.requestedHeight, actualWidth,
                                       actualHeight];
    } else {
        entry.window.title = [NSString
            stringWithFormat:@"%@ · actual %ux%u", prefix, actualWidth, actualHeight];
    }
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
