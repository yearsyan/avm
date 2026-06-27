// MacMu macOS shell for IOSurface display export.
// SPDX-License-Identifier: MIT
//
// Build:
//   cmake -S shell -B build/shell
//   cmake --build build/shell --target macmu_shell
//
// By default this shell launches qemu-system-aarch64-headless with IOSurface export enabled.

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits.h>
#include <mach-o/dyld.h>
#include <sstream>
#include <string>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

constexpr const char* kDefaultMetadataPath = "/tmp/macmu-iosurface.json";
constexpr const char* kQemuHeadlessRelativePath =
    "qemu/darwin-aarch64/qemu-system-aarch64-headless";
constexpr const char* kDefaultAvdName = "aemu_aosp35_arm64";

struct ShellOptions {
    bool launchQemu = true;
    std::string metadataPath = kDefaultMetadataPath;
    std::string launcherDir;
    std::string qemuPath;
    std::string dyldLibraryPath;
    std::string androidSdkRoot;
    std::string avdName = kDefaultAvdName;
};

struct SurfaceMetadata {
    IOSurfaceID iosurfaceId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frame = 0;
};

std::string envOrDefault(const char* name, const std::string& fallback) {
    if (const char* value = std::getenv(name)) {
        if (value[0] != '\0') {
            return value;
        }
    }
    return fallback;
}

std::string envOrDefault(const char* name, const char* legacyName, const std::string& fallback) {
    return envOrDefault(name, envOrDefault(legacyName, fallback));
}

std::string pathJoin(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs.back() == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

std::string directoryName(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

std::string baseName(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

bool fileExists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

std::string currentWorkingDirectory() {
    char path[PATH_MAX];
    if (!getcwd(path, sizeof(path))) {
        return {};
    }
    return path;
}

std::string executableDirectory() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }
    path.resize(std::strlen(path.c_str()));
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved)) {
        path = resolved;
    }
    return directoryName(path);
}

std::string defaultLauncherDir() {
    const std::string executableDir = executableDirectory();
    if (!executableDir.empty() &&
        fileExists(pathJoin(executableDir, kQemuHeadlessRelativePath))) {
        return executableDir;
    }

    if (baseName(executableDir) == "MacOS") {
        const std::string contentsDir = directoryName(executableDir);
        if (baseName(contentsDir) == "Contents") {
            const std::string bundledDist = pathJoin(contentsDir, "Resources/emulator");
            if (fileExists(pathJoin(bundledDist, kQemuHeadlessRelativePath))) {
                return bundledDist;
            }
        }
    }

    if (!executableDir.empty()) {
        const std::string siblingDist =
            pathJoin(directoryName(executableDir), "distribution/emulator");
        if (fileExists(pathJoin(siblingDist, kQemuHeadlessRelativePath))) {
            return siblingDist;
        }
    }

    const std::string cwd = currentWorkingDirectory();
    if (!cwd.empty()) {
        const std::string repoDist = pathJoin(cwd, "build/cmake/distribution/emulator");
        if (fileExists(pathJoin(repoDist, kQemuHeadlessRelativePath))) {
            return repoDist;
        }
    }

    return executableDir.empty() ? "." : executableDir;
}

std::string defaultQemuPath(const std::string& launcherDir) {
    return pathJoin(launcherDir, kQemuHeadlessRelativePath);
}

std::string defaultDyldLibraryPath(const std::string& launcherDir) {
    const std::string lib64 = pathJoin(launcherDir, "lib64");
    return lib64 + ":" + pathJoin(lib64, "gles_angle") + ":" + pathJoin(lib64, "vulkan");
}

std::string defaultAndroidSdkRoot() {
    if (const char* home = std::getenv("HOME")) {
        if (home[0] != '\0') {
            return pathJoin(home, "Library/Android/sdk");
        }
    }
    return {};
}

NSSize fittedWindowContentSize(uint32_t pixelWidth, uint32_t pixelHeight) {
    if (pixelWidth == 0 || pixelHeight == 0) {
        return NSMakeSize(420, 720);
    }

    CGFloat width = static_cast<CGFloat>(pixelWidth);
    CGFloat height = static_cast<CGFloat>(pixelHeight);
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

ShellOptions parseOptions(int argc, char** argv) {
    ShellOptions options;
    options.launcherDir =
        envOrDefault("MACMU_LAUNCHER_DIR", "AEMU_SHELL_LAUNCHER_DIR", defaultLauncherDir());
    options.qemuPath =
        envOrDefault("MACMU_QEMU_PATH", "AEMU_SHELL_QEMU_PATH",
                     defaultQemuPath(options.launcherDir));
    options.dyldLibraryPath =
        envOrDefault("MACMU_DYLD_LIBRARY_PATH", "AEMU_SHELL_DYLD_LIBRARY_PATH",
                     defaultDyldLibraryPath(options.launcherDir));
    options.metadataPath =
        envOrDefault("MACMU_IOSURFACE_EXPORT_PATH", "AEMU_IOSURFACE_EXPORT_PATH",
                     options.metadataPath);
    options.androidSdkRoot = envOrDefault(
        "MACMU_SDK_ROOT",
        envOrDefault("ANDROID_SDK_ROOT", envOrDefault("ANDROID_HOME", defaultAndroidSdkRoot())));
    options.avdName = envOrDefault("MACMU_AVD_NAME", "AEMU_SHELL_AVD_NAME", options.avdName);
    bool qemuPathOverridden =
        std::getenv("MACMU_QEMU_PATH") != nullptr ||
        std::getenv("AEMU_SHELL_QEMU_PATH") != nullptr;
    bool dyldLibraryPathOverridden =
        std::getenv("MACMU_DYLD_LIBRARY_PATH") != nullptr ||
        std::getenv("AEMU_SHELL_DYLD_LIBRARY_PATH") != nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const char* option) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", option);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--attach") {
            options.launchQemu = false;
        } else if (arg == "--metadata") {
            if (const char* value = requireValue("--metadata")) {
                options.metadataPath = value;
            }
        } else if (arg == "--launcher-dir") {
            if (const char* value = requireValue("--launcher-dir")) {
                options.launcherDir = value;
                if (!qemuPathOverridden) {
                    options.qemuPath = defaultQemuPath(options.launcherDir);
                }
                if (!dyldLibraryPathOverridden) {
                    options.dyldLibraryPath = defaultDyldLibraryPath(options.launcherDir);
                }
            }
        } else if (arg == "--qemu") {
            if (const char* value = requireValue("--qemu")) {
                options.qemuPath = value;
                qemuPathOverridden = true;
            }
        } else if (arg == "--dyld-library-path") {
            if (const char* value = requireValue("--dyld-library-path")) {
                options.dyldLibraryPath = value;
                dyldLibraryPathOverridden = true;
            }
        } else if (arg == "--sdk-root") {
            if (const char* value = requireValue("--sdk-root")) {
                options.androidSdkRoot = value;
            }
        } else if (arg == "--avd") {
            if (const char* value = requireValue("--avd")) {
                options.avdName = value;
            }
        } else if (!arg.empty() && arg[0] != '-') {
            options.metadataPath = arg;
        }
    }
    return options;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::ostringstream stream;
    stream << in.rdbuf();
    return stream.str();
}

uint64_t readJsonNumber(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return 0;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return 0;
    }
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
        ++pos;
    }
    return std::strtoull(json.c_str() + pos, nullptr, 10);
}

bool readMetadata(const std::string& path, SurfaceMetadata* metadata) {
    const std::string json = readFile(path);
    if (json.empty()) {
        return false;
    }
    SurfaceMetadata next = {
        .iosurfaceId = static_cast<IOSurfaceID>(readJsonNumber(json, "iosurface_id")),
        .width = static_cast<uint32_t>(readJsonNumber(json, "width")),
        .height = static_cast<uint32_t>(readJsonNumber(json, "height")),
        .frame = readJsonNumber(json, "frame"),
    };
    if (next.iosurfaceId == 0 || next.width == 0 || next.height == 0) {
        return false;
    }
    *metadata = next;
    return true;
}

bool hasKey(const std::vector<std::pair<std::string, std::string>>& overrides,
            const std::string& key) {
    for (const auto& override : overrides) {
        if (override.first == key) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> makeEnvironment(
    const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::string> environment;
    for (char** current = environ; current && *current; ++current) {
        std::string entry(*current);
        const size_t equals = entry.find('=');
        const std::string key = equals == std::string::npos ? entry : entry.substr(0, equals);
        if (!hasKey(overrides, key)) {
            environment.push_back(std::move(entry));
        }
    }
    for (const auto& override : overrides) {
        environment.push_back(override.first + "=" + override.second);
    }
    return environment;
}

std::vector<char*> makeCStringVector(std::vector<std::string>& values) {
    std::vector<char*> pointers;
    pointers.reserve(values.size() + 1);
    for (std::string& value : values) {
        pointers.push_back(value.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

void removeMetadataFiles(const std::string& metadataPath) {
    std::remove(metadataPath.c_str());
    std::remove((metadataPath + ".tmp").c_str());
}

pid_t launchQemu(const ShellOptions& options) {
    removeMetadataFiles(options.metadataPath);

    std::vector<std::string> args = {
        options.qemuPath,
        "-avd",
        options.avdName,
        "-no-window",
        "-no-audio",
        "-no-snapshot",
        "-no-boot-anim",
        "-gpu",
        "host",
    };
    std::vector<char*> argv = makeCStringVector(args);

    std::vector<std::string> environment = makeEnvironment({
        {"MACMU_IOSURFACE_EXPORT", "1"},
        {"MACMU_IOSURFACE_EXPORT_PATH", options.metadataPath},
        {"AEMU_IOSURFACE_EXPORT", "1"},
        {"AEMU_IOSURFACE_EXPORT_PATH", options.metadataPath},
        {"ANDROID_SDK_ROOT", options.androidSdkRoot},
        {"ANDROID_HOME", options.androidSdkRoot},
        {"ANDROID_EMULATOR_LAUNCHER_DIR", options.launcherDir},
        {"ANDROID_EMULATOR_WRAPPER_PID", std::to_string(getpid())},
        {"DYLD_LIBRARY_PATH", options.dyldLibraryPath},
        {"LC_ALL", "C"},
        {"MESA_RGB_VISUAL", "TrueColor 24"},
        {"SWIFT_BACKTRACE", "enable=no"},
    });
    std::vector<char*> envp = makeCStringVector(environment);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    pid_t pid = -1;
    const int result = posix_spawn(&pid, options.qemuPath.c_str(), nullptr, &attributes,
                                   argv.data(), envp.data());
    posix_spawnattr_destroy(&attributes);
    if (result != 0) {
        std::fprintf(stderr, "Failed to launch qemu-system-aarch64-headless: %s\n",
                     std::strerror(result));
        return -1;
    }
    return pid;
}

void terminateQemu(pid_t pid) {
    if (pid <= 0) {
        return;
    }

    if (waitpid(pid, nullptr, WNOHANG) == pid) {
        return;
    }

    if (kill(-pid, SIGTERM) != 0) {
        kill(pid, SIGTERM);
    }
    for (int i = 0; i < 50; ++i) {
        if (waitpid(pid, nullptr, WNOHANG) == pid) {
            return;
        }
        usleep(100000);
    }

    if (kill(-pid, SIGKILL) != 0) {
        kill(pid, SIGKILL);
    }
    waitpid(pid, nullptr, 0);
}

}  // namespace

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
    terminateQemu(_qemuPid);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

@end

@interface MacMuSurfaceRenderer : NSObject <MTKViewDelegate>
- (instancetype)initWithView:(MTKView*)view metadataPath:(std::string)metadataPath;
@end

@implementation MacMuSurfaceRenderer {
    MTKView* _view;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    std::string _metadataPath;
    SurfaceMetadata _metadata;
    IOSurfaceRef _surface;
    id<MTLTexture> _surfaceTexture;
}

- (instancetype)initWithView:(MTKView*)view metadataPath:(std::string)metadataPath {
    self = [super init];
    if (!self) {
        return nil;
    }

    _view = view;
    _device = view.device;
    _queue = [_device newCommandQueue];
    _metadataPath = metadataPath;
    _surface = nullptr;

    static NSString* shaderSource =
        @"#include <metal_stdlib>\n"
        @"using namespace metal;\n"
        @"struct VSOut { float4 position [[position]]; float2 uv; };\n"
        @"vertex VSOut vertexMain(uint vertexId [[vertex_id]]) {\n"
        @"  float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), "
        @"float2(-1.0, 3.0) };\n"
        @"  VSOut out;\n"
        @"  out.position = float4(positions[vertexId], 0.0, 1.0);\n"
        @"  out.uv = float2((positions[vertexId].x + 1.0) * 0.5, "
        @"1.0 - (positions[vertexId].y + 1.0) * 0.5);\n"
        @"  return out;\n"
        @"}\n"
        @"fragment half4 fragmentMain(VSOut in [[stage_in]], "
        @"texture2d<half> surfaceTexture [[texture(0)]]) {\n"
        @"  constexpr sampler textureSampler(address::clamp_to_edge, filter::linear);\n"
        @"  return surfaceTexture.sample(textureSampler, in.uv);\n"
        @"}\n";

    NSError* error = nil;
    id<MTLLibrary> library = [_device newLibraryWithSource:shaderSource options:nil error:&error];
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
}

- (void)reloadSurfaceIfNeeded {
    SurfaceMetadata next = {};
    if (!readMetadata(_metadataPath, &next)) {
        return;
    }
    if (next.iosurfaceId == _metadata.iosurfaceId && next.width == _metadata.width &&
        next.height == _metadata.height) {
        _metadata.frame = next.frame;
        return;
    }

    IOSurfaceRef surface = IOSurfaceLookup(next.iosurfaceId);
    if (!surface) {
        return;
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
        return;
    }

    if (_surface) {
        CFRelease(_surface);
    }
    _surface = surface;
    _surfaceTexture = texture;
    _metadata = next;
    const NSSize contentSize = fittedWindowContentSize(next.width, next.height);
    [_view.window setContentAspectRatio:NSMakeSize(next.width, next.height)];
    [_view.window setContentSize:contentSize];
    [_view.window setTitle:[NSString stringWithFormat:@"MacMu IOSurface %u", next.iosurfaceId]];
}

- (MTLViewport)aspectFitViewportForView:(MTKView*)view {
    const CGSize drawableSize = view.drawableSize;
    if (_metadata.width == 0 || _metadata.height == 0 || drawableSize.width <= 0 ||
        drawableSize.height <= 0) {
        return MTLViewport{0.0, 0.0, static_cast<double>(drawableSize.width),
                           static_cast<double>(drawableSize.height), 0.0, 1.0};
    }

    const double drawableWidth = drawableSize.width;
    const double drawableHeight = drawableSize.height;
    const double sourceAspect =
        static_cast<double>(_metadata.width) / static_cast<double>(_metadata.height);
    const double drawableAspect = drawableWidth / drawableHeight;

    double width = drawableWidth;
    double height = drawableHeight;
    if (drawableAspect > sourceAspect) {
        width = drawableHeight * sourceAspect;
    } else {
        height = drawableWidth / sourceAspect;
    }

    return MTLViewport{(drawableWidth - width) * 0.5, (drawableHeight - height) * 0.5,
                       width, height, 0.0, 1.0};
}

- (void)drawInMTKView:(MTKView*)view {
    [self reloadSurfaceIfNeeded];
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

    id<MTLCommandBuffer> commandBuffer = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    const MTLViewport viewport = [self aspectFitViewportForView:view];
    [encoder setViewport:viewport];
    [encoder setRenderPipelineState:_pipeline];
    [encoder setFragmentTexture:_surfaceTexture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
}

@end

static MacMuAppDelegate* gAppDelegate;
static MacMuSurfaceRenderer* gRenderer;

int main(int argc, char** argv) {
    @autoreleasepool {
        const ShellOptions options = parseOptions(argc, argv);
        const pid_t qemuPid = options.launchQemu ? launchQemu(options) : -1;
        if (options.launchQemu && qemuPid <= 0) {
            return 1;
        }

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

        MTKView* view = [[MTKView alloc] initWithFrame:frame device:device];
        view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        view.clearColor = MTLClearColorMake(0.03, 0.03, 0.035, 1.0);
        view.paused = NO;
        view.enableSetNeedsDisplay = NO;
        view.preferredFramesPerSecond = 60;
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        gRenderer = [[MacMuSurfaceRenderer alloc] initWithView:view metadataPath:options.metadataPath];
        view.delegate = gRenderer;
        window.contentView = view;
        [window makeKeyAndOrderFront:nil];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
