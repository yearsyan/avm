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

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
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
#include "image_importer.h"
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

// Use the English source text as the localization key. This keeps standalone
// development builds readable when they are launched outside MacMu.app, while
// packaged builds resolve the key from Contents/Resources/<language>.lproj.
NSString* tr(NSString* key) {
    return [[NSBundle mainBundle] localizedStringForKey:key value:key table:nil];
}

NSString* formatted_byte_count(uint64_t bytes) {
    return [NSByteCountFormatter stringFromByteCount:static_cast<long long>(bytes)
                                           countStyle:NSByteCountFormatterCountStyleFile];
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
constexpr char kAndroidSettingsPackage[] = "com.android.settings";
constexpr char kScreenMatchedProfileTitle[] = "Match Current Screen";
constexpr NSInteger kScreenMatchedProfileTag = -1;
constexpr NSInteger kCustomFixedProfileTag = -2;

struct DefaultDisplaySettings {
    bool matchCurrentScreen;
    uint32_t width;
    uint32_t height;
    uint32_t dpi;
};

DefaultDisplaySettings initial_default_display_settings() {
    return {true, 720, 1600, kNewDisplayDpi};
}

NSScreen* resolved_screen(NSScreen* screen) {
    return screen ?: [NSScreen mainScreen];
}

DisplayLaunchProfile screen_matched_display_profile(NSScreen* screen) {
    NSScreen* resolved = resolved_screen(screen);
    const CGFloat backingScale = resolved ? std::max<CGFloat>(1.0, resolved.backingScaleFactor) : 1.0;
    const NSSize pointSize = resolved ? resolved.frame.size : NSMakeSize(720, 1600);
    const uint32_t pixelWidth =
        std::max<uint32_t>(1, static_cast<uint32_t>(std::llround(pointSize.width * backingScale)));
    const uint32_t pixelHeight =
        std::max<uint32_t>(1, static_cast<uint32_t>(std::llround(pointSize.height * backingScale)));
    const uint32_t dpi = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::llround(kNewDisplayDpi * backingScale)), 120, 640);
    return {kScreenMatchedProfileTitle, std::min(pixelWidth, pixelHeight),
            std::max(pixelWidth, pixelHeight), dpi};
}

uint32_t screen_number(NSScreen* screen) {
    NSNumber* number = resolved_screen(screen).deviceDescription[@"NSScreenNumber"];
    return [number isKindOfClass:[NSNumber class]] ? number.unsignedIntValue : 0;
}

NSScreen* screen_with_number(uint32_t number) {
    if (number != 0) {
        for (NSScreen* screen in [NSScreen screens]) {
            if (screen_number(screen) == number) {
                return screen;
            }
        }
    }
    return [NSScreen mainScreen];
}

void center_window_on_screen(NSWindow* window, NSScreen* screen) {
    if (!window) {
        return;
    }
    NSScreen* resolved = resolved_screen(screen);
    if (!resolved) {
        [window center];
        return;
    }
    const NSRect available = resolved.visibleFrame;
    const NSRect frame = window.frame;
    [window setFrameOrigin:NSMakePoint(NSMidX(available) - frame.size.width * 0.5,
                                       NSMidY(available) - frame.size.height * 0.5)];
}

// User-creatable display ids on the qemu side are 1..kMaxUserDisplayId; ids
// above that are the emulator-internal range. Must match
// kMaxUserDisplayId in macmu-control-receiver.cpp.
constexpr uint32_t kMaxUserDisplayId = 5;

// Common fixed app-window aspect ratios offered by the launch and settings
// menus. Keep 9:20 first as a useful phone preset; the product default is the
// dynamic current-screen profile.
constexpr DisplayLaunchProfile kDisplayLaunchProfiles[] = {
    {"9:20  720 x 1600", 720, 1600, kNewDisplayDpi},
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

bool valid_display_profile(uint32_t width, uint32_t height, uint32_t dpi) {
    if (dpi < 120 || dpi > 640 || width > 16384 || height > 16384) {
        return false;
    }
    const uint32_t minimumPixels = 320 * dpi / 160;
    return width >= minimumPixels && height >= minimumPixels;
}

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
constexpr double kOrientationPollIntervalSeconds = 0.5;
constexpr std::chrono::milliseconds kOrientationResizeSettleTime(2000);

struct DisplayWindow {
    NSWindow* __strong window = nil;
    MTKView* __strong view = nil;
    MacMuSurfaceRendererRef __strong renderer = nil;
};

}  // namespace

static NSUserInterfaceItemIdentifier const kApplicationItemIdentifier =
    @"MacMuApplicationItem";

@interface MacMuApplicationItem : NSCollectionViewItem
- (void)setApplicationRunning:(BOOL)running
                       opening:(BOOL)opening
                       closing:(BOOL)closing
                  uninstalling:(BOOL)uninstalling;
@end

@implementation MacMuApplicationItem {
    NSTextField* _runningLabel;
}

- (void)loadView {
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 120, 148)];
    view.wantsLayer = YES;
    view.layer.cornerRadius = 14.0;

    NSImageView* icon = [[NSImageView alloc] initWithFrame:NSMakeRect(19, 64, 82, 82)];
    icon.imageAlignment = NSImageAlignCenter;
    icon.imageScaling = NSImageScaleProportionallyDown;
    icon.wantsLayer = YES;
    icon.layer.cornerRadius = 18.0;
    icon.layer.masksToBounds = YES;
    [view addSubview:icon];

    NSTextField* name = [[NSTextField alloc] initWithFrame:NSMakeRect(4, 23, 112, 36)];
    name.bezeled = NO;
    name.drawsBackground = NO;
    name.editable = NO;
    name.selectable = NO;
    name.alignment = NSTextAlignmentCenter;
    name.lineBreakMode = NSLineBreakByWordWrapping;
    name.maximumNumberOfLines = 2;
    name.usesSingleLineMode = NO;
    name.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightMedium];
    name.textColor = [NSColor labelColor];
    [view addSubview:name];

    _runningLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(4, 5, 112, 14)];
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

- (void)setApplicationRunning:(BOOL)running
                       opening:(BOOL)opening
                       closing:(BOOL)closing
                  uninstalling:(BOOL)uninstalling {
    _runningLabel.stringValue = uninstalling ? tr(@"Uninstalling…")
                                             : (opening ? tr(@"Opening…")
                                                        : (closing ? tr(@"Closing…")
                                                                   : (running ? tr(@"●  Running")
                                                                              : @"")));
    _runningLabel.textColor = (opening || closing || uninstalling)
                                  ? [NSColor secondaryLabelColor]
                                  : [NSColor systemGreenColor];
}

@end

@interface MacMuApplicationsCollectionView : NSCollectionView
@property(nonatomic, weak) id activationTarget;
@property(nonatomic) SEL activationAction;
@property(nonatomic, weak) id apkDropTarget;
@property(nonatomic) SEL apkDropAction;
@property(nonatomic) BOOL apkDropEnabled;
@property(nonatomic, readonly) NSArray<NSURL*>* droppedApkURLs;
@end

@implementation MacMuApplicationsCollectionView {
    NSArray<NSURL*>* _droppedApkURLs;
    BOOL _apkDropEnabled;
}

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _apkDropEnabled = NO;
    }
    return self;
}

- (NSArray<NSURL*>*)apkURLsFromDraggingInfo:(id<NSDraggingInfo>)sender {
    if (!_apkDropEnabled) {
        return @[];
    }
    NSDictionary* options = @{NSPasteboardURLReadingFileURLsOnlyKey : @YES};
    NSArray* objects = [sender.draggingPasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                                 options:options];
    NSMutableArray<NSURL*>* apks = [NSMutableArray array];
    for (id object in objects) {
        if (![object isKindOfClass:[NSURL class]]) {
            continue;
        }
        NSURL* url = (NSURL*)object;
        if (url.isFileURL && [url.pathExtension caseInsensitiveCompare:@"apk"] == NSOrderedSame) {
            [apks addObject:url];
        }
    }
    return apks;
}

- (void)setApkDropHighlighted:(BOOL)highlighted {
    self.wantsLayer = YES;
    self.layer.cornerRadius = 12.0;
    self.layer.borderWidth = highlighted ? 3.0 : 0.0;
    self.layer.borderColor =
        [[[NSColor selectedContentBackgroundColor] colorWithAlphaComponent:0.75] CGColor];
}

- (void)setApkDropEnabled:(BOOL)enabled {
    if (_apkDropEnabled == enabled) {
        return;
    }
    _apkDropEnabled = enabled;
    if (enabled) {
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    } else {
        [self unregisterDraggedTypes];
        [self setApkDropHighlighted:NO];
    }
}

- (BOOL)apkDropEnabled {
    return _apkDropEnabled;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    const BOOL accepted = [self apkURLsFromDraggingInfo:sender].count > 0;
    [self setApkDropHighlighted:accepted];
    return accepted ? NSDragOperationCopy : NSDragOperationNone;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    return [self draggingEntered:sender];
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    [self setApkDropHighlighted:NO];
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
    return [self apkURLsFromDraggingInfo:sender].count > 0;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray<NSURL*>* urls = [self apkURLsFromDraggingInfo:sender];
    [self setApkDropHighlighted:NO];
    if (urls.count == 0 || !_apkDropAction) {
        return NO;
    }
    _droppedApkURLs = [urls copy];
    const BOOL handled = [NSApp sendAction:_apkDropAction to:_apkDropTarget from:self];
    _droppedApkURLs = nil;
    return handled;
}

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
    // Finder-style single-click launch. Context clicks are handled by
    // menuForEvent: and must only select the item for the right-click menu.
    if (event.clickCount != 1 ||
        (event.modifierFlags & NSEventModifierFlagControl) != 0 ||
        !self.activationAction) {
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
                NSCollectionViewDelegate, NSMenuDelegate>
- (instancetype)initWithOptions:(const ShellOptions&)options;
// Opens (or focuses) the application window bound to virtual display
// |displayId|. Display 0 is deliberately rejected: it is an internal Android
// boot surface, never a user-facing application window.
- (void)openDisplayWindowForDisplay:(uint32_t)displayId
                        aspectWidth:(uint32_t)aspectWidth
                       aspectHeight:(uint32_t)aspectHeight
                               dpi:(uint32_t)dpi;
- (void)launchApplicationComponent:(NSString*)component
                        withProfile:(DisplayLaunchProfile)profile
               matchesCurrentScreen:(BOOL)matchesCurrentScreen;
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
- (void)installDroppedApks:(MacMuApplicationsCollectionView*)sender;
- (void)chooseApksToInstall:(id)sender;
- (void)installApkURLs:(NSArray<NSURL*>*)urls;
- (void)apkInstallFinished:(NSString*)fileName ok:(BOOL)ok error:(NSString*)error;
- (void)searchApplications:(id)sender;
- (void)applyApplicationSearchFilter;
- (void)uninstallSelectedApplication:(id)sender;
- (void)importOfficialSystemImage:(id)sender;
- (void)importSystemImageSource:(NSURL*)sourceURL;
- (void)updateImageImportProgressPhase:(NSInteger)phase
                        completedBytes:(uint64_t)completedBytes
                            totalBytes:(uint64_t)totalBytes
                        completedItems:(size_t)completedItems
                            totalItems:(size_t)totalItems
                               network:(BOOL)network;
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
    NSProgressIndicator* _bootProgressBar;
    NSTextField* _bootProgressValue;
    NSButton* _officialImageButton;
    NSButton* _createMachineButton;
    NSURL* _pendingImageImportSourceURL;
    BOOL _imageSourcePanelOpen;
    NSButton* _installApkButton;
    NSButton* _refreshAppsButton;
    NSSearchField* _appsSearchField;
    NSButton* _startupRetryButton;
    NSView* _startupOverlay;
    BOOL _bootPresentationReady;
    NSMenuItem* _displayMenuItem;
    NSMenuItem* _rotateDisplayMenuItem;
    NSTimer* _orientationTimer;
    NSWindow* _displaySettingsWindow;
    NSPopUpButton* _defaultDisplayProfilePopup;
    NSTextField* _defaultDisplayProfileDetailValue;
    NSTextField* _displaySettingsPathValue;
    NSString* _displaySettingsPath;
    DefaultDisplaySettings _defaultDisplaySettings;

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
    // Packages with an accepted uninstall request. They cannot be launched,
    // refreshed away, or submitted twice until the guest responds.
    std::set<std::string> _uninstallingAppPackages;
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
    // Displays launched with the dynamic default continue matching the backing
    // pixel dimensions of whichever NSScreen contains their window.
    std::set<uint32_t> _screenMatchedDisplayIds;
    // Display id -> NSScreenNumber chosen when the launch was requested. This
    // keeps initial geometry and placement on the same monitor as Applications.
    std::map<uint32_t, uint32_t> _displayTargetScreenNumbers;
    // Only the focused application is queried. A resize is issued by sending
    // DISPLAY_ADD again for the same id; QEMU and MultiDisplayService treat it
    // as an in-place resize rather than allocating a second display.
    std::set<uint32_t> _displayResizePending;
    std::set<uint32_t> _orientationPollInFlight;
    std::map<uint32_t, std::chrono::steady_clock::time_point>
        _orientationSyncSuppressedUntil;
    // DISPLAY_REMOVE can complete via both an event and a request callback.
    // Track host-initiated removals so each slot has one release owner.
    std::set<uint32_t> _displayRemovalPending;
    // User display ids (1..kMaxUserDisplayId) reserved by applications.
    std::set<uint32_t> _activeUserDisplayIds;

    MacMuApplicationsCollectionView* _appsCollection;
    NSMenuItem* _openAppMenuItem;
    NSMenuItem* _openAppWithSizeMenuItem;
    NSMenuItem* _uninstallAppMenuItem;
    NSTextField* _appsStatusValue;
    NSTextField* _appsEmptyValue;
    // Complete launcher catalog and the currently displayed search result.
    NSMutableArray<NSDictionary*>* _allApps;
    NSMutableArray<NSDictionary*>* _apps;
    // Android Settings is a system entry exposed from the status-item menu,
    // not a tile in the Applications grid.
    NSDictionary* _androidSettingsEntry;
    // pkg -> friendly label and decoded icon, populated in applyAppList:.
    // Used by the icon table cell; missing entries fall back to a placeholder.
    NSMutableDictionary<NSString*, NSString*>* _appNames;
    NSMutableDictionary<NSString*, NSImage*>* _appIcons;
    NSOperationQueue* _apkInstallQueue;
    NSUInteger _pendingApkInstalls;
    NSUInteger _successfulApkInstalls;
    NSMutableArray<NSString*>* _apkInstallErrors;

    NSStatusItem* _statusItem;
    NSMenuItem* _machineStatusMenuItem;
    NSMenuItem* _machineStatusDetailMenuItem;
    NSMenuItem* _androidSettingsMenuItem;

    std::shared_ptr<ControlChannel> _controlChannel;  // guarded by _controlMutex
    std::mutex _controlMutex;

    std::atomic<bool> _shuttingDown;
    std::atomic<bool> _runtimeShutdownComplete;
    std::atomic<bool> _imageImportInProgress;
    std::atomic<bool> _imageImportFailed;
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
    _bootProgressBar = nil;
    _bootProgressValue = nil;
    _officialImageButton = nil;
    _createMachineButton = nil;
    _pendingImageImportSourceURL = nil;
    _imageSourcePanelOpen = NO;
    _installApkButton = nil;
    _refreshAppsButton = nil;
    _appsSearchField = nil;
    _startupRetryButton = nil;
    _startupOverlay = nil;
    _bootPresentationReady = NO;
    _displayMenuItem = nil;
    _rotateDisplayMenuItem = nil;
    _orientationTimer = nil;
    _displaySettingsWindow = nil;
    _defaultDisplayProfilePopup = nil;
    _defaultDisplayProfileDetailValue = nil;
    _displaySettingsPathValue = nil;
    _displaySettingsPath = ns_string(_options.appDataDir + "/display-settings.json");
    _defaultDisplaySettings = initial_default_display_settings();
    _appsCollection = nil;
    _openAppMenuItem = nil;
    _openAppWithSizeMenuItem = nil;
    _uninstallAppMenuItem = nil;
    _appsStatusValue = nil;
    _appsEmptyValue = nil;
    _allApps = [[NSMutableArray alloc] init];
    _apps = [[NSMutableArray alloc] init];
    _androidSettingsEntry = nil;
    _appNames = [[NSMutableDictionary alloc] init];
    _appIcons = [[NSMutableDictionary alloc] init];
    _apkInstallQueue = [[NSOperationQueue alloc] init];
    _apkInstallQueue.name = @"dev.macmu.apk-install";
    _apkInstallQueue.qualityOfService = NSQualityOfServiceUserInitiated;
    _apkInstallQueue.maxConcurrentOperationCount = 1;
    _pendingApkInstalls = 0;
    _successfulApkInstalls = 0;
    _apkInstallErrors = [[NSMutableArray alloc] init];
    _statusItem = nil;
    _machineStatusMenuItem = nil;
    _machineStatusDetailMenuItem = nil;
    _androidSettingsMenuItem = nil;
    _shuttingDown.store(false, std::memory_order_relaxed);
    _runtimeShutdownComplete.store(false, std::memory_order_relaxed);
    _imageImportInProgress.store(false, std::memory_order_relaxed);
    _imageImportFailed.store(false, std::memory_order_relaxed);
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
    [self loadDefaultDisplaySettings];
    [self createRuntimeChannels];
    [self createStatusWindow];
    _orientationTimer =
        [NSTimer scheduledTimerWithTimeInterval:kOrientationPollIntervalSeconds
                                         target:self
                                       selector:@selector(pollFocusedApplicationOrientation:)
                                       userInfo:nil
                                        repeats:YES];
    _orientationTimer.tolerance = 0.1;
    [self updateMachineControls];
    [self showStatusWindow:nil];
    std::string imageImportSource = _options.imageImportSource;
    if (imageImportSource.empty() && _options.autoImportDefaultImage) {
        imageImportSource = macmu::kDefaultImageManifestUrl;
    }
    if (!imageImportSource.empty() && !macmu_system_image_exists(_options)) {
        NSString* source = ns_string(imageImportSource);
        NSURL* sourceURL =
            [source hasPrefix:@"https://"] || [source hasPrefix:@"file://"]
                ? [NSURL URLWithString:source]
                : [NSURL fileURLWithPath:source];
        [self importSystemImageSource:sourceURL];
    }
    [self startQemuSupervisor];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    macmu_cancel_system_image_import();
    if (!_runtimeShutdownComplete.load(std::memory_order_acquire)) {
        [self shutdownRuntime];
    }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    macmu_cancel_system_image_import();
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
    dispatch_async(dispatch_get_main_queue(), ^{
        [self updateDisplayMenu];
    });
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    [self updateDisplayMenu];
}

- (void)windowDidResignKey:(NSNotification*)notification {
    NSWindow* window = [notification object];
    for (const auto& entry : _displayWindows) {
        if (entry.second.window == window && entry.second.view) {
            // A physical UHID keyboard retains every key until a later report
            // releases it. Clear the complete report when host focus leaves
            // this application window so modifiers cannot remain stuck.
            macmu_input_view_reset_state(entry.second.view);
            break;
        }
    }
    // AppKit assigns the next key window after sending the resign callback.
    dispatch_async(dispatch_get_main_queue(), ^{
        [self updateDisplayMenu];
    });
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
    for (const auto& entry : _displayWindows) {
        if (entry.second.window != window) {
            continue;
        }
        const uint32_t displayId = entry.first;
        _displayTargetScreenNumbers[displayId] = screen_number(window.screen);
        if (_screenMatchedDisplayIds.count(displayId) > 0) {
            auto current = _displayLaunchProfiles.find(displayId);
            if (current != _displayLaunchProfiles.end()) {
                DisplayLaunchProfile target = screen_matched_display_profile(window.screen);
                if (current->second.width > current->second.height) {
                    std::swap(target.width, target.height);
                }
                if (target.width != current->second.width ||
                    target.height != current->second.height || target.dpi != current->second.dpi) {
                    NSLog(@"MacMu display %u follows NSScreen %@: %ux%u dpi=%u", displayId,
                          window.screen.localizedName, target.width, target.height, target.dpi);
                    [self resizeDisplay:displayId
                                  width:target.width
                                 height:target.height
                                    dpi:target.dpi
                          userInitiated:NO];
                }
            }
        }
        break;
    }
    // Re-send DISPLAY_STREAM whenever an application window crosses screens;
    // each display is paced to its current NSScreen's maximum refresh rate.
    [self setDisplayStreamingForWindow:window enabled:visible];
}

- (void)teardownDisplayWindowEntry:(DisplayWindow&)entry {
    if (entry.view) {
        macmu_input_view_reset_state(entry.view);
        entry.view.paused = YES;
        entry.view.delegate = nil;
        macmu_input_view_set_renderer(entry.view, nil);
    }
    entry.renderer = nil;
    entry.view = nil;
    entry.window = nil;
}

#pragma mark - Default display settings

- (NSScreen*)defaultDisplayTargetScreen {
    if (_statusWindow.screen) {
        return _statusWindow.screen;
    }
    if (NSApp.keyWindow.screen) {
        return NSApp.keyWindow.screen;
    }
    return [NSScreen mainScreen];
}

- (DisplayLaunchProfile)defaultDisplayLaunchProfile {
    if (_defaultDisplaySettings.matchCurrentScreen) {
        return screen_matched_display_profile([self defaultDisplayTargetScreen]);
    }
    return {"Saved Default", _defaultDisplaySettings.width, _defaultDisplaySettings.height,
            _defaultDisplaySettings.dpi};
}

- (BOOL)saveDefaultDisplaySettingsReportingErrors:(BOOL)reportErrors {
    NSDictionary* display = nil;
    if (_defaultDisplaySettings.matchCurrentScreen) {
        display = @{ @"mode" : @"match-current-screen" };
    } else {
        display = @{
            @"mode" : @"fixed",
            @"width" : @(_defaultDisplaySettings.width),
            @"height" : @(_defaultDisplaySettings.height),
            @"dpi" : @(_defaultDisplaySettings.dpi),
        };
    }
    NSDictionary* root = @{ @"version" : @1, @"defaultDisplay" : display };
    NSError* error = nil;
    NSData* data = [NSJSONSerialization dataWithJSONObject:root
                                                   options:NSJSONWritingPrettyPrinted |
                                                           NSJSONWritingSortedKeys
                                                     error:&error];
    if (data && ![data writeToFile:_displaySettingsPath
                           options:NSDataWritingAtomic
                             error:&error]) {
        data = nil;
    }
    if (!data) {
        NSLog(@"MacMu could not save display settings to %@: %@", _displaySettingsPath, error);
        if (reportErrors) {
            NSAlert* alert = [[NSAlert alloc] init];
            alert.messageText = tr(@"Could not save display settings");
            alert.informativeText =
                error.localizedDescription ?: tr(@"The JSON file could not be written.");
            if (_displaySettingsWindow.visible) {
                [alert beginSheetModalForWindow:_displaySettingsWindow completionHandler:nil];
            } else {
                [alert runModal];
            }
        }
        return NO;
    }
    NSLog(@"MacMu display settings saved to %@ (%@)", _displaySettingsPath,
          _defaultDisplaySettings.matchCurrentScreen ? @"match-current-screen" : @"fixed");
    return YES;
}

- (void)loadDefaultDisplaySettings {
    _defaultDisplaySettings = initial_default_display_settings();
    if (![[NSFileManager defaultManager] fileExistsAtPath:_displaySettingsPath]) {
        [self saveDefaultDisplaySettingsReportingErrors:NO];
        return;
    }

    NSError* error = nil;
    NSData* data = [NSData dataWithContentsOfFile:_displaySettingsPath options:0 error:&error];
    id json = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&error] : nil;
    if (![json isKindOfClass:[NSDictionary class]]) {
        NSLog(@"MacMu ignored invalid display settings at %@: %@", _displaySettingsPath, error);
        return;
    }
    NSDictionary* root = (NSDictionary*)json;
    NSNumber* version = root[@"version"];
    NSDictionary* display = [root[@"defaultDisplay"] isKindOfClass:[NSDictionary class]]
                                ? root[@"defaultDisplay"]
                                : nil;
    NSString* mode = [display[@"mode"] isKindOfClass:[NSString class]] ? display[@"mode"] : nil;
    if (![version isKindOfClass:[NSNumber class]] || version.integerValue != 1 || !mode) {
        NSLog(@"MacMu ignored display settings with an unsupported schema at %@",
              _displaySettingsPath);
        return;
    }
    if ([mode isEqualToString:@"match-current-screen"]) {
        NSLog(@"MacMu display settings loaded: match current screen");
        return;
    }
    if (![mode isEqualToString:@"fixed"] ||
        ![display[@"width"] isKindOfClass:[NSNumber class]] ||
        ![display[@"height"] isKindOfClass:[NSNumber class]] ||
        ![display[@"dpi"] isKindOfClass:[NSNumber class]]) {
        NSLog(@"MacMu ignored invalid default display mode at %@", _displaySettingsPath);
        return;
    }
    const uint32_t width = [display[@"width"] unsignedIntValue];
    const uint32_t height = [display[@"height"] unsignedIntValue];
    const uint32_t dpi = [display[@"dpi"] unsignedIntValue];
    if (!valid_display_profile(width, height, dpi)) {
        NSLog(@"MacMu ignored invalid fixed display geometry %ux%u dpi=%u", width, height, dpi);
        return;
    }
    _defaultDisplaySettings = {false, width, height, dpi};
    NSLog(@"MacMu display settings loaded: fixed %ux%u dpi=%u", width, height, dpi);
}

- (void)createDisplaySettingsWindow {
    if (_displaySettingsWindow) {
        return;
    }

    const NSRect frame = NSMakeRect(0, 0, 560, 330);
    _displaySettingsWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _displaySettingsWindow.title = tr(@"Default Display — MacMu");
    _displaySettingsWindow.releasedWhenClosed = NO;
    _displaySettingsWindow.delegate = self;

    NSView* content = [[NSView alloc] initWithFrame:frame];
    _displaySettingsWindow.contentView = content;

    NSTextField* title =
        make_label(tr(@"Default application display"), NSMakeRect(28, 280, 500, 28));
    title.font = [NSFont systemFontOfSize:22.0 weight:NSFontWeightBold];
    title.textColor = [NSColor labelColor];
    [content addSubview:title];

    NSTextField* subtitle =
        make_label(tr(@"Used when a new Android application window is opened."),
                   NSMakeRect(29, 255, 500, 18));
    subtitle.font = [NSFont systemFontOfSize:13.0 weight:NSFontWeightRegular];
    [content addSubview:subtitle];

    NSTextField* profileLabel = make_label(tr(@"Resolution and aspect ratio"),
                                           NSMakeRect(29, 218, 240, 18));
    profileLabel.textColor = [NSColor labelColor];
    [content addSubview:profileLabel];

    _defaultDisplayProfilePopup =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(28, 184, 504, 30) pullsDown:NO];
    _defaultDisplayProfilePopup.target = self;
    _defaultDisplayProfilePopup.action = @selector(defaultDisplayProfileChanged:);
    [content addSubview:_defaultDisplayProfilePopup];

    NSView* detailCard = make_card(NSMakeRect(28, 87, 504, 82));
    [content addSubview:detailCard];
    _defaultDisplayProfileDetailValue = make_label(@"", NSMakeRect(16, 12, 472, 58));
    _defaultDisplayProfileDetailValue.font =
        [NSFont systemFontOfSize:12.0 weight:NSFontWeightRegular];
    _defaultDisplayProfileDetailValue.textColor = [NSColor secondaryLabelColor];
    _defaultDisplayProfileDetailValue.lineBreakMode = NSLineBreakByWordWrapping;
    _defaultDisplayProfileDetailValue.maximumNumberOfLines = 0;
    _defaultDisplayProfileDetailValue.usesSingleLineMode = NO;
    [detailCard addSubview:_defaultDisplayProfileDetailValue];

    _displaySettingsPathValue = make_value(@"", NSMakeRect(29, 59, 500, 16));
    _displaySettingsPathValue.font = [NSFont monospacedSystemFontOfSize:10.0
                                                               weight:NSFontWeightRegular];
    _displaySettingsPathValue.textColor = [NSColor tertiaryLabelColor];
    _displaySettingsPathValue.stringValue =
        [NSString stringWithFormat:tr(@"JSON: %@"), _displaySettingsPath];
    [content addSubview:_displaySettingsPathValue];

    NSButton* cancel = make_button(tr(@"Cancel"), @selector(cancelDisplaySettings:), self,
                                   NSMakeRect(324, 16, 100, 32));
    [content addSubview:cancel];
    NSButton* save = make_button(tr(@"Save"), @selector(saveDisplaySettings:), self,
                                 NSMakeRect(432, 16, 100, 32));
    save.keyEquivalent = @"\r";
    [content addSubview:save];
}

- (void)reloadDefaultDisplayProfilePopup {
    [_defaultDisplayProfilePopup removeAllItems];
    const DisplayLaunchProfile matched =
        screen_matched_display_profile([self defaultDisplayTargetScreen]);
    NSString* matchTitle =
        [NSString stringWithFormat:tr(@"Match Current Screen — %u × %u"), matched.width,
                                   matched.height];
    [_defaultDisplayProfilePopup addItemWithTitle:matchTitle];
    _defaultDisplayProfilePopup.lastItem.tag = kScreenMatchedProfileTag;
    [_defaultDisplayProfilePopup.menu addItem:[NSMenuItem separatorItem]];

    NSInteger selectedTag = kScreenMatchedProfileTag;
    bool foundFixedProfile = false;
    for (size_t i = 0; i < kDisplayLaunchProfileCount; ++i) {
        const DisplayLaunchProfile& profile = kDisplayLaunchProfiles[i];
        [_defaultDisplayProfilePopup
            addItemWithTitle:[NSString stringWithUTF8String:profile.title]];
        _defaultDisplayProfilePopup.lastItem.tag = static_cast<NSInteger>(i);
        if (!_defaultDisplaySettings.matchCurrentScreen &&
            profile.width == _defaultDisplaySettings.width &&
            profile.height == _defaultDisplaySettings.height &&
            profile.dpi == _defaultDisplaySettings.dpi) {
            selectedTag = static_cast<NSInteger>(i);
            foundFixedProfile = true;
        }
    }
    if (!_defaultDisplaySettings.matchCurrentScreen && !foundFixedProfile) {
        NSString* title =
            [NSString stringWithFormat:tr(@"Custom  %u × %u"), _defaultDisplaySettings.width,
                                       _defaultDisplaySettings.height];
        [_defaultDisplayProfilePopup addItemWithTitle:title];
        _defaultDisplayProfilePopup.lastItem.tag = kCustomFixedProfileTag;
        selectedTag = kCustomFixedProfileTag;
    }
    NSMenuItem* selectedItem = [_defaultDisplayProfilePopup.menu itemWithTag:selectedTag];
    if (selectedItem) {
        [_defaultDisplayProfilePopup selectItem:selectedItem];
    }
    [self updateDefaultDisplayProfileDetail];
}

- (void)updateDefaultDisplayProfileDetail {
    const NSInteger tag = _defaultDisplayProfilePopup.selectedItem.tag;
    if (tag == kScreenMatchedProfileTag) {
        NSScreen* screen = resolved_screen([self defaultDisplayTargetScreen]);
        const DisplayLaunchProfile profile = screen_matched_display_profile(screen);
        NSString* screenName = screen.localizedName ?: tr(@"Current Screen");
        _defaultDisplayProfileDetailValue.stringValue = [NSString
            stringWithFormat:tr(@"%@ backing pixels at %u dpi. Portrait uses %u × %u; landscape "
                                @"uses %u × %u, so a full-screen Metal drawable is pixel-aligned."),
                             screenName, profile.dpi, profile.width, profile.height,
                             profile.height, profile.width];
        return;
    }

    uint32_t width = _defaultDisplaySettings.width;
    uint32_t height = _defaultDisplaySettings.height;
    uint32_t dpi = _defaultDisplaySettings.dpi;
    if (tag >= 0 && static_cast<size_t>(tag) < kDisplayLaunchProfileCount) {
        const DisplayLaunchProfile& profile = kDisplayLaunchProfiles[tag];
        width = profile.width;
        height = profile.height;
        dpi = profile.dpi;
    }
    _defaultDisplayProfileDetailValue.stringValue =
        [NSString stringWithFormat:tr(@"New applications use a fixed %u × %u Android surface at "
                                      @"%u dpi. Existing application windows are not changed."),
                                   width, height, dpi];
}

- (void)defaultDisplayProfileChanged:(id)sender {
    [self updateDefaultDisplayProfileDetail];
}

- (void)showDisplaySettings:(id)sender {
    [self createDisplaySettingsWindow];
    [self reloadDefaultDisplayProfilePopup];
    center_window_on_screen(_displaySettingsWindow, [self defaultDisplayTargetScreen]);
    [_displaySettingsWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)cancelDisplaySettings:(id)sender {
    [_displaySettingsWindow orderOut:nil];
}

- (void)saveDisplaySettings:(id)sender {
    const NSInteger tag = _defaultDisplayProfilePopup.selectedItem.tag;
    if (tag == kScreenMatchedProfileTag) {
        _defaultDisplaySettings.matchCurrentScreen = true;
    } else if (tag >= 0 && static_cast<size_t>(tag) < kDisplayLaunchProfileCount) {
        const DisplayLaunchProfile& profile = kDisplayLaunchProfiles[tag];
        _defaultDisplaySettings = {false, profile.width, profile.height, profile.dpi};
    } else if (tag != kCustomFixedProfileTag) {
        NSBeep();
        return;
    }
    if ([self saveDefaultDisplaySettingsReportingErrors:YES]) {
        [_displaySettingsWindow orderOut:nil];
    }
}

- (void)installMainMenu {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"MacMu"];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [menu addItem:appItem];

    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"MacMu"];
    NSMenuItem* showItem = [appMenu addItemWithTitle:tr(@"Show Applications")
                                              action:@selector(showStatusWindow:)
                                       keyEquivalent:@"0"];
    showItem.target = self;
    NSMenuItem* refreshItem = [appMenu addItemWithTitle:tr(@"Refresh Applications")
                                                 action:@selector(refreshApps:)
                                          keyEquivalent:@"r"];
    refreshItem.target = self;
    NSMenuItem* settingsItem = [appMenu addItemWithTitle:tr(@"Display Settings…")
                                                  action:@selector(showDisplaySettings:)
                                           keyEquivalent:@","];
    settingsItem.target = self;
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* quitItem = [appMenu addItemWithTitle:tr(@"Quit MacMu")
                                              action:@selector(terminate:)
                                       keyEquivalent:@"q"];
    quitItem.target = NSApp;
    [appItem setSubmenu:appMenu];

    _displayMenuItem = [[NSMenuItem alloc] initWithTitle:tr(@"Display")
                                                  action:nil
                                           keyEquivalent:@""];
    NSMenu* displayMenu = [[NSMenu alloc] initWithTitle:tr(@"Display")];
    _rotateDisplayMenuItem =
        [displayMenu addItemWithTitle:tr(@"Rotate to Landscape")
                                action:@selector(rotateFocusedApplication:)
                         keyEquivalent:@"r"];
    _rotateDisplayMenuItem.target = self;
    _rotateDisplayMenuItem.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [_displayMenuItem setSubmenu:displayMenu];
    _displayMenuItem.hidden = YES;
    [menu addItem:_displayMenuItem];

    [NSApp setMainMenu:menu];
}

// Returns the emulator display id belonging to the current key application
// window. The Applications window and unrelated windows intentionally return
// zero so the contextual Display menu disappears with app focus.
- (uint32_t)focusedApplicationDisplayId {
    NSWindow* keyWindow = NSApp.keyWindow;
    if (!keyWindow) {
        return 0;
    }
    for (const auto& entry : _displayWindows) {
        if (entry.second.window == keyWindow) {
            return entry.first;
        }
    }
    return 0;
}

- (void)updateDisplayMenu {
    if (!_displayMenuItem || !_rotateDisplayMenuItem) {
        return;
    }
    const uint32_t displayId = [self focusedApplicationDisplayId];
    _displayMenuItem.hidden = displayId == 0;
    if (displayId == 0) {
        _rotateDisplayMenuItem.enabled = NO;
        return;
    }
    auto profile = _displayLaunchProfiles.find(displayId);
    const bool landscape =
        profile != _displayLaunchProfiles.end() && profile->second.width > profile->second.height;
    _rotateDisplayMenuItem.title =
        landscape ? tr(@"Rotate to Portrait") : tr(@"Rotate to Landscape");
    auto channel = [self controlChannel];
    _rotateDisplayMenuItem.enabled =
        profile != _displayLaunchProfiles.end() && ![self isShuttingDown] &&
        _displayResizePending.count(displayId) == 0 && channel && channel->alive();
}

- (void)rotateFocusedApplication:(id)sender {
    const uint32_t displayId = [self focusedApplicationDisplayId];
    auto profile = _displayLaunchProfiles.find(displayId);
    if (displayId == 0 || profile == _displayLaunchProfiles.end()) {
        NSBeep();
        return;
    }
    [self resizeDisplay:displayId
                  width:profile->second.height
                 height:profile->second.width
                    dpi:profile->second.dpi
          userInitiated:YES];
}

- (void)installStatusItem {
    _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    _statusItem.button.title = tr(@"MacMu: Starting");
    _statusItem.button.toolTip = tr(@"Starting Android");

    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"MacMu"];
    _machineStatusMenuItem =
        [[NSMenuItem alloc] initWithTitle:tr(@"Status — Starting Android")
                                  action:nil
                           keyEquivalent:@""];
    _machineStatusMenuItem.enabled = NO;
    [menu addItem:_machineStatusMenuItem];
    _machineStatusDetailMenuItem =
        [[NSMenuItem alloc] initWithTitle:tr(@"Preparing the emulator core…")
                                  action:nil
                           keyEquivalent:@""];
    _machineStatusDetailMenuItem.enabled = NO;
    [menu addItem:_machineStatusDetailMenuItem];
    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* showItem = [[NSMenuItem alloc] initWithTitle:tr(@"Show Applications")
                                                      action:@selector(showStatusWindow:)
                                               keyEquivalent:@""];
    showItem.target = self;
    [menu addItem:showItem];

    _androidSettingsMenuItem =
        [[NSMenuItem alloc] initWithTitle:tr(@"Android Settings…")
                                  action:@selector(openAndroidSettings:)
                           keyEquivalent:@""];
    _androidSettingsMenuItem.target = self;
    _androidSettingsMenuItem.enabled = NO;
    [menu addItem:_androidSettingsMenuItem];

    NSMenuItem* settingsItem = [[NSMenuItem alloc] initWithTitle:tr(@"Display Settings…")
                                                          action:@selector(showDisplaySettings:)
                                                   keyEquivalent:@""];
    settingsItem.target = self;
    [menu addItem:settingsItem];
    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* refreshItem = [[NSMenuItem alloc] initWithTitle:tr(@"Refresh Applications")
                                                         action:@selector(refreshApps:)
                                                  keyEquivalent:@""];
    refreshItem.target = self;
    [menu addItem:refreshItem];
    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:tr(@"Quit MacMu")
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
                    [delegate updateBootPresentation:tr(@"Android started")
                                               detail:tr(@"MacMu Agent is ready. Loading applications…")
                                                stage:tr(@"LOADING")
                                                 busy:YES
                                                 ready:NO];
                    [delegate setAppsStatus:tr(@"Loading applications…")];
                    for (const auto& cleanup : delegate->_deferredLaunchCleanup) {
                        NSString* component = ns_string(cleanup.second);
                        [delegate cleanupFailedLaunchForDisplay:cleanup.first
                                                     component:component
                                                       message:tr(@"The previous application launch did not complete")];
                    }
                    [delegate refreshApps:nil];
                } else {
                    [delegate clearApplicationCatalog];
                    NSString* title = [delegate currentQemuPid] > 0
                                          ? tr(@"Reconnecting to Android")
                                          : tr(@"Starting Android");
                    [delegate updateBootPresentation:title
                                               detail:tr(@"Waiting for Android and MacMu Agent…")
                                                stage:tr(@"STARTING")
                                                 busy:YES
                                                 ready:NO];
                    [delegate setAppsStatus:tr(@"Waiting for Android…")];
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

    const NSRect frame = NSMakeRect(0, 0, 960, 720);
    _statusWindow = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _statusWindow.title = tr(@"Applications — MacMu");
    _statusWindow.titleVisibility = NSWindowTitleHidden;
    _statusWindow.titlebarAppearsTransparent = YES;
    _statusWindow.releasedWhenClosed = NO;
    _statusWindow.delegate = self;
    _statusWindow.minSize = NSMakeSize(720, 560);
    // Keep the Applications shell native to the current macOS appearance.
    // A semantic NSColor remains dynamic when the user switches between the
    // light and dark appearances while this window is already open.
    _statusWindow.backgroundColor = [NSColor windowBackgroundColor];

    NSView* content = [[NSView alloc] initWithFrame:frame];
    _statusWindow.contentView = content;

    // Match the pared-back Finder Applications layout: title at the upper
    // left, search/install/refresh controls at the upper right, and no chrome
    // around the icon grid.
    NSTextField* title =
        make_label(tr(@"Applications"), NSMakeRect(28, frame.size.height - 64, 240, 34));
    title.font = [NSFont systemFontOfSize:26.0 weight:NSFontWeightBold];
    title.textColor = [NSColor labelColor];
    title.autoresizingMask = NSViewMinYMargin;
    [content addSubview:title];

    _appsSearchField =
        [[NSSearchField alloc] initWithFrame:NSMakeRect(frame.size.width - 400,
                                                        frame.size.height - 65, 252, 28)];
    _appsSearchField.placeholderString = tr(@"Search Applications");
    _appsSearchField.target = self;
    _appsSearchField.action = @selector(searchApplications:);
    _appsSearchField.sendsSearchStringImmediately = YES;
    _appsSearchField.sendsWholeSearchString = NO;
    _appsSearchField.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    _appsSearchField.enabled = NO;
    [_appsSearchField setAccessibilityLabel:tr(@"Search Applications")];
    [content addSubview:_appsSearchField];

    NSImage* installImage =
        [NSImage imageWithSystemSymbolName:@"plus"
                  accessibilityDescription:tr(@"Install APK…")];
    if (installImage) {
        NSImageSymbolConfiguration* configuration =
            [NSImageSymbolConfiguration configurationWithPointSize:17.0
                                                            weight:NSFontWeightSemibold];
        installImage = [installImage imageWithSymbolConfiguration:configuration];
    }
    _installApkButton =
        [[NSButton alloc] initWithFrame:NSMakeRect(frame.size.width - 124,
                                                   frame.size.height - 73, 44, 44)];
    _installApkButton.target = self;
    _installApkButton.action = @selector(chooseApksToInstall:);
    _installApkButton.image = installImage;
    _installApkButton.title = installImage ? @"" : @"+";
    _installApkButton.font = [NSFont systemFontOfSize:20.0 weight:NSFontWeightMedium];
    _installApkButton.bezelStyle = NSBezelStyleCircular;
    _installApkButton.buttonType = NSButtonTypeMomentaryPushIn;
    _installApkButton.imagePosition = installImage ? NSImageOnly : NSNoImage;
    _installApkButton.imageScaling = NSImageScaleProportionallyDown;
    _installApkButton.contentTintColor = [NSColor labelColor];
    _installApkButton.toolTip = tr(@"Install APK…");
    _installApkButton.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    _installApkButton.enabled = NO;
    [_installApkButton setAccessibilityLabel:tr(@"Install APK…")];
    [content addSubview:_installApkButton];

    NSImage* refreshImage =
        [NSImage imageWithSystemSymbolName:@"arrow.clockwise"
                  accessibilityDescription:tr(@"Refresh Applications")];
    if (refreshImage) {
        NSImageSymbolConfiguration* configuration =
            [NSImageSymbolConfiguration configurationWithPointSize:17.0
                                                            weight:NSFontWeightSemibold];
        refreshImage = [refreshImage imageWithSymbolConfiguration:configuration];
    }
    _refreshAppsButton =
        [[NSButton alloc] initWithFrame:NSMakeRect(frame.size.width - 72,
                                                   frame.size.height - 73, 44, 44)];
    _refreshAppsButton.target = self;
    _refreshAppsButton.action = @selector(refreshApps:);
    _refreshAppsButton.image = refreshImage;
    _refreshAppsButton.title = refreshImage ? @"" : @"↻";
    _refreshAppsButton.font = [NSFont systemFontOfSize:20.0 weight:NSFontWeightMedium];
    _refreshAppsButton.bezelStyle = NSBezelStyleCircular;
    _refreshAppsButton.buttonType = NSButtonTypeMomentaryPushIn;
    _refreshAppsButton.imagePosition = refreshImage ? NSImageOnly : NSNoImage;
    _refreshAppsButton.imageScaling = NSImageScaleProportionallyDown;
    _refreshAppsButton.contentTintColor = [NSColor labelColor];
    _refreshAppsButton.toolTip = tr(@"Refresh Applications");
    _refreshAppsButton.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    _refreshAppsButton.enabled = NO;
    [_refreshAppsButton setAccessibilityLabel:tr(@"Refresh Applications")];
    [content addSubview:_refreshAppsButton];

    NSScrollView* scroll =
        [[NSScrollView alloc] initWithFrame:NSMakeRect(12, 36, frame.size.width - 24,
                                                       frame.size.height - 134)];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = NO;
    scroll.autohidesScrollers = YES;
    scroll.scrollerStyle = NSScrollerStyleOverlay;
    scroll.borderType = NSNoBorder;
    scroll.drawsBackground = NO;

    NSCollectionViewFlowLayout* layout = [[NSCollectionViewFlowLayout alloc] init];
    layout.itemSize = NSMakeSize(120, 148);
    layout.sectionInset = NSEdgeInsetsMake(20, 14, 24, 14);
    layout.minimumInteritemSpacing = 8.0;
    layout.minimumLineSpacing = 16.0;

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
    _appsCollection.apkDropTarget = self;
    _appsCollection.apkDropAction = @selector(installDroppedApks:);
    _appsCollection.toolTip = nil;
    [_appsCollection registerClass:[MacMuApplicationItem class]
            forItemWithIdentifier:kApplicationItemIdentifier];

    NSMenu* appsMenu = [[NSMenu alloc] initWithTitle:tr(@"Applications")];
    appsMenu.delegate = self;
    // menuNeedsUpdate: owns enablement because uninstall availability depends
    // on package metadata and live application/display transactions.
    appsMenu.autoenablesItems = NO;
    _openAppMenuItem = [appsMenu addItemWithTitle:tr(@"Open")
                                           action:@selector(openSelectedApplication:)
                                    keyEquivalent:@""];
    _openAppMenuItem.target = self;
    _openAppWithSizeMenuItem =
        [appsMenu addItemWithTitle:tr(@"Open with Window Size")
                            action:nil
                     keyEquivalent:@""];
    NSMenu* ratioMenu = [[NSMenu alloc] initWithTitle:tr(@"Open with Window Size")];
    for (size_t i = 0; i < kDisplayLaunchProfileCount; ++i) {
        NSMenuItem* item =
            [ratioMenu addItemWithTitle:[NSString stringWithUTF8String:kDisplayLaunchProfiles[i].title]
                                 action:@selector(launchAppWithProfile:)
                          keyEquivalent:@""];
        item.target = self;
        item.representedObject = @(i);
    }
    [appsMenu setSubmenu:ratioMenu forItem:_openAppWithSizeMenuItem];
    [appsMenu addItem:[NSMenuItem separatorItem]];
    _uninstallAppMenuItem =
        [appsMenu addItemWithTitle:tr(@"Uninstall…")
                            action:@selector(uninstallSelectedApplication:)
                     keyEquivalent:@""];
    _uninstallAppMenuItem.target = self;
    _appsCollection.menu = appsMenu;
    scroll.documentView = _appsCollection;
    [content addSubview:scroll];

    _appsStatusValue = make_label(tr(@"Waiting for Android…"),
                                  NSMakeRect(28, 10, frame.size.width - 56, 18));
    _appsStatusValue.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightRegular];
    _appsStatusValue.textColor = [NSColor tertiaryLabelColor];
    _appsStatusValue.lineBreakMode = NSLineBreakByTruncatingTail;
    _appsStatusValue.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    [content addSubview:_appsStatusValue];

    _appsEmptyValue = make_label(tr(@"Waiting for Android to finish starting…"),
                                 NSMakeRect(60, (frame.size.height - 112) / 2.0,
                                            frame.size.width - 120, 24));
    _appsEmptyValue.font = [NSFont systemFontOfSize:14.0 weight:NSFontWeightRegular];
    _appsEmptyValue.textColor = [NSColor secondaryLabelColor];
    _appsEmptyValue.alignment = NSTextAlignmentCenter;
    _appsEmptyValue.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin | NSViewMaxYMargin;
    [content addSubview:_appsEmptyValue];

    // --- Full-window startup state --------------------------------------
    // Setup/loading is presented directly on the native window background.
    // There is intentionally no nested card or dark panel: the status text,
    // progress, and actions are first-class window content until Android is
    // ready.
    NSVisualEffectView* startupOverlay =
        [[NSVisualEffectView alloc] initWithFrame:content.bounds];
    startupOverlay.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    startupOverlay.material = NSVisualEffectMaterialWindowBackground;
    startupOverlay.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    startupOverlay.state = NSVisualEffectStateActive;
    _startupOverlay = startupOverlay;

    const NSAutoresizingMaskOptions centeredMask =
        NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin | NSViewMaxYMargin;

    _bootStageValue =
        make_label(tr(@"STARTING"), NSMakeRect(170, 470, 620, 18));
    _bootStageValue.font = [NSFont systemFontOfSize:10.0 weight:NSFontWeightSemibold];
    _bootStageValue.textColor = [NSColor tertiaryLabelColor];
    _bootStageValue.alignment = NSTextAlignmentCenter;
    _bootStageValue.autoresizingMask = centeredMask;
    [startupOverlay addSubview:_bootStageValue];

    _bootSpinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(462, 414, 36, 36)];
    _bootSpinner.style = NSProgressIndicatorStyleSpinning;
    _bootSpinner.controlSize = NSControlSizeRegular;
    _bootSpinner.displayedWhenStopped = NO;
    _bootSpinner.autoresizingMask = centeredMask;
    [_bootSpinner startAnimation:nil];
    [startupOverlay addSubview:_bootSpinner];

    _bootTitleValue =
        make_label(tr(@"Starting Android"), NSMakeRect(150, 362, 660, 36));
    _bootTitleValue.font = [NSFont systemFontOfSize:26.0 weight:NSFontWeightSemibold];
    _bootTitleValue.textColor = [NSColor labelColor];
    _bootTitleValue.alignment = NSTextAlignmentCenter;
    _bootTitleValue.autoresizingMask = centeredMask;
    [startupOverlay addSubview:_bootTitleValue];

    _bootDetailValue =
        make_label(tr(@"Preparing MacMu to start…"), NSMakeRect(170, 306, 620, 44));
    _bootDetailValue.font = [NSFont systemFontOfSize:14.0 weight:NSFontWeightRegular];
    _bootDetailValue.textColor = [NSColor secondaryLabelColor];
    _bootDetailValue.alignment = NSTextAlignmentCenter;
    _bootDetailValue.lineBreakMode = NSLineBreakByWordWrapping;
    _bootDetailValue.maximumNumberOfLines = 2;
    _bootDetailValue.usesSingleLineMode = NO;
    _bootDetailValue.autoresizingMask = centeredMask;
    [startupOverlay addSubview:_bootDetailValue];

    _bootProgressBar =
        [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(210, 268, 540, 12)];
    _bootProgressBar.style = NSProgressIndicatorStyleBar;
    _bootProgressBar.indeterminate = NO;
    _bootProgressBar.minValue = 0.0;
    _bootProgressBar.maxValue = 100.0;
    _bootProgressBar.doubleValue = 0.0;
    _bootProgressBar.hidden = YES;
    _bootProgressBar.autoresizingMask = centeredMask;
    [startupOverlay addSubview:_bootProgressBar];

    _bootProgressValue =
        make_label(@"", NSMakeRect(170, 232, 620, 24));
    _bootProgressValue.font =
        [NSFont monospacedDigitSystemFontOfSize:12.0 weight:NSFontWeightMedium];
    _bootProgressValue.textColor = [NSColor secondaryLabelColor];
    _bootProgressValue.alignment = NSTextAlignmentCenter;
    _bootProgressValue.hidden = YES;
    _bootProgressValue.autoresizingMask = centeredMask;
    [startupOverlay addSubview:_bootProgressValue];

    NSTextField* startupHint =
        make_label(tr(@"Applications appear automatically when Android is ready."),
                   NSMakeRect(170, 198, 620, 18));
    startupHint.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightRegular];
    startupHint.textColor = [NSColor tertiaryLabelColor];
    startupHint.alignment = NSTextAlignmentCenter;
    startupHint.autoresizingMask = centeredMask;
    [startupOverlay addSubview:startupHint];

    _officialImageButton =
        make_button(tr(@"Official Image"), @selector(importOfficialSystemImage:), self,
                    NSMakeRect(390, 150, 180, 32));
    _officialImageButton.hidden = YES;
    _officialImageButton.autoresizingMask = centeredMask;
    _officialImageButton.keyEquivalent = @"\r";
    [startupOverlay addSubview:_officialImageButton];

    _createMachineButton = make_button(tr(@"Other Source…"), @selector(prepareDevice:), self,
                                       NSMakeRect(390, 106, 180, 32));
    _createMachineButton.hidden = YES;
    _createMachineButton.autoresizingMask = centeredMask;
    [startupOverlay addSubview:_createMachineButton];

    _startupRetryButton = make_button(tr(@"Try Again"), @selector(refreshApps:), self,
                                      NSMakeRect(405, 150, 150, 32));
    _startupRetryButton.hidden = YES;
    _startupRetryButton.autoresizingMask = centeredMask;
    [startupOverlay addSubview:_startupRetryButton];

    [content addSubview:startupOverlay];

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
        [self updateDisplayMenu];
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
    window.title = tr(@"Opening Application…");
    window.releasedWhenClosed = NO;
    window.delegate = self;
    // Lock the window's content aspect ratio to the request immediately; the
    // renderer also reasserts this on the first frame, but setting it now
    // keeps the resize handle honest before pixels arrive.
    if (aspectWidth > 0 && aspectHeight > 0) {
        [window setContentAspectRatio:NSMakeSize(aspectWidth, aspectHeight)];
    }
    auto targetScreen = _displayTargetScreenNumbers.find(displayId);
    center_window_on_screen(window,
                            targetScreen != _displayTargetScreenNumbers.end()
                                ? screen_with_number(targetScreen->second)
                                : [self defaultDisplayTargetScreen]);

    MTKView* view =
        macmu_input_view_create(frame, _metalDevice, _inputSender, _guestInputSender, displayId);
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
    [self updateDisplayMenu];

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

// Resize an existing application display in place. Reusing DISPLAY_ADD with
// the same id updates QEMU's display pose and makes Android's
// MultiDisplayService call VirtualDisplay.resize(), which in turn produces a
// new IOSurface at the requested orientation.
- (void)resizeDisplay:(uint32_t)displayId
                width:(uint32_t)width
               height:(uint32_t)height
                  dpi:(uint32_t)dpi
        userInitiated:(BOOL)userInitiated {
    if ([self isShuttingDown] || displayId == 0 || displayId > kMaxUserDisplayId || width == 0 ||
        height == 0 || dpi == 0 || _displayResizePending.count(displayId) > 0 ||
        _displayRemovalPending.count(displayId) > 0 ||
        _displayAppBindings.find(displayId) == _displayAppBindings.end()) {
        return;
    }
    auto profile = _displayLaunchProfiles.find(displayId);
    if (profile == _displayLaunchProfiles.end() ||
        (profile->second.width == width && profile->second.height == height &&
         profile->second.dpi == dpi)) {
        return;
    }
    auto channel = [self controlChannel];
    if (!channel || !channel->alive()) {
        if (userInitiated) {
            NSBeep();
        }
        return;
    }

    const DisplayLaunchProfile previous = profile->second;
    profile->second = {previous.title, width, height, dpi};
    _displayResizePending.insert(displayId);
    _orientationSyncSuppressedUntil[displayId] =
        std::chrono::steady_clock::now() + kOrientationResizeSettleTime;
    auto window = _displayWindows.find(displayId);
    if (window != _displayWindows.end()) {
        [window->second.window setContentAspectRatio:NSMakeSize(width, height)];
    }
    [self updateDisplayMenu];

    macmu::ControlDisplayAdd request = {};
    request.displayId = displayId;
    request.width = width;
    request.height = height;
    request.dpi = dpi;
    request.flags = kNewDisplayFlags;
    const bool notifyUser = userInitiated == YES;
    MacMuAppDelegate* delegate = self;
    channel->request(
        macmu::ControlMessageType::kDisplayAdd, &request, sizeof(request), 10000,
        [delegate, displayId, width, height, previous, notifyUser](
            ControlChannel::Response response) {
            bool accepted = response.ok &&
                            response.payload.size() >= sizeof(macmu::ControlDisplayAddOk);
            uint32_t responseDisplayId = 0;
            if (accepted) {
                macmu::ControlDisplayAddOk ok = {};
                std::memcpy(&ok, response.payload.data(), sizeof(ok));
                responseDisplayId = ok.displayId;
                accepted = responseDisplayId == displayId;
            }
            std::string error = response.errorMessage;
            if (!accepted && error.empty()) {
                error = "invalid display resize response";
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                delegate->_displayResizePending.erase(displayId);
                if (delegate->_displayAppBindings.find(displayId) ==
                    delegate->_displayAppBindings.end()) {
                    delegate->_orientationSyncSuppressedUntil.erase(displayId);
                    [delegate updateDisplayMenu];
                    return;
                }
                if (!accepted) {
                    delegate->_displayLaunchProfiles[displayId] = previous;
                    delegate->_orientationSyncSuppressedUntil.erase(displayId);
                    auto displayWindow = delegate->_displayWindows.find(displayId);
                    if (displayWindow != delegate->_displayWindows.end()) {
                        [displayWindow->second.window
                            setContentAspectRatio:NSMakeSize(previous.width, previous.height)];
                    }
                    NSLog(@"MacMu display %u resize failed: %s", displayId, error.c_str());
                    if (notifyUser) {
                        [delegate setAppsStatus:[NSString
                            stringWithFormat:tr(@"Could not rotate application: %@"),
                                             ns_string(error)]];
                        NSBeep();
                    }
                } else {
                    NSLog(@"MacMu display %u resized to %ux%u", displayId, width, height);
                }
                [delegate updateDisplayMenu];
            });
        });
}

// The Android framework can rotate a VirtualDisplay logically without
// changing its physical surface dimensions. Poll the focused app's logical
// size and make the host surface orientation follow it. Restricting this to the
// key app keeps background tasks from changing or focusing unrelated windows.
- (void)pollFocusedApplicationOrientation:(NSTimer*)timer {
    (void)timer;
    if ([self isShuttingDown] || !_guestControlClient || !_guestControlClient->ready()) {
        return;
    }
    const uint32_t displayId = [self focusedApplicationDisplayId];
    if (displayId == 0 || _displayResizePending.count(displayId) > 0 ||
        _orientationPollInFlight.count(displayId) > 0 ||
        _displayRemovalPending.count(displayId) > 0) {
        return;
    }
    auto suppressed = _orientationSyncSuppressedUntil.find(displayId);
    if (suppressed != _orientationSyncSuppressedUntil.end()) {
        if (std::chrono::steady_clock::now() < suppressed->second) {
            return;
        }
        _orientationSyncSuppressedUntil.erase(suppressed);
    }

    _orientationPollInFlight.insert(displayId);
    MacMuAppDelegate* delegate = self;
    _guestControlClient->request(
        "display-state " + std::to_string(displayId), 1500,
        [delegate, displayId](bool ok, std::string payload) {
            uint32_t logicalWidth = 0;
            uint32_t logicalHeight = 0;
            uint32_t rotation = 0;
            const bool parsed =
                ok && std::sscanf(payload.c_str(), "%u %u %u", &logicalWidth, &logicalHeight,
                                  &rotation) == 3 &&
                logicalWidth > 0 && logicalHeight > 0;
            dispatch_async(dispatch_get_main_queue(), ^{
                delegate->_orientationPollInFlight.erase(displayId);
                if (!parsed || [delegate isShuttingDown] ||
                    [delegate focusedApplicationDisplayId] != displayId ||
                    delegate->_displayResizePending.count(displayId) > 0 ||
                    delegate->_displayRemovalPending.count(displayId) > 0) {
                    return;
                }
                auto profile = delegate->_displayLaunchProfiles.find(displayId);
                if (profile == delegate->_displayLaunchProfiles.end() ||
                    logicalWidth == logicalHeight ||
                    profile->second.width == profile->second.height) {
                    return;
                }
                const bool logicalLandscape = logicalWidth > logicalHeight;
                const bool physicalLandscape = profile->second.width > profile->second.height;
                if (logicalLandscape == physicalLandscape) {
                    return;
                }
                NSLog(@"MacMu display %u follows app orientation: logical=%ux%u rotation=%u, "
                       "physical=%ux%u",
                      displayId, logicalWidth, logicalHeight, rotation, profile->second.width,
                      profile->second.height);
                [delegate resizeDisplay:displayId
                                  width:profile->second.height
                                 height:profile->second.width
                                    dpi:profile->second.dpi
                          userInitiated:NO];
            });
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
    _screenMatchedDisplayIds.clear();
    _displayTargetScreenNumbers.clear();
    _displayResizePending.clear();
    _orientationPollInFlight.clear();
    _orientationSyncSuppressedUntil.clear();
    _displayRemovalPending.clear();
    _activeUserDisplayIds.clear();
    [_appsCollection reloadData];
    [self updateApplicationsSummary];
    [self updateDisplayMenu];
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
    const bool importing = _imageImportInProgress.load(std::memory_order_acquire);
    const NSRect centeredActionFrame = NSMakeRect(390, 150, 180, 32);
    const NSRect officialActionFrame = NSMakeRect(390, 150, 180, 32);
    const NSRect otherSourceActionFrame = NSMakeRect(390, 106, 180, 32);
    if (_officialImageButton) {
        const bool choosingInitialSource = !importing && !hasSystemImage;
        _officialImageButton.enabled = choosingInitialSource && !qemuRunning;
        _officialImageButton.hidden = !choosingInitialSource;
        _officialImageButton.frame = officialActionFrame;
    }
    if (_createMachineButton) {
        if (importing) {
            _createMachineButton.enabled =
                !qemuRunning && _pendingImageImportSourceURL == nil;
            _createMachineButton.hidden = NO;
            _createMachineButton.title =
                _pendingImageImportSourceURL != nil
                    ? tr(@"Switching Image Source…")
                    : tr(@"Choose Another Image…");
            _createMachineButton.frame = centeredActionFrame;
        } else if (!hasSystemImage) {
            _createMachineButton.enabled = !qemuRunning;
            _createMachineButton.hidden = NO;
            _createMachineButton.title = tr(@"Other Source…");
            _createMachineButton.frame = otherSourceActionFrame;
        } else if (!hasMachine) {
            _createMachineButton.enabled = !qemuRunning;
            _createMachineButton.hidden = NO;
            _createMachineButton.title = tr(@"Prepare Device");
            _createMachineButton.frame = centeredActionFrame;
        } else {
            _createMachineButton.enabled = NO;
            _createMachineButton.hidden = YES;
            _createMachineButton.title = tr(@"Device Ready");
        }
    }
    // Import progress owns the startup presentation while an image source is
    // active. Do not replace it with the idle "System image required" state,
    // especially for complete ZIP imports which do not emit chunk progress.
    if (!_agentConnected && !importing) {
        if (!hasSystemImage) {
            [self updateBootPresentation:tr(@"System image required")
                                   detail:tr(@"Choose the official image or another source to continue.")
                                    stage:tr(@"SETUP")
                                     busy:NO
                                     ready:NO];
            [self setAppsStatus:tr(@"Android image required")];
        } else if (!hasMachine) {
            [self updateBootPresentation:tr(@"Preparing Android device")
                                   detail:tr(@"Creating the managed MacMu virtual device…")
                                    stage:tr(@"PREPARING")
                                     busy:YES
                                     ready:NO];
        } else if (!qemuRunning) {
            [self updateBootPresentation:tr(@"Starting Android")
                                   detail:tr(@"Launching the emulator core…")
                                    stage:tr(@"STARTING")
                                     busy:YES
                                     ready:NO];
        }
    }
}

- (void)prepareDevice:(id)sender {
    if (_imageImportInProgress.load(std::memory_order_acquire) ||
        !macmu_system_image_exists(_options)) {
        [self importSystemImage:sender];
        return;
    }
    [self createMachine:sender];
}

- (void)importOfficialSystemImage:(id)sender {
    NSString* source = ns_string(macmu::kDefaultImageManifestUrl);
    [self importSystemImageSource:[NSURL URLWithString:source]];
}

- (void)importSystemImage:(id)sender {
    if ([self currentQemuPid] > 0) {
        [self publishQemuStatus:tr(@"Quit MacMu before importing an image")];
        NSBeep();
        return;
    }
    if (_imageSourcePanelOpen) {
        return;
    }

    [self showStatusWindow:nil];
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = tr(@"Import MacMu System Image");
    panel.message = tr(@"Choose a complete image ZIP, a chunk manifest, or a folder containing manifest.json.");
    panel.prompt = tr(@"Import");
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = NO;
    panel.resolvesAliases = YES;
    panel.allowedContentTypes = @[
        [UTType typeWithIdentifier:@"public.zip-archive"],
        [UTType typeWithIdentifier:@"public.json"]
    ];

    _imageSourcePanelOpen = YES;
    MacMuAppDelegate* delegate = self;
    [panel beginSheetModalForWindow:_statusWindow
                  completionHandler:^(NSModalResponse result) {
                      delegate->_imageSourcePanelOpen = NO;
                      if (result != NSModalResponseOK || panel.URL == nil) {
                          return;
                      }
                      [delegate importSystemImageSource:panel.URL];
                  }];
}

- (void)importSystemImageSource:(NSURL*)sourceURL {
    if ([self isShuttingDown]) {
        return;
    }
    if ([self currentQemuPid] > 0) {
        [self publishQemuStatus:tr(@"Quit MacMu before importing an image")];
        NSBeep();
        return;
    }
    ShellOptions options = _options;
    NSString* source =
        sourceURL.isFileURL ? sourceURL.path : sourceURL.absoluteString;
    if (source.length == 0) {
        [self publishQemuStatus:tr(@"Import failed: empty path")];
        return;
    }

    if (_imageImportInProgress.load(std::memory_order_acquire)) {
        _pendingImageImportSourceURL = [sourceURL copy];
        macmu_cancel_system_image_import();
        [self publishBootPresentation:tr(@"Switching image source")
                                detail:tr(@"Stopping the current import before opening the selected image…")
                                 stage:tr(@"WORKING")
                                  busy:YES
                                 ready:NO];
        [self updateMachineControls];
        return;
    }

    _imageImportInProgress.store(true, std::memory_order_release);
    _imageImportFailed.store(false, std::memory_order_release);
    [self publishQemuStatus:tr(@"Importing system image")];
    [self updateMachineControls];

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
                const std::string imageSource = source.UTF8String ?: "";
                ok = macmu_extract_system_image_source(
                    options, imageSource, tempRoot.UTF8String,
                    [delegate](const ImageImportProgress& progress) {
                        const NSInteger phase = static_cast<NSInteger>(progress.phase);
                        const uint64_t completedBytes = progress.completedBytes;
                        const uint64_t totalBytes = progress.totalBytes;
                        const size_t completedItems = progress.completedItems;
                        const size_t totalItems = progress.totalItems;
                        const BOOL network = progress.network;
                        dispatch_async(dispatch_get_main_queue(), ^{
                            [delegate updateImageImportProgressPhase:phase
                                                     completedBytes:completedBytes
                                                         totalBytes:totalBytes
                                                     completedItems:completedItems
                                                         totalItems:totalItems
                                                            network:network];
                        });
                    },
                    &error);
            }

            std::string extractedImageDir;
            if (ok) {
                [delegate publishBootPresentation:tr(@"Validating Android image")
                                            detail:tr(@"Checking the reconstructed image files…")
                                             stage:tr(@"VERIFYING")
                                              busy:YES
                                             ready:NO];
                ok = macmu_find_system_image_directory(tempRoot.UTF8String, &extractedImageDir,
                                                       &error);
            }
            if (ok) {
                [delegate publishBootPresentation:tr(@"Installing Android image")
                                            detail:tr(@"Installing the verified image into MacMu…")
                                             stage:tr(@"INSTALLING")
                                              busy:YES
                                             ready:NO];
                ok = macmu_replace_system_image_from_directory(options, extractedImageDir, &error);
            }
            if (ok) {
                [delegate publishBootPresentation:tr(@"Preparing Android device")
                                            detail:tr(@"Creating the managed MacMu virtual device…")
                                             stage:tr(@"PREPARING")
                                              busy:YES
                                             ready:NO];
                ok = macmu_create_default_machine(options, &error);
            }

            if (tempRoot) {
                [[NSFileManager defaultManager] removeItemAtPath:tempRoot error:nil];
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                NSURL* pendingSource = delegate->_pendingImageImportSourceURL;
                delegate->_pendingImageImportSourceURL = nil;
                if (pendingSource != nil) {
                    // Keep the supervisor blocked across the hand-off. The new
                    // import sets _imageImportInProgress before clearing this
                    // temporary failure flag, so there is no launch window.
                    delegate->_imageImportFailed.store(true, std::memory_order_release);
                    delegate->_imageImportInProgress.store(false, std::memory_order_release);
                    [delegate importSystemImageSource:pendingSource];
                    return;
                }
                delegate->_imageImportFailed.store(!ok, std::memory_order_release);
                delegate->_imageImportInProgress.store(false, std::memory_order_release);
                [delegate updateMachineControls];
                if (ok) {
                    [delegate publishBootPresentation:tr(@"System image imported")
                                                detail:tr(@"The Android device is ready. Starting MacMu…")
                                                 stage:tr(@"STARTING")
                                                  busy:YES
                                                 ready:NO];
                } else {
                    [delegate publishQemuStatus:[NSString
                        stringWithFormat:tr(@"Import failed: %@"), ns_string(error)]];
                    NSLog(@"MacMu image import failed: %s", error.c_str());
                }
            });
        }
    });
}

- (void)createMachine:(id)sender {
    std::string error;
    if (macmu_create_default_machine(_options, &error)) {
        [self updateMachineControls];
        [self publishBootPresentation:tr(@"Android device prepared")
                                detail:tr(@"Starting the emulator core…")
                                 stage:tr(@"STARTING")
                                  busy:YES
                                 ready:NO];
        return;
    }
    [self publishBootPresentation:tr(@"Device preparation failed")
                            detail:ns_string(error)
                             stage:tr(@"ATTENTION")
                              busy:NO
                             ready:NO];
    NSLog(@"MacMu machine creation failed: %s", error.c_str());
    [self updateMachineControls];
}

- (void)updateImageImportProgressPhase:(NSInteger)phase
                        completedBytes:(uint64_t)completedBytes
                            totalBytes:(uint64_t)totalBytes
                        completedItems:(size_t)completedItems
                            totalItems:(size_t)totalItems
                               network:(BOOL)network {
    const uint64_t boundedBytes = std::min(completedBytes, totalBytes);
    const double percentage =
        totalBytes > 0 ? (100.0 * static_cast<double>(boundedBytes) /
                          static_cast<double>(totalBytes))
                       : 0.0;
    NSString* completedSize = formatted_byte_count(boundedBytes);
    NSString* totalSize = formatted_byte_count(totalBytes);
    NSString* title = nil;
    NSString* detail = nil;
    NSString* stage = nil;
    NSString* progressText = nil;

    if (phase == static_cast<NSInteger>(ImageImportPhase::kAcquiringObjects)) {
        title = network ? tr(@"Downloading Android image")
                        : tr(@"Verifying Android image parts");
        detail = network ? tr(@"Receiving and verifying image parts from MacMu storage…")
                         : tr(@"Checking the selected local image parts…");
        stage = network ? tr(@"DOWNLOADING") : tr(@"VERIFYING");
        progressText =
            [NSString stringWithFormat:
                          tr(@"%.1f%% · %@ / %@ · %zu / %zu parts verified"),
                          percentage, completedSize, totalSize, completedItems,
                          totalItems];
    } else {
        title = tr(@"Assembling Android image");
        detail = tr(@"Reconstructing and verifying the Android system image…");
        stage = tr(@"ASSEMBLING");
        progressText =
            [NSString stringWithFormat:
                          tr(@"%.1f%% · %@ / %@ · %zu / %zu chunks assembled"),
                          percentage, completedSize, totalSize, completedItems,
                          totalItems];
    }

    [self updateBootPresentation:title
                           detail:detail
                            stage:stage
                             busy:YES
                            ready:NO];
    if (_bootSpinner) {
        [_bootSpinner stopAnimation:nil];
        _bootSpinner.hidden = YES;
    }
    if (_bootProgressBar) {
        _bootProgressBar.doubleValue = percentage;
        _bootProgressBar.hidden = NO;
    }
    if (_bootProgressValue) {
        _bootProgressValue.stringValue = progressText;
        _bootProgressValue.hidden = NO;
    }
}

- (void)updateBootPresentation:(NSString*)title
                         detail:(NSString*)detail
                          stage:(NSString*)stage
                           busy:(BOOL)busy
                          ready:(BOOL)ready {
    NSString* resolvedTitle = title ?: @"MacMu";
    NSString* resolvedDetail = detail ?: @"";
    NSString* resolvedStage = stage ?: @"";
    _bootPresentationReady = ready;
    if (_bootTitleValue) {
        _bootTitleValue.stringValue = resolvedTitle;
    }
    if (_bootDetailValue) {
        _bootDetailValue.stringValue = resolvedDetail;
    }
    if (_bootStageValue) {
        _bootStageValue.stringValue = resolvedStage;
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
    if (_bootProgressBar) {
        _bootProgressBar.hidden = YES;
    }
    if (_bootProgressValue) {
        _bootProgressValue.hidden = YES;
    }
    if (_startupRetryButton) {
        _startupRetryButton.hidden = ready || busy || !_agentConnected;
    }
    if (_startupOverlay) {
        if (ready) {
            if (!_startupOverlay.hidden) {
                [NSAnimationContext
                    runAnimationGroup:^(NSAnimationContext* context) {
                        context.duration = 0.18;
                        self->_startupOverlay.animator.alphaValue = 0.0;
                    }
                    completionHandler:^{
                        if (self->_bootPresentationReady) {
                            self->_startupOverlay.hidden = YES;
                        }
                        self->_startupOverlay.alphaValue = 1.0;
                    }];
            }
        } else {
            _startupOverlay.hidden = NO;
            _startupOverlay.alphaValue = 1.0;
        }
    }
    if (_machineStatusMenuItem) {
        _machineStatusMenuItem.title =
            [NSString stringWithFormat:tr(@"Status — %@"), resolvedTitle];
    }
    if (_machineStatusDetailMenuItem) {
        _machineStatusDetailMenuItem.title = resolvedDetail;
    }
    if (_statusItem.button) {
        _statusItem.button.title = ready ? tr(@"MacMu: Ready")
                                         : (busy ? tr(@"MacMu: Starting")
                                                 : tr(@"MacMu: Attention"));
        _statusItem.button.toolTip = resolvedDetail.length > 0
                                         ? [NSString stringWithFormat:@"%@ — %@", resolvedTitle,
                                                                      resolvedDetail]
                                         : resolvedTitle;
    }
    // APK installation (button and drag destination) is deliberately exposed
    // only after the ready presentation and application catalog agree that
    // startup is complete.
    [self updateApplicationActions];
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
    const BOOL busy = [text isEqualToString:tr(@"Importing system image")] ||
                      [text isEqualToString:tr(@"Preparing Android device")] ||
                      [text isEqualToString:tr(@"Starting Android")];
    [self publishBootPresentation:text
                           detail:busy ? tr(@"This may take a moment…")
                                       : tr(@"Check the MacMu setup and try again.")
                            stage:busy ? tr(@"WORKING") : tr(@"ATTENTION")
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
    const NSUInteger total = _allApps.count;
    const NSUInteger visible = _apps.count;
    NSString* query = [_appsSearchField.stringValue
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (query.length > 0) {
        [self setAppsStatus:[NSString stringWithFormat:tr(@"%lu of %lu applications"),
                                                       static_cast<unsigned long>(visible),
                                                       static_cast<unsigned long>(total)]];
        return;
    }
    const auto isCatalogApplication = [](const std::string& packageName) {
        return packageName != kAndroidSettingsPackage;
    };
    const size_t opening = static_cast<size_t>(
        std::count_if(_pendingAppPackages.begin(), _pendingAppPackages.end(),
                      isCatalogApplication));
    const size_t closing = static_cast<size_t>(
        std::count_if(_closingAppPackages.begin(), _closingAppPackages.end(),
                      isCatalogApplication));
    const size_t bound = static_cast<size_t>(std::count_if(
        _appDisplayBindings.begin(), _appDisplayBindings.end(),
        [&isCatalogApplication](const auto& binding) {
            return isCatalogApplication(binding.first);
        }));
    const size_t transitional = opening + closing;
    const size_t running = bound >= transitional ? bound - transitional : 0;
    NSString* status = nil;
    if (opening > 0) {
        status = [NSString stringWithFormat:tr(@"%lu applications · %zu opening"),
                                            static_cast<unsigned long>(total), opening];
    } else if (closing > 0) {
        status = [NSString stringWithFormat:tr(@"%lu applications · %zu closing"),
                                            static_cast<unsigned long>(total), closing];
    } else if (running > 0) {
        status = [NSString stringWithFormat:tr(@"%lu applications · %zu running"),
                                            static_cast<unsigned long>(total), running];
    } else {
        status = [NSString stringWithFormat:tr(@"%lu applications"),
                                            static_cast<unsigned long>(total)];
    }
    [self setAppsStatus:status];
}

- (void)updateApplicationActions {
    const BOOL startupComplete =
        _agentConnected && _appsLoaded && _bootPresentationReady;
    const BOOL canManageApks = startupComplete && ![self isShuttingDown] &&
                               _uninstallingAppPackages.empty();
    if (_refreshAppsButton) {
        _refreshAppsButton.enabled = _agentConnected && _pendingApkInstalls == 0 &&
                                     _uninstallingAppPackages.empty();
    }
    if (_installApkButton) {
        _installApkButton.enabled = canManageApks;
    }
    if (_appsSearchField) {
        _appsSearchField.enabled = _appsLoaded;
    }
    if (_appsCollection) {
        _appsCollection.apkDropEnabled = canManageApks;
        _appsCollection.toolTip =
            canManageApks ? tr(@"Drop APK files here to install") : nil;
    }
    if (_androidSettingsMenuItem) {
        _androidSettingsMenuItem.enabled = _agentConnected && _androidSettingsEntry != nil;
        _androidSettingsMenuItem.state =
            _appDisplayBindings.find(kAndroidSettingsPackage) != _appDisplayBindings.end()
                ? NSControlStateValueOn
                : NSControlStateValueOff;
    }
}

- (void)clearApplicationCatalog {
    _appsLoaded = false;
    [_allApps removeAllObjects];
    [_apps removeAllObjects];
    _androidSettingsEntry = nil;
    [_appNames removeAllObjects];
    [_appIcons removeAllObjects];
    _appsCollection.selectionIndexPaths = [NSSet set];
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
    _screenMatchedDisplayIds.erase(displayId);
    _displayTargetScreenNumbers.erase(displayId);
    _displayResizePending.erase(displayId);
    _orientationPollInFlight.erase(displayId);
    _orientationSyncSuppressedUntil.erase(displayId);
    _displayAppBindings.erase(binding);
    [_appsCollection reloadData];
    [self updateApplicationsSummary];
    [self updateApplicationActions];
    [self updateDisplayMenu];
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
        auto profile = _displayLaunchProfiles.find(event.displayId);
        if (profile != _displayLaunchProfiles.end() && event.width > 0 && event.height > 0 &&
            event.dpi > 0) {
            profile->second = {profile->second.title, event.width, event.height, event.dpi};
        }
        [self openDisplayWindowForDisplay:event.displayId
                              aspectWidth:event.width
                             aspectHeight:event.height
                                     dpi:event.dpi];
        [self updateDisplayMenu];
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
        [self setAppsStatus:tr(@"Android agent disconnected; the application is still open")];
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
                                                 [delegate setAppsStatus:[NSString
                                                     stringWithFormat:tr(@"Could not open application: %@"),
                                                                      ns_string(message)]];
                                                 [delegate requestDisplayRemove:displayId];
                                                 return;
                                             }
                                             // Preserve and re-show the display rather than
                                             // orphaning a task on Android's hidden display 0.
                                             delegate->_closingAppPackages.erase(packageName);
                                             [delegate openDisplayWindowForDisplay:displayId];
                                             [delegate->_appsCollection reloadData];
                                             [delegate updateApplicationActions];
                                             [delegate setAppsStatus:[NSString
                                                 stringWithFormat:tr(@"Could not close application: %@"),
                                                                  ns_string(payload)]];
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

- (void)installDroppedApks:(MacMuApplicationsCollectionView*)sender {
    [self installApkURLs:[sender.droppedApkURLs copy]];
}

- (void)chooseApksToInstall:(id)sender {
    if ([self isShuttingDown] || !_guestControlClient || !_guestControlClient->ready() ||
        !_agentConnected || !_appsLoaded || !_bootPresentationReady ||
        !_uninstallingAppPackages.empty()) {
        [self setAppsStatus:!_uninstallingAppPackages.empty()
                                ? tr(@"Wait for application uninstall to finish")
                                : tr(@"Waiting for Android…")];
        NSBeep();
        return;
    }

    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = tr(@"Choose APK Files");
    panel.message = tr(@"Choose one or more APK files to install.");
    panel.prompt = tr(@"Install");
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = YES;
    UTType* apkType = [UTType typeWithFilenameExtension:@"apk"];
    if (apkType) {
        panel.allowedContentTypes = @[ apkType ];
    }

    MacMuAppDelegate* delegate = self;
    [panel beginSheetModalForWindow:_statusWindow
                  completionHandler:^(NSModalResponse result) {
                      if (result != NSModalResponseOK || panel.URLs.count == 0) {
                          return;
                      }
                      [delegate installApkURLs:panel.URLs];
                  }];
}

- (void)installApkURLs:(NSArray<NSURL*>*)urls {
    if ([self isShuttingDown] || !_guestControlClient || !_guestControlClient->ready() ||
        !_agentConnected || !_appsLoaded || !_bootPresentationReady ||
        !_uninstallingAppPackages.empty()) {
        [self setAppsStatus:!_uninstallingAppPackages.empty()
                                ? tr(@"Wait for application uninstall to finish")
                                : tr(@"Waiting for Android…")];
        NSBeep();
        return;
    }
    NSMutableArray<NSURL*>* apkURLs = [NSMutableArray array];
    for (id candidate in urls) {
        if (![candidate isKindOfClass:[NSURL class]]) {
            continue;
        }
        NSURL* url = (NSURL*)candidate;
        if (url.isFileURL &&
            [url.pathExtension caseInsensitiveCompare:@"apk"] == NSOrderedSame) {
            [apkURLs addObject:url];
        }
    }
    if (apkURLs.count == 0) {
        if (urls.count > 0) {
            [self setAppsStatus:tr(@"Only APK files can be installed")];
            NSBeep();
        }
        return;
    }

    if (_pendingApkInstalls == 0) {
        _successfulApkInstalls = 0;
        [_apkInstallErrors removeAllObjects];
    }
    _pendingApkInstalls += apkURLs.count;
    [self updateApplicationActions];

    MacMuAppDelegate* delegate = self;
    for (NSURL* url in apkURLs) {
        NSString* fileName = url.lastPathComponent.length > 0 ? url.lastPathComponent : @"APK";
        [_apkInstallQueue addOperationWithBlock:^{
            @autoreleasepool {
                if ([delegate isShuttingDown]) {
                    return;
                }
                const BOOL securityScope = [url startAccessingSecurityScopedResource];
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (![delegate isShuttingDown]) {
                        [delegate setAppsStatus:[NSString
                            stringWithFormat:tr(@"Installing %@…"), fileName]];
                    }
                });

                dispatch_semaphore_t finished = dispatch_semaphore_create(0);
                const char* fileSystemPath = url.fileSystemRepresentation;
                if (!fileSystemPath || !delegate->_guestControlClient) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [delegate apkInstallFinished:fileName
                                                 ok:NO
                                              error:tr(@"The APK path is unavailable")];
                    });
                    dispatch_semaphore_signal(finished);
                } else {
                    const std::string path(fileSystemPath);
                    delegate->_guestControlClient->install_apk(
                        path, 10 * 60 * 1000,
                        [delegate, fileName, finished](bool ok, std::string payload) {
                            dispatch_async(dispatch_get_main_queue(), ^{
                                [delegate apkInstallFinished:fileName
                                                         ok:ok ? YES : NO
                                                      error:ns_string(payload)];
                            });
                            dispatch_semaphore_signal(finished);
                        });
                }
                dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);
                if (securityScope) {
                    [url stopAccessingSecurityScopedResource];
                }
            }
        }];
    }
}

- (void)apkInstallFinished:(NSString*)fileName ok:(BOOL)ok error:(NSString*)error {
    if ([self isShuttingDown]) {
        return;
    }
    if (_pendingApkInstalls > 0) {
        --_pendingApkInstalls;
    }
    if (ok) {
        ++_successfulApkInstalls;
    } else {
        NSString* detail = error.length > 0 ? error : tr(@"Unknown installation error");
        [_apkInstallErrors
            addObject:[NSString stringWithFormat:@"%@: %@", fileName, detail]];
    }

    [self updateApplicationActions];
    if (_pendingApkInstalls > 0) {
        [self setAppsStatus:[NSString stringWithFormat:tr(@"Installing APK files… %lu remaining"),
                                                       static_cast<unsigned long>(_pendingApkInstalls)]];
        return;
    }

    const NSUInteger installed = _successfulApkInstalls;
    NSArray<NSString*>* errors = [_apkInstallErrors copy];
    _successfulApkInstalls = 0;
    [_apkInstallErrors removeAllObjects];

    if (installed > 0) {
        [self setAppsStatus:installed == 1
                                ? tr(@"APK installed. Refreshing applications…")
                                : [NSString stringWithFormat:
                                      tr(@"%lu APK files installed. Refreshing applications…"),
                                      static_cast<unsigned long>(installed)]];
        [self refreshApps:nil];
    } else if (errors.count > 0) {
        [self setAppsStatus:errors.firstObject];
    }

    if (errors.count > 0 && _statusWindow) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleWarning;
        alert.messageText = errors.count == 1 ? tr(@"Could not install APK")
                                              : tr(@"Could not install APK files");
        alert.informativeText = [errors componentsJoinedByString:@"\n\n"];
        [alert beginSheetModalForWindow:_statusWindow completionHandler:nil];
    }
}

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
    if (_pendingApkInstalls > 0) {
        [self setAppsStatus:tr(@"Wait for APK installation to finish")];
        if (sender) {
            NSBeep();
        }
        return;
    }
    if (!_uninstallingAppPackages.empty()) {
        [self setAppsStatus:tr(@"Wait for application uninstall to finish")];
        if (sender) {
            NSBeep();
        }
        return;
    }
    if (!_guestControlClient || !_guestControlClient->ready()) {
        [self setAppsStatus:tr(@"Waiting for Android…")];
        return;
    }
    [self setAppsStatus:tr(@"Loading applications…")];
    [self updateBootPresentation:tr(@"Android started")
                           detail:tr(@"Discovering installed applications…")
                            stage:tr(@"LOADING")
                             busy:YES
                             ready:NO];
    MacMuAppDelegate* delegate = self;
    _guestControlClient->request("apps", 15000, [delegate](bool ok, std::string payload) {
        if (!ok) {
            [delegate setAppsStatus:[NSString
                stringWithFormat:tr(@"Could not load applications: %@"), ns_string(payload)]];
            [delegate publishBootPresentation:tr(@"Android started")
                                        detail:tr(@"Application discovery failed. Use Refresh to retry.")
                                         stage:tr(@"ATTENTION")
                                          busy:NO
                                         ready:NO];
            return;
        }
        NSData* data = [NSData dataWithBytes:payload.data() length:payload.size()];
        NSError* error = nil;
        id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
        if (![parsed isKindOfClass:[NSArray class]]) {
            [delegate setAppsStatus:tr(@"Could not read the application list")];
            [delegate publishBootPresentation:tr(@"Android started")
                                        detail:tr(@"The application list was malformed. Use Refresh to retry.")
                                         stage:tr(@"ATTENTION")
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
    [_allApps removeAllObjects];
    [_apps removeAllObjects];
    [_appNames removeAllObjects];
    [_appIcons removeAllObjects];
    _androidSettingsEntry = nil;
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
        if ([pkg isEqualToString:@"com.android.settings"]) {
            _androidSettingsEntry = dict;
            continue;
        }
        [_allApps addObject:dict];
    }
    // Sort by friendly name when available, else by package — names make the
    // list more scannable than raw package ids.
    [_allApps sortUsingComparator:^NSComparisonResult(NSDictionary* a, NSDictionary* b) {
        NSString* nameA = _appNames[a[@"pkg"]] ?: a[@"pkg"];
        NSString* nameB = _appNames[b[@"pkg"]] ?: b[@"pkg"];
        return [nameA localizedCaseInsensitiveCompare:nameB];
    }];
    _appsLoaded = true;
    [self applyApplicationSearchFilter];
    for (const auto& binding : _displayAppBindings) {
        [self updateApplicationWindowTitleForDisplay:binding.first];
    }
    NSString* detail = _allApps.count == 0
                           ? tr(@"Android is ready, but no launcher applications were found.")
                           : [NSString stringWithFormat:tr(@"%lu applications are ready to open."),
                                                        static_cast<unsigned long>(_allApps.count)];
    [self updateBootPresentation:tr(@"Ready")
                           detail:detail
                            stage:tr(@"READY")
                             busy:NO
                             ready:YES];
    NSLog(@"MacMu application catalog ready (%lu applications; Android Settings %@).",
          static_cast<unsigned long>(_allApps.count),
          _androidSettingsEntry ? @"available in status menu" : @"not found");

}

- (void)searchApplications:(id)sender {
    [self applyApplicationSearchFilter];
}

- (void)applyApplicationSearchFilter {
    if (!_appsLoaded) {
        return;
    }

    NSString* query = [_appsSearchField.stringValue
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    NSMutableArray<NSString*>* terms = [NSMutableArray array];
    for (NSString* term in
         [query componentsSeparatedByCharactersInSet:
                    [NSCharacterSet whitespaceAndNewlineCharacterSet]]) {
        if (term.length > 0) {
            [terms addObject:term];
        }
    }

    [_apps removeAllObjects];
    for (NSDictionary* app in _allApps) {
        NSString* package = [app[@"pkg"] isKindOfClass:[NSString class]] ? app[@"pkg"] : @"";
        NSString* activity =
            [app[@"activity"] isKindOfClass:[NSString class]] ? app[@"activity"] : @"";
        NSString* name = _appNames[package] ?: package;
        NSString* searchable = [NSString stringWithFormat:@"%@ %@ %@", name, package, activity];
        BOOL matches = YES;
        for (NSString* term in terms) {
            if ([searchable rangeOfString:term
                                  options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch]
                    .location == NSNotFound) {
                matches = NO;
                break;
            }
        }
        if (matches) {
            [_apps addObject:app];
        }
    }

    _appsCollection.selectionIndexPaths = [NSSet set];
    [_appsCollection reloadData];
    _appsEmptyValue.hidden = _apps.count > 0;
    [self updateApplicationsSummary];
    [self updateApplicationActions];
    if (_apps.count == 0) {
        _appsEmptyValue.stringValue = query.length > 0
                                         ? [NSString stringWithFormat:
                                               tr(@"No applications match “%@”."), query]
                                         : tr(@"Android is ready, but no launcher applications were found.");
        _appsEmptyValue.hidden = NO;
    }
}

- (NSDictionary*)selectedApplication {
    NSIndexPath* indexPath = _appsCollection.selectionIndexPaths.anyObject;
    if (!indexPath || indexPath.item >= _apps.count) {
        return nil;
    }
    return _apps[indexPath.item];
}

- (NSString*)selectedAppComponent {
    NSDictionary* app = [self selectedApplication];
    if (!app) {
        return nil;
    }
    return [NSString stringWithFormat:@"%@/%@", app[@"pkg"], app[@"activity"]];
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
    if (!_appsCollection || menu != _appsCollection.menu) {
        return;
    }

    NSDictionary* app = [self selectedApplication];
    NSString* package = [app[@"pkg"] isKindOfClass:[NSString class]] ? app[@"pkg"] : nil;
    NSString* name = package ? (_appNames[package] ?: package) : nil;
    const std::string packageName = package ? std::string(package.UTF8String) : std::string();
    const BOOL uninstalling = !packageName.empty() &&
                              _uninstallingAppPackages.count(packageName) > 0;
    const BOOL runningOrTransitioning =
        !packageName.empty() &&
        (_appDisplayBindings.count(packageName) > 0 ||
         _pendingAppPackages.count(packageName) > 0 ||
         _closingAppPackages.count(packageName) > 0);

    const BOOL canOpen = app != nil && _agentConnected && !uninstalling &&
                         _pendingApkInstalls == 0 && _uninstallingAppPackages.empty();
    _openAppMenuItem.enabled = canOpen;
    _openAppWithSizeMenuItem.enabled = canOpen;

    _uninstallAppMenuItem.toolTip = nil;
    if (!app || !package) {
        _uninstallAppMenuItem.title = tr(@"Uninstall…");
        _uninstallAppMenuItem.enabled = NO;
        return;
    }
    if (uninstalling) {
        _uninstallAppMenuItem.title = tr(@"Uninstalling…");
        _uninstallAppMenuItem.enabled = NO;
        return;
    }

    NSNumber* uninstallableValue = app[@"uninstallable"];
    const BOOL hasUninstallMetadata =
        [uninstallableValue isKindOfClass:[NSNumber class]];
    const BOOL system = [app[@"system"] isKindOfClass:[NSNumber class]] &&
                        [app[@"system"] boolValue];
    if (system) {
        _uninstallAppMenuItem.title = tr(@"System Application — Cannot Uninstall");
        _uninstallAppMenuItem.toolTip =
            tr(@"System applications are part of the Android system image.");
        _uninstallAppMenuItem.enabled = NO;
        return;
    }
    if (!hasUninstallMetadata || !uninstallableValue.boolValue) {
        _uninstallAppMenuItem.title = tr(@"Uninstall Unavailable");
        _uninstallAppMenuItem.enabled = NO;
        return;
    }
    if (runningOrTransitioning) {
        _uninstallAppMenuItem.title = tr(@"Close Application Before Uninstalling");
        _uninstallAppMenuItem.enabled = NO;
        return;
    }

    _uninstallAppMenuItem.title =
        [NSString stringWithFormat:tr(@"Uninstall “%@”…"), name];
    _uninstallAppMenuItem.enabled = _agentConnected && _pendingApkInstalls == 0;
}

- (void)uninstallSelectedApplication:(id)sender {
    if ([self isShuttingDown]) {
        return;
    }
    NSDictionary* app = [self selectedApplication];
    NSString* package = [app[@"pkg"] isKindOfClass:[NSString class]] ? app[@"pkg"] : nil;
    if (!app || !package) {
        NSBeep();
        return;
    }
    NSString* name = _appNames[package] ?: package;
    const std::string packageName(package.UTF8String);
    const BOOL system = [app[@"system"] isKindOfClass:[NSNumber class]] &&
                        [app[@"system"] boolValue];
    NSNumber* uninstallable = app[@"uninstallable"];
    if (system || ![uninstallable isKindOfClass:[NSNumber class]] ||
        !uninstallable.boolValue) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleWarning;
        alert.messageText = system ? tr(@"Cannot Uninstall System Application")
                                   : tr(@"Cannot Uninstall Application");
        alert.informativeText = system
                                    ? [NSString stringWithFormat:
                                          tr(@"%@ is part of the Android system image."), name]
                                    : [NSString stringWithFormat:
                                          tr(@"MacMu could not verify that %@ is removable."), name];
        [alert beginSheetModalForWindow:_statusWindow completionHandler:nil];
        return;
    }
    if (_appDisplayBindings.count(packageName) > 0 ||
        _pendingAppPackages.count(packageName) > 0 ||
        _closingAppPackages.count(packageName) > 0) {
        [self setAppsStatus:[NSString stringWithFormat:
                                  tr(@"Close %@ before uninstalling it."), name]];
        NSBeep();
        return;
    }
    if (!_guestControlClient || !_guestControlClient->ready() ||
        _pendingApkInstalls > 0 || _uninstallingAppPackages.count(packageName) > 0) {
        [self setAppsStatus:tr(@"Waiting for Android…")];
        NSBeep();
        return;
    }

    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = [NSString stringWithFormat:tr(@"Uninstall “%@”?"), name];
    alert.informativeText = tr(@"This removes the application and its data from Android.");
    [alert addButtonWithTitle:tr(@"Uninstall")];
    [alert addButtonWithTitle:tr(@"Cancel")];
    if (@available(macOS 11.0, *)) {
        alert.buttons.firstObject.hasDestructiveAction = YES;
    }

    MacMuAppDelegate* delegate = self;
    [alert beginSheetModalForWindow:_statusWindow
                  completionHandler:^(NSModalResponse response) {
        if (response != NSAlertFirstButtonReturn || [delegate isShuttingDown]) {
            return;
        }
        if (!delegate->_guestControlClient || !delegate->_guestControlClient->ready()) {
            [delegate setAppsStatus:tr(@"Waiting for Android…")];
            return;
        }

        delegate->_uninstallingAppPackages.insert(packageName);
        [delegate->_appsCollection reloadData];
        [delegate updateApplicationActions];
        [delegate setAppsStatus:[NSString stringWithFormat:tr(@"Uninstalling %@…"), name]];

        std::string command = "uninstall " + packageName;
        delegate->_guestControlClient->request(
            command, 60000,
            [delegate, packageName, name](bool ok, std::string payload) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if ([delegate isShuttingDown]) {
                        return;
                    }
                    delegate->_uninstallingAppPackages.erase(packageName);
                    [delegate->_appsCollection reloadData];
                    [delegate updateApplicationActions];
                    if (ok) {
                        [delegate setAppsStatus:[NSString stringWithFormat:
                                                     tr(@"%@ uninstalled. Refreshing applications…"),
                                                     name]];
                        [delegate refreshApps:nil];
                        return;
                    }

                    NSString* detail = payload.empty() ? tr(@"Unknown uninstall error")
                                                       : ns_string(payload);
                    [delegate setAppsStatus:[NSString stringWithFormat:
                                                  tr(@"Could not uninstall %@: %@"), name, detail]];
                    NSAlert* failure = [[NSAlert alloc] init];
                    failure.alertStyle = NSAlertStyleWarning;
                    failure.messageText =
                        [NSString stringWithFormat:tr(@"Could not uninstall %@"), name];
                    failure.informativeText = detail;
                    [failure beginSheetModalForWindow:delegate->_statusWindow
                                     completionHandler:nil];
                });
            });
    }];
}

- (void)openSelectedApplication:(id)sender {
    [self launchSelectedApplicationWithProfile:[self defaultDisplayLaunchProfile]
                          matchesCurrentScreen:_defaultDisplaySettings.matchCurrentScreen];
}

- (void)openAndroidSettings:(id)sender {
    if (!_androidSettingsEntry ||
        ![_androidSettingsEntry[@"pkg"] isKindOfClass:[NSString class]] ||
        ![_androidSettingsEntry[@"activity"] isKindOfClass:[NSString class]]) {
        NSBeep();
        return;
    }
    NSString* component =
        [NSString stringWithFormat:@"%@/%@", _androidSettingsEntry[@"pkg"],
                                   _androidSettingsEntry[@"activity"]];
    [self launchApplicationComponent:component
                         withProfile:[self defaultDisplayLaunchProfile]
                matchesCurrentScreen:_defaultDisplaySettings.matchCurrentScreen];
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
    [self launchSelectedApplicationWithProfile:kDisplayLaunchProfiles[index]
                          matchesCurrentScreen:NO];
}

- (void)launchSelectedApplicationWithProfile:(DisplayLaunchProfile)profile
                         matchesCurrentScreen:(BOOL)matchesCurrentScreen {
    if ([self isShuttingDown]) {
        return;
    }
    NSString* component = [self selectedAppComponent];
    if (!component) {
        NSBeep();
        return;
    }
    [self launchApplicationComponent:component
                         withProfile:profile
                matchesCurrentScreen:matchesCurrentScreen];
}

- (void)launchApplicationComponent:(NSString*)component
                        withProfile:(DisplayLaunchProfile)profile
               matchesCurrentScreen:(BOOL)matchesCurrentScreen {
    if ([self isShuttingDown] || component.length == 0) {
        return;
    }
    const std::string componentValue = [component UTF8String];
    const std::string packageName = package_from_component(componentValue);
    if (!_uninstallingAppPackages.empty()) {
        [self setAppsStatus:tr(@"Wait for application uninstall to finish")];
        NSBeep();
        return;
    }
    auto existingApp = _appDisplayBindings.find(packageName);
    if (existingApp != _appDisplayBindings.end()) {
        const uint32_t existingDisplayId = existingApp->second;
        NSString* name = [self applicationNameForPackage:packageName];
        if (_closingAppPackages.count(packageName) > 0 ||
            _displayRemovalPending.count(existingDisplayId) > 0) {
            [self setAppsStatus:[NSString stringWithFormat:tr(@"Closing %@…"), name]];
            return;
        }
        auto window = _displayWindows.find(existingDisplayId);
        if (window != _displayWindows.end()) {
            [window->second.window makeKeyAndOrderFront:nil];
            [self setDisplayStreaming:existingDisplayId enabled:YES];
            [NSApp activateIgnoringOtherApps:YES];
        }
        [self setAppsStatus:_pendingAppPackages.count(packageName) > 0
                                ? [NSString stringWithFormat:tr(@"Opening %@…"), name]
                                : [NSString stringWithFormat:tr(@"%@ is already open"), name]];
        return;
    }
    if (_pendingApkInstalls > 0) {
        [self setAppsStatus:tr(@"Wait for APK installation to finish")];
        NSBeep();
        return;
    }
    if (!_agentConnected) {
        [self setAppsStatus:tr(@"Waiting for Android…")];
        return;
    }
    auto channel = [self controlChannel];
    if (!channel || !channel->alive()) {
        [self setAppsStatus:tr(@"Control channel not connected")];
        return;
    }
    // Reserve both sides of the package/display bijection before DISPLAY_ADD.
    // This closes the double-click race and lets display-added events know the
    // new surface belongs to an application rather than a blank display.
    const uint32_t displayId = [self allocateUserDisplayId];
    if (displayId == 0) {
        [self setAppsStatus:tr(@"Up to 5 applications can run at the same time")];
        NSBeep();
        return;
    }
    _displayAppBindings[displayId] = componentValue;
    _appDisplayBindings[packageName] = displayId;
    _pendingAppPackages.insert(packageName);
    _displayLaunchProfiles[displayId] = profile;
    _displayTargetScreenNumbers[displayId] = screen_number([self defaultDisplayTargetScreen]);
    if (matchesCurrentScreen) {
        _screenMatchedDisplayIds.insert(displayId);
    }
    [_appsCollection reloadData];
    [self updateApplicationActions];
    [self setAppsStatus:[NSString stringWithFormat:tr(@"Opening %@…"),
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
                    NSString* message = [NSString
                        stringWithFormat:tr(@"Could not open application: %@"),
                                         ns_string(response.errorMessage)];
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
                                                  message:tr(@"Could not read the application window response")];
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
                                                  message:tr(@"Could not reserve an application window")];
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
                                                  message:tr(@"Application window is unavailable")];
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
        [self setAppsStatus:[NSString stringWithFormat:tr(@"%@; cleanup will resume after Android reconnects"),
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
                    [delegate setAppsStatus:[NSString
                        stringWithFormat:tr(@"%@; close the application window to retry cleanup"),
                                         ns_string(messageValue)]];
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
                                    message:tr(@"Android agent disconnected during launch")];
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
                                                message:[NSString
                                                    stringWithFormat:tr(@"Could not open application: %@"),
                                                                     ns_string(payload)]];
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
    item.imageView.image = icon ?: placeholder_icon(name, NSMakeSize(82, 82));
    item.textField.stringValue = name;
    item.view.toolTip = [NSString stringWithFormat:@"%@\n%@", package, activity];

    const std::string packageName = package.UTF8String;
    const BOOL bound = _appDisplayBindings.find(packageName) != _appDisplayBindings.end();
    const BOOL opening = _pendingAppPackages.find(packageName) != _pendingAppPackages.end();
    const BOOL closing = _closingAppPackages.find(packageName) != _closingAppPackages.end();
    const BOOL uninstalling =
        _uninstallingAppPackages.find(packageName) != _uninstallingAppPackages.end();
    [item setApplicationRunning:bound && !opening && !closing
                         opening:opening
                         closing:closing
                    uninstalling:uninstalling];
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
        if (_imageImportInProgress.load(std::memory_order_acquire) ||
            _imageImportFailed.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        if (!macmu_system_image_exists(_options)) {
            [self publishBootPresentation:tr(@"System image required")
                                    detail:tr(@"Choose the official image or another source to continue.")
                                     stage:tr(@"SETUP")
                                      busy:NO
                                     ready:NO];
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        if (!macmu_machine_exists(_options)) {
            [self publishBootPresentation:tr(@"Preparing Android device")
                                    detail:tr(@"Creating the managed virtual device…")
                                     stage:tr(@"PREPARING")
                                      busy:YES
                                     ready:NO];
            std::string error;
            if (!macmu_create_default_machine(_options, &error)) {
                [self publishBootPresentation:tr(@"Device preparation failed")
                                        detail:ns_string(error)
                                         stage:tr(@"ATTENTION")
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
        [self publishBootPresentation:tr(@"Starting Android")
                                detail:tr(@"Launching the emulator core…")
                                 stage:tr(@"STARTING")
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
            [self publishBootPresentation:tr(@"Could not start Android")
                                    detail:tr(@"MacMu will retry automatically.")
                                     stage:tr(@"RETRYING")
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
                        [delegate updateBootPresentation:tr(@"Android is booting")
                                                   detail:tr(@"Waiting for Android to finish startup and launch MacMu Agent…")
                                                    stage:tr(@"BOOTING")
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
            [self publishBootPresentation:tr(@"Android is booting")
                                    detail:tr(@"Waiting for boot completion and MacMu Agent…")
                                     stage:tr(@"BOOTING")
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
            [delegate setAppsStatus:tr(@"Restarting Android…")];
            [delegate updateMachineControls];
        });
        [self publishBootPresentation:tr(@"Restarting Android")
                                detail:tr(@"The emulator core exited; MacMu is starting it again.")
                                 stage:tr(@"RESTARTING")
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
    [_displaySettingsWindow orderOut:nil];
}

- (void)beginAsyncTermination {
    [_orientationTimer invalidate];
    _orientationTimer = nil;
    if (_shuttingDown.exchange(true, std::memory_order_acq_rel)) {
        [self hideWindowsForTermination];
        return;
    }

    [self hideWindowsForTermination];
    [self publishQemuStatus:tr(@"Stopping")];

    MacMuAppDelegate* delegate = self;
    std::thread([delegate] {
        [delegate performRuntimeShutdown];
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
        });
    }).detach();
}

- (void)shutdownRuntime {
    [_orientationTimer invalidate];
    _orientationTimer = nil;
    if (_shuttingDown.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    [self performRuntimeShutdown];
}

- (void)performRuntimeShutdown {
    [self stopDoorbellThread];
    [self stopGuestInputSender];
    [_apkInstallQueue cancelAllOperations];
    if (_guestControlClient) {
        _guestControlClient->stop();
    }
    [_apkInstallQueue waitUntilAllOperationsAreFinished];

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
