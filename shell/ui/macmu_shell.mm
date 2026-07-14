// MacMu macOS shell for IOSurface display export.
// SPDX-License-Identifier: MIT
//
// The app shell owns the AppKit lifecycle, the Applications window, one
// IOSurface window per launched Android app, the menu-bar status item, and the
// qemu supervisor. Android's built-in display 0 remains an internal boot
// surface and is never exposed by the product UI. Every visible app gets one
// dedicated virtual display created over the MMCP control channel.

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
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

std::string package_from_component(const std::string& component) {
    const size_t slash = component.find('/');
    return slash == std::string::npos ? component : component.substr(0, slash);
}

uint32_t maximum_frames_per_second(NSScreen* screen) {
    NSScreen* resolvedScreen = screen ?: [NSScreen mainScreen];
    const NSInteger framesPerSecond = resolvedScreen.maximumFramesPerSecond;
    return framesPerSecond > 0 ? static_cast<uint32_t>(framesPerSecond) : 60u;
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
};

}  // namespace

static NSUserInterfaceItemIdentifier const kApplicationItemIdentifier =
    @"MacMuApplicationItem";

@interface MacMuApplicationItem : NSCollectionViewItem
- (void)setApplicationRunning:(BOOL)running opening:(BOOL)opening closing:(BOOL)closing;
@end

@implementation MacMuApplicationItem {
    NSTextField* _runningLabel;
}

- (void)loadView {
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 116, 120)];
    view.wantsLayer = YES;
    view.layer.cornerRadius = 12.0;

    NSImageView* icon = [[NSImageView alloc] initWithFrame:NSMakeRect(26, 46, 64, 64)];
    icon.imageAlignment = NSImageAlignCenter;
    icon.imageScaling = NSImageScaleProportionallyDown;
    icon.wantsLayer = YES;
    icon.layer.cornerRadius = 14.0;
    icon.layer.masksToBounds = YES;
    [view addSubview:icon];

    NSTextField* name = [[NSTextField alloc] initWithFrame:NSMakeRect(6, 23, 104, 18)];
    name.bezeled = NO;
    name.drawsBackground = NO;
    name.editable = NO;
    name.selectable = NO;
    name.alignment = NSTextAlignmentCenter;
    name.lineBreakMode = NSLineBreakByTruncatingTail;
    name.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightMedium];
    name.textColor = [NSColor labelColor];
    [view addSubview:name];

    _runningLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(6, 6, 104, 14)];
    _runningLabel.bezeled = NO;
    _runningLabel.drawsBackground = NO;
    _runningLabel.editable = NO;
    _runningLabel.selectable = NO;
    _runningLabel.alignment = NSTextAlignmentCenter;
    _runningLabel.font = [NSFont systemFontOfSize:10.0 weight:NSFontWeightMedium];
    _runningLabel.textColor = [NSColor systemGreenColor];
    [view addSubview:_runningLabel];

    self.view = view;
    self.imageView = icon;
    self.textField = name;
}

- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    self.view.layer.backgroundColor =
        (selected ? [[NSColor selectedContentBackgroundColor] colorWithAlphaComponent:0.18]
                  : [NSColor clearColor])
            .CGColor;
}

- (void)setApplicationRunning:(BOOL)running opening:(BOOL)opening closing:(BOOL)closing {
    _runningLabel.stringValue = opening ? @"Opening…"
                                        : (closing ? @"Closing…"
                                                   : (running ? @"●  Running" : @""));
    _runningLabel.textColor = (opening || closing) ? [NSColor secondaryLabelColor]
                                                   : [NSColor systemGreenColor];
}

@end

@interface MacMuApplicationsCollectionView : NSCollectionView
@property(nonatomic, weak) id activationTarget;
@property(nonatomic) SEL activationAction;
@end

@implementation MacMuApplicationsCollectionView
- (NSMenu*)menuForEvent:(NSEvent*)event {
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSIndexPath* indexPath = [self indexPathForItemAtPoint:point];
    if (indexPath) {
        self.selectionIndexPaths = [NSSet setWithObject:indexPath];
        return self.menu;
    }
    // A blank-area context click must not operate on a stale selection.
    self.selectionIndexPaths = [NSSet set];
    return nil;
}

- (void)mouseDown:(NSEvent*)event {
    [super mouseDown:event];
    if (event.clickCount != 2 || !self.activationAction) {
        return;
    }
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSIndexPath* indexPath = [self indexPathForItemAtPoint:point];
    if (!indexPath) {
        return;
    }
    self.selectionIndexPaths = [NSSet setWithObject:indexPath];
    [NSApp sendAction:self.activationAction to:self.activationTarget from:self];
}
@end

@interface MacMuAppDelegate
    : NSObject <NSApplicationDelegate, NSWindowDelegate, NSCollectionViewDataSource,
                NSCollectionViewDelegate>
- (instancetype)initWithOptions:(const ShellOptions&)options;
// Opens (or focuses) the application window bound to virtual display
// |displayId|. Display 0 is deliberately rejected: it is an internal Android
// boot surface, never a user-facing application window.
- (void)openDisplayWindowForDisplay:(uint32_t)displayId
                        aspectWidth:(uint32_t)aspectWidth
                       aspectHeight:(uint32_t)aspectHeight
                               dpi:(uint32_t)dpi;
// Picks the smallest user display id (1..kMaxUserDisplayId) not currently in
// _activeUserDisplayIds, marks it active, and returns it. Returns 0 if all are
// in use. Pair with releaseUserDisplayId: when the window closes / is removed.
- (uint32_t)allocateUserDisplayId;
- (void)releaseUserDisplayId:(uint32_t)displayId;
- (void)resetSecondaryDisplayStateAfterQemuExit;
- (void)cleanupFailedLaunchForDisplay:(uint32_t)displayId
                            component:(NSString*)component
                              message:(NSString*)message;
- (void)restoreDisplaySubscriptions;
- (void)enqueueFramePresentationForDisplay:(uint32_t)displayId;
@end

@implementation MacMuAppDelegate {
    ShellOptions _options;

    FrameConsumer* _frameConsumer;
    InputSender* _inputSender;
    GuestInputSender* _guestInputSender;
    GuestControlClient* _guestControlClient;
    id<MTLDevice> _metalDevice;

    NSWindow* _statusWindow;
    NSTextField* _bootTitleValue;
    NSTextField* _bootDetailValue;
    NSTextField* _bootStageValue;
    NSProgressIndicator* _bootSpinner;
    NSButton* _createMachineButton;
    NSButton* _refreshAppsButton;
    NSButton* _openApplicationButton;

    // displayId -> window/view/renderer. Main thread only.
    std::map<uint32_t, DisplayWindow> _displayWindows;
    // Display ids being closed because the guest/event said so; suppresses the
    // windowWillClose -> DISPLAY_REMOVE echo.
    std::map<uint32_t, bool> _suppressRemoveOnClose;
    // Every visible display is bound to exactly one Android application.
    // Closing the host window stops that app before removing the guest display
    // so Android does not reparent the task to its internal display 0.
    std::map<uint32_t, std::string> _displayAppBindings;
    // Reverse package -> display index enforces the product invariant that one
    // application package owns at most one display/window. Bindings are
    // reserved before DISPLAY_ADD so repeated clicks focus (or wait for) the
    // same launch transaction instead of allocating another display.
    std::map<std::string, uint32_t> _appDisplayBindings;
    std::set<std::string> _pendingAppPackages;
    // Subset of pending packages whose guest launch RPC has actually been
    // sent. This distinguishes cancelling the 0.2s pre-launch handoff from
    // closing an application while launch is already in flight.
    std::set<std::string> _launchRequestsInFlight;
    std::set<std::string> _closingAppPackages;
    // When a launch RPC returns a definitive guest-side error during a close
    // transaction, preserve it until the ordered close response arrives. If
    // close also fails, the display is empty and should be removed rather than
    // reopened and mislabeled as Running.
    std::map<uint32_t, std::string> _closingLaunchFailures;
    // Failed/ambiguous launches are stopped before their display is removed.
    // If the agent disconnects, retain the transaction and retry on reconnect.
    std::map<uint32_t, std::string> _deferredLaunchCleanup;
    // Preserve the requested geometry from reservation through first frame;
    // a producer frame can arrive before DISPLAY_ADD_OK reaches the main
    // queue, and must still open the window with the chosen aspect ratio.
    std::map<uint32_t, DisplayLaunchProfile> _displayLaunchProfiles;
    // DISPLAY_REMOVE can complete via both an event and a request callback.
    // Track host-initiated removals so each slot has one release owner.
    std::set<uint32_t> _displayRemovalPending;
    // User display ids (1..kMaxUserDisplayId) reserved by applications.
    std::set<uint32_t> _activeUserDisplayIds;

    MacMuApplicationsCollectionView* _appsCollection;
    NSTextField* _appsStatusValue;
    NSTextField* _appsEmptyValue;
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
    std::array<std::atomic_bool, kMacmuFrameSlotCount> _framePresentationPending;
    std::thread _qemuMonitorThread;
    std::thread _doorbellThread;
    std::mutex _qemuExitMutex;
    std::condition_variable _qemuExitCondition;
    std::mutex _guestInputMutex;
    std::atomic<pid_t> _qemuPid;
    bool _channelReady;
    bool _agentConnected;
    bool _appsLoaded;
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
    _bootTitleValue = nil;
    _bootDetailValue = nil;
    _bootStageValue = nil;
    _bootSpinner = nil;
    _createMachineButton = nil;
    _refreshAppsButton = nil;
    _openApplicationButton = nil;
    _appsCollection = nil;
    _appsStatusValue = nil;
    _appsEmptyValue = nil;
    _apps = [[NSMutableArray alloc] init];
    _appNames = [[NSMutableDictionary alloc] init];
    _appIcons = [[NSMutableDictionary alloc] init];
    _statusItem = nil;
    _shuttingDown.store(false, std::memory_order_relaxed);
    _runtimeShutdownComplete.store(false, std::memory_order_relaxed);
    _doorbellShutdown.store(true, std::memory_order_relaxed);
    _qemuGeneration.store(0, std::memory_order_relaxed);
    for (auto& pending : _framePresentationPending) {
        pending.store(false, std::memory_order_relaxed);
    }
    _qemuPid.store(-1, std::memory_order_relaxed);
    _channelReady = false;
    _agentConnected = false;
    _appsLoaded = false;
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

// True once termination has begun. The runtime channel objects
// (_frameConsumer, _guestControlClient, ...) are deleted on a background
// thread from that point, so main-thread entry points that dereference them
// must bail out early instead.
- (BOOL)isShuttingDown {
    return _shuttingDown.load(std::memory_order_acquire);
}

- (void)windowWillClose:(NSNotification*)notification {
    if ([self isShuttingDown]) {
        return;  // hideWindowsForTermination already tore the entries down
    }
    NSWindow* window = [notification object];
    for (auto it = _displayWindows.begin(); it != _displayWindows.end(); ++it) {
        if (it->second.window != window) {
            continue;
        }
        const uint32_t displayId = it->first;
        NSLog(@"MacMu [diag] windowWillClose displayId=%u suppressed=%d binding=%s", displayId,
              _suppressRemoveOnClose.count(displayId) > 0,
              (_displayAppBindings.count(displayId) ? _displayAppBindings[displayId].c_str()
                                                    : "<none>"));
        [self setDisplayStreaming:displayId enabled:NO];
        [self teardownDisplayWindowEntry:it->second];
        _displayWindows.erase(it);

        const auto suppress = _suppressRemoveOnClose.find(displayId);
        const bool suppressed = suppress != _suppressRemoveOnClose.end();
        if (suppressed) {
            _suppressRemoveOnClose.erase(suppress);
        }
        // Every user-visible window owns one app/display pair. A normal close
        // stops the app and removes its display. A suppressed close came from a
        // guest/display event; only release locally when no host remove request
        // already owns completion for this id.
        if (!suppressed) {
            [self closeBoundAppAndRemoveDisplay:displayId];
        } else {
            [self unbindApplicationFromDisplay:displayId];
            if (_displayRemovalPending.count(displayId) == 0) {
                [self releaseUserDisplayId:displayId];
            }
        }
        break;
    }
}

- (void)setDisplayStreamingForWindow:(NSWindow*)window enabled:(BOOL)enabled {
    if ([self isShuttingDown]) {
        return;
    }
    for (const auto& entry : _displayWindows) {
        if (entry.second.window == window) {
            [self setDisplayStreaming:entry.first enabled:enabled];
            return;
        }
    }
}

- (void)windowDidMiniaturize:(NSNotification*)notification {
    [self setDisplayStreamingForWindow:notification.object enabled:NO];
}

- (void)windowDidDeminiaturize:(NSNotification*)notification {
    [self setDisplayStreamingForWindow:notification.object enabled:YES];
}

- (void)windowDidChangeOcclusionState:(NSNotification*)notification {
    NSWindow* window = notification.object;
    const BOOL visible = (window.occlusionState & NSWindowOcclusionStateVisible) != 0 &&
                         !window.miniaturized;
    [self setDisplayStreamingForWindow:window enabled:visible];
}

- (void)windowDidChangeScreen:(NSNotification*)notification {
    NSWindow* window = notification.object;
    const BOOL visible = (window.occlusionState & NSWindowOcclusionStateVisible) != 0 &&
                         !window.miniaturized;
    // Re-send DISPLAY_STREAM whenever an application window crosses screens;
    // each display is paced to its current NSScreen's maximum refresh rate.
    [self setDisplayStreamingForWindow:window enabled:visible];
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
    NSMenuItem* showItem = [appMenu addItemWithTitle:@"Show Applications"
                                              action:@selector(showStatusWindow:)
                                       keyEquivalent:@"0"];
    showItem.target = self;
    NSMenuItem* refreshItem = [appMenu addItemWithTitle:@"Refresh Applications"
                                                 action:@selector(refreshApps:)
                                          keyEquivalent:@"r"];
    refreshItem.target = self;
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
    NSMenuItem* showItem = [[NSMenuItem alloc] initWithTitle:@"Show Applications"
                                                      action:@selector(showStatusWindow:)
                                               keyEquivalent:@""];
    showItem.target = self;
    [menu addItem:showItem];

    NSMenuItem* refreshItem = [[NSMenuItem alloc] initWithTitle:@"Refresh Applications"
                                                         action:@selector(refreshApps:)
                                                  keyEquivalent:@""];
    refreshItem.target = self;
    [menu addItem:refreshItem];
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
    MacMuAppDelegate* delegate = self;
    if (_guestControlClient->start(_options.guestCtrlSocketPath, [delegate](bool connected) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if ([delegate isShuttingDown]) {
                    return;
                }
                delegate->_agentConnected = connected;
                delegate->_appsLoaded = false;
                if (connected) {
                    NSLog(@"MacMu Android boot completed; agent connected. Loading applications.");
                    [delegate updateBootPresentation:@"Android started"
                                               detail:@"MacMu Agent is ready. Loading applications…"
                                                stage:@"LOADING"
                                                 busy:YES
                                                 ready:NO];
                    [delegate setAppsStatus:@"Loading applications…"];
                    for (const auto& cleanup : delegate->_deferredLaunchCleanup) {
                        NSString* component = ns_string(cleanup.second);
                        [delegate cleanupFailedLaunchForDisplay:cleanup.first
                                                     component:component
                                                       message:@"The previous application launch did not complete"];
                    }
                    [delegate refreshApps:nil];
                } else {
                    [delegate clearApplicationCatalog];
                    NSString* title = [delegate currentQemuPid] > 0
                                          ? @"Reconnecting to Android"
                                          : @"Starting Android";
                    [delegate updateBootPresentation:title
                                               detail:@"Waiting for Android and MacMu Agent…"
                                                stage:@"STARTING"
                                                 busy:YES
                                                 ready:NO];
                    [delegate setAppsStatus:@"Waiting for Android…"];
                }
                [delegate updateApplicationActions];
            });
        })) {
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

    const NSRect frame = NSMakeRect(0, 0, 840, 700);
    _statusWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _statusWindow.title = @"Applications — MacMu";
    _statusWindow.releasedWhenClosed = NO;
    _statusWindow.delegate = self;
    _statusWindow.minSize = NSMakeSize(720, 600);
    _statusWindow.backgroundColor = [NSColor windowBackgroundColor];

    NSView* content = [[NSView alloc] initWithFrame:frame];
    _statusWindow.contentView = content;

    // --- Title region ----------------------------------------------------
    NSTextField* title = make_label(@"Applications", NSMakeRect(24, 654, 420, 30));
    title.font = [NSFont systemFontOfSize:26.0 weight:NSFontWeightBold];
    title.textColor = [NSColor labelColor];
    title.autoresizingMask = NSViewMinYMargin;
    [content addSubview:title];

    NSTextField* subtitle =
        make_label(@"Android apps, each in its own Mac window", NSMakeRect(26, 630, 520, 18));
    subtitle.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightRegular];
    subtitle.textColor = [NSColor secondaryLabelColor];
    subtitle.autoresizingMask = NSViewMinYMargin;
    [content addSubview:subtitle];

    // --- Boot card -------------------------------------------------------
    const CGFloat cardX = 20.0;
    const CGFloat cardW = frame.size.width - cardX * 2.0;
    NSView* bootCard = make_card(NSMakeRect(cardX, 514, cardW, 104));
    bootCard.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:bootCard];

    _bootSpinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 35, 32, 32)];
    _bootSpinner.style = NSProgressIndicatorStyleSpinning;
    _bootSpinner.controlSize = NSControlSizeRegular;
    _bootSpinner.displayedWhenStopped = YES;
    [_bootSpinner startAnimation:nil];
    [bootCard addSubview:_bootSpinner];

    _bootTitleValue = make_label(@"Checking Android environment", NSMakeRect(68, 57, 560, 23));
    _bootTitleValue.font = [NSFont systemFontOfSize:16.0 weight:NSFontWeightSemibold];
    _bootTitleValue.textColor = [NSColor labelColor];
    _bootTitleValue.autoresizingMask = NSViewWidthSizable;
    [bootCard addSubview:_bootTitleValue];

    _bootDetailValue = make_label(@"Preparing MacMu to start…", NSMakeRect(68, 31, 580, 20));
    _bootDetailValue.font = [NSFont systemFontOfSize:12.0 weight:NSFontWeightRegular];
    _bootDetailValue.textColor = [NSColor secondaryLabelColor];
    _bootDetailValue.autoresizingMask = NSViewWidthSizable;
    [bootCard addSubview:_bootDetailValue];

    _bootStageValue = make_label(@"STARTING", NSMakeRect(cardW - 116, 62, 96, 16));
    _bootStageValue.font = [NSFont systemFontOfSize:10.0 weight:NSFontWeightSemibold];
    _bootStageValue.textColor = [NSColor tertiaryLabelColor];
    _bootStageValue.alignment = NSTextAlignmentRight;
    _bootStageValue.autoresizingMask = NSViewMinXMargin;
    [bootCard addSubview:_bootStageValue];

    // --- Applications grid -----------------------------------------------
    // The collection view mirrors Finder's Applications icon view. The list
    // remains the primary surface while the compact boot card above changes
    // state from Android startup through agent/application discovery.
    const CGFloat appsCardY = 76.0;
    const CGFloat appsCardH = 426.0;
    NSView* appsCard = make_card(NSMakeRect(cardX, appsCardY, cardW, appsCardH));
    appsCard.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:appsCard];

    NSTextField* appsHeader =
        make_label(@"APPLICATIONS", NSMakeRect(16, appsCardH - 28, 160, 16));
    appsHeader.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightSemibold];
    appsHeader.textColor = [NSColor tertiaryLabelColor];
    appsHeader.autoresizingMask = NSViewMinYMargin;
    [appsCard addSubview:appsHeader];

    _appsStatusValue = make_value(@"Waiting for Android…",
                                  NSMakeRect(cardW - 330, appsCardH - 28, 314, 16));
    _appsStatusValue.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightRegular];
    _appsStatusValue.textColor = [NSColor tertiaryLabelColor];
    _appsStatusValue.alignment = NSTextAlignmentRight;
    _appsStatusValue.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    [appsCard addSubview:_appsStatusValue];

    NSScrollView* scroll =
        [[NSScrollView alloc] initWithFrame:NSMakeRect(8, 8, cardW - 16, appsCardH - 46)];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;

    NSCollectionViewFlowLayout* layout = [[NSCollectionViewFlowLayout alloc] init];
    layout.itemSize = NSMakeSize(116, 120);
    layout.sectionInset = NSEdgeInsetsMake(16, 16, 16, 16);
    layout.minimumInteritemSpacing = 8.0;
    layout.minimumLineSpacing = 10.0;

    _appsCollection = [[MacMuApplicationsCollectionView alloc] initWithFrame:scroll.bounds];
    _appsCollection.collectionViewLayout = layout;
    _appsCollection.dataSource = self;
    _appsCollection.delegate = self;
    _appsCollection.selectable = YES;
    _appsCollection.allowsEmptySelection = YES;
    _appsCollection.allowsMultipleSelection = NO;
    _appsCollection.backgroundColors = @[ [NSColor clearColor] ];
    _appsCollection.activationTarget = self;
    _appsCollection.activationAction = @selector(openSelectedApplication:);
    [_appsCollection registerClass:[MacMuApplicationItem class]
            forItemWithIdentifier:kApplicationItemIdentifier];

    NSMenu* appsMenu = [[NSMenu alloc] initWithTitle:@"Applications"];
    NSMenuItem* openItem = [appsMenu addItemWithTitle:@"Open"
                                               action:@selector(openSelectedApplication:)
                                        keyEquivalent:@""];
    openItem.target = self;
    NSMenuItem* openAsItem = [appsMenu addItemWithTitle:@"Open with Window Size"
                                                 action:nil
                                          keyEquivalent:@""];
    NSMenu* ratioMenu = [[NSMenu alloc] initWithTitle:@"Open with Window Size"];
    for (size_t i = 0; i < kDisplayLaunchProfileCount; ++i) {
        NSMenuItem* item =
            [ratioMenu addItemWithTitle:[NSString stringWithUTF8String:kDisplayLaunchProfiles[i].title]
                                 action:@selector(launchAppWithProfile:)
                          keyEquivalent:@""];
        item.target = self;
        item.representedObject = @(i);
    }
    [appsMenu setSubmenu:ratioMenu forItem:openAsItem];
    _appsCollection.menu = appsMenu;
    scroll.documentView = _appsCollection;
    [appsCard addSubview:scroll];

    _appsEmptyValue = make_label(@"Waiting for Android to finish starting…",
                                 NSMakeRect(60, appsCardH / 2.0 - 12, cardW - 120, 24));
    _appsEmptyValue.font = [NSFont systemFontOfSize:14.0 weight:NSFontWeightRegular];
    _appsEmptyValue.textColor = [NSColor secondaryLabelColor];
    _appsEmptyValue.alignment = NSTextAlignmentCenter;
    _appsEmptyValue.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin | NSViewMaxYMargin;
    [appsCard addSubview:_appsEmptyValue];

    // --- Bottom button row ----------------------------------------------
    NSBox* separator = [[NSBox alloc] initWithFrame:NSMakeRect(cardX, 66, cardW, 1)];
    separator.boxType = NSBoxSeparator;
    separator.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    [content addSubview:separator];

    _refreshAppsButton = make_button(@"Refresh", @selector(refreshApps:), self,
                                     NSMakeRect(cardX, 20, 110, 32));
    _refreshAppsButton.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
    _refreshAppsButton.enabled = NO;
    [content addSubview:_refreshAppsButton];

    _createMachineButton = make_button(@"Import Image…", @selector(prepareDevice:), self,
                                       NSMakeRect(cardX + 120, 20, 140, 32));
    _createMachineButton.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
    [content addSubview:_createMachineButton];

    _openApplicationButton =
        make_button(@"Open", @selector(openSelectedApplication:), self,
                    NSMakeRect(cardX + cardW - 110, 20, 110, 32));
    _openApplicationButton.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    _openApplicationButton.keyEquivalent = @"\r";
    _openApplicationButton.enabled = NO;
    [content addSubview:_openApplicationButton];

    [_statusWindow center];
}

- (void)showStatusWindow:(id)sender {
    [self createStatusWindow];
    [_statusWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

// Idempotent: brings an application's virtual-display window to front,
// creating it if needed. Main thread only.
- (void)openDisplayWindowForDisplay:(uint32_t)displayId {
    auto profile = _displayLaunchProfiles.find(displayId);
    if (profile != _displayLaunchProfiles.end()) {
        [self openDisplayWindowForDisplay:displayId
                              aspectWidth:profile->second.width
                             aspectHeight:profile->second.height
                                     dpi:profile->second.dpi];
        return;
    }
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
    (void)dpi;
    if ([self isShuttingDown] || displayId == 0 || displayId > kMaxUserDisplayId ||
        _displayAppBindings.find(displayId) == _displayAppBindings.end()) {
        return;
    }
    const std::string packageName =
        package_from_component(_displayAppBindings.find(displayId)->second);
    if (_closingAppPackages.count(packageName) > 0 ||
        _displayRemovalPending.count(displayId) > 0) {
        // A queued frame or late DISPLAY_ADDED event must not resurrect a
        // window after the user has closed it.
        return;
    }
    if (!_channelReady || !_frameConsumer || !_frameConsumer->valid() || !_metalDevice) {
        NSBeep();
        return;
    }
    auto existing = _displayWindows.find(displayId);
    if (existing != _displayWindows.end()) {
        if (aspectWidth > 0 && aspectHeight > 0) {
            [existing->second.window setContentAspectRatio:NSMakeSize(aspectWidth, aspectHeight)];
        }
        [existing->second.window makeKeyAndOrderFront:nil];
        [self setDisplayStreaming:displayId enabled:YES];
        [NSApp activateIgnoringOtherApps:YES];
        return;
    }

    NSSize initialSize;
    if (aspectWidth > 0 && aspectHeight > 0) {
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
    window.title = @"Opening Application…";
    window.releasedWhenClosed = NO;
    window.delegate = self;
    // Lock the window's content aspect ratio to the request immediately; the
    // renderer also reasserts this on the first frame, but setting it now
    // keeps the resize handle honest before pixels arrive.
    if (aspectWidth > 0 && aspectHeight > 0) {
        [window setContentAspectRatio:NSMakeSize(aspectWidth, aspectHeight)];
    }
    [window center];

    MTKView* view =
        macmu_input_view_create(frame, _metalDevice, _inputSender, _guestInputSender);
    view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    view.clearColor = MTLClearColorMake(0.03, 0.03, 0.035, 1.0);
    view.preferredFramesPerSecond =
        static_cast<NSInteger>(maximum_frames_per_second(window.screen));
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
    if (displayId <= kMaxUserDisplayId) {
        _activeUserDisplayIds.insert(displayId);
    }
    [self updateApplicationWindowTitleForDisplay:displayId];

    // Export is demand-driven; only visible application displays subscribe.
    [self setDisplayStreaming:displayId enabled:YES];

    [self startDoorbellThreadIfNeeded];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)setDisplayStreaming:(uint32_t)displayId enabled:(BOOL)enabled {
    auto channel = [self controlChannel];
    if (!channel || !channel->ready()) {
        return;
    }
    uint32_t maximumFramesPerSecond = 60;
    auto display = _displayWindows.find(displayId);
    if (display != _displayWindows.end()) {
        maximumFramesPerSecond = maximum_frames_per_second(display->second.window.screen);
        display->second.view.preferredFramesPerSecond =
            static_cast<NSInteger>(maximumFramesPerSecond);
    }
    macmu::ControlDisplayStream request = {
        displayId,
        enabled ? 1u : 0u,
        maximumFramesPerSecond,
    };
    channel->request(macmu::ControlMessageType::kDisplayStream, &request, sizeof(request), 5000,
                     [displayId, maximumFramesPerSecond, enabled](ControlChannel::Response response) {
                         if (!response.ok) {
                             NSLog(@"MacMu display %u streaming toggle failed: %s", displayId,
                                   response.errorMessage.c_str());
                         } else if (enabled) {
                             NSLog(@"MacMu display %u streaming at screen maximum %u Hz", displayId,
                                   maximumFramesPerSecond);
                         }
                     });
}

// Main thread only. A VirtualDisplay belongs to one QEMU process
// generation; keeping its window/id across a backend restart would make the
// shell send input and control requests to a display that no longer exists.
- (void)resetSecondaryDisplayStateAfterQemuExit {
    for (auto& entry : _displayWindows) {
        macmu_input_view_reset_state(entry.second.view);
    }
    for (auto it = _displayWindows.begin(); it != _displayWindows.end();) {
        NSWindow* window = it->second.window;
        window.delegate = nil;
        [self teardownDisplayWindowEntry:it->second];
        [window orderOut:nil];
        [window close];
        it = _displayWindows.erase(it);
    }
    _suppressRemoveOnClose.clear();
    _displayAppBindings.clear();
    _appDisplayBindings.clear();
    _pendingAppPackages.clear();
    _launchRequestsInFlight.clear();
    _closingAppPackages.clear();
    _closingLaunchFailures.clear();
    _deferredLaunchCleanup.clear();
    _displayLaunchProfiles.clear();
    _displayRemovalPending.clear();
    _activeUserDisplayIds.clear();
    [_appsCollection reloadData];
    [self updateApplicationsSummary];
}

- (void)restoreDisplaySubscriptions {
    for (const auto& entry : _displayWindows) {
        NSWindow* window = entry.second.window;
        const BOOL visible = window && !window.miniaturized &&
                             (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
        if (visible) {
            [self setDisplayStreaming:entry.first enabled:YES];
        }
    }
}

- (void)quitFromStatusItem:(id)sender {
    [NSApp terminate:nil];
}

- (uint32_t)allocateUserDisplayId {
    for (uint32_t candidate = 1; candidate <= kMaxUserDisplayId; ++candidate) {
        if (_activeUserDisplayIds.find(candidate) == _activeUserDisplayIds.end() &&
            _displayRemovalPending.find(candidate) == _displayRemovalPending.end()) {
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
        if (erased) {
            NSLog(@"MacMu [diag] releaseUserDisplayId(%u) (active now: {%s})", displayId,
                  [self describeActiveIds].UTF8String);
        }
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
        _createMachineButton.hidden = hasSystemImage && hasMachine;
        if (!hasSystemImage) {
            _createMachineButton.title = @"Import Image…";
        } else if (!hasMachine) {
            _createMachineButton.title = @"Prepare Device";
        } else {
            _createMachineButton.title = @"Device Ready";
        }
    }
    if (!_agentConnected) {
        if (!hasSystemImage) {
            [self updateBootPresentation:@"System image required"
                                   detail:@"Import a MacMu Android 16 image to continue."
                                    stage:@"SETUP"
                                     busy:NO
                                     ready:NO];
            [self setAppsStatus:@"Android image required"];
        } else if (!hasMachine) {
            [self updateBootPresentation:@"Preparing Android device"
                                   detail:@"Creating the managed MacMu virtual device…"
                                    stage:@"PREPARING"
                                     busy:YES
                                     ready:NO];
        } else if (!qemuRunning) {
            [self updateBootPresentation:@"Starting Android"
                                   detail:@"Launching the emulator core…"
                                    stage:@"STARTING"
                                     busy:YES
                                     ready:NO];
        }
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
                task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/ditto"];
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
                    [delegate publishBootPresentation:@"System image imported"
                                                detail:@"The Android device is ready. Starting MacMu…"
                                                 stage:@"STARTING"
                                                  busy:YES
                                                 ready:NO];
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
        [self publishBootPresentation:@"Android device prepared"
                                detail:@"Starting the emulator core…"
                                 stage:@"STARTING"
                                  busy:YES
                                 ready:NO];
        return;
    }
    [self publishQemuStatus:ns_string(error)];
    NSLog(@"MacMu machine creation failed: %s", error.c_str());
    [self updateMachineControls];
}

- (void)updateBootPresentation:(NSString*)title
                         detail:(NSString*)detail
                          stage:(NSString*)stage
                           busy:(BOOL)busy
                          ready:(BOOL)ready {
    if (_bootTitleValue) {
        _bootTitleValue.stringValue = title ?: @"MacMu";
    }
    if (_bootDetailValue) {
        _bootDetailValue.stringValue = detail ?: @"";
    }
    if (_bootStageValue) {
        _bootStageValue.stringValue = stage ?: @"";
        _bootStageValue.textColor = ready ? [NSColor systemGreenColor]
                                          : [NSColor tertiaryLabelColor];
    }
    if (_bootSpinner) {
        _bootSpinner.hidden = !busy;
        if (busy) {
            [_bootSpinner startAnimation:nil];
        } else {
            [_bootSpinner stopAnimation:nil];
        }
    }
    if (_statusItem.button) {
        _statusItem.button.title = ready ? @"MacMu: Ready" : (busy ? @"MacMu: Starting" : @"MacMu");
    }
}

- (void)publishBootPresentation:(NSString*)title
                          detail:(NSString*)detail
                           stage:(NSString*)stage
                            busy:(BOOL)busy
                           ready:(BOOL)ready {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self updateBootPresentation:title detail:detail stage:stage busy:busy ready:ready];
    });
}

// Compatibility helper for setup/import errors that only carry one message.
- (void)publishQemuStatus:(NSString*)text {
    const BOOL busy = [text hasPrefix:@"Importing"] || [text hasPrefix:@"Preparing"] ||
                      [text hasPrefix:@"Starting"] || [text hasPrefix:@"Exited"];
    [self publishBootPresentation:text
                           detail:busy ? @"This may take a moment…" : @"Check the MacMu setup and try again."
                            stage:busy ? @"WORKING" : @"ATTENTION"
                             busy:busy
                            ready:NO];
}

- (NSString*)applicationNameForPackage:(const std::string&)packageName {
    NSString* package = ns_string(packageName);
    NSString* name = _appNames[package];
    return name.length > 0 ? name : package;
}

- (void)updateApplicationWindowTitleForDisplay:(uint32_t)displayId {
    auto window = _displayWindows.find(displayId);
    auto binding = _displayAppBindings.find(displayId);
    if (window == _displayWindows.end() || binding == _displayAppBindings.end() ||
        !window->second.window) {
        return;
    }
    const std::string packageName = package_from_component(binding->second);
    window->second.window.title = [self applicationNameForPackage:packageName];
}

- (void)updateApplicationsSummary {
    if (!_appsLoaded) {
        return;
    }
    const NSUInteger total = _apps.count;
    const size_t opening = _pendingAppPackages.size();
    const size_t closing = _closingAppPackages.size();
    const size_t transitional = opening + closing;
    const size_t running = _appDisplayBindings.size() >= transitional
                               ? _appDisplayBindings.size() - transitional
                               : 0;
    NSString* status = nil;
    if (opening > 0) {
        status = [NSString stringWithFormat:@"%lu applications · %zu opening",
                                            static_cast<unsigned long>(total), opening];
    } else if (closing > 0) {
        status = [NSString stringWithFormat:@"%lu applications · %zu closing",
                                            static_cast<unsigned long>(total), closing];
    } else if (running > 0) {
        status = [NSString stringWithFormat:@"%lu applications · %zu running",
                                            static_cast<unsigned long>(total), running];
    } else {
        status = [NSString stringWithFormat:@"%lu applications",
                                            static_cast<unsigned long>(total)];
    }
    [self setAppsStatus:status];
}

- (void)updateApplicationActions {
    const BOOL hasSelection = _appsCollection.selectionIndexPaths.count > 0;
    if (_refreshAppsButton) {
        _refreshAppsButton.enabled = _agentConnected;
    }
    if (_openApplicationButton) {
        _openApplicationButton.enabled = _agentConnected && hasSelection;
    }
}

- (void)clearApplicationCatalog {
    [_apps removeAllObjects];
    [_appNames removeAllObjects];
    [_appIcons removeAllObjects];
    [_appsCollection reloadData];
    _appsEmptyValue.hidden = NO;
    [self updateApplicationActions];
}

- (void)unbindApplicationFromDisplay:(uint32_t)displayId {
    auto binding = _displayAppBindings.find(displayId);
    if (binding == _displayAppBindings.end()) {
        return;
    }
    const std::string packageName = package_from_component(binding->second);
    auto reverse = _appDisplayBindings.find(packageName);
    if (reverse != _appDisplayBindings.end() && reverse->second == displayId) {
        _appDisplayBindings.erase(reverse);
    }
    _pendingAppPackages.erase(packageName);
    _launchRequestsInFlight.erase(packageName);
    _closingAppPackages.erase(packageName);
    _closingLaunchFailures.erase(displayId);
    _deferredLaunchCleanup.erase(displayId);
    _displayLaunchProfiles.erase(displayId);
    _displayAppBindings.erase(binding);
    [_appsCollection reloadData];
    [self updateApplicationsSummary];
    [self updateApplicationActions];
}

- (void)rollbackApplicationDisplay:(uint32_t)displayId message:(NSString*)message {
    [self unbindApplicationFromDisplay:displayId];
    [self setAppsStatus:message];
    [self requestDisplayRemove:displayId];
    auto window = _displayWindows.find(displayId);
    if (window != _displayWindows.end()) {
        _suppressRemoveOnClose[displayId] = true;
        [window->second.window close];
    }
}

#pragma mark - Control channel

- (std::shared_ptr<ControlChannel>)controlChannel {
    std::lock_guard<std::mutex> lock(_controlMutex);
    return _controlChannel;
}

- (void)handleControlEvent:(uint16_t)type payload:(std::vector<uint8_t>)payload {
    if ([self isShuttingDown]) {
        return;
    }
    if (type != static_cast<uint16_t>(macmu::ControlMessageType::kEventDisplay) ||
        payload.size() < sizeof(macmu::ControlEventDisplay)) {
        return;
    }
    macmu::ControlEventDisplay event;
    std::memcpy(&event, payload.data(), sizeof(event));
    if (event.displayId == 0 || event.displayId > kMaxUserDisplayId) {
        return;
    }
    if (event.state == macmu::kControlDisplayAdded) {
        if (_displayAppBindings.find(event.displayId) == _displayAppBindings.end()) {
            // MacMu never exposes blank displays. Tear down an unexpected
            // guest-created surface instead of opening an unbound window.
            NSLog(@"MacMu ignoring unbound display %u", event.displayId);
            [self requestDisplayRemove:event.displayId];
            return;
        }
        [self openDisplayWindowForDisplay:event.displayId
                              aspectWidth:event.width
                             aspectHeight:event.height
                                     dpi:event.dpi];
        return;
    }
    if (event.state == macmu::kControlDisplayRemoved) {
        // The receiver writes DISPLAY_REMOVE_OK followed by this event on the
        // same stream. Keep the id reserved through the ACK and let the event
        // own release, preventing a stale remove event from hitting a newly
        // reused application slot.
        _displayRemovalPending.erase(event.displayId);
        [self unbindApplicationFromDisplay:event.displayId];
        auto it = _displayWindows.find(event.displayId);
        if (it != _displayWindows.end()) {
            _suppressRemoveOnClose[event.displayId] = true;
            [it->second.window close];
        } else {
            // There is no window-close path to release the slot.
            [self releaseUserDisplayId:event.displayId];
        }
    }
}

- (void)requestDisplayRemove:(uint32_t)displayId {
    if (displayId == 0 || displayId > kMaxUserDisplayId ||
        _displayRemovalPending.count(displayId) > 0) {
        return;
    }
    _displayRemovalPending.insert(displayId);
    auto channel = [self controlChannel];
    if (!channel || !channel->alive()) {
        // Only a confirmed QEMU exit proves the guest-side display is gone.
        // If QEMU is still alive, retain this id as a tombstone so it cannot be
        // reused against an orphaned display; the generation reset will clear
        // it when the backend exits.
        if ([self currentQemuPid] <= 0) {
            _displayRemovalPending.erase(displayId);
            [self unbindApplicationFromDisplay:displayId];
            [self releaseUserDisplayId:displayId];
        } else {
            NSLog(@"MacMu display %u removal deferred until control reconnect or QEMU reset",
                  displayId);
        }
        return;
    }
    macmu::ControlDisplayRemove request = {displayId};
    channel->request(macmu::ControlMessageType::kDisplayRemove, &request, sizeof(request), 5000,
                     [displayId](ControlChannel::Response response) {
                         if (!response.ok) {
                             NSLog(@"MacMu display %u remove failed: %s", displayId,
                                   response.errorMessage.c_str());
                             // An error or timeout cannot prove that the guest
                             // display is absent. Keep the tombstone reserved;
                             // either a late DISPLAY_REMOVED event or the next
                             // QEMU generation reset will release it safely.
                         }
                         // On success the ordered DISPLAY_REMOVED event owns
                         // cleanup and release. Do not make the id reusable at
                         // the ACK boundary.
                     });
}

- (void)closeBoundAppAndRemoveDisplay:(uint32_t)displayId {
    if ([self isShuttingDown]) {
        return;
    }
    auto binding = _displayAppBindings.find(displayId);
    if (binding == _displayAppBindings.end()) {
        [self requestDisplayRemove:displayId];
        return;
    }

    const std::string component = binding->second;
    const std::string packageName = package_from_component(component);
    // Closing the window during the short DISPLAY_ADD -> launch handoff is a
    // launch cancellation. Clear pending before the delayed block runs so it
    // cannot start the task after close/remove has already begun.
    const bool wasPending = _pendingAppPackages.erase(packageName) > 0;
    const bool launchWasSent = _launchRequestsInFlight.erase(packageName) > 0;
    _closingLaunchFailures.erase(displayId);
    _closingAppPackages.insert(packageName);
    [_appsCollection reloadData];
    [self updateApplicationActions];
    [self updateApplicationsSummary];
    if (wasPending && !launchWasSent) {
        // The task was never started, so there is nothing to stop in Android.
        // Remove only the reserved display and let its completion event unbind.
        [self requestDisplayRemove:displayId];
        return;
    }
    if (!_guestControlClient || !_guestControlClient->ready()) {
        // Keep the application/display binding intact. Removing its display
        // without stopping the task can reparent it onto hidden display 0.
        NSLog(@"MacMu display %u app close deferred; guest control is not connected.", displayId);
        _closingAppPackages.erase(packageName);
        [self openDisplayWindowForDisplay:displayId];
        [_appsCollection reloadData];
        [self updateApplicationActions];
        [self setAppsStatus:@"Android agent disconnected; the application is still open"];
        return;
    }

    std::string command = "close ";
    command += component;
    command += " " + std::to_string(displayId);
    MacMuAppDelegate* delegate = self;
    _guestControlClient->request(command, 5000,
                                 [delegate, displayId, component, packageName](bool ok,
                                                                               std::string payload) {
                                     if (!ok) {
                                         NSLog(@"MacMu display %u bound app close failed: %s",
                                               displayId, payload.c_str());
                                     }
                                     dispatch_async(dispatch_get_main_queue(), ^{
                                         auto binding = delegate->_displayAppBindings.find(displayId);
                                         if (binding == delegate->_displayAppBindings.end() ||
                                             binding->second != component) {
                                             return;
                                         }
                                         if (!ok) {
                                             auto launchFailure =
                                                 delegate->_closingLaunchFailures.find(displayId);
                                             if (launchFailure !=
                                                 delegate->_closingLaunchFailures.end()) {
                                                 // The launch itself definitively failed, so
                                                 // this is an empty display even though close
                                                 // also failed. Remove it instead of reopening
                                                 // a blank window as a running application.
                                                 const std::string message = launchFailure->second;
                                                 [delegate setAppsStatus:ns_string(
                                                                             "Could not open application: " +
                                                                             message)];
                                                 [delegate requestDisplayRemove:displayId];
                                                 return;
                                             }
                                             // Preserve and re-show the display rather than
                                             // orphaning a task on Android's hidden display 0.
                                             delegate->_closingAppPackages.erase(packageName);
                                             [delegate openDisplayWindowForDisplay:displayId];
                                             [delegate->_appsCollection reloadData];
                                             [delegate updateApplicationActions];
                                             [delegate setAppsStatus:ns_string(
                                                                         "Could not close application: " +
                                                                         payload)];
                                             return;
                                         }
                                         [delegate requestDisplayRemove:displayId];
                                     });
                                 });
}

// Main thread only.
- (BOOL)hasDisplayWindow:(uint32_t)displayId {
    return _displayWindows.find(displayId) != _displayWindows.end();
}

#pragma mark - Applications

- (void)setAppsStatus:(NSString*)status {
    void (^update)(void) = ^{
        if (self->_appsStatusValue) {
            self->_appsStatusValue.stringValue = status;
        }
        if (self->_appsEmptyValue && self->_apps.count == 0) {
            self->_appsEmptyValue.stringValue = status;
            self->_appsEmptyValue.hidden = NO;
        }
    };
    if ([NSThread isMainThread]) {
        update();
    } else {
        dispatch_async(dispatch_get_main_queue(), update);
    }
}

- (void)refreshApps:(id)sender {
    if ([self isShuttingDown]) {
        return;
    }
    if (!_guestControlClient || !_guestControlClient->ready()) {
        [self setAppsStatus:@"Waiting for Android…"];
        return;
    }
    [self setAppsStatus:@"Loading applications…"];
    [self updateBootPresentation:@"Android started"
                           detail:@"Discovering installed applications…"
                            stage:@"LOADING"
                             busy:YES
                             ready:NO];
    MacMuAppDelegate* delegate = self;
    _guestControlClient->request("apps", 15000, [delegate](bool ok, std::string payload) {
        if (!ok) {
            [delegate setAppsStatus:ns_string("Could not load applications: " + payload)];
            [delegate publishBootPresentation:@"Android started"
                                        detail:@"Application discovery failed. Use Refresh to retry."
                                         stage:@"ATTENTION"
                                          busy:NO
                                         ready:NO];
            return;
        }
        NSData* data = [NSData dataWithBytes:payload.data() length:payload.size()];
        NSError* error = nil;
        id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
        if (![parsed isKindOfClass:[NSArray class]]) {
            [delegate setAppsStatus:@"Could not read the application list"];
            [delegate publishBootPresentation:@"Android started"
                                        detail:@"The application list was malformed. Use Refresh to retry."
                                         stage:@"ATTENTION"
                                          busy:NO
                                         ready:NO];
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
    NSMutableSet<NSString*>* seenPackages = [NSMutableSet set];
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
        // Applications are package-scoped. A package can expose more than one
        // launcher activity, but it still receives one tile and one display.
        if ([seenPackages containsObject:pkg]) {
            continue;
        }
        [seenPackages addObject:pkg];
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
    _appsLoaded = true;
    [_appsCollection reloadData];
    for (const auto& binding : _displayAppBindings) {
        [self updateApplicationWindowTitleForDisplay:binding.first];
    }
    _appsEmptyValue.hidden = _apps.count > 0;
    [self updateApplicationsSummary];
    [self updateApplicationActions];
    NSString* detail = _apps.count == 0
                           ? @"Android is ready, but no launcher applications were found."
                           : [NSString stringWithFormat:@"%lu applications are ready to open.",
                                                        static_cast<unsigned long>(_apps.count)];
    [self updateBootPresentation:@"Ready" detail:detail stage:@"READY" busy:NO ready:YES];
    NSLog(@"MacMu application catalog ready (%lu packages).",
          static_cast<unsigned long>(_apps.count));
}

- (NSString*)selectedAppComponent {
    NSIndexPath* indexPath = _appsCollection.selectionIndexPaths.anyObject;
    if (!indexPath || indexPath.item >= _apps.count) {
        return nil;
    }
    NSDictionary* app = _apps[indexPath.item];
    return [NSString stringWithFormat:@"%@/%@", app[@"pkg"], app[@"activity"]];
}

- (void)openSelectedApplication:(id)sender {
    [self launchSelectedApplicationWithProfile:
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
    [self launchSelectedApplicationWithProfile:kDisplayLaunchProfiles[index]];
}

- (void)launchSelectedApplicationWithProfile:(DisplayLaunchProfile)profile {
    if ([self isShuttingDown]) {
        return;
    }
    NSString* component = [self selectedAppComponent];
    if (!component) {
        NSBeep();
        return;
    }
    const std::string componentValue = [component UTF8String];
    const std::string packageName = package_from_component(componentValue);
    auto existingApp = _appDisplayBindings.find(packageName);
    if (existingApp != _appDisplayBindings.end()) {
        const uint32_t existingDisplayId = existingApp->second;
        NSString* name = [self applicationNameForPackage:packageName];
        if (_closingAppPackages.count(packageName) > 0 ||
            _displayRemovalPending.count(existingDisplayId) > 0) {
            [self setAppsStatus:[NSString stringWithFormat:@"Closing %@…", name]];
            return;
        }
        auto window = _displayWindows.find(existingDisplayId);
        if (window != _displayWindows.end()) {
            [window->second.window makeKeyAndOrderFront:nil];
            [self setDisplayStreaming:existingDisplayId enabled:YES];
            [NSApp activateIgnoringOtherApps:YES];
        }
        [self setAppsStatus:_pendingAppPackages.count(packageName) > 0
                                ? [NSString stringWithFormat:@"Opening %@…", name]
                                : [NSString stringWithFormat:@"%@ is already open", name]];
        return;
    }
    if (!_agentConnected) {
        [self setAppsStatus:@"Waiting for Android…"];
        return;
    }
    auto channel = [self controlChannel];
    if (!channel || !channel->alive()) {
        [self setAppsStatus:@"Control channel not connected"];
        return;
    }
    // Reserve both sides of the package/display bijection before DISPLAY_ADD.
    // This closes the double-click race and lets display-added events know the
    // new surface belongs to an application rather than a blank display.
    const uint32_t displayId = [self allocateUserDisplayId];
    if (displayId == 0) {
        [self setAppsStatus:@"Up to 5 applications can run at the same time"];
        NSBeep();
        return;
    }
    _displayAppBindings[displayId] = componentValue;
    _appDisplayBindings[packageName] = displayId;
    _pendingAppPackages.insert(packageName);
    _displayLaunchProfiles[displayId] = profile;
    [_appsCollection reloadData];
    [self updateApplicationActions];
    [self setAppsStatus:[NSString stringWithFormat:@"Opening %@…",
                                                   [self applicationNameForPackage:packageName]]];
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
        [delegate, component, packageName, displayId, aspectWidth, aspectHeight, aspectDpi](
            ControlChannel::Response response) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!response.ok) {
                    NSString* message = ns_string("Could not open application: " +
                                                  response.errorMessage);
                    if (response.type ==
                        static_cast<uint16_t>(macmu::ControlMessageType::kError)) {
                        // A direct QEMU error is authoritative: DISPLAY_ADD did
                        // not apply, so this reservation can be released without
                        // waiting for a remove event.
                        [delegate unbindApplicationFromDisplay:displayId];
                        [delegate releaseUserDisplayId:displayId];
                        [delegate setAppsStatus:message];
                    } else {
                        // Timeout/channel failure is ambiguous; issue a remove
                        // and retain the id until its completion event.
                        [delegate rollbackApplicationDisplay:displayId message:message];
                    }
                    return;
                }
                if (response.payload.size() < sizeof(macmu::ControlDisplayAddOk)) {
                    [delegate rollbackApplicationDisplay:displayId
                                                  message:@"Could not read the application window response"];
                    return;
                }
                macmu::ControlDisplayAddOk ok;
                std::memcpy(&ok, response.payload.data(), sizeof(ok));
                auto reserved = delegate->_appDisplayBindings.find(packageName);
                if (reserved == delegate->_appDisplayBindings.end() ||
                    reserved->second != displayId) {
                    // The user closed this launch while DISPLAY_ADD was in
                    // flight. Remove the accepted surface without surfacing an
                    // error for an intentional close.
                    [delegate requestDisplayRemove:ok.displayId];
                    return;
                }
                if (ok.displayId != displayId) {
                    [delegate rollbackApplicationDisplay:displayId
                                                  message:@"Could not reserve an application window"];
                    [delegate requestDisplayRemove:ok.displayId];
                    return;
                }
                NSLog(@"MacMu: opening %@ on virtual display %u (%ux%u)", component, ok.displayId,
                      aspectWidth, aspectHeight);
                [delegate openDisplayWindowForDisplay:ok.displayId
                                          aspectWidth:aspectWidth
                                         aspectHeight:aspectHeight
                                                 dpi:aspectDpi];
                if (![delegate hasDisplayWindow:ok.displayId]) {
                    [delegate rollbackApplicationDisplay:ok.displayId
                                                  message:@"Application window is unavailable"];
                    return;
                }
                [delegate updateApplicationWindowTitleForDisplay:ok.displayId];
                // Give the guest a brief moment to register the new
                // VirtualDisplay, then launch. The agent also waits on the
                // display internally, so a short delay here is enough; the
                // previous 1.5 s blind wait made launches feel sluggish.
                dispatch_after(
                    dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.2 * NSEC_PER_SEC)),
                    dispatch_get_main_queue(), ^{
                        auto binding = delegate->_displayAppBindings.find(ok.displayId);
                        if (binding != delegate->_displayAppBindings.end() &&
                            binding->second == [component UTF8String] &&
                            delegate->_pendingAppPackages.count(packageName) > 0 &&
                            delegate->_closingAppPackages.count(packageName) == 0) {
                            delegate->_launchRequestsInFlight.insert(packageName);
                            [delegate launchComponent:component onDisplay:ok.displayId];
                        }
                    });
            });
        });
}

- (void)cleanupFailedLaunchForDisplay:(uint32_t)displayId
                            component:(NSString*)component
                              message:(NSString*)message {
    if ([self isShuttingDown] || !component) {
        return;
    }
    const std::string componentValue = component.UTF8String;
    auto binding = _displayAppBindings.find(displayId);
    if (binding == _displayAppBindings.end() || binding->second != componentValue) {
        return;
    }

    const std::string packageName = package_from_component(componentValue);
    _pendingAppPackages.erase(packageName);
    _launchRequestsInFlight.erase(packageName);
    _closingAppPackages.insert(packageName);
    _deferredLaunchCleanup[displayId] = componentValue;
    [_appsCollection reloadData];
    [self updateApplicationActions];
    [self updateApplicationsSummary];

    if (!_guestControlClient || !_guestControlClient->ready()) {
        [self setAppsStatus:[NSString stringWithFormat:@"%@; cleanup will resume after Android reconnects",
                                                       message]];
        return;
    }

    std::string command = "close ";
    command += componentValue;
    command += " " + std::to_string(displayId);
    const std::string messageValue = message.UTF8String;
    MacMuAppDelegate* delegate = self;
    _guestControlClient->request(
        command, 5000,
        [delegate, displayId, componentValue, packageName, messageValue](bool ok,
                                                                         std::string payload) {
            dispatch_async(dispatch_get_main_queue(), ^{
                auto cleanup = delegate->_deferredLaunchCleanup.find(displayId);
                auto binding = delegate->_displayAppBindings.find(displayId);
                if (cleanup == delegate->_deferredLaunchCleanup.end() ||
                    cleanup->second != componentValue ||
                    binding == delegate->_displayAppBindings.end() ||
                    binding->second != componentValue) {
                    return;
                }
                if (!ok) {
                    NSLog(@"MacMu display %u failed-launch cleanup failed: %s", displayId,
                          payload.c_str());
                    [delegate setAppsStatus:ns_string(messageValue +
                                                      "; close the application window to retry cleanup")];
                    return;
                }
                delegate->_deferredLaunchCleanup.erase(cleanup);
                [delegate rollbackApplicationDisplay:displayId message:ns_string(messageValue)];
            });
        });
}

- (void)launchComponent:(NSString*)component onDisplay:(uint32_t)displayId {
    if ([self isShuttingDown]) {
        return;
    }
    if (!_guestControlClient || !_guestControlClient->ready()) {
        [self cleanupFailedLaunchForDisplay:displayId
                                  component:component
                                    message:@"Android agent disconnected during launch"];
        return;
    }
    std::string command = "launch ";
    command += [component UTF8String];
    command += " " + std::to_string(displayId);
    MacMuAppDelegate* delegate = self;
    _guestControlClient->request(command, 15000, [delegate, displayId, component](
                                                     bool ok, std::string payload) {
        dispatch_async(dispatch_get_main_queue(), ^{
            const std::string componentValue = [component UTF8String];
            const std::string packageName = package_from_component(componentValue);
            delegate->_launchRequestsInFlight.erase(packageName);
            auto binding = delegate->_displayAppBindings.find(displayId);
            if (binding == delegate->_displayAppBindings.end() ||
                binding->second != componentValue) {
                return;  // the user closed the window while the request ran
            }
            if (delegate->_closingAppPackages.count(packageName) > 0) {
                const bool ambiguousTransportFailure =
                    payload == "guest request timed out" ||
                    payload == "guest agent disconnected" ||
                    payload == "guest control client stopped" ||
                    payload == "guest control write failed" ||
                    payload == "guest agent not connected";
                if (!ok && !ambiguousTransportFailure) {
                    delegate->_closingLaunchFailures[displayId] = payload;
                }
                return;  // the ordered close transaction owns final cleanup
            }
            if (!ok) {
                [delegate cleanupFailedLaunchForDisplay:displayId
                                              component:component
                                                message:ns_string("Could not open application: " +
                                                                  payload)];
                return;
            }
            delegate->_pendingAppPackages.erase(packageName);
            [delegate->_appsCollection reloadData];
            [delegate updateApplicationActions];
            [delegate updateApplicationsSummary];
            [delegate updateApplicationWindowTitleForDisplay:displayId];
        });
    });
}

#pragma mark - Applications collection data source

- (NSInteger)collectionView:(NSCollectionView*)collectionView
      numberOfItemsInSection:(NSInteger)section {
    return static_cast<NSInteger>(_apps.count);
}

- (NSCollectionViewItem*)collectionView:(NSCollectionView*)collectionView
    itemForRepresentedObjectAtIndexPath:(NSIndexPath*)indexPath {
    MacMuApplicationItem* item = (MacMuApplicationItem*)[collectionView
        makeItemWithIdentifier:kApplicationItemIdentifier
                  forIndexPath:indexPath];
    if (indexPath.item >= _apps.count) {
        return item;
    }
    NSDictionary* app = _apps[indexPath.item];
    NSString* package = app[@"pkg"];
    NSString* activity = app[@"activity"];
    NSString* name = _appNames[package] ?: package;
    NSImage* icon = _appIcons[package];
    item.imageView.image = icon ?: placeholder_icon(name, NSMakeSize(64, 64));
    item.textField.stringValue = name;
    item.view.toolTip = [NSString stringWithFormat:@"%@\n%@", package, activity];

    const std::string packageName = package.UTF8String;
    const BOOL bound = _appDisplayBindings.find(packageName) != _appDisplayBindings.end();
    const BOOL opening = _pendingAppPackages.find(packageName) != _pendingAppPackages.end();
    const BOOL closing = _closingAppPackages.find(packageName) != _closingAppPackages.end();
    [item setApplicationRunning:bound && !opening && !closing
                         opening:opening
                         closing:closing];
    return item;
}

- (void)collectionView:(NSCollectionView*)collectionView
    didSelectItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths {
    [self updateApplicationActions];
}

- (void)collectionView:(NSCollectionView*)collectionView
    didDeselectItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths {
    [self updateApplicationActions];
}

#pragma mark - qemu supervisor

- (void)startQemuSupervisor {
    MacMuAppDelegate* delegate = self;
    _qemuMonitorThread = std::thread([delegate] { [delegate qemuMonitorLoop]; });
}

- (void)qemuMonitorLoop {
    while (!_shuttingDown.load(std::memory_order_acquire)) {
        if (!macmu_system_image_exists(_options)) {
            [self publishBootPresentation:@"System image required"
                                    detail:@"Import a MacMu Android 16 image to continue."
                                     stage:@"SETUP"
                                      busy:NO
                                     ready:NO];
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        if (!macmu_machine_exists(_options)) {
            [self publishBootPresentation:@"Preparing Android device"
                                    detail:@"Creating the managed virtual device…"
                                     stage:@"PREPARING"
                                      busy:YES
                                     ready:NO];
            std::string error;
            if (!macmu_create_default_machine(_options, &error)) {
                [self publishBootPresentation:@"Device preparation failed"
                                        detail:ns_string(error)
                                         stage:@"ATTENTION"
                                          busy:NO
                                         ready:NO];
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            MacMuAppDelegate* delegate = self;
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate updateMachineControls];
            });
        }
        [self publishBootPresentation:@"Starting Android"
                                detail:@"Launching the emulator core…"
                                 stage:@"STARTING"
                                  busy:YES
                                 ready:NO];
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
            [self publishBootPresentation:@"Could not start Android"
                                    detail:@"MacMu will retry automatically."
                                     stage:@"RETRYING"
                                      busy:YES
                                     ready:NO];
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        if (_inputSender) {
            _inputSender->set_enabled(true);
        }

        MacMuAppDelegate* delegate = self;
        {
            std::lock_guard<std::mutex> lock(_controlMutex);
            _controlChannel = controlChannel;
        }
        controlChannel->start(
            [delegate](uint16_t type, std::vector<uint8_t> payload) {
                // Reader thread -> main queue.
                auto shared = std::make_shared<std::vector<uint8_t>>(std::move(payload));
                dispatch_async(dispatch_get_main_queue(), ^{
                    [delegate handleControlEvent:type payload:std::move(*shared)];
                });
            },
            [] {},
            [delegate] {
                dispatch_async(dispatch_get_main_queue(), ^{
                    [delegate restoreDisplaySubscriptions];
                    if (!delegate->_agentConnected) {
                        [delegate updateBootPresentation:@"Android is booting"
                                                   detail:@"Waiting for Android to finish startup and launch MacMu Agent…"
                                                    stage:@"BOOTING"
                                                     busy:YES
                                                     ready:NO];
                    }
                });
            });

        _qemuPid.store(pid, std::memory_order_release);
        _qemuGeneration.fetch_add(1, std::memory_order_acq_rel);
        if (_shuttingDown.load(std::memory_order_acquire)) {
            request_qemu_termination(pid);
        } else {
            [self publishBootPresentation:@"Android is booting"
                                    detail:@"Waiting for boot completion and MacMu Agent…"
                                     stage:@"BOOTING"
                                      busy:YES
                                     ready:NO];
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
        if (_inputSender) {
            _inputSender->set_enabled(false);
        }

        pid_t expectedPid = pid;
        if (_qemuPid.compare_exchange_strong(expectedPid, -1, std::memory_order_acq_rel)) {
            _qemuExitCondition.notify_all();
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
        dispatch_sync(dispatch_get_main_queue(), ^{
            delegate->_agentConnected = false;
            delegate->_appsLoaded = false;
            [delegate resetSecondaryDisplayStateAfterQemuExit];
            [delegate clearApplicationCatalog];
            [delegate setAppsStatus:@"Restarting Android…"];
            [delegate updateMachineControls];
        });
        [self publishBootPresentation:@"Restarting Android"
                                detail:@"The emulator core exited; MacMu is starting it again."
                                 stage:@"RESTARTING"
                                  busy:YES
                                 ready:NO];
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
    return _qemuPid.load(std::memory_order_acquire);
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
        if (readyDisplayId == 0) {
            continue;  // Android's boot surface is intentionally never shown.
        }

        const uint32_t displayId = readyDisplayId;
        [self enqueueFramePresentationForDisplay:displayId];
    }
}

- (void)enqueueFramePresentationForDisplay:(uint32_t)displayId {
    if (displayId == 0 || displayId >= kMacmuFrameSlotCount ||
        _framePresentationPending[displayId].exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    MacMuAppDelegate* delegate = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        SurfaceMetadata latest = {};
        uint64_t presentedFrame = 0;
        if (![delegate isShuttingDown] && delegate->_frameConsumer &&
            delegate->_frameConsumer->read(displayId, &latest)) {
            presentedFrame = latest.frame;
            [delegate presentFrameForDisplay:displayId
                                 actualWidth:latest.width
                                actualHeight:latest.height];
        }

        delegate->_framePresentationPending[displayId].store(false,
                                                              std::memory_order_release);

        // A doorbell may have advanced the consumer cursor while this main-
        // queue block was pending. Re-arm exactly once for the newest slot so
        // coalescing cannot lose the final redraw.
        SurfaceMetadata after = {};
        if (![delegate isShuttingDown] && delegate->_frameConsumer &&
            delegate->_frameConsumer->read(displayId, &after) &&
            after.frame != presentedFrame) {
            [delegate enqueueFramePresentationForDisplay:displayId];
        }
    });
}

// Main thread. Marks a bound application's view dirty. Unknown displays and
// Android's internal display 0 never create a product window.
- (void)presentFrameForDisplay:(uint32_t)displayId
                   actualWidth:(uint32_t)actualWidth
                  actualHeight:(uint32_t)actualHeight {
    (void)actualWidth;
    (void)actualHeight;
    if (_shuttingDown.load(std::memory_order_acquire) || displayId == 0 ||
        _displayAppBindings.find(displayId) == _displayAppBindings.end()) {
        return;
    }
    auto it = _displayWindows.find(displayId);
    if (it == _displayWindows.end()) {
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
        NSWindow* window = entry.second.window;
        window.delegate = nil;
        [self teardownDisplayWindowEntry:entry.second];
        [window orderOut:nil];
    }
    _displayWindows.clear();
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
        request_qemu_termination(pid);
        std::unique_lock<std::mutex> lock(_qemuExitMutex);
        const bool exited = _qemuExitCondition.wait_for(
            lock, std::chrono::seconds(5),
            [self, pid] { return self->_qemuPid.load(std::memory_order_acquire) != pid; });
        if (!exited && _qemuPid.load(std::memory_order_acquire) == pid) {
            force_kill_qemu(pid);
        }
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
