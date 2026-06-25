// Copyright 2015 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "host-common/opengl/emugl_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "aemu/base/files/PathUtils.h"
#include "aemu/base/files/Stream.h"
#include "aemu/base/system/System.h"
#include "android/utils/path.h"

#include "aemu/base/StringFormat.h"
#include "aemu/base/logging/Log.h"
#include "android/avd/info.h"
#include "android/base/system/System.h"
#include "android/console.h"
#include "android/cpu_accelerator.h"
#include "android/opengl/EmuglBackendList.h"
#include "android/opengl/gpuinfo.h"
#include "android/skin/backend-defs.h"
#include "host-common/FeatureControl.h"
#include "host-common/crash-handler.h"
#include "host-common/feature_control.h"
#include "host-common/opengles.h"
#include "vulkan/vk_enum_string_helper.h"
#include "vulkan/vulkan.h"

#if defined(__APPLE__)
#if (VK_HEADER_VERSION > 216)
#include <vulkan/vulkan_beta.h>
#else
// Manually define MoltenVK related parts until we update the Vulkan headers
#ifndef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
#define VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME \
    "VK_KHR_portability_enumeration"
#endif
static const uint32_t VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR =
        0x00000001;
#endif
#endif

#ifndef _WIN32
#include <dlfcn.h>
#endif

#define D(...)                                           \
    do {                                                 \
        dprint(__VA_ARGS__);                             \
        crashhandler_append_message_format(__VA_ARGS__); \
    } while (0)

using android::base::RunOptions;
using android::base::StringFormat;
using android::base::System;
using android::opengl::EmuglBackendList;
namespace fc = android::featurecontrol;

static EmuglBackendList* sBackendList = NULL;

static void resetBackendList() {
    delete sBackendList;
    sBackendList =
            new EmuglBackendList(System::get()->getLauncherDirectory().c_str());
}

static bool stringVectorContains(const std::vector<std::string>& list,
                                 const char* value) {
    for (size_t n = 0; n < list.size(); ++n) {
        if (!strcmp(list[n].c_str(), value)) {
            return true;
        }
    }
    return false;
}

bool isHostGpuBlacklisted() {
    return async_query_host_gpu_blacklisted();
}

// Get a description of host GPU properties.
// Need to free after use.
emugl_host_gpu_prop_list emuglConfig_get_host_gpu_props() {
    const GpuInfoList& gpulist = globalGpuInfoList();
    emugl_host_gpu_prop_list res;
    res.num_gpus = gpulist.infos.size();
    res.props = new emugl_host_gpu_props[res.num_gpus];

    const std::vector<GpuInfo>& infos = gpulist.infos;
    for (int i = 0; i < res.num_gpus; i++) {
        res.props[i].make = strdup(infos[i].make.c_str());
        res.props[i].model = strdup(infos[i].model.c_str());
        res.props[i].device_id = strdup(infos[i].device_id.c_str());
        res.props[i].revision_id = strdup(infos[i].revision_id.c_str());
        res.props[i].version = strdup(infos[i].version.c_str());
        res.props[i].renderer = strdup(infos[i].renderer.c_str());
    }
    return res;
}

SelectedRenderer emuglConfig_get_renderer(const char* gpu_mode) {
    if (!gpu_mode) {
        return SELECTED_RENDERER_UNKNOWN;
    } else if (!strcmp(gpu_mode, "host") || !strcmp(gpu_mode, "on")) {
        return SELECTED_RENDERER_HOST;
    } else if (!strcmp(gpu_mode, "swiftshader")) {
        return SELECTED_RENDERER_SWIFTSHADER_INDIRECT;
    } else if (!strcmp(gpu_mode, "swangle")) {
        return SELECTED_RENDERER_ANGLE_INDIRECT;
    } else if (!strcmp(gpu_mode, "lavapipe") || !strcmp(gpu_mode, "llvmpipe")) {
        return SELECTED_RENDERER_LAVAPIPE;
    } else if (!strcmp(gpu_mode, "error")) {
        return SELECTED_RENDERER_ERROR;
    } else {
        return SELECTED_RENDERER_UNKNOWN;
    }
}

static SelectedRenderer sCurrentGlesRenderer = SELECTED_RENDERER_UNKNOWN;
static SelectedRenderer sCurrentVulkanRenderer = SELECTED_RENDERER_UNKNOWN;
static bool sCurrentRendererSet = false;

SelectedRenderer emuglConfig_get_current_gles_renderer() {
    if (!sCurrentRendererSet) {
        derror("%s called before selecting the renderer!", __func__);
    }
    return sCurrentGlesRenderer;
}

SelectedRenderer emuglConfig_get_current_vulkan_renderer() {
    if (!sCurrentRendererSet) {
        derror("%s called before selecting the renderer!", __func__);
    }
    return sCurrentVulkanRenderer;
}

SelectedRenderer emuglConfig_get_current_renderer() {
    // deprecated function, only checks gles mode, to be removed
    return emuglConfig_get_current_gles_renderer();
}

static std::string sGpuOption;

const char* emuglConfig_get_user_gpu_option() {
    return sGpuOption.c_str();
}

const char* emuglConfig_renderer_to_string(SelectedRenderer renderer) {
    switch (renderer) {
        case SELECTED_RENDERER_UNKNOWN:
            return "(Unknown)";
        case SELECTED_RENDERER_HOST:
            return "Host";
        case SELECTED_RENDERER_SWIFTSHADER_INDIRECT:
            return "Swiftshader Indirect";
        case SELECTED_RENDERER_ANGLE_INDIRECT:
            return "Angle Indirect";
        case SELECTED_RENDERER_LAVAPIPE:
            return "Lavapipe";
        case SELECTED_RENDERER_ERROR:
            return "(Error)";
    }
    return "(Bad value)";
}

void free_emugl_host_gpu_props(emugl_host_gpu_prop_list proplist) {
    for (int i = 0; i < proplist.num_gpus; i++) {
        free(proplist.props[i].make);
        free(proplist.props[i].model);
        free(proplist.props[i].device_id);
        free(proplist.props[i].revision_id);
        free(proplist.props[i].version);
        free(proplist.props[i].renderer);
    }
    delete[] proplist.props;
}

static void setCurrentRenderer(const char* glesMode, const char* vulkanMode) {
    sCurrentGlesRenderer = emuglConfig_get_renderer(glesMode);
    sCurrentVulkanRenderer = emuglConfig_get_renderer(vulkanMode);
    sCurrentRendererSet = true;
    dprint("%s: %s %s gles:%s vulkan:%s", __func__, glesMode, vulkanMode,
           emuglConfig_renderer_to_string(sCurrentGlesRenderer),
           emuglConfig_renderer_to_string(sCurrentVulkanRenderer));
}

struct DeviceSupportInfo {
    VkPhysicalDeviceProperties physdevProps;
    VkPhysicalDeviceMemoryProperties memProperties;
    bool hasGraphicsQueueFamily;
    bool supportsExternalMemory;
    bool supportsSwapchain;
    bool supportsYcbcrConversion;

    uint64_t getDeviceMaxAllocationCount() const {
        return physdevProps.limits.maxMemoryAllocationCount;
    }

    uint64_t getDeviceLocalMemorySize() const {
        uint64_t deviceLocalMemorySize = 0;
        for (uint32_t i = 0; i < memProperties.memoryHeapCount; i++) {
            if (memProperties.memoryHeaps[i].flags &
                VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                deviceLocalMemorySize += memProperties.memoryHeaps[i].size;
            }
        }
        return deviceLocalMemorySize;
    }

    void getApiVersion(int* major, int* minor, int* patch) const {
        if (major) {
            *major = VK_API_VERSION_MAJOR(physdevProps.apiVersion);
        }
        if (minor) {
            *minor = VK_API_VERSION_MINOR(physdevProps.apiVersion);
        }
        if (patch) {
            *patch = VK_API_VERSION_PATCH(physdevProps.apiVersion);
        }
    }

    std::string getDriverVersionStr() const {
        bool isNvidia = (physdevProps.vendorID == 4318);
        std::string driverVersionStr;
        if (isNvidia) {
            // Decode Nvidia driver version to make it meaningful to the users
            // Reference: VulkanDeviceInfo::getDriverVersion() at
            // https://github.com/SaschaWillems/VulkanCapsViewer/blob/master/vulkanDeviceInfo.cpp
            // 10 bits = major version (up to r1023)
            // 8 bits = minor version (up to 255)
            // 8 bits = secondary branch version/build version (up to 255)
            // 6 bits = tertiary branch/build version (up to 63)
            const uint32_t major = (physdevProps.driverVersion >> 22) & 0x3ff;
            const uint32_t minor = (physdevProps.driverVersion >> 14) & 0x0ff;

            return std::to_string(major) + "." + std::to_string(minor);
        }

        // Use regular VK_API_VERSION encoding to print the version.
        return std::to_string(
                       VK_API_VERSION_MAJOR(physdevProps.driverVersion)) +
               "." +
               std::to_string(
                       VK_API_VERSION_MINOR(physdevProps.driverVersion)) +
               "." +
               std::to_string(VK_API_VERSION_PATCH(physdevProps.driverVersion));
    }
};

// Checks if the user enforced a specific GPU, it can be done via index or name.
// Otherwise try to find the best device with discrete GPU and high vulkan API
// level. Scoring of the devices is done by some implicit choices based on known
// driver quality, stability and performance issues of current GPUs. Only one
// Vulkan device is selected; this makes things simple for now, but we could
// consider utilizing multiple devices in use cases that make sense.
int getSelectedGpuIndex(const std::vector<DeviceSupportInfo>& deviceInfos) {
    const int physdevCount = deviceInfos.size();
    if (physdevCount == 1) {
        return 0;
    }

    const char* EnvVarSelectGpu = "ANDROID_EMU_VK_SELECT_GPU";
    std::string enforcedGpuStr =
            android::base::getEnvironmentVariable(EnvVarSelectGpu);
    int enforceGpuIndex = -1;
    if (enforcedGpuStr.size()) {
        dinfo("%s is set to %s", EnvVarSelectGpu, enforcedGpuStr.c_str());

        if (enforcedGpuStr[0] == '0') {
            enforceGpuIndex = 0;
        } else {
            enforceGpuIndex = (atoi(enforcedGpuStr.c_str()));
            if (enforceGpuIndex == 0) {
                // Could not convert to an integer, try searching with device
                // name Do the comparison case insensitive as vendor names don't
                // have consistency
                enforceGpuIndex = -1;
                std::transform(enforcedGpuStr.begin(), enforcedGpuStr.end(),
                               enforcedGpuStr.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                for (int i = 0; i < physdevCount; ++i) {
                    std::string deviceName =
                            std::string(deviceInfos[i].physdevProps.deviceName);
                    std::transform(deviceName.begin(), deviceName.end(),
                                   deviceName.begin(), [](unsigned char c) {
                                       return std::tolower(c);
                                   });
                    dinfo("Physical device [%d] = %s", i, deviceName.c_str());

                    if (deviceName.find(enforcedGpuStr) != std::string::npos) {
                        enforceGpuIndex = i;
                    }
                }
            }
        }

        if (enforceGpuIndex != -1 && enforceGpuIndex >= 0 &&
            enforceGpuIndex < deviceInfos.size()) {
            dinfo("Selecting GPU (%s) at index %d.",
                  deviceInfos[enforceGpuIndex].physdevProps.deviceName,
                  enforceGpuIndex);
        } else {
            dwarning(
                    "Could not select the GPU with ANDROID_EMU_VK_GPU_SELECT.");
            enforceGpuIndex = -1;
        }
    }

    if (enforceGpuIndex != -1) {
        return enforceGpuIndex;
    }

    // If there are multiple devices, and none of them are enforced to use,
    // score each device and select the best
    int selectedGpuIndex = 0;
    auto getDeviceScore = [](const DeviceSupportInfo& deviceInfo) {
        uint32_t deviceScore = 0;
        if (!deviceInfo.hasGraphicsQueueFamily) {
            // Not supporting graphics, cannot be used.
            return deviceScore;
        }

        // Matches the ordering in VkPhysicalDeviceType
        const uint32_t deviceTypeScoreTable[] = {
                100,   // VK_PHYSICAL_DEVICE_TYPE_OTHER = 0,
                1000,  // VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1,
                2000,  // VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2,
                500,   // VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU = 3,
                600,   // VK_PHYSICAL_DEVICE_TYPE_CPU = 4,
        };

        // Prefer discrete GPUs, then integrated and then others..
        const int deviceType = deviceInfo.physdevProps.deviceType;
        deviceScore += deviceTypeScoreTable[deviceInfo.physdevProps.deviceType];

        // Prefer higher level of Vulkan API support, restrict version numbers
        // to common limits to ensure an always increasing scoring change
        const uint32_t major =
                VK_API_VERSION_MAJOR(deviceInfo.physdevProps.apiVersion);
        const uint32_t minor =
                VK_API_VERSION_MINOR(deviceInfo.physdevProps.apiVersion);
        const uint32_t patch =
                VK_API_VERSION_PATCH(deviceInfo.physdevProps.apiVersion);
        deviceScore += major * 5000 + std::min(minor, 10u) * 500 +
                       std::min(patch, 400u);

        return deviceScore;
    };

    uint32_t maxScore = 0;
    for (int i = 0; i < physdevCount; ++i) {
        const uint32_t score = getDeviceScore(deviceInfos[i]);
        if (score > maxScore) {
            selectedGpuIndex = i;
            maxScore = score;
        }
    }

    return selectedGpuIndex;
}

static std::string sVkRuntimePath;

const char* emuglConfig_get_vulkan_runtime_full_path() {
    if (sVkRuntimePath.size()) {
        return sVkRuntimePath.c_str();
    }

    const std::string explicitPath =
            System::getEnvironmentVariable("ANDROID_EMU_VK_LOADER_PATH");
    if (!explicitPath.empty()) {
        sVkRuntimePath = explicitPath;
        return sVkRuntimePath.c_str();
    }

#if defined(_WIN32)
    const char* myLibName = "vulkan-1.dll";
#elif defined(__linux__)
    const char* myLibName = "libvulkan.so";
#elif defined(__APPLE__)
    const char* myLibName = "libvulkan.dylib";
#endif

    const std::string localVkRuntimePath = android::base::PathUtils::join(
            android::base::System::get()->getLauncherDirectory(), "lib64",
            "vulkan", myLibName);

    // Use local by default, switch to system if it's newer on supported
    // platforms
    std::string selectedPath = localVkRuntimePath;

#if defined(_WIN32)
    const std::string systemVkRuntimePath = myLibName;

    // Make sure users can enforce the selection with an envvar
    const std::string vkRuntimeOption =
            System::getEnvironmentVariable("ANDROID_EMU_VK_RUNTIME");
    if (vkRuntimeOption == "SYSTEM") {
        selectedPath = systemVkRuntimePath;
    } else if (vkRuntimeOption == "LOCAL") {
        selectedPath = localVkRuntimePath;
    } else {
        // Check if the locally distributed version of the vulkan runtime is
        // newer
        int globalMajor, globalMinor, globalBuild_1, globalBuild_2;
        int localMajor, localMinor, localBuild_1, localBuild_2;
        dprint("%s: Checking for %s versions", __func__, myLibName);
        if (System::queryFileVersionInfo(systemVkRuntimePath.c_str(),
                                         &globalMajor, &globalMinor,
                                         &globalBuild_1, &globalBuild_2) &&
            System::queryFileVersionInfo(localVkRuntimePath.c_str(),
                                         &localMajor, &localMinor,
                                         &localBuild_1, &localBuild_2)) {
            dprint("%s version: %d.%d.%d.%d", systemVkRuntimePath.c_str(),
                   globalMajor, globalMinor, globalBuild_1, globalBuild_2);
            dprint("%s version: %d.%d.%d.%d", localVkRuntimePath.c_str(),
                   localMajor, localMinor, localBuild_1, localBuild_2);

            if ((localMajor > globalMajor) ||
                (localMajor == globalMajor && localMinor > globalMinor) ||
                (localMajor == globalMajor && localMinor == globalMinor &&
                 localBuild_1 > globalBuild_1)) {
                // Use globally available runtime if newer
                selectedPath = systemVkRuntimePath;
            }
        }
    }
#endif

    sVkRuntimePath = selectedPath;
    dprint("%s: Using vulkan runtime path: %s", __func__,
           sVkRuntimePath.c_str());

    return sVkRuntimePath.c_str();
}

bool emuglConfig_get_vulkan_hardware_gpu_support_info(
        DeviceSupportInfo* outProps);

void emuglConfig_get_vulkan_hardware_gpu(char** vendor,
                                         int* major,
                                         int* minor,
                                         int* patch,
                                         uint64_t* deviceMemBytes,
                                         uint32_t* driverVersion,
                                         uint64_t* deviceMaxAllocationCount,
                                         bool* supportsExternalMemory,
                                         bool* supportsSwapchain,
                                         bool* supportsYcbcrConversion) {
    if (!vendor || !major || !minor || !patch) {
        derror("%s: Invalid argument!", __func__);
        return;
    }

    DeviceSupportInfo vkProps = {};
    if (!emuglConfig_get_vulkan_hardware_gpu_support_info(&vkProps)) {
        *vendor = nullptr;
        return;
    }
    const char* mylibname = emuglConfig_get_vulkan_runtime_full_path();

    const VkPhysicalDeviceProperties& physicalProp = vkProps.physdevProps;
    const VkPhysicalDeviceMemoryProperties& memProps = vkProps.memProperties;

    // TODO: expose emuglConfig_get_vulkan_hardware_gpu_support_info outside
    // Here we make sure sure 'vendor' starts with the vendor's name.
    // We do not expose vendorID with this code path, and the resulting value
    // is actually used as 'device name'. But some old code will incorrectly
    // depend on string comparison for vendor matching.
    std::string vendorName = physicalProp.deviceName;
    std::vector<std::pair<uint32_t, std::string>> vendorIdPairs = {
            {4318, "NVIDIA"},
            {32902, "Intel"},
            {4098, "AMD"},
    };
    for (auto& p : vendorIdPairs) {
        if (physicalProp.vendorID == p.first) {
            if (vendorName.rfind(p.second, 0) != 0) {
                // Doesn't start with vendor name
                vendorName = p.second + " " + vendorName;
            }
            break;
        }
    }
    *vendor = strdup(vendorName.c_str());

    vkProps.getApiVersion(major, minor, patch);
    if (deviceMemBytes) {
        *deviceMemBytes = vkProps.getDeviceLocalMemorySize();
    }
    if (deviceMaxAllocationCount) {
        *deviceMaxAllocationCount = vkProps.getDeviceMaxAllocationCount();
    }
    if (driverVersion) {
        *driverVersion = physicalProp.driverVersion;
    }
    if (supportsExternalMemory) {
        *supportsExternalMemory = vkProps.supportsExternalMemory;
    }
    if (supportsSwapchain) {
        *supportsSwapchain = vkProps.supportsSwapchain;
    }
    if (supportsYcbcrConversion) {
        *supportsYcbcrConversion = vkProps.supportsYcbcrConversion;
    }
}

static bool vulkanExtensionSupported(
        const std::vector<VkExtensionProperties>& currentProps,
        const char* wantedExtName) {
    for (uint32_t i = 0; i < currentProps.size(); ++i) {
        if (!strcmp(wantedExtName, currentProps[i].extensionName)) {
            return true;
        }
    }
    return false;
}

static bool vulkanExtensionsSupported(
        const std::vector<VkExtensionProperties>& currentProps,
        const std::vector<const char*>& wantedExtNames) {
    for (size_t i = 0; i < wantedExtNames.size(); ++i) {
        if (!vulkanExtensionSupported(currentProps, wantedExtNames[i])) {
            return false;
        }
    }
    return true;
}

// Must be kept in sync with 'hasSufficientHwGpu' in hw_gpu_check.cpp
bool hasSufficientHostVulkanDriver(bool isXrAvd) {
    // Allow users and tests to skip compatibility checks
    if (System::get()->envGet("ANDROID_EMU_SKIP_GPU_CHECKS") == "1") {
        return true;
    }

#if defined(__APPLE__)
#if defined(__arm64__)
    // Known hardware configuration, no need to check
    return true;
#else
    // Host Vulkan is not supported on Mac Intel
    dwarning("%s: unsupported architecture.", __func__);
    return false;
#endif
#endif

    if (isXrAvd) {
        // Always use hardware for XR, as the host driver tests are done within
        // the Avd compatibility tests at startup, and the users are warned in
        // case of any incompatibilities
        return true;
    }

    if (async_query_host_gpu_VulkanBlacklisted()) {
        dwarning("%s: unsupported GPU", __func__);
        return false;
    }

    char* vkVendor = nullptr;
    int vkMajor = 0;
    int vkMinor = 0;
    int vkPatch = 0;
    uint64_t vkDeviceMemBytes = 0;
    uint32_t vkDriverVersion = 0;
    uint64_t vkDeviceMaxAllocationCount = 0;
    bool externalMemorySupported = false;
    bool swapchainSupported = false;
    bool ycbcrSupported = false;

    emuglConfig_get_vulkan_hardware_gpu(
            &vkVendor, &vkMajor, &vkMinor, &vkPatch, &vkDeviceMemBytes,
            &vkDriverVersion, &vkDeviceMaxAllocationCount,
            &externalMemorySupported, &swapchainSupported, &ycbcrSupported);

    if (!vkVendor) {
        dwarning("%s: could not detect host Vulkan driver.", __func__);
        return false;
    }

    const std::string vendorName = vkVendor;
    free(vkVendor);

    if (!externalMemorySupported) {
        dwarning("%s: external memory is not supported.", __func__);
        return false;
    }

    bool isLavapipe = (strncmp("llvmpipe", vendorName.c_str(), 8) == 0);
    if (isLavapipe) {
        // TODO(b/476996927): Avoid CIE/FDE errors in gpu auto mode, by forcing
        // the usage of bundled lavapipe driver instead.
        dwarning("%s: host lavapipe is not supported.", __func__);
        return false;
    }

    // TODO(b/381540970): Use servers side flags and deny listings for filtering
    // GPU compatibility
    bool isAMD = (strncmp("AMD", vendorName.c_str(), 3) == 0);
    bool isIntel = (strncmp("Intel", vendorName.c_str(), 5) == 0);
    bool isNvidia = (strncmp("NVIDIA", vendorName.c_str(), 6) == 0);

    // Use regular VK_API_VERSION encoding to print the version by default.
    uint32_t driverVersionMajor = VK_API_VERSION_MAJOR(vkDriverVersion);
    uint32_t driverVersionMinor = VK_API_VERSION_MINOR(vkDriverVersion);
    uint32_t driverVersionPatch = VK_API_VERSION_PATCH(vkDriverVersion);

    uint32_t minDriverVersionMajor = 0;
    uint32_t minDriverVersionMinor = 0;
    uint32_t minVkApiVersionMajor = 1;
    uint32_t minVkApiVersionMinor = 0;
    uint32_t minVkApiVersionPatch = 0;

    if (isNvidia) {
        // Decode Nvidia driver version to make it meaningful to the users
        // Reference: VulkanDeviceInfo::getDriverVersion() at
        // https://github.com/SaschaWillems/VulkanCapsViewer/blob/master/vulkanDeviceInfo.cpp
        // 10 bits = major version (up to r1023)
        // 8 bits = minor version (up to 255)
        // 8 bits = secondary branch version/build version (up to 255)
        // 6 bits = tertiary branch/build version (up to 63)
        driverVersionMajor = (vkDriverVersion >> 22) & 0x3ff;
        driverVersionMinor = (vkDriverVersion >> 14) & 0x0ff;
        driverVersionPatch = (vkDriverVersion >> 6) & 0x0ff;

        // Disallow driver versions below 553.35 as they may cause BSODs
        // (ref:b/379178011).
        minDriverVersionMajor = 553;
        minDriverVersionMinor = 35;
    } else {
#if defined(_WIN32)
        // Based on androidEmuglConfigInit
        if (isAMD) {
            // on windows, amd gpu with api 1.2.x does not work
            // for vulkan, disable it
            minVkApiVersionMinor = 3;
        } else if (isIntel) {
            // intel gpu with api < 1.3.240 does not work
            // for vulkan, disable it
            minVkApiVersionMinor = 3;
            minVkApiVersionPatch = 240;
        }
#endif
#if defined(__linux__)
        if (isAMD && strcmp("AMD Custom GPU 0405 (RADV VANGOGH)",
                            vendorName.c_str()) == 0) {
            dwarning("%s: unsupported GPU model.", __func__);
            return false;
        }
#endif
    }

    bool isUnsupportedVulkanLevel = false;
    if (vkMajor < minVkApiVersionMajor) {
        isUnsupportedVulkanLevel = true;
    } else if (vkMajor == minVkApiVersionMajor) {
        if ((vkMinor < minVkApiVersionMinor) ||
            ((vkMinor == minVkApiVersionMinor) &&
             (vkPatch < minVkApiVersionPatch))) {
            isUnsupportedVulkanLevel = true;
        }
    }
    if (isUnsupportedVulkanLevel) {
        dwarning(
                "%s: unsupported Vulkan API level (%d.%d.%d, min required: "
                "%d.%d.%d, vendor: %s)",
                __func__, vkMajor, vkMinor, vkPatch, minVkApiVersionMajor,
                minVkApiVersionMinor, minVkApiVersionPatch, vendorName.c_str());
        return false;
    }

    if (driverVersionMajor < minDriverVersionMajor ||
        (driverVersionMajor == minDriverVersionMajor &&
         driverVersionMinor < minDriverVersionMinor)) {
        dwarning(
                "%s: unsupported driver version (%d.%d.%d, min required: "
                "%d.%d.0, vendor: %s).",
                __func__, driverVersionMajor, driverVersionMinor,
                driverVersionPatch, minDriverVersionMajor,
                minDriverVersionMinor, vendorName.c_str());
        return false;
    }

    // If vulkan composition is requested, swapchain extensions must be
    // supported
    if (!swapchainSupported && fc::isEnabled(fc::VulkanNativeSwapchain)) {
        dwarning(
                "Vulkan composition is requested, but the host Vulkan "
                "driver does not support Vulkan swapchain features "
                "required.");
        return false;
    }

    if (!ycbcrSupported) {
        dwarning("Vulkan device do not support YCbCr conversion.");
        return false;
    }

    return true;
}

static bool sVkPropsInitialized = false;
static DeviceSupportInfo sVkProps = {};

bool emuglConfig_get_vulkan_hardware_gpu_support_info(
        DeviceSupportInfo* outProps) {
    if (!outProps) {
        derror("%s: Invalid argument!", __func__);
        return false;
    }

    if (sVkPropsInitialized) {
        *outProps = sVkProps;
        return true;
    }

    const char* mylibname = emuglConfig_get_vulkan_runtime_full_path();

#if defined(_WIN32)
    HMODULE library = LoadLibraryA(mylibname);
    if (!library) {
        dwarning("%s: cannot open vulkan lib %s\n", __func__, mylibname);
        return false;
    }
    auto* pvkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(library, "vkGetInstanceProcAddr"));
#else
    auto library = dlopen(mylibname, RTLD_NOW);
    if (!library) {
        dwarning("%s: failed to open %s", __func__, mylibname);
        return false;
    }
    auto* pvkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(library, "vkGetInstanceProcAddr"));
#endif

    if (!pvkGetInstanceProcAddr) {
        derror("Failed to load vkGetInstanceProcAddr function!");
        return false;
    }

#define GET_VK_INSTANCE_PROC(inst, name) \
    PFN_##name(pvkGetInstanceProcAddr(inst, #name));

    auto* pvkCreateInstance = GET_VK_INSTANCE_PROC(nullptr, vkCreateInstance);
    if (!pvkCreateInstance) {
        derror("Failed to load vkCreateInstance function!");
        return false;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "DetectGpuInfo";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 1, 0);
    appInfo.pEngineName = "test_engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    std::vector<const char*> extNames;

#if defined(__APPLE__)
    // MoltenVK requires portability enumeratiion and extension enabled
    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    extNames.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

    createInfo.enabledExtensionCount = (uint32_t)extNames.size();
    createInfo.ppEnabledExtensionNames = extNames.data();

    VkInstance instance;

    VkResult result = pvkCreateInstance(&createInfo, 0, &instance);

    if (result != VK_SUCCESS) {
        derror("%s: Failed to create vulkan instance. Error: [%s] %d\n",
               __func__, string_VkResult(result), result);
        return false;
    }
    dprint("%s: Successfully created vulkan instance\n", __func__);

    auto* pvkDestroyInstance =
            GET_VK_INSTANCE_PROC(instance, vkDestroyInstance);
    auto* pvkEnumeratePhysicalDevices =
            GET_VK_INSTANCE_PROC(instance, vkEnumeratePhysicalDevices);
    auto* pvkGetPhysicalDeviceProperties =
            GET_VK_INSTANCE_PROC(instance, vkGetPhysicalDeviceProperties);
    auto* pvkGetPhysicalDeviceMemoryProperties =
            GET_VK_INSTANCE_PROC(instance, vkGetPhysicalDeviceMemoryProperties);
    auto* pvkGetPhysicalDeviceQueueFamilyProperties = GET_VK_INSTANCE_PROC(
            instance, vkGetPhysicalDeviceQueueFamilyProperties);
    auto* pvkEnumerateInstanceExtensionProperties = GET_VK_INSTANCE_PROC(
            instance, vkEnumerateInstanceExtensionProperties);
    auto* pvkEnumerateDeviceExtensionProperties = GET_VK_INSTANCE_PROC(
            instance, vkEnumerateDeviceExtensionProperties);
    auto* pvkGetPhysicalDeviceFeatures2 =
            GET_VK_INSTANCE_PROC(instance, vkGetPhysicalDeviceFeatures2);

#undef GET_VK_INSTANCE_PROC

    if (!pvkEnumeratePhysicalDevices || !pvkGetPhysicalDeviceProperties ||
        !pvkDestroyInstance || !pvkGetPhysicalDeviceMemoryProperties ||
        !pvkGetPhysicalDeviceQueueFamilyProperties ||
        !pvkEnumerateInstanceExtensionProperties ||
        !pvkEnumerateDeviceExtensionProperties) {
        derror("Failed to load Vulkan functions!");
        return false;
    }

    uint32_t deviceCount = 0;
    result = pvkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS) {
        pvkDestroyInstance(instance, nullptr);
        derror("%s: Failed to query physical devices count. Error: %s [%d]\n",
               __func__, string_VkResult(result), result);
        return false;
    }
    dprint("%s: Physical devices count is %d\n", __func__, (int)(deviceCount));
    if (deviceCount == 0) {
        pvkDestroyInstance(instance, nullptr);
        derror("%s: Could not find any Vulkan supported devices, try updating "
               "your GPU drivers.\n",
               __func__);
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    result =
            pvkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    if (result != VK_SUCCESS) {
        pvkDestroyInstance(instance, nullptr);
        derror("%s: Failed to query physical devices. Error: %s [%d]\n",
               __func__, string_VkResult(result), result);
        return false;
    }

    std::vector<const char*> externalMemoryDeviceExtensionsRequired = {
            VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
            VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
#ifdef _WIN32
            "VK_KHR_external_memory_win32",
#elif defined(__APPLE__)
            "VK_EXT_external_memory_metal",
#elif defined(__linux__)
            "VK_KHR_external_memory_fd",
#endif
    };

    std::vector<const char*> swapchainInstanceExtensionsRequired = {
            VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _WIN32
            "VK_KHR_win32_surface",
#elif defined(__APPLE__)
            "VK_EXT_metal_surface",
#elif defined(__linux__)
            "VK_KHR_xcb_surface",
#endif
    };
    std::vector<const char*> swapchainDeviceExtensionsRequired = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    uint32_t instanceExtensionCount = 0;
    pvkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount,
                                            nullptr);
    std::vector<VkExtensionProperties> availableInstanceExtensions(
            instanceExtensionCount);
    pvkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount,
                                            availableInstanceExtensions.data());
    const bool instanceSupportsSwapchain = vulkanExtensionsSupported(
            availableInstanceExtensions, swapchainInstanceExtensionsRequired);

    std::vector<VkExtensionProperties> availableDeviceExtensions;
    std::vector<DeviceSupportInfo> deviceInfos(deviceCount);
    for (int i = 0; i < deviceCount; i++) {
        pvkGetPhysicalDeviceProperties(devices[i],
                                       &deviceInfos[i].physdevProps);

        pvkGetPhysicalDeviceMemoryProperties(devices[i],
                                             &deviceInfos[i].memProperties);

        deviceInfos[i].hasGraphicsQueueFamily = false;
        {
            uint32_t queueFamilyCount = 0;
            pvkGetPhysicalDeviceQueueFamilyProperties(
                    devices[i], &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilyProps(
                    queueFamilyCount);
            pvkGetPhysicalDeviceQueueFamilyProperties(
                    devices[i], &queueFamilyCount, queueFamilyProps.data());

            for (uint32_t j = 0; j < queueFamilyCount; ++j) {
                auto count = queueFamilyProps[j].queueCount;
                auto flags = queueFamilyProps[j].queueFlags;
                if (count > 0 && (flags & VK_QUEUE_GRAPHICS_BIT)) {
                    deviceInfos[i].hasGraphicsQueueFamily = true;
                    break;
                }
            }
        }

        uint32_t deviceExtensionCount = 0;
        pvkEnumerateDeviceExtensionProperties(devices[i], nullptr,
                                              &deviceExtensionCount, nullptr);
        availableDeviceExtensions.resize(deviceExtensionCount);
        pvkEnumerateDeviceExtensionProperties(devices[i], nullptr,
                                              &deviceExtensionCount,
                                              availableDeviceExtensions.data());

        // Check if external memory extensions are supported
        deviceInfos[i].supportsExternalMemory = vulkanExtensionsSupported(
                availableDeviceExtensions,
                externalMemoryDeviceExtensionsRequired);

        // Check if swapchain extensions are supported, support is required for
        // vulkan composition
        deviceInfos[i].supportsSwapchain =
                instanceSupportsSwapchain &&
                vulkanExtensionsSupported(availableDeviceExtensions,
                                          swapchainDeviceExtensionsRequired);

        // Check if the device supports ycbcr conversion, necessary for video
        // rendering.
        deviceInfos[i].supportsYcbcrConversion = false;
        if (pvkGetPhysicalDeviceFeatures2 &&
            vulkanExtensionSupported(
                    availableDeviceExtensions,
                    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME)) {
            VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures = {
                    .sType =
                            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
            };
            VkPhysicalDeviceFeatures2 features2 = {
                    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                    .pNext = &ycbcrFeatures,
            };
            pvkGetPhysicalDeviceFeatures2(devices[i], &features2);

            deviceInfos[i].supportsYcbcrConversion =
                    ycbcrFeatures.samplerYcbcrConversion == VK_TRUE;
        }

        // Put the GPU information into the logs to be able to track down any
        // errors more easily
        const VkPhysicalDeviceProperties& physdevProps =
                deviceInfos[i].physdevProps;
        const char* deviceType =
                string_VkPhysicalDeviceType(physdevProps.deviceType);
        int vkMajor, vkMinor, vkPatch;
        deviceInfos[i].getApiVersion(&vkMajor, &vkMinor, &vkPatch);
        std::string driverVersionStr = deviceInfos[i].getDriverVersionStr();
        dinfo("%s: Found physical GPU '%s', type: %s, apiVersion: %d.%d.%d, "
              "driverVersion: %s\n",
              __func__, physdevProps.deviceName, deviceType, vkMajor, vkMinor,
              vkPatch, driverVersionStr.c_str());
    }

    uint32_t selectedGpuIndex = getSelectedGpuIndex(deviceInfos);

    // save the props
    sVkProps = deviceInfos[selectedGpuIndex];
    sVkPropsInitialized = true;

    *outProps = sVkProps;

    pvkDestroyInstance(instance, nullptr);

    return true;
}

bool emuglConfig_init(EmuglConfig* config,
                      const char* gpu_mode_requested,
                      bool no_window) {
    D("%s: gpu_mode_requested: %s, no_window: %d\n", __FUNCTION__,
      gpu_mode_requested, no_window);

    // zero all fields first.
    memset(config, 0, sizeof(*config));

    const char DEFAULT_SOFTWARE_VULKAN_MODE[] = "lavapipe";

    std::vector<std::string> allowedOptions = {
            "auto", "host", "lavapipe", "swiftshader", "swangle",
    };
    bool isValid = false;
    for (auto& option : allowedOptions) {
        if (option == gpu_mode_requested) {
            isValid = true;
            break;
        }
    }

    if (!isValid) {
        // At this point we should have fixed the arguments
        std::string error = StringFormat(
                "Invalid GPU mode '%s', use one of: 'auto', 'host', "
                "'lavapipe', 'swiftshader' or 'swangle'",
                gpu_mode_requested);

        D("%s: Error: [%s]\n", __func__, error.c_str());
        derror("%s: %s", __func__, error);

        const char* gpu_mode_out = "error";
        snprintf(config->vulkan_backend, sizeof(config->vulkan_backend), "%s",
                 gpu_mode_out);
        snprintf(config->gles_backend, sizeof(config->gles_backend), "%s",
                 gpu_mode_out);
        snprintf(config->status, sizeof(config->status), "%s", error.c_str());
        setCurrentRenderer(gpu_mode_out, gpu_mode_out);
        return false;
    }

    sGpuOption = gpu_mode_requested;

    // Select Vulkan mode
    std::string vulkan_mode_selected = gpu_mode_requested;

    // TODO(b/367273570): fix the test runner to remove
    // agentsAvailable() call
    const bool is_xr_mode =
            agentsAvailable() && getConsoleAgents() &&
            getConsoleAgents()->settings &&
            getConsoleAgents()->settings->avdInfo() &&
            (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
             AVD_XR);

    // If nothing is enforced so far, and we're using 'auto' mode, decide
    // based on some other parameters and prefer host
    bool sufficientHostVulkanDriver = true;
    if (vulkan_mode_selected == "auto") {
        bool switchToSoftwareVulkan = false;
        if (!is_xr_mode) {
#if defined(__APPLE__)
            // Force MoltenVK with 'auto' modes on XR, but otherwise use
            // software due to known issues
            switchToSoftwareVulkan = true;
#else
            switchToSoftwareVulkan = no_window;
#endif
        }

        if (!switchToSoftwareVulkan) {
            // Check if the driver is compatible
            if (!hasSufficientHostVulkanDriver(is_xr_mode)) {
                dinfo("Host Vulkan driver is not supported.");
                sufficientHostVulkanDriver = false;
                switchToSoftwareVulkan = true;
            }
        }

        if (switchToSoftwareVulkan) {
            vulkan_mode_selected = DEFAULT_SOFTWARE_VULKAN_MODE;
        } else {
            vulkan_mode_selected = "host";
        }
    }

#if defined(__APPLE__) && !defined(__arm64__)
    // Do not enable host vulkan driver (e.g. moltenvk), or
    // lavapipe(b/462005807) on mac Intel
    if (vulkan_mode_selected == "host" || vulkan_mode_selected == "lavapipe") {
        vulkan_mode_selected = "swiftshader";
    }
#endif

    if (vulkan_mode_selected == "swangle") {
        // No 'swangle' for vulkan mode, use swiftshader
        vulkan_mode_selected = "swiftshader";
    }

    // Select GLES mode
    std::string gles_mode_selected = gpu_mode_requested;
    if (gles_mode_selected == "lavapipe") {
        // By default, use swangle
        gles_mode_selected = "swangle";

#if defined(__linux__)
        const char* EnvVarSelectLLVMPipe =
                "ANDROID_EMU_LAVAPIPE_GL_MODE_LLVMPIPE";
        if (android::base::getEnvironmentVariable(EnvVarSelectLLVMPipe) ==
            "1") {
            gles_mode_selected = "llvmpipe";
            dinfo("Forcing 'llvmpipe' mode for GLES");
        }
#endif

        const bool force_swiftshader = fc::isEnabled(fc::ForceSwiftshader);
        const char* EnvVarSelectGL = "ANDROID_EMU_LAVAPIPE_GL_MODE_SWIFTSHADER";
        if (force_swiftshader ||
            android::base::getEnvironmentVariable(EnvVarSelectGL) == "1") {
            gles_mode_selected = "swiftshader";
            dinfo("Forcing 'swiftshader' mode for GLES");
        }
    }

    // If nothing is enforced so far, and we're using 'auto' mode, decide
    // based on some other parameters and prefer host
    if (gles_mode_selected == "auto") {
        bool switchToSoftwareGles = false;

        // If the host GPU is not good enough to support Vulkan in auto mode,
        // use software rendering for GLES as well. This is mainly related to
        // the lack of OpenGL related checks we can do at this point, and
        // the longtail of old drivers causing problems on GLES rendering which
        // are not added into the server side deny list.
        if (no_window || async_query_host_gpu_blacklisted() ||
            !sufficientHostVulkanDriver) {
            switchToSoftwareGles = true;
        }
#ifdef __APPLE__
        if (!switchToSoftwareGles) {
            const int hostGpuMemoryLimitMB = 5 * 1024;  // 5GB
            int freeRamMB = 0;
            System::isUnderMemoryPressure(&freeRamMB);

            // TODO(b/479126903): New macOS system update (Tahoe) leaks memory
            // when host OpenGL driver is used, which is deprecated on macOS for
            // some time. Check memory usage and decide to use software
            // rendering for GL emulation if there is possibly a leak that may
            // have cause system restarts.
            if (freeRamMB < hostGpuMemoryLimitMB) {
                dwarning(
                        "Software GL rendering will be used due to system memory "
                        "pressure, performance will be affected!"
                        " (Available Memory: %d MB, Required: %d MB)",
                        freeRamMB, hostGpuMemoryLimitMB);
                switchToSoftwareGles = true;
            } else {
                dprint("System has sufficient memory available (%d MB) for "
                       "hardware GL rendering",
                       freeRamMB);
            }
        }
#endif

        if (switchToSoftwareGles) {
            gles_mode_selected = "swangle";
        } else {
            gles_mode_selected = "host";
        }
    }

#ifdef _WIN32
    // swangle / llvmpipe are not supported on Windows
    if (gles_mode_selected == "swangle" || gles_mode_selected == "llvmpipe") {
        gles_mode_selected = "swiftshader";
    }
#elif defined(__APPLE__)
    // swiftshader / llvmpipe are not supported on macOS
    if (gles_mode_selected == "swiftshader" ||
        gles_mode_selected == "llvmpipe") {
        gles_mode_selected = "swangle";
    }
#endif

    dinfo("%s: vulkan_mode_selected:%s gles_mode_selected:%s", __func__,
          vulkan_mode_selected.c_str(), gles_mode_selected.c_str());

    resetBackendList();

    // 'host' is a special value corresponding to the default translation
    // to desktop GL, anything else must be checked against existing host-side
    // backends.
    if (gles_mode_selected != "host") {
        std::string gles_library_name = gles_mode_selected;
        if (gles_library_name == "swangle") {
            // library path uses gles_angle as the folder name
            gles_library_name = "angle";
        }
        const std::vector<std::string>& backends = sBackendList->names();
        if (!stringVectorContains(backends, gles_library_name.c_str())) {
            std::string error = StringFormat(
                    "Invalid GLES library mode '%s' for GLES. Backends available: ",
                    gles_library_name.c_str());

            for (size_t n = 0; n < backends.size(); ++n) {
                error += " ";
                error += backends[n];
            }

            D("%s: Error: [%s]\n", __func__, error.c_str());
            derror("%s: %s", __func__, error);

            const char* gpu_mode_out = "error";
            snprintf(config->vulkan_backend, sizeof(config->vulkan_backend),
                     "%s", gpu_mode_out);
            snprintf(config->gles_backend, sizeof(config->gles_backend), "%s",
                     gpu_mode_out);
            snprintf(config->status, sizeof(config->status), "%s",
                     error.c_str());
            setCurrentRenderer(gpu_mode_out, gpu_mode_out);
            return false;
        }
    }

    // GPU mode should not change after this point
    snprintf(config->vulkan_backend, sizeof(config->vulkan_backend), "%s",
             vulkan_mode_selected.c_str());
    snprintf(config->gles_backend, sizeof(config->gles_backend), "%s",
             gles_mode_selected.c_str());
    snprintf(config->status, sizeof(config->status),
             "GPU emulation enabled using Vulkan:'%s' GLES:'%s' modes",
             vulkan_mode_selected.c_str(), gles_mode_selected.c_str());
    setCurrentRenderer(gles_mode_selected.c_str(),
                       vulkan_mode_selected.c_str());

#if defined(__linux__) || defined(_WIN32)
    const bool hwGpuRequested = (emuglConfig_get_current_vulkan_renderer() ==
                                 SELECTED_RENDERER_HOST);
    const bool vulkanIsNotDisabled =
            (!agentsAvailable() || !fc::isOverridden(fc::Vulkan) ||
             fc::isEnabled(fc::Vulkan));
    if (hwGpuRequested && vulkanIsNotDisabled) {
        char* vkVendor = nullptr;
        int vkMajor = 0;
        int vkMinor = 0;
        int vkPatch = 0;
        // bug: 324086743
        // we have to enable VulkanAllocateDeviceMemoryOnly
        // to work around the kvm+amdgpu driver bug
        // where kvm apparently error out with Bad Address
        emuglConfig_get_vulkan_hardware_gpu(&vkVendor, &vkMajor, &vkMinor,
                                            &vkPatch, nullptr, nullptr, nullptr,
                                            nullptr, nullptr, nullptr);
        if (vkVendor) {
            bool isAMD = (strncmp("AMD", vkVendor, 3) == 0);
            bool isIntel = (strncmp("Intel", vkVendor, 5) == 0);
            bool isNvidia = (strncmp("NVIDIA", vkVendor, 6) == 0);
            if (isAMD) {
                feature_set_if_not_overridden(
                        kFeature_VulkanAllocateDeviceMemoryOnly, true);
                if (fc::isEnabled(fc::VulkanAllocateDeviceMemoryOnly)) {
                    dinfo("Enabled VulkanAllocateDeviceMemoryOnly feature for "
                          "gpu "
                          "vendor %s on Linux\n",
                          vkVendor);
                }
            }
            bool hostMemoryOnWindows = false;
            bool hostMemoryOnLinux = false;
#if defined(__linux__)
            hostMemoryOnLinux = (isIntel || isAMD);
#endif
#if defined(_WIN32)
            // bug: 382621412
            // Nvidia GPUs can create BSODs and hangs when AEHD is enabled.
            AndroidCpuAccelerator accelerator =
                    androidCpuAcceleration_getAccelerator();
            hostMemoryOnWindows =
                    isNvidia && (accelerator == ANDROID_CPU_ACCELERATOR_AEHD);
#endif
            if (hostMemoryOnLinux || hostMemoryOnWindows) {
                feature_set_if_not_overridden(kFeature_VulkanAllocateHostMemory,
                                              true);
                if (fc::isEnabled(fc::VulkanAllocateHostMemory)) {
                    dinfo("Enabled VulkanAllocateHostMemory feature for "
                          "gpu "
                          "vendor %s",
                          vkVendor);
                }
            }

            free(vkVendor);
        }
    }
#endif

    D("%s: %s\n", __func__, config->status);
    return true;
}

void emuglConfig_setupEnv(const EmuglConfig* config) {
    System* system = System::get();

    // Setup Vulkan
    const bool use_host_vulkan = (strcmp(config->vulkan_backend, "host") == 0);
    if (use_host_vulkan) {
#ifdef __APPLE__
        // TODO(b/433496880) temprary way of enabling kosmickrisp ICD.
        // Ideally, we should instead just respect user's VK_DRIVER_FILES
        // selection.
        const char* EnvVarSelectICD = "ANDROID_EMU_VK_SELECT_ICD";
        std::string selectedICDStr =
                android::base::getEnvironmentVariable(EnvVarSelectICD);
        if (selectedICDStr.empty()) {
            selectedICDStr = "moltenvk";
        } else {
            dinfo("%s: Setting ICD from envvar %s, to '%s'", __func__,
                  EnvVarSelectICD, selectedICDStr.c_str());
        }
        system->envSet("ANDROID_EMU_VK_ICD", selectedICDStr);
#else
        system->envSet("ANDROID_EMU_VK_ICD", NULL);
#endif
    } else if ((strcmp(config->vulkan_backend, "swiftshader") == 0)) {
        // Use Swiftshader vk icd if using swiftshader_indirect
        system->envSet("ANDROID_EMU_VK_ICD", "swiftshader");
    } else {
        // Use lavapipe vk icd by default
        system->envSet("ANDROID_EMU_VK_ICD", "lavapipe");
    }

    // Setup GLES
    bool use_swangle = strstr(config->gles_backend, "angle");
    // $EXEC_DIR/<lib>/ is already added to the library search path by default,
    // since generic libraries are bundled there. We may need more though:
    resetBackendList();
    if (strcmp(config->gles_backend, "host") != 0) {
        // If the backend is not 'host', we also need to add the
        // backend directory.
        std::string dir = sBackendList->getLibDirPath(config->gles_backend);
        if (dir.size()) {
            dprint("Adding to the library search path: %s\n", dir.c_str());
            system->addLibrarySearchDir(dir);
        }
    }

    if (!strcmp(config->gles_backend, "host")) {
        // Nothing more to do for the 'host' backend.
        return;
    }

    if (use_swangle) {
        system->envSet("ANGLE_DEFAULT_PLATFORM", "swiftshader");
    }

    if (!strcmp(config->gles_backend, "swiftshader") ||
        !strcmp(config->gles_backend, "llvmpipe") ||
        !strcmp(config->gles_backend, "swangle")) {
        system->envSet("ANDROID_EGL_ON_EGL", "1");
        return;
    }

    // For now, EmuGL selects its own translation libraries for
    // EGL/GLES libraries, unless the following environment
    // variables are defined:
    //    ANDROID_EGL_LIB
    //    ANDROID_GLESv1_LIB
    //    ANDROID_GLESv2_LIB
    //
    // If a backend provides one of these libraries, use it.
    const char* gles_library = (strcmp(config->gles_backend, "swangle") == 0)
                                       ? "angle"
                                       : config->gles_backend;
    std::string lib;
    if (sBackendList->getBackendLibPath(gles_library,
                                        EmuglBackendList::LIBRARY_EGL, &lib)) {
        system->envSet("ANDROID_EGL_LIB", lib);
    }
    if (sBackendList->getBackendLibPath(
                gles_library, EmuglBackendList::LIBRARY_GLESv1, &lib)) {
        system->envSet("ANDROID_GLESv1_LIB", lib);
    } else {
        derror("OpenGL backend '%s' without OpenGL ES 1.x library detected. "
               "Using GLESv2 only.",
               gles_library);
        // A GLESv1 lib is optional---we can deal with a GLESv2 only
        // backend by using CoreProfileEngine in the Translator.
    }

    if (sBackendList->getBackendLibPath(
                gles_library, EmuglBackendList::LIBRARY_GLESv2, &lib)) {
        system->envSet("ANDROID_GLESv2_LIB", lib);
    }
}
