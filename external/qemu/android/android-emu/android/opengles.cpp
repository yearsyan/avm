/* Copyright (C) 2011 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

#include "host-common/opengles.h"
#include "android/opengles-overrides.h"

#include "aemu/base/CpuUsage.h"
#include "aemu/base/async/ThreadLooper.h"
#include "aemu/base/files/PathUtils.h"
#include "aemu/base/files/Stream.h"
#include "aemu/base/memory/MemoryTracker.h"
#include "aemu/base/Log.h"
#include "aemu/base/logging/LogSeverity.h"
#include "android/base/system/System.h"
#include "android/console.h"
#include "android/metrics/DependentMetrics.h"
#include "android/opengl/GLProcessPipe.h"
#include "android/snapshot/PathUtils.h"
#include "android/snapshot/Snapshotter.h"
#include "android/utils/bufprint.h"
#include "android/utils/debug.h"
#include "android/utils/dll.h"
#include "android/utils/path.h"
#include "config-host.h"
#include "gfxstream/host/features.h"
#include "host-common/FeatureControl.h"
#include "host-common/GfxstreamFatalError.h"
#include "host-common/GoldfishDma.h"
#include "host-common/RefcountPipe.h"
#include "host-common/address_space_device.h"
#include "host-common/address_space_graphics.h"
#include "host-common/crash-handler.h"
#include "host-common/goldfish_sync.h"
#include "host-common/opengl/emugl_config.h"
#include "host-common/opengl/gpuinfo.h"
#include "OpenGLESDispatch/EGLDispatch.h"
#include "OpenGLESDispatch/GLESv2Dispatch.h"
#include "render-utils/address_space_graphics_types.h"
#include "render-utils/address_space_operations.h"
#include "render-utils/dma_device.h"
#include "render-utils/logging_operations.h"
#include "render-utils/render_api_functions.h"
#include "aemu/base/Log.h"
#include "absl/strings/str_cat.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <optional>

using namespace gfxstream::host::gl;
using android::base::pj;
using android::base::System;
using android::emulation::asg::AddressSpaceGraphicsContext;
using gfxstream::AsgConsumerCreateInfo;
using gfxstream::ConsumerCallbacks;
using gfxstream::ConsumerInterface;

/* Name of the GLES rendering library we're going to use */
#define RENDERER_LIB_NAME "libgfxstream_backend"

static struct AndroidOpenglesFuncs* sActiveOpenglesFuncs = nullptr;
static struct AndroidOpenglesFuncs* getDefaultOpenglesFuncs();

/**
 * @brief Retrieves the active OpenGLES function dispatch table.
 *
 * This function ensures that sActiveOpenglesFuncs is initialized. If no backend
 * has called android_setOpenglesFuncs, it defaults to the standard
 * gfxstream-based implementation.
 *
 * @return Pointer to the active AndroidOpenglesFuncs struct.
 */
static struct AndroidOpenglesFuncs* getActiveOpenglesFuncs() {
    if (sActiveOpenglesFuncs) {
        return sActiveOpenglesFuncs;
    }
    return getDefaultOpenglesFuncs();
}

static void (*gfxstream_android_setOpenglesRenderer)(
        gfxstream::RendererPtr* ptr) = NULL;
static void (*gfxstream_android_stopOpenglesRenderer)(bool wait) = NULL;

/* Declared in "android/console.h" */
int android_gles_fast_pipes = 1;

// Define the Render API function pointers.
#define FUNCTION_(ret, name, sig, params) static ret(*name) sig = NULL;
LIST_RENDER_API_FUNCTIONS(FUNCTION_)
#undef FUNCTION_

// Define a function that initializes the function pointers by looking up
// the symbols from the shared library.
static int initOpenglesEmulationFuncs(ADynamicLibrary* rendererLib) {
    void* symbol;
    char* error;

#define FUNCTION_(ret, name, sig, params)                                   \
    symbol = adynamicLibrary_findSymbol(rendererLib, #name, &error);        \
    if (symbol != NULL) {                                                   \
        using type = ret(sig);                                              \
        name = (type*)symbol;                                               \
    } else {                                                                \
        derror("GLES emulation: Could not find required symbol (%s): %s", #name, \
          error);                                                           \
        free(error);                                                        \
        return -1;                                                          \
    }
    LIST_RENDER_API_FUNCTIONS(FUNCTION_)
#undef FUNCTION_

#define LIST_GFXSTREAM_MISC_FUNCTIONS(X)                                \
    X(void, android_setOpenglesRenderer, (gfxstream::RendererPtr*), ()) \
    X(void, android_stopOpenglesRenderer, (bool), ())

#define FUNCTION_(ret, name, sig, params)                                   \
    symbol = adynamicLibrary_findSymbol(rendererLib, #name, &error);        \
    if (symbol != NULL) {                                                   \
        using type = ret(sig);                                              \
        gfxstream_##name = (type*)symbol;                                   \
    } else {                                                                \
        derror("GLES emulation: Could not find required symbol (%s): %s", #name, \
          error);                                                           \
        free(error);                                                        \
        return -1;                                                          \
    }
    LIST_GFXSTREAM_MISC_FUNCTIONS(FUNCTION_)
#undef LIST_GFXSTREAM_MISC_FUNCTIONS
#undef FUNCTION_
    return 0;
}

static bool sRendererUsesSubWindow = false;
static bool sEgl2egl = false;
static gfxstream::RenderLibPtr sRenderLib = nullptr;
static gfxstream::RendererPtr sRenderer = nullptr;

static const EGLDispatch* sEgl = nullptr;
static const GLESv2Dispatch* sGlesv2 = nullptr;

static bool sRunningInGfxstreamBackend = false;

void android_setOpenglesFuncs(struct AndroidOpenglesFuncs* funcs) {
    sActiveOpenglesFuncs = funcs;
}

// The two functions below only are called in gfxstream backend, which assumes
// RenderLib is a static library.
static int prepareOpenglesEmulationImpl(void) {
    sRunningInGfxstreamBackend = true;
    return android_initOpenglesEmulation();
}

int android_prepareOpenglesEmulation(void) {
    return getActiveOpenglesFuncs()->prepareOpenglesEmulation();
}

static int setOpenglesEmulationImpl(void* renderLib,
                                    void* eglDispatch,
                                    void* glesv2Dispatch) {
    sRunningInGfxstreamBackend = true;
    return 0;
}

int android_setOpenglesEmulation(void* renderLib,
                                 void* eglDispatch,
                                 void* glesv2Dispatch) {
    return getActiveOpenglesFuncs()->setOpenglesEmulation(
            renderLib, eglDispatch, glesv2Dispatch);
}

static int initOpenglesEmulationImpl() {
    char* error = NULL;

    if (sRenderLib != NULL)
        return 0;

    dinfo("Initializing gfxstream backend");

    ADynamicLibrary* rendererSo =
            adynamicLibrary_open(RENDERER_LIB_NAME, &error);
    if (rendererSo == NULL) {
        derror("Could not load gfxstream backend library [%s]: %s",
          RENDERER_LIB_NAME, error);

        derror("Retrying in program directory/lib64...");

        auto progDir = System::get()->getProgramDirectory();

        auto retryLibPath = pj({progDir, "lib64", RENDERER_LIB_NAME});

        rendererSo = adynamicLibrary_open(retryLibPath.c_str(), &error);

        if (rendererSo == nullptr) {
            derror("Could not load gfxstream backend library [%s]: %s (2nd try)",
              retryLibPath.c_str(), error);
            return -1;
        }
    }

    /* Resolve the functions */
    if (initOpenglesEmulationFuncs(rendererSo) < 0) {
        derror("Gfxstream backend library mismatch. Be sure to use the correct "
          "version!");
        crashhandler_append_message_format(
                "Gfxstream backend library mismatch. Be sure to use the "
                "correct version!");
        goto BAD_EXIT;
    }

    sRenderLib = initLibrary();
    if (!sRenderLib) {
        derror("Gfxstream backend initialization failed!");
        crashhandler_append_message_format("Gfxstream backend initialization failed!");
        goto BAD_EXIT;
    }

    sRendererUsesSubWindow = true;
    if (const char* env = getenv("ANDROID_GL_SOFTWARE_RENDERER")) {
        if (env[0] != '\0' && env[0] != '0') {
            sRendererUsesSubWindow = false;
        }
    }

    sEgl2egl = false;
    if (const char* env = getenv("ANDROID_EGL_ON_EGL")) {
        if (env[0] != '\0' && env[0] == '1') {
            sEgl2egl = true;
        }
    }

    return 0;

BAD_EXIT:
    derror("Gfxstream backend library could not be initialized!");
    adynamicLibrary_close(rendererSo);
    return -1;
}

int android_initOpenglesEmulation() {
    return getActiveOpenglesFuncs()->initOpenglesEmulation();
}

namespace android {
namespace featurecontrol {
extern bool isEnabledLocal(Feature feature);
}
}  // namespace android

void gfxstreamLoggingCallback(
        gfxstream_logging_level level,
        const char* file,
        int line,
        const char* function,
        const char* message) {
    LogSeverity priority = EMULATOR_LOG_INFO;
    switch (level) {
        case GFXSTREAM_LOGGING_LEVEL_FATAL:
            priority = EMULATOR_LOG_FATAL;
            break;
        case GFXSTREAM_LOGGING_LEVEL_ERROR:
            priority = EMULATOR_LOG_ERROR;
            break;
        case GFXSTREAM_LOGGING_LEVEL_WARNING:
            priority = EMULATOR_LOG_WARNING;
            break;
        case GFXSTREAM_LOGGING_LEVEL_INFO:
            priority = EMULATOR_LOG_INFO;
            break;
        case GFXSTREAM_LOGGING_LEVEL_DEBUG:
            priority = EMULATOR_LOG_DEBUG;
            break;
        case GFXSTREAM_LOGGING_LEVEL_VERBOSE:
            priority = EMULATOR_LOG_VERBOSE;
            break;
    }

    __emu_log_print_str(priority, file, line, message);
}

static const QAndroidVmOperations* sVmOperations = nullptr;

void RendererUnmapMemoryAsyncCallback(uint64_t gpa, uint64_t size) {
    if (sVmOperations != nullptr) {
        // See b/405345461.
        android::base::ThreadLooper::runOnMainLooper(
                [gpa, size, ops = sVmOperations] {
                    ops->unmapUserBackedRam(gpa, size);
                });
    }
}


static int startOpenglesRendererImpl(
        int width,
        int height,
        bool guestPhoneApi,
        int guestApiLevel,
        const QAndroidVmOperations* vm_operations,
        const QAndroidEmulatorWindowAgent* window_agent,
        const QAndroidMultiDisplayAgent* multi_display_agent,
        const void* /*gfxstreamFeatureSet*/,
        int* glesMajorVersion_out,
        int* glesMinorVersion_out) {
    if (!sRenderLib) {
        derror("Can't start the renderer without support libraries");
        return -1;
    }

    if (sRenderer) {
        return 0;
    }

    const GpuInfoList& gpuList = globalGpuInfoList();
    std::string gpuInfoAsString = gpuList.dump();
    dinfo("%s: gpu info", __func__);
    dinfo("%s", gpuInfoAsString.c_str());

    sRenderLib->setLogger(gfxstreamLoggingCallback);
    if (getMinLogLevel() <= EMULATOR_LOG_DEBUG) {
        // Set verbose logging on gfxstream with -verbose option
        sRenderLib->setLogLevel(GFXSTREAM_LOGGING_LEVEL_VERBOSE);
    }

    sRenderLib->setRenderer(emuglConfig_get_current_renderer());
    sRenderLib->setGuestAndroidApiLevel(guestApiLevel);

    gfxstream::host::FeatureSet gfxstreamFeatures;
    using GfxstreamFeaturePtr =
            gfxstream::host::BoolFeatureInfo(gfxstream::host::FeatureSet::*);
    const std::map<android::featurecontrol::Feature, GfxstreamFeaturePtr>
            kAemuToGfxstreamFeatureMap = {
                    {android::featurecontrol::AsyncComposeSupport,
                     &gfxstream::host::FeatureSet::AsyncComposeSupport},
                    {android::featurecontrol::ExternalBlob,
                     &gfxstream::host::FeatureSet::ExternalBlob},
                    {android::featurecontrol::GLAsyncSwap,
                     &gfxstream::host::FeatureSet::GlAsyncSwap},
                    {android::featurecontrol::GLDirectMem,
                     &gfxstream::host::FeatureSet::GlDirectMem},
                    {android::featurecontrol::GLDMA,
                     &gfxstream::host::FeatureSet::GlDma},
                    {android::featurecontrol::GLDMA2,
                     &gfxstream::host::FeatureSet::GlDma2},
                    {android::featurecontrol::GLESDynamicVersion,
                     &gfxstream::host::FeatureSet::GlesDynamicVersion},
                    {android::featurecontrol::GLPipeChecksum,
                     &gfxstream::host::FeatureSet::GlPipeChecksum},
                    {android::featurecontrol::GrallocSync,
                     &gfxstream::host::FeatureSet::GrallocSync},
                    {android::featurecontrol::GuestAngle,
                     &gfxstream::host::FeatureSet::GuestVulkanOnly},
                    {android::featurecontrol::HasSharedSlotsHostMemoryAllocator,
                     &gfxstream::host::FeatureSet::
                             HasSharedSlotsHostMemoryAllocator},
                    {android::featurecontrol::HostComposition,
                     &gfxstream::host::FeatureSet::HostComposition},
                    {android::featurecontrol::HWCMultiConfigs,
                     &gfxstream::host::FeatureSet::HwcMultiConfigs},
                    {android::featurecontrol::Minigbm,
                     &gfxstream::host::FeatureSet::Minigbm},
                    {android::featurecontrol::NativeTextureDecompression,
                     &gfxstream::host::FeatureSet::NativeTextureDecompression},
                    {android::featurecontrol::NoDelayCloseColorBuffer,
                     &gfxstream::host::FeatureSet::NoDelayCloseColorBuffer},
                    {android::featurecontrol::RefCountPipe,
                     &gfxstream::host::FeatureSet::RefCountPipe},
                    {android::featurecontrol::SystemBlob,
                     &gfxstream::host::FeatureSet::SystemBlob},
                    {android::featurecontrol::VirtioGpuFenceContexts,
                     &gfxstream::host::FeatureSet::VirtioGpuFenceContexts},
                    {android::featurecontrol::VirtioGpuNativeSync,
                     &gfxstream::host::FeatureSet::VirtioGpuNativeSync},
                    {android::featurecontrol::VirtioGpuNext,
                     &gfxstream::host::FeatureSet::VirtioGpuNext},
                    {android::featurecontrol::Vulkan,
                     &gfxstream::host::FeatureSet::Vulkan},
                    {android::featurecontrol::VulkanAllocateDeviceMemoryOnly,
                     &gfxstream::host::FeatureSet::
                             VulkanAllocateDeviceMemoryOnly},
                    {android::featurecontrol::VulkanAllocateHostMemory,
                     &gfxstream::host::FeatureSet::VulkanAllocateHostMemory},
                    {android::featurecontrol::VulkanBatchedDescriptorSetUpdate,
                     &gfxstream::host::FeatureSet::
                             VulkanBatchedDescriptorSetUpdate},
                    {android::featurecontrol::VulkanIgnoredHandles,
                     &gfxstream::host::FeatureSet::VulkanIgnoredHandles},
                    {android::featurecontrol::VulkanNativeSwapchain,
                     &gfxstream::host::FeatureSet::VulkanNativeSwapchain},
                    {android::featurecontrol::VulkanNullOptionalStrings,
                     &gfxstream::host::FeatureSet::VulkanNullOptionalStrings},
                    {android::featurecontrol::VulkanQueueSubmitWithCommands,
                     &gfxstream::host::FeatureSet::
                             VulkanQueueSubmitWithCommands},
                    {android::featurecontrol::VulkanShaderFloat16Int8,
                     &gfxstream::host::FeatureSet::VulkanShaderFloat16Int8},
                    {android::featurecontrol::VulkanSnapshots,
                     &gfxstream::host::FeatureSet::VulkanSnapshots},
                    {android::featurecontrol::YUV420888toNV21,
                     &gfxstream::host::FeatureSet::Yuv420888ToNv21},
                    {android::featurecontrol::YUVCache,
                     &gfxstream::host::FeatureSet::YuvCache},
                    {android::featurecontrol::BypassVulkanDeviceFeatureOverrides,
                     &gfxstream::host::FeatureSet::BypassVulkanDeviceFeatureOverrides},
                    {android::featurecontrol::VulkanDebugUtils,
                     &gfxstream::host::FeatureSet::VulkanDebugUtils},
                    {android::featurecontrol::VulkanCommandBufferCheckpoints,
                     &gfxstream::host::FeatureSet::VulkanCommandBufferCheckpoints},
                    {android::featurecontrol::VulkanVirtualQueue,
                     &gfxstream::host::FeatureSet::VulkanVirtualQueue},
                    {android::featurecontrol::VulkanRobustness,
                     &gfxstream::host::FeatureSet::VulkanRobustness},
                    {android::featurecontrol::VulkanProtectedMemoryEmulation,
                     &gfxstream::host::FeatureSet::VulkanProtectedMemoryEmulation},
            };

    std::string reportGfxstreamFeatures;
    reportGfxstreamFeatures.reserve(1024);
    for (const auto& [aemuFeature, gfxstreamFeaturePtr] :
         kAemuToGfxstreamFeatureMap) {
        (gfxstreamFeatures.*gfxstreamFeaturePtr)
                .setEnabled(android::featurecontrol::isEnabled(aemuFeature));

        // TODO(b/389646068): special handling of 'GuestVulkanOnly' as we still
        // depend on flush-to-gl behavior on GL host composition (ie. when
        // VulkanNativeSwapchain is NOT enabled). So this feature requires 2
        // separate aemu features to be checked to enable.
        if (gfxstreamFeaturePtr ==
            &gfxstream::host::FeatureSet::GuestVulkanOnly) {
            gfxstreamFeatures.GuestVulkanOnly.setEnabled(
                    android::featurecontrol::isEnabled(
                            android::featurecontrol::GuestAngle) &&
                    android::featurecontrol::isEnabled(
                            android::featurecontrol::VulkanNativeSwapchain));
        }

        const std::string& featureName = (gfxstreamFeatures.*gfxstreamFeaturePtr).getName();
        const bool featureEnabled = (gfxstreamFeatures.*gfxstreamFeaturePtr).enabled();
        dprint("gfxstreamFeature:%s = %d", featureName.c_str(), featureEnabled);
        if (featureEnabled) {
            absl::StrAppend(&reportGfxstreamFeatures, featureName);
            absl::StrAppend(&reportGfxstreamFeatures, ", ");
        }

        // These flags should not be changed anymore, as that won't be
        // reflected on the gfxstream side.
        android::featurecontrol::makeReadOnly(aemuFeature);
    }

    std::string maxGuestVulkan = "1.3";
    if (const char* env = getenv("ANDROID_EMU_VK_ENABLE_1_4")) {
        // Allow force-enabling with env variable for tests
        if (env[0] == '1') {
            maxGuestVulkan = "1.4";
        }
    } else if (guestApiLevel >= 37) {
        // TODO(b/512021931): Issues with descriptor binding support with
        // Vulkan 1.4, reenable the support for API 37+ once the issues are
        // fixed.
        dinfo("Vulkan 1.4 is not supported for the guest and won't be enabled");
    }
    if(!gfxstreamFeatures.GuestVulkanMaxApiVersion.parseValue(maxGuestVulkan)) {
        derror("Could not set GuestVulkanMaxApiVersion to '%s'", maxGuestVulkan.c_str());
    }

    gfxstreamFeatures.EglOnEgl.setEnabled(sEgl2egl);
    crashhandler_add_string("gfxstream_features", reportGfxstreamFeatures.c_str());

    sRenderLib->setSyncDevice(
            goldfish_sync_create_timeline, goldfish_sync_create_fence,
            goldfish_sync_timeline_inc, goldfish_sync_destroy_timeline,
            goldfish_sync_register_trigger_wait, goldfish_sync_device_exists);
    sRenderLib->setGrallocImplementation(
            android::featurecontrol::isEnabled(
                    android::featurecontrol::Minigbm) ||
                            getConsoleAgents()->settings->is_fuchsia()
                    ? MINIGBM
                    : GOLDFISH_GRALLOC);

    gfxstream_dma_ops dma_ops;
    dma_ops.get_host_addr = android_goldfish_dma_ops.get_host_addr;
    dma_ops.unlock = android_goldfish_dma_ops.unlock;
    sRenderLib->setDmaOps(dma_ops);

    sVmOperations = vm_operations;

    sRenderLib->setVmOps(gfxstream_vm_ops{
            .map_user_memory = vm_operations->mapUserBackedRam,
            .unmap_user_memory = vm_operations->unmapUserBackedRam,
            .unmap_user_memory_async = RendererUnmapMemoryAsyncCallback,
            .lookup_user_memory = vm_operations->physicalMemoryGetAddr,
            .register_vulkan_instance = vm_operations->vulkanInstanceRegister,
            .unregister_vulkan_instance = vm_operations->vulkanInstanceUnregister,
            .set_skip_snapshot_save = vm_operations->setSkipSnapshotSave,
            .set_skip_snapshot_save_reason = vm_operations->setSkipSnapshotSaveReason,
            .set_snapshot_uses_vulkan = vm_operations->setStatSnapshotUseVulkan,
            .add_crash_reporter_log = vm_operations->addCrashReporterLog,
    });
    sRenderLib->setAddressSpaceDeviceControlOps(
            get_address_space_device_control_ops());
    sRenderLib->setWindowOps(gfxstream_window_ops{
        .is_current_thread_ui_thread = window_agent->isRunningInUiThread,
        .run_on_ui_thread = window_agent->runOnUiThread,
        .paint_multi_display_window = window_agent->paintMultiDisplayWindow,
        .is_folded = window_agent->isFolded,
        .get_folded_area = window_agent->getFoldedArea,
    });
    sRenderLib->setDisplayOps(gfxstream_multi_display_ops{
        .is_multi_display_enabled = multi_display_agent->isMultiDisplayEnabled,
        .is_multi_window = multi_display_agent->isMultiDisplayWindow,
        .is_pixel_fold = multi_display_agent->isPixelFold,
        .get_combined_size = multi_display_agent->getCombinedDisplaySize,
        .get_display_info = multi_display_agent->getMultiDisplay,
        .get_next_display_info = multi_display_agent->getNextMultiDisplay,
        .create_display = multi_display_agent->createDisplay,
        .destroy_display = multi_display_agent->destroyDisplay,
        .get_display_color_buffer = multi_display_agent->getDisplayColorBuffer,
        .set_display_color_buffer = multi_display_agent->setDisplayColorBuffer,
        .get_color_buffer_display = multi_display_agent->getColorBufferDisplay,
        .get_display_pose = multi_display_agent->getDisplayPose,
        .set_display_pose = multi_display_agent->setDisplayPose,
        .get_color_transform_matrix = multi_display_agent->getDisplayColorTransform,
        .set_color_transform_matrix = multi_display_agent->setDisplayColorTransform,
    });

    sRenderer = sRenderLib->initRenderer(width, height, gfxstreamFeatures,
                                         sRendererUsesSubWindow);

    if (sRenderer == nullptr) {
        derror("Can't initialize RenderLib with parameters: width=%d, height=%d "
        "sRendererUsesSubWindow=%d",
        width, height, sRendererUsesSubWindow);
        return -2;
    }

    gfxstream_android_setOpenglesRenderer(&sRenderer);

    sEgl = (const EGLDispatch*)sRenderer->getEglDispatch();
    sGlesv2 = (const GLESv2Dispatch*)sRenderer->getGles2Dispatch();

    android::snapshot::Snapshotter::get().addOperationCallback(
            [](android::snapshot::Snapshotter::Operation op,
               android::snapshot::Snapshotter::Stage stage) {
                switch (op) {
                    case android::snapshot::Snapshotter::Operation::Load:
                        if (stage == android::snapshot::Snapshotter::Stage::Start) {
                            sRenderer->preLoad();
                        }
                        if (stage == android::snapshot::Snapshotter::Stage::End) {
                            sRenderer->postLoad();
                        }
                        break;
                    default:
                        break;
                }
            });

    android::emulation::registerOnLastRefCallback(
            sRenderLib->getOnLastColorBufferRef());

    ConsumerInterface interface = {
            // create
            [](const AsgConsumerCreateInfo& info, gfxstream::Stream* loadStream) {
                return sRenderer->addressSpaceGraphicsConsumerCreate(info, loadStream);

            },
            // destroy
            [](void* consumer) {
                sRenderer->addressSpaceGraphicsConsumerDestroy(consumer);
            },
            // pre save
            [](void* consumer) {
                sRenderer->addressSpaceGraphicsConsumerPreSave(consumer);
            },
            // global presave
            []() { sRenderer->pauseAllPreSave(); },
            // save
            [](void* consumer, gfxstream::Stream* stream) {
                sRenderer->addressSpaceGraphicsConsumerSave(consumer, stream);
            },
            // global postsave
            []() { sRenderer->resumeAll(); },
            // postSave
            [](void* consumer) {
                sRenderer->addressSpaceGraphicsConsumerPostSave(consumer);
            },
            // postLoad
            [](void* consumer) {
                sRenderer
                        ->addressSpaceGraphicsConsumerRegisterPostLoadRenderThread(
                                consumer);
            },
            // global preload
            []() {
                // This wants to address that when using asg, pipe wants to
                // clean up all render threads and wait for gl objects, but
                // framebuffer notices that there is a render thread info that
                // is still not cleaned up because these render threads come
                // from asg.
                android::opengl::forEachProcessPipeIdRunAndErase(
                        [](uint64_t id) { android_cleanupProcGLObjects(id); });
                android_waitForOpenglesProcessCleanup();
            },
            // reloadRingConfig
            [](void* consumer) {
                sRenderer->addressSpaceGraphicsConsumerReloadRingConfig(consumer);
            },
    };
    AddressSpaceGraphicsContext::setConsumer(interface);

    // after initRenderer is a success, the maximum GLES API is calculated
    // depending on feature control and host GPU support. Set the obtained GLES
    // version here.
    if (glesMajorVersion_out && glesMinorVersion_out)
        sRenderLib->getGlesVersion(glesMajorVersion_out, glesMinorVersion_out);
    return 0;
}

int android_startOpenglesRenderer(
        int width,
        int height,
        bool guestPhoneApi,
        int guestApiLevel,
        const QAndroidVmOperations* vm_operations,
        const QAndroidEmulatorWindowAgent* window_agent,
        const QAndroidMultiDisplayAgent* multi_display_agent,
        const void* gfxstreamFeatureSet,
        int* glesMajorVersion_out,
        int* glesMinorVersion_out) {
    return getActiveOpenglesFuncs()->startOpenglesRenderer(
            width, height, guestPhoneApi, guestApiLevel, vm_operations,
            window_agent, multi_display_agent, gfxstreamFeatureSet,
            glesMajorVersion_out, glesMinorVersion_out);
}

static bool asyncReadbackSupportedImpl() {
    if (sRenderer) {
        return sRenderer->asyncReadbackSupported();
    } else {
        dprint("tried to query async readback support "
               "before renderer initialized. Likely guest rendering");
        return false;
    }
}

bool android_asyncReadbackSupported() {
    return getActiveOpenglesFuncs()->asyncReadbackSupported();
}

static void setPostCallbackImpl(OnPostFunc onPost,
                                void* onPostContext,
                                bool useBgraReadback,
                                uint32_t displayId) {
    if (sRenderer) {
        sRenderer->setPostCallback(onPost, onPostContext, useBgraReadback,
                                   displayId);
    }
}

void android_setPostCallback(OnPostFunc onPost,
                             void* onPostContext,
                             bool useBgraReadback,
                             uint32_t displayId) {
    getActiveOpenglesFuncs()->setPostCallback(onPost, onPostContext,
                                                  useBgraReadback, displayId);
}

static ReadPixelsFunc getReadPixelsFuncImpl() {
    if (sRenderer) {
        return sRenderer->getReadPixelsCallback();
    } else {
        return nullptr;
    }
}

ReadPixelsFunc android_getReadPixelsFunc() {
    return getActiveOpenglesFuncs()->getReadPixelsFunc();
}

static FlushReadPixelPipeline getFlushReadPixelPipelineImpl() {
    if (sRenderer) {
        return sRenderer->getFlushReadPixelPipeline();
    } else {
        return nullptr;
    }
}

FlushReadPixelPipeline android_getFlushReadPixelPipeline() {
    return getActiveOpenglesFuncs()->getFlushReadPixelPipeline();
}

static char* strdupBaseString(const char* src) {
    const char* begin = strchr(src, '(');
    if (!begin) {
        return strdup(src);
    }

    const char* end = strrchr(begin + 1, ')');
    if (!end) {
        return strdup(src);
    }

    // src is of the form:
    // "foo (barzzzzzzzzzz)"
    //       ^            ^
    //       (b+1)        e
    //     = 5            18
    int len;
    begin += 1;
    len = end - begin;

    char* result;
    result = (char*)malloc(len + 1);
    memcpy(result, begin, len);
    result[len] = '\0';
    return result;
}

static void getOpenglesHardwareStringsImpl(char** vendor,
                                           char** renderer,
                                           char** version) {
    assert(vendor != NULL && renderer != NULL && version != NULL);
    assert(*vendor == NULL && *renderer == NULL && *version == NULL);
    if (!sRenderer) {
        derror("Can't get GPU strings when renderer not started");
        return;
    }

    auto strings = sRenderer->getHardwareStrings();
    dinfo("GPU Vendor=[%s]", strings.vendor.c_str());
    dinfo("GPU Renderer=[%s]", strings.renderer.c_str());
    dinfo("GPU Version=[%s]", strings.version.c_str());

    /* Special case for the default ES to GL translators: extract the strings
     * of the underlying OpenGL implementation. */
    if (strncmp(strings.vendor.c_str(), "Google", 6) == 0 &&
        strncmp(strings.renderer.c_str(),
                "Android Emulator OpenGL ES Translator", 37) == 0) {
        *vendor = strdupBaseString(strings.vendor.c_str());
        *renderer = strdupBaseString(strings.renderer.c_str());
        *version = strdupBaseString(strings.version.c_str());
    } else {
        *vendor = strdup(strings.vendor.c_str());
        *renderer = strdup(strings.renderer.c_str());
        *version = strdup(strings.version.c_str());
    }
}

void android_getOpenglesHardwareStrings(char** vendor,
                                        char** renderer,
                                        char** version) {
    getActiveOpenglesFuncs()->getOpenglesHardwareStrings(vendor, renderer,
                                                             version);
}

static void getOpenglesVersionImpl(int* maj, int* min) {
    sRenderLib->getGlesVersion(maj, min);
    dinfo("%s: maj min %d %d", __func__, *maj, *min);
}

void android_getOpenglesVersion(int* maj, int* min) {
    getActiveOpenglesFuncs()->getOpenglesVersion(maj, min);
}

static void stopOpenglesRendererImpl(bool wait) {
    if (sRenderer) {
        sRenderer->stop(wait);
        if (wait) {
            sRenderer.reset();
            if (gfxstream_android_stopOpenglesRenderer) {
                gfxstream_android_stopOpenglesRenderer(wait);
            }
        }
    }
}

void android_stopOpenglesRenderer(bool wait) {
    getActiveOpenglesFuncs()->stopOpenglesRenderer(wait);
}

static void finishOpenglesRendererImpl() {
    if (sRenderer) {
        sRenderer->finish();
    }
}

void android_finishOpenglesRenderer() {
    getActiveOpenglesFuncs()->finishOpenglesRenderer();
}

static gfxstream::RenderOpt sOpt;
static int sWidth, sHeight;
static int sNewWidth, sNewHeight;

static int showOpenglesWindowImpl(void* window,
                                  int wx,
                                  int wy,
                                  int ww,
                                  int wh,
                                  int fbw,
                                  int fbh,
                                  float dpr,
                                  float rotation,
                                  bool deleteExisting,
                                  bool hideWindow) {
    if (!sRenderer) {
        return -1;
    }
    FBNativeWindowType win = (FBNativeWindowType)(uintptr_t)window;
    bool success = sRenderer->showOpenGLSubwindow(win, wx, wy, ww, wh, fbw, fbh,
                                                  dpr, rotation, deleteExisting,
                                                  hideWindow);
    sNewWidth = ww * dpr;
    sNewHeight = wh * dpr;
    return success ? 0 : -1;
}

int android_showOpenglesWindow(void* window,
                               int wx,
                               int wy,
                               int ww,
                               int wh,
                               int fbw,
                               int fbh,
                               float dpr,
                               float rotation,
                               bool deleteExisting,
                               bool hideWindow) {
    return getActiveOpenglesFuncs()->showOpenglesWindow(
            window, wx, wy, ww, wh, fbw, fbh, dpr, rotation, deleteExisting,
            hideWindow);
}

static void setOpenglesTranslationImpl(float px, float py) {
    if (sRenderer) {
        sRenderer->setOpenGLDisplayTranslation(px, py);
    }
}

void android_setOpenglesTranslation(float px, float py) {
    getActiveOpenglesFuncs()->setOpenglesTranslation(px, py);
}

static void setOpenglesScreenMaskImpl(int width,
                                      int height,
                                      const uint8_t* rgbaData) {
    if (sRenderer) {
        sRenderer->setScreenMask(width, height, rgbaData);
    }
}

void android_setOpenglesScreenMask(int width,
                                   int height,
                                   const uint8_t* rgbaData) {
    getActiveOpenglesFuncs()->setOpenglesScreenMask(width, height, rgbaData);
}

static void setOpenglesScreenBackgroundImpl(int width,
                                            int height,
                                            const uint8_t* rgbaData) {
    if (sRenderer) {
        sRenderer->setScreenBackground(width, height, rgbaData);
    }
}

void android_setOpenglesScreenBackground(int width,
                                         int height,
                                         const uint8_t* rgbaData) {
    getActiveOpenglesFuncs()->setOpenglesScreenBackground(width, height,
                                                              rgbaData);
}

static void setOpenglesDisplayLayoutImpl(int screenWidth,
                                         int screenHeight,
                                         int displayPosX,
                                         int displayPosY,
                                         int displayWidth,
                                         int displayHeight) {
    if (sRenderer) {
        dprint("%s: screen:%dx%d, display:%d %d %dx%d", __func__, screenWidth,
               screenHeight, displayPosX, displayPosY, displayWidth,
               displayHeight);
        gfxstream::Rect displayRect;
        displayRect.pos.x = displayPosX;
        displayRect.pos.y = displayPosY;
        displayRect.size.w = displayWidth;
        displayRect.size.h = displayHeight;
        sRenderer->setDisplayLayout(screenWidth, screenHeight, displayRect);
    }
}

void android_setOpenglesDisplayLayout(int screenWidth,
                                      int screenHeight,
                                      int displayPosX,
                                      int displayPosY,
                                      int displayWidth,
                                      int displayHeight) {
    getActiveOpenglesFuncs()->setOpenglesDisplayLayout(
            screenWidth, screenHeight, displayPosX, displayPosY, displayWidth,
            displayHeight);
}

static int hideOpenglesWindowImpl(void) {
    if (!sRenderer) {
        return -1;
    }
    bool success = sRenderer->destroyOpenGLSubwindow();
    return success ? 0 : -1;
}

int android_hideOpenglesWindow(void) {
    return getActiveOpenglesFuncs()->hideOpenglesWindow();
}

static void redrawOpenglesWindowImpl(void) {
    if (sRenderer) {
        sRenderer->repaintOpenGLDisplay();
    }
}

void android_redrawOpenglesWindow(void) {
    getActiveOpenglesFuncs()->redrawOpenglesWindow();
}

static void setShouldSkipDrawImpl(bool skip) {
    if (sRenderer) {
        sRenderer->setShouldSkipDraw(skip);
    }
}

void android_setShouldSkipDraw(bool skip) {
    getActiveOpenglesFuncs()->setShouldSkipDraw(skip);
}

static bool getShouldSkipDrawImpl(void) {
    if (sRenderer) {
        return sRenderer->getShouldSkipDraw();
    }
    return false;
}

bool android_getShouldSkipDraw(void) {
    return getActiveOpenglesFuncs()->getShouldSkipDraw();
}

static bool hasGuestPostedAFrameImpl(void) {
    if (sRenderer) {
        return sRenderer->hasGuestPostedAFrame();
    }
    return false;
}

bool android_hasGuestPostedAFrame(void) {
    return getActiveOpenglesFuncs()->hasGuestPostedAFrame();
}

static void resetGuestPostedAFrameImpl(void) {
    if (sRenderer) {
        sRenderer->resetGuestPostedAFrame();
    }
}

void android_resetGuestPostedAFrame(void) {
    getActiveOpenglesFuncs()->resetGuestPostedAFrame();
}

static ScreenshotFunc sScreenshotFunc = nullptr;

static void registerScreenshotFuncImpl(ScreenshotFunc f) {
    sScreenshotFunc = f;
}

void android_registerScreenshotFunc(ScreenshotFunc f) {
    getActiveOpenglesFuncs()->registerScreenshotFunc(f);
}

static bool screenShotImpl(const char* dirname, uint32_t displayId) {
    if (sScreenshotFunc) {
        return sScreenshotFunc(dirname, displayId);
    }
    return false;
}

bool android_screenShot(const char* dirname, uint32_t displayId) {
    return getActiveOpenglesFuncs()->screenShot(dirname, displayId);
}

extern "C" {
const gfxstream::RendererPtr& android_getOpenglesRenderer() {
    return sRenderer;
}

void android_setOpenglesRenderer(gfxstream::RendererPtr* ptr) {
    sRenderer = *ptr;
    // We inject our own opengles.cpp into gfxstream.
    ConsumerInterface interface = {
            // create
            [](const AsgConsumerCreateInfo& info, gfxstream::Stream* loadStream) {
                return sRenderer->addressSpaceGraphicsConsumerCreate(info, loadStream);
            },
            // destroy
            [](void* consumer) {
                sRenderer->addressSpaceGraphicsConsumerDestroy(consumer);
            },
            // pre save
            [](void* consumer) {
                sRenderer->addressSpaceGraphicsConsumerPreSave(consumer);
            },
            // global presave
            []() { sRenderer->pauseAllPreSave(); },
            // save
            [](void* consumer, gfxstream::Stream* stream) {
                sRenderer->addressSpaceGraphicsConsumerSave(consumer, stream);
            },
            // global postsave
            []() { sRenderer->resumeAll(); },
            // postSave
            [](void* consumer) {
                sRenderer->addressSpaceGraphicsConsumerPostSave(consumer);
            },
            // postLoad
            [](void* consumer) {
                sRenderer
                        ->addressSpaceGraphicsConsumerRegisterPostLoadRenderThread(
                                consumer);
            },
            // global preload
            []() {
                // This wants to address that when using asg, pipe wants to
                // clean up all render threads and wait for gl objects, but
                // framebuffer notices that there is a render thread info that
                // is still not cleaned up because these render threads come
                // from asg.
                android::opengl::forEachProcessPipeIdRunAndErase(
                        [](uint64_t id) { android_cleanupProcGLObjects(id); });
                android_waitForOpenglesProcessCleanup();
            },
            // reloadRingConfig
            [](void* consumer) {
                sRenderer->addressSpaceGraphicsConsumerReloadRingConfig(consumer);
            },
    };
    AddressSpaceGraphicsContext::setConsumer(interface);
}
}  // extern "C"

static void onGuestGraphicsProcessCreateImpl(uint64_t puid) {
    if (sRenderer) {
        sRenderer->onGuestGraphicsProcessCreate(puid);
    }
}

void android_onGuestGraphicsProcessCreate(uint64_t puid) {
    getActiveOpenglesFuncs()->onGuestGraphicsProcessCreate(puid);
}

static void cleanupProcGLObjectsImpl(uint64_t puid) {
    if (sRenderer) {
        sRenderer->cleanupProcGLObjects(puid);
    }
}

void android_cleanupProcGLObjects(uint64_t puid) {
    getActiveOpenglesFuncs()->cleanupProcGLObjects(puid);
}

void android_cleanupProcGLObjectsAndWaitFinished(uint64_t puid) {
    getActiveOpenglesFuncs()->cleanupProcGLObjects(puid);
}

static void waitForOpenglesProcessCleanupImpl() {
    if (sRenderer) {
        sRenderer->waitForProcessCleanup();
    }
}

void android_waitForOpenglesProcessCleanup() {
    getActiveOpenglesFuncs()->waitForOpenglesProcessCleanup();
}

static struct AndroidVirtioGpuOps* getVirtioGpuOpsImpl() {
    if (sRenderer) {
        return sRenderer->getVirtioGpuOps();
    }
    return nullptr;
}

struct AndroidVirtioGpuOps* android_getVirtioGpuOps() {
    return getActiveOpenglesFuncs()->getVirtioGpuOps();
}

static const void* getEGLDispatchImpl() {
    return sEgl;
}

const void* android_getEGLDispatch() {
    return getActiveOpenglesFuncs()->getEGLDispatch();
}

static const void* getGLESv2DispatchImpl() {
    return sGlesv2;
}

const void* android_getGLESv2Dispatch() {
    return getActiveOpenglesFuncs()->getGLESv2Dispatch();
}


static void setVsyncHzImpl(int vsyncHz) {
    if (sRenderer) {
        sRenderer->setVsyncHz(vsyncHz);
    }
}

void android_setVsyncHz(int vsyncHz) {
    getActiveOpenglesFuncs()->setVsyncHz(vsyncHz);
}

static void setOpenglesDisplayConfigsImpl(int configId,
                                          int w,
                                          int h,
                                          int dpiX,
                                          int dpiY) {
    if (sRenderer) {
        sRenderer->setDisplayConfigs(configId, w, h, dpiX, dpiY);
    }
}

void android_setOpenglesDisplayConfigs(int configId,
                                       int w,
                                       int h,
                                       int dpiX,
                                       int dpiY) {
    getActiveOpenglesFuncs()->setOpenglesDisplayConfigs(configId, w, h, dpiX,
                                                            dpiY);
}

static void setOpenglesDisplayActiveConfigImpl(int configId) {
    if (sRenderer) {
        sRenderer->setDisplayActiveConfig(configId);
    }
}

void android_setOpenglesDisplayActiveConfig(int configId) {
    getActiveOpenglesFuncs()->setOpenglesDisplayActiveConfig(configId);
}

/**
 * @brief Provides the default OpenGLES implementation.
 *
 * This implementation is based on the standard gfxstream renderer and is
 * used unless a custom backend overrides it via android_setOpenglesFuncs.
 *
 * @return Pointer to a static AndroidOpenglesFuncs struct containing the
 * default implementations.
 */
static struct AndroidOpenglesFuncs* getDefaultOpenglesFuncs() {
    static struct AndroidOpenglesFuncs sDefaultOpenglesFuncs = {
        .prepareOpenglesEmulation = prepareOpenglesEmulationImpl,
        .setOpenglesEmulation = setOpenglesEmulationImpl,
        .initOpenglesEmulation = initOpenglesEmulationImpl,
        .startOpenglesRenderer = startOpenglesRendererImpl,
        .asyncReadbackSupported = asyncReadbackSupportedImpl,
        .setPostCallback = setPostCallbackImpl,
        .getReadPixelsFunc = getReadPixelsFuncImpl,
        .getFlushReadPixelPipeline = getFlushReadPixelPipelineImpl,
        .getOpenglesHardwareStrings = getOpenglesHardwareStringsImpl,
        .getOpenglesVersion = getOpenglesVersionImpl,
        .showOpenglesWindow = showOpenglesWindowImpl,
        .hideOpenglesWindow = hideOpenglesWindowImpl,
        .setOpenglesTranslation = setOpenglesTranslationImpl,
        .setOpenglesScreenMask = setOpenglesScreenMaskImpl,
        .setOpenglesScreenBackground = setOpenglesScreenBackgroundImpl,
        .setOpenglesDisplayLayout = setOpenglesDisplayLayoutImpl,
        .redrawOpenglesWindow = redrawOpenglesWindowImpl,
        .setShouldSkipDraw = setShouldSkipDrawImpl,
        .getShouldSkipDraw = getShouldSkipDrawImpl,
        .hasGuestPostedAFrame = hasGuestPostedAFrameImpl,
        .resetGuestPostedAFrame = resetGuestPostedAFrameImpl,
        .registerScreenshotFunc = registerScreenshotFuncImpl,
        .screenShot = screenShotImpl,
        .stopOpenglesRenderer = stopOpenglesRendererImpl,
        .finishOpenglesRenderer = finishOpenglesRendererImpl,
        .onGuestGraphicsProcessCreate = onGuestGraphicsProcessCreateImpl,
        .cleanupProcGLObjects = cleanupProcGLObjectsImpl,
        .waitForOpenglesProcessCleanup = waitForOpenglesProcessCleanupImpl,
        .getVirtioGpuOps = getVirtioGpuOpsImpl,
        .getEGLDispatch = getEGLDispatchImpl,
        .getGLESv2Dispatch = getGLESv2DispatchImpl,
        .setVsyncHz = setVsyncHzImpl,
        .setOpenglesDisplayConfigs = setOpenglesDisplayConfigsImpl,
        .setOpenglesDisplayActiveConfig = setOpenglesDisplayActiveConfigImpl,
    };
    return &sDefaultOpenglesFuncs;
}
