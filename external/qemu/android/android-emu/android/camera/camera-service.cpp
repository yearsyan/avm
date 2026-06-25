/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <stdio.h>
#include <stdlib.h>

#include "android/camera/camera-service.h"

#include "aemu/base/files/PathUtils.h"
#include "android/avd/info.h"
#include "android/boot-properties.h"
#include "android/camera/camera-capture.h"
#include "android/camera/camera-format-converters.h"
#include "android/camera/camera-metrics.h"
#include "android/camera/camera-virtualscene.h"
#include "android/console.h" /* for android_hw */
#include "android/emulation/android_qemud.h"
#include "android/hw-sensors.h"
#include "android/utils/debug.h"
#include "android/utils/looper.h"
#include "android/utils/misc.h"
#include "android/utils/system.h"
#include "host-common/address_space_device.h"
#include "host-common/feature_control.h"
#include "host-common/hw-config-helper.h"
#include "host-common/hw-config.h"

#include <gfxstream/virtio-gpu-gfxstream-renderer.h>

using android::base::PathUtils;

namespace {
using namespace std::literals;

constexpr uint32_t kPixelFormat_RGBA_8888 = 0x1;
constexpr uint32_t kPixelFormat_YCBCR_420_888 = 0x23;
constexpr size_t kMaxStreamDim = 16384;
constexpr size_t kMaxStreamBytes =
        kMaxStreamDim * kMaxStreamDim * sizeof(uint32_t);

// TODO(b/173651912): remove this thing and call the callback from
// camera_XYZ_(start|stop)_capturing instead.
struct CameraCallbackDesc {
    void set(camera_callback_t cb, void* ctx, CameraSourceType src) {
        callback = cb;
        context = ctx;
        source = src;
    }

    void operator()(CameraSourceType src, bool value) const {
        if (callback && (source == src)) {
            callback(context, value);
        }
    }

    camera_callback_t callback = nullptr;
    void* context = nullptr;
    CameraSourceType source = {};
};

struct WhiteBalance {
    float red, green, blue;
};

size_t align16(const size_t x) {
    return (x + 15U) / 16U * 16U;
};

int64_t getTimestamp(void) {
    struct timeval t;
    gettimeofday(&t, nullptr);
    return int64_t(t.tv_sec) * 1000000L + t.tv_usec;
}

void cameraSleep(const int64_t millisec) {
    int64_t toSleep = millisec * 1000L;
    const int64_t wakeAt = getTimestamp() + toSleep;

    while (toSleep > 0) {
        const lldiv_t parts = ::lldiv(toSleep, 1000000L);

        struct timeval interval = {
            .tv_sec = parts.quot,
            .tv_usec = parts.rem,
        };

        if ((select(0, nullptr, nullptr, nullptr, &interval) < 0)
                && (errno == EINTR)) {
            toSleep = wakeAt - getTimestamp();
        } else {
            break;
        }
    }
}

void sendPayloadSize(QemudClient* qc, const size_t size) {
    char str[9];
    ::snprintf(str, sizeof(str), "%08zx", size);
    qemud_client_send(qc, reinterpret_cast<const uint8_t*>(str), 8);
}

constexpr size_t kReplyPrefixSize = 3;
constexpr uint8_t kOkReplyData[kReplyPrefixSize] = {'o', 'k', ':'};

void qemuClientReply(QemudClient* qc, const bool okko,
                     const void* data, const size_t dataSize) {
    static constexpr uint8_t kOkReply[kReplyPrefixSize] = {'o', 'k', 0};
    static constexpr uint8_t kKoReply[kReplyPrefixSize] = {'k', 'o', 0};
    static constexpr uint8_t kKoReplyData[kReplyPrefixSize] = {'k', 'o', ':'};

    const uint8_t* okkoStr = dataSize ?
        (okko ? kOkReplyData : kKoReplyData) :
        (okko ? kOkReply : kKoReply);

    sendPayloadSize(qc, kReplyPrefixSize + dataSize);
    qemud_client_send(qc, okkoStr, kReplyPrefixSize);
    if (dataSize) {
        qemud_client_send(qc, static_cast<const uint8_t*>(data),
                          dataSize);
    }
}

void qemuClientReply(QemudClient* qc, const bool okko,
                     const std::string_view str = {}) {
    qemuClientReply(qc, okko, str.data(), str.size());
}

void qemuClientReplyASCIZ(QemudClient* qc, const bool okko, const char* str) {
    qemuClientReply(qc, okko, str, ::strlen(str));
}

std::optional<std::string_view> getTokenValueStr(const std::string_view params,
                                                 const std::string_view name) {
    const size_t paramsSize = params.size();
    const size_t nameSize = name.size();

    size_t i = 0;
    while ((i = params.find(name, i)) != params.npos) {
        const size_t nameEnd = i + nameSize;
        if (nameEnd >= paramsSize) {
            return std::nullopt;
        } else if (params[nameEnd] == '=') {
            const size_t valueBegin = nameEnd + 1;
            const size_t valueEnd = params.find(' ', valueBegin);

            return (valueEnd == params.npos) ?
                params.substr(valueBegin) :
                params.substr(valueBegin, valueEnd - valueBegin);
        } else {
            ++i;
        }
    }

    return std::nullopt;
}

template <class T, class P> bool getParamValue(T& destination,
                                               const std::string_view params,
                                               const std::string_view name,
                                               const P valueParser) {
    const std::optional<std::string_view> maybeValueStr =
        getTokenValueStr(params, name);
    if (!maybeValueStr) {
        return false;
    }

    auto maybeValue = valueParser(maybeValueStr.value());
    if (!maybeValue) {
        return false;
    }

    destination = std::move(maybeValue.value());
    return true;
}

template <class T, class P> bool getParamValueV(T& destination,
                                                const std::string_view params,
                                                const std::string_view name,
                                                const P valueParser,
                                                const T& defValue) {
    const std::optional<std::string_view> maybeValueStr =
        getTokenValueStr(params, name);
    if (!maybeValueStr) {
        destination = defValue;
        return true;
    }

    auto maybeValue = valueParser(maybeValueStr.value());
    if (!maybeValue) {
        return false;
    }

    destination = std::move(maybeValue.value());
    return true;
}

template <class T, class P, class F> bool getParamValueF(T& destination,
                                                         const std::string_view params,
                                                         const std::string_view name,
                                                         const P valueParser,
                                                         const F& getDefValue) {
    const std::optional<std::string_view> maybeValueStr =
        getTokenValueStr(params, name);
    if (!maybeValueStr) {
        destination = getDefValue();
        return true;
    }

    auto maybeValue = valueParser(maybeValueStr.value());
    if (!maybeValue) {
        return false;
    }

    destination = std::move(maybeValue.value());
    return true;
}

std::optional<bool> parseBool(const std::string_view str) {
    if (str == "0"sv) {
        return false;
    } else if (str == "1"sv) {
        return true;
    } else {
        return std::nullopt;
    }
}

template <class T> std::optional<T> parseInt(const std::string_view str,
                                             const unsigned base = 10) {
    if (str.empty()) {
        return std::nullopt;
    }

    T value;
    const auto [ptr, ec] =
        std::from_chars(&*str.begin(), &*str.end(), value, base);
    if ((ec != std::errc()) || (ptr != &*str.end())) {
        return std::nullopt;
    }

    return value;
}

template <class T, class F>
std::optional<T> parseIntValidated(const std::string_view str,
                                   const F& isValid,
                                   const unsigned base = 10) {
    const auto maybeValue = parseInt<T>(str, base);
    if (maybeValue && isValid(maybeValue.value())) {
        return maybeValue;
    } else {
        return std::nullopt;
    }
}

template <class F> std::optional<float>parseFloatValidated(const std::string_view str,
                                                           const F& isValid) {
    if (str.empty()) {
        return std::nullopt;
    }

    char* end = const_cast<char*>(&*str.end());
    const float value = std::strtof(&*str.begin(), &end);
    if (end != &*str.end()) {
        return std::nullopt;
    }

    if (isValid(value)) {
        return value;
    } else {
        return std::nullopt;
    }
}

std::optional<size_t> parseSize(const std::string_view str) {
    return parseInt<size_t>(str);
}

std::optional<uint64_t> parseOffset(const std::string_view str) {
    return parseInt<uint64_t>(str);
}

std::optional<uint32_t> parsePix(const std::string_view str) {
    return parseInt<uint32_t>(str);
}

std::optional<uint32_t> parseInpChannel(const std::string_view str) {
    return parseInt<uint32_t>(str);
}

std::optional<std::pair<uint32_t, uint32_t>>
parseDim(const std::string_view str) {
    static constexpr auto parseDim1 =
        [](const std::string_view str){
            static constexpr auto isValidDimValue =
                [](const uint32_t value){ return value > 0; };

            return parseIntValidated<uint32_t>(str, isValidDimValue);
        };

    const size_t xpos = str.find('x');
    if (xpos == str.npos) {
        return std::nullopt;
    }

    std::optional<uint32_t> maybeWidth = parseDim1(str.substr(0, xpos));
    if (!maybeWidth) {
        return std::nullopt;
    }

    std::optional<uint32_t> maybeHeight = parseDim1(str.substr(xpos + 1));
    if (!maybeHeight) {
        return std::nullopt;
    }

    return std::make_pair(maybeWidth.value(), maybeHeight.value());
}

std::optional<WhiteBalance> parseWhiteBalance(const std::string_view str) {
    static constexpr auto parseWhiteBalance1 =
        [](const std::string_view str){
            static constexpr auto isValidWhiteBalanceValue =
                [](const float value){
                    return value > 0.0f;
                };

            return parseFloatValidated(str, isValidWhiteBalanceValue);
        };

    WhiteBalance whiteBalance;
    std::optional<float> maybeValue;

    const size_t comma1 = str.find(',');
    if (comma1 == str.npos) {
        return std::nullopt;
    }

    maybeValue = parseWhiteBalance1(str.substr(0, comma1));
    if (!maybeValue) {
        return std::nullopt;
    }

    whiteBalance.red = maybeValue.value();

    const size_t comma2 = str.find(',', comma1 + 1);
    if (comma2 == str.npos) {
        return std::nullopt;
    }

    maybeValue = parseWhiteBalance1(str.substr(comma1 + 1, comma2 - comma1 - 1));
    if (!maybeValue) {
        return std::nullopt;
    }

    whiteBalance.green = maybeValue.value();

    maybeValue = parseWhiteBalance1(str.substr(comma2 + 1));
    if (!maybeValue) {
        return std::nullopt;
    }

    whiteBalance.blue = maybeValue.value();
    return whiteBalance;
}

std::optional<float> parseExpComp(const std::string_view str) {
    return parseFloatValidated(str, [](const float value){ return value > 0.0f; });
}

template <class Sink > bool tokenize(const std::string_view str,
                                     const char separator,
                                     const bool allowEmpty,
                                     const Sink sink) {
    size_t i = 0;
    size_t sepPos;
    while ((sepPos = str.find(separator, i)) != str.npos) {
        if (((sepPos == i) && !allowEmpty) ||
                !sink(str.substr(i, sepPos - i))) {
            return false;
        }
        i = sepPos + 1;
    }

    return ((i < str.size()) || allowEmpty) && sink(str.substr(i));
}

std::string cameraInfoToString(const CameraInfo& ci) {
    if (ci.frame_sizes_num == 0) {
        return {};
    }

    char buf[256];
    int len = ::snprintf(buf, sizeof(buf), "name=%s channel=%u pix=%u "
                         "dir=%s sensor_orientation=%u framedims=%ux%u", ci.device_name,
                         ci.inp_channel, ci.pixel_format, ci.direction, ci.orientation,
                         ci.frame_sizes[0].width, ci.frame_sizes[0].height);

    std::string info(buf, len);

    for (int i = 1; i < ci.frame_sizes_num; ++i) {
        len = ::snprintf(buf, sizeof(buf), ",%ux%u",
                         ci.frame_sizes[i].width,
                         ci.frame_sizes[i].height);
        info.append(buf, len);
    }

    info.push_back('\n');

    return info;
}

template <class T>
std::pair<uint32_t, uint32_t> getMaxResolution(const T* begin, const T* end) {
    const auto cmp = [](const T& lhs, const T& rhs) {
        return (size_t(lhs.width) * size_t(lhs.height)) <
               (size_t(rhs.width) * size_t(rhs.height));
    };

    const T* result = std::max_element(begin, end, cmp);
    return {result->width, result->height};
}

std::pair<uint32_t, uint32_t>
cameraClientGetMaxResolution(const CameraInfo& info) {
    const CameraFrameDim *maxDim = info.frame_sizes;
    const int frameSizesNum = info.frame_sizes_num;
    if (!maxDim || (frameSizesNum <= 0)) {
        return {0, 0};
    }

    return getMaxResolution(maxDim, maxDim + frameSizesNum);
}

struct ICppQemudClient {
    virtual ~ICppQemudClient() = default;
    virtual void recv(QemudClient* qc, const uint8_t* data, size_t size) = 0;
    virtual void save(Stream* f) const = 0;
    virtual int load(Stream* f) = 0;
};

int android_camera_sensors_get_coarse_orientation(int orientation) {
    int coarse_base_orientation = orientation/90 - 1;

    return (static_cast<int>(android_sensors_get_coarse_orientation()) + coarse_base_orientation) % 4;
}
struct CameraService {
    CameraService() {
        set_coarse_orientation_getter(android_camera_sensors_get_coarse_orientation);

        // TODO: `hwCfg` should be `const AndroidHwConfig*`
        AndroidHwConfig* hwCfg = getConsoleAgents()->settings->hw();

        const char* const cameraBack = hwCfg->hw_camera_back;
        const char* const cameraFront = hwCfg->hw_camera_front;

        const bool uses_orientation = feature_is_enabled(kFeature_QemuCameraSensorOrientation);

        const int cameraBackOrientation = uses_orientation ? hwCfg->hw_camera_back_orientation : 90;
        const int cameraFrontOrientation = uses_orientation ? hwCfg->hw_camera_front_orientation : 90;

        static const auto isWebcam = [](const char* name){
            return !strncmp(name, "webcam", 6);
        };

        // Filename is optional, if not given a default image/video will be used
        constexpr char* kVideofileCamPrefix = "videofile";
        constexpr std::size_t kVideofileCamPrefixSize = std::string_view(kVideofileCamPrefix).length();
        static const auto isVideofileCam = [](const char* name){
            return !strncmp(name, kVideofileCamPrefix, kVideofileCamPrefixSize);
        };

        constexpr const char* kImagefileCamPrefix = "imagefile";
        constexpr std::size_t kImagefileCamPrefixSize = std::string_view(kImagefileCamPrefix).length();
        static auto isImagefileCam = [](const char* name) {
            return !strncmp(name, kImagefileCamPrefix, kImagefileCamPrefixSize);
        };

        constexpr const char* kImage360CamPrefix = "image360";
        constexpr std::size_t kImage360CamPrefixSize = std::string_view(kImage360CamPrefix).length();
        static auto isImage360Cam = [](const char* name) {
            return !strncmp(name, kImage360CamPrefix, kImage360CamPrefixSize);
        };


        static auto getCameraFilename =
                [](const char* name,
                   const std::size_t prefixSize) -> const char* {
            if (strlen(name) < (prefixSize + 1) || name[prefixSize] != ':') {
                return nullptr;
            }
            return name + prefixSize + 1;
        };

        if (androidHwConfig_hasEnvironmentBackCamera(hwCfg)) {
            // Same with virtual scene, but it'll share the global environment scene
            virtualscenecameraSetup("back", cameraBackOrientation, kEnvironment);
        } else if (androidHwConfig_hasVirtualSceneCamera(hwCfg)) {
            virtualscenecameraSetup("back", cameraBackOrientation, kVirtualScene);
        } else if (androidHwConfig_hasVideoPlaybackBackCamera(hwCfg)) {
            virtualscenecameraSetup("back", cameraBackOrientation, kVideoPlayback);
        } else if (isVideofileCam(cameraBack)) {
            const char* filename = getCameraFilename(cameraBack, kVideofileCamPrefixSize);
            virtualscenecameraSetup("back", cameraBackOrientation, kVideofile,
                                    filename);
        } else if (isImagefileCam(cameraBack)) {
            const char* filename = getCameraFilename(cameraBack, kImagefileCamPrefixSize);
            virtualscenecameraSetup("back", cameraBackOrientation, kImagefile, filename);
        } else if (isImage360Cam(cameraBack)) {
            const char* filename = getCameraFilename(cameraBack, kImage360CamPrefixSize);
            virtualscenecameraSetup("back", cameraBackOrientation, kImage360, filename);
        }

        if (androidHwConfig_hasEnvironmentFrontCamera(hwCfg)) {
            virtualscenecameraSetup("front", cameraFrontOrientation, kEnvironment);
        } else if (androidHwConfig_hasVideoPlaybackFrontCamera(hwCfg)) {
            virtualscenecameraSetup("front", cameraFrontOrientation, kVideoPlayback);
        } else if (isVideofileCam(cameraFront)) {
            const char* filename = getCameraFilename(cameraFront, kVideofileCamPrefixSize);
            virtualscenecameraSetup("front", cameraFrontOrientation, kVideofile,
                                    filename);
        } else if (isImagefileCam(cameraFront)) {
            const char* filename = getCameraFilename(cameraFront, kImagefileCamPrefixSize);
            virtualscenecameraSetup("front", cameraFrontOrientation, kImagefile, filename);
        } else if (isImage360Cam(cameraFront)) {
            const char* filename = getCameraFilename(cameraFront, kImage360CamPrefixSize);
            virtualscenecameraSetup("front", cameraFrontOrientation, kImage360, filename);
        }

        /* Lets see if HW config uses emulated cameras. */
        if (isWebcam(cameraBack) || isWebcam(cameraFront)) {
            CameraInfo ci[MAX_CAMERA] = {};

            /* Enumerate web cameras connected to the host. */
            const int connectedCnt = camera_enumerate_devices(ci, MAX_CAMERA);
            if (connectedCnt > 0) {
                /* Set up back camera emulation. */
                if (isWebcam(cameraBack)) {
                    webcamSetup(cameraBack, "back", cameraBackOrientation, ci, connectedCnt);
                } else if (isWebcam(cameraFront)) {
                    webcamSetup(cameraFront, "front", cameraFrontOrientation, ci, connectedCnt);
                }

                for (int i = 0; i < connectedCnt; ++i) {
                    camera_info_done(&ci[i]);
                }
            }
        }

        static constexpr char kServiceCamera[] = "camera";
        QemudService* serv = qemud_service_register(kServiceCamera, 0,
                this, &connectStatic, nullptr, nullptr);
        if (!serv) {
            derror("Could not register the '%s' service.", kServiceCamera);
            return;
        }

        static const auto isEmulated = [](const char* name){
            return !strncmp(name, "emulated", 6);
        };

        const bool isEmulatedBack = isEmulated(cameraBack);
        const bool isEmulatedFront = isEmulated(cameraFront);

        if (isEmulatedBack) {
            if (isEmulatedFront) {
                boot_property_add_qemu_sf_fake_camera("both");
            } else {
                boot_property_add_qemu_sf_fake_camera("back");
            }
        } else if (isEmulatedFront) {
            boot_property_add_qemu_sf_fake_camera("front");
        } else {
            boot_property_add_qemu_sf_fake_camera("none");
        }
    }

private:
    CameraInfo* findCameraByDeviceName(const std::string_view deviceName) {
        CameraInfo* ci = std::find_if(
                mCameraInfo, &mCameraInfo[mCameraCount],
                [&deviceName](const CameraInfo& ci){
                    return ci.device_name &&
                           !strncmp(ci.device_name, deviceName.data(), deviceName.size()) &&
                           !ci.device_name[deviceName.size()];
                });

        return (ci == &mCameraInfo[mCameraCount]) ? nullptr : ci;
    }

    void listCameras(QemudClient* qc) const {
        if (!mCameraCount) {
            /* No cameras connected to the host. Reply with "\n" */
            qemuClientReply(qc, true, "\n"sv);
            return;
        }

        std::string reply;
        for (unsigned i = 0; i < mCameraCount; ++i) {
            reply += cameraInfoToString(mCameraInfo[i]);
        }

        qemuClientReply(qc, true, reply);
    }

    void recv(QemudClient* qc, const std::string_view msg) {
        static constexpr std::string_view kQueryList = "list"sv;

        if (msg == kQueryList) {
            listCameras(qc);
        } else {
            qemuClientReply(qc, false, "Unknown query name"sv);
        }
    }

    static void cameraServiceRecvStatic(void*         that,
                                        uint8_t*      msg,
                                        const int     msglen,
                                        QemudClient*  client) {
        // this code assumed `msg` contains a whole request.
        if ((msglen <= 1) || msg[msglen - 1]) {
            return;  // ignore bad lengths and incomplete queries
        }

        static_cast<CameraService*>(that)->recv(
                client,
                std::string_view(reinterpret_cast<const char*>(msg),
                                 msglen - 1));
    }

    static void cameraServiceCloseStatic(void* that) { /* do nothing */ }

    static void cppQemudClientRecvStatic(void*         that,
                                         uint8_t*      msg,
                                         const int     msglen,
                                         QemudClient*  client) {
        if (msglen <= 0) {
            return;
        }

        static_cast<ICppQemudClient*>(that)->recv(client, msg, msglen);
    }

    static void cppQemudClientCloseStatic(void* that) {
        delete static_cast<ICppQemudClient*>(that);
    }

    static void cppQemudClientSaveStatic(Stream* f, QemudClient*,
                                         void* that) {
        static_cast<ICppQemudClient*>(that)->save(f);
    }

    static int cppQemudClientLoadStatic(Stream* f, QemudClient*,
                                        void* that) {
        return static_cast<ICppQemudClient*>(that)->load(f);
    }

    ICppQemudClient* cameraClientCreate(std::string_view params);

    QemudClient* connect(QemudService*  serv,
                         const int      channel,
                         const char*    params) {
        if (!params || !*params) {
            return qemud_client_new(serv, channel, params, this,
                                    &cameraServiceRecvStatic,
                                    &cameraServiceCloseStatic,
                                    nullptr, nullptr);
        } else {
            ICppQemudClient* cc = cameraClientCreate(params);
            if (cc) {
                return qemud_client_new(serv, channel, params, cc,
                                        &cppQemudClientRecvStatic,
                                        &cppQemudClientCloseStatic,
                                        &cppQemudClientSaveStatic,
                                        &cppQemudClientLoadStatic);
            }
        }

        return nullptr;
    }

    static QemudClient* connectStatic(void*          that,
                                      QemudService*  serv,
                                      const int      channel,
                                      const char*    clientParams) {
        return static_cast<CameraService*>(that)->connect(serv, channel,
                                                          clientParams);
    }

    void virtualscenecameraSetup(const char* dir,
                                 int sensor_orientation,
                                 CameraSourceType sourceType,
                                 const char* sceneFilename = nullptr) {
        std::string device_name;
        switch (sourceType) {
            case kVirtualScene:
                // virtualscene=environment with default config
                device_name = "environment";
                break;
            case kEnvironment:
                device_name = "environment";

                // Environment mode supports different camera directions
                // add it into the device name to be able to access
                // multiple cameras with their unique names.
                device_name += camera_virtualscene_name_argument_separator();
                device_name += dir;
                break;
            case kVideoPlayback:
                device_name = "videoplayback";
                break;
            case kVideofile:
                device_name = "videofile";
                break;
            case kImagefile:
                device_name = "imagefile";
                break;
            case kImage360:
                device_name = "image360";
                break;
            default: {
                derror("%s: unknown camera source type", __func__);
                device_name = "environment";
            }
        }
        if (sceneFilename) {
            device_name += camera_virtualscene_name_argument_separator();
            device_name.append(sceneFilename);
        }

        static const CameraInfoVtbl vtbl = {
            .open = &camera_virtualscene_open,
            .start_capturing = &camera_virtualscene_start_capturing,
            .read_frame = &camera_virtualscene_read_frame,
            .stop_capturing = &camera_virtualscene_stop_capturing,
            .close = &camera_virtualscene_close,
            .camera_source = sourceType,
        };

        static const CameraFrameDim kEmulateDims[] = {
                {640, 480},
                {352, 288},
                {320, 240},
                // Our RGB to YUV converter produces a broken image
                // for 176x144 and even writes outside of the image
                // memory range.
                //{176, 144},
                {1280, 720},
                {1280, 960}};

        CameraInfo ci;
        ci.frame_sizes = (CameraFrameDim*)malloc(sizeof(kEmulateDims));
        if (!ci.frame_sizes) {
            return;
        }

        ci.vtbl = &vtbl;
        memcpy(ci.frame_sizes, kEmulateDims, sizeof(kEmulateDims));
        ci.frame_sizes_num = sizeof(kEmulateDims) / sizeof(*kEmulateDims);
        ci.display_name = ASTRDUP(device_name.c_str());
        ci.device_name = ASTRDUP(device_name.c_str());
        ci.camera_name = ASTRDUP(device_name.c_str());
        ci.inp_channel = 0;
        ci.pixel_format = camera_virtualscene_preferred_format();
        ci.direction = ASTRDUP(dir);
        ci.orientation = sensor_orientation;
        ci.in_use = 0;

        addCameraInfo(std::move(ci));
    }

    void webcamSetup(const char* dispName, const char* dir,
                     int sensor_orientation,
                     CameraInfo* webcams, int webcamsCnt) {
        static const CameraInfoVtbl vtbl = {
            .open = &camera_device_open,
            .start_capturing = &camera_device_start_capturing,
            .read_frame = &camera_device_read_frame,
            .stop_capturing = &camera_device_stop_capturing,
            .close = &camera_device_close,
            .camera_source = kWebcam,
        };

        CameraInfo* srcCi = std::find_if(
                webcams, &webcams[webcamsCnt],
                [dispName](const CameraInfo& ci){
                    return ci.display_name && !strcmp(ci.display_name, dispName);
                });
        if (srcCi == &webcams[webcamsCnt]) {
            dwarning("Camera '%s' is not found in the list of connected cameras. "
                    "Use '-webcam-list' emulator option to obtain the list of connected "
                    "camera names.", dispName);
            return;
        }
        if (srcCi->in_use) {
            dwarning("Camera '%s' is already used.", dispName);
            return;
        }

        CameraInfo dstCi = {};
        camera_info_copy(&dstCi, srcCi);
        srcCi->in_use = 1;
        dstCi.vtbl = &vtbl;
        free(dstCi.direction);
        dstCi.direction = ASTRDUP(dir);
        dstCi.orientation = sensor_orientation;

        addCameraInfo(std::move(dstCi));
    }

    static constexpr size_t MAX_CAMERA = 8;

    void addCameraInfo(CameraInfo ci) {
        if (mCameraCount < MAX_CAMERA) {
            mCameraInfo[mCameraCount] = std::move(ci);
            ++mCameraCount;
        } else {
            dwarning("Don't have a room for the '%s' camera.", ci.display_name);
            camera_info_done(&ci);
        }
    }

    CameraInfo  mCameraInfo[MAX_CAMERA] = {};
    unsigned    mCameraCount = 0;
};

struct QueryParser {
    using ParseResult =
        std::optional<std::pair<std::string_view, std::string_view>>;

    ParseResult recv(const std::string_view msg) {
        constexpr char kQuerySeparator = 0;

        const size_t separator = msg.find(kQuerySeparator);
        if (separator == msg.npos) {
            mBuffer.insert(mBuffer.end(), msg.begin(), msg.end());
            return std::nullopt;
        }

        ParseResult result;
        if (mBuffer.empty()) {
            result = process(msg.substr(0, separator));
        } else {
            const size_t querySize = mBuffer.size() + separator;

            mBuffer.insert(mBuffer.end(),
                           msg.begin(), msg.begin() + separator);

            result = process(std::string_view(mBuffer.data(), querySize));
        }

        mBuffer.assign(msg.begin() + separator + 1, msg.end());
        return result;
    }

    void save(Stream* f) const {
        stream_put_be32(f, mBuffer.size());
        stream_write(f, mBuffer.data(), mBuffer.size());
    }

    int load(Stream* f) {
        const ssize_t size = stream_get_be32(f);
        mBuffer.resize(size);
        return (stream_read(f, mBuffer.data(), size) == size) ? 0 : -EIO;
    }

private:
    static std::pair<std::string_view, std::string_view>
    process(std::string_view query) {
        const size_t separator = query.find(' ');
        if (separator != query.npos) {
            return {query.substr(0, separator), query.substr(separator + 1)};
        } else {
            return {std::move(query), {}};
        }
    }

    std::vector<char> mBuffer;
};

CameraCallbackDesc g_cameraCallbackDesc;

struct BaseCameraClient : public ICppQemudClient {
    BaseCameraClient(CameraInfo& ci, CameraDevice& cd)
            : mCameraInfo(ci)
            , mCameraDevice(cd) {
        ci.in_use = 1;
    }

    virtual ~BaseCameraClient() {
        (mCameraInfo.vtbl->close)(&mCameraDevice);
        mCameraInfo.in_use = 0;
    }

protected:
    virtual void processQuery(std::string_view query,
                              std::string_view params,
                              QemudClient* qc) = 0;

    void recv(QemudClient* qc, const uint8_t* data, const size_t size) override {
        auto maybeQueryAndParams = mQueryParser.recv(
                std::string_view(reinterpret_cast<const char*>(data), size));
        if (maybeQueryAndParams) {
            auto [query, params] = std::move(maybeQueryAndParams.value());
            processQuery(std::move(query), std::move(params), qc);
        }
    }

    void save(Stream* f) const override {
        stream_put_be64(f, mFrameCounter);
        mQueryParser.save(f);
    }

    int load(Stream* f) override {
        mFrameCounter = stream_get_be64(f);
        return mQueryParser.load(f);
    }

    bool startCapturingImpl(const uint32_t width, uint32_t height,
                            const uint32_t pixelFormat) {
        camera_metrics_report_start_session(
                mCameraInfo.vtbl->camera_source, mCameraInfo.direction,
                width, height, pixelFormat);

        const int res =
            (mCameraInfo.vtbl->start_capturing)(
                &mCameraDevice, pixelFormat, width, height);
        if (res) {
            dwarning("Can't start the '%s' camera: %d.",
                     mCameraInfo.display_name, res);
            return false;
        }

        camera_metrics_report_start_result(CLIENT_START_RESULT_SUCCESS);
        g_cameraCallbackDesc(mCameraInfo.vtbl->camera_source, true);
        mFrameCounter = 0;
        return true;
    }

    int readFrameImpl(const WhiteBalance& wb, const float expComp,
                      ClientFrame& frame, QemudClient* qc) const {
        const auto readFrame = mCameraInfo.vtbl->read_frame;

        int retry = readFrame(&mCameraDevice, &frame,
                              wb.red, wb.green, wb.blue,
                              expComp, mCameraInfo.direction, mCameraInfo.orientation);
        if (!retry) {
            return 0;
        }

        const int64_t timeout = getTimestamp() + 2000000L;
        do {
            cameraSleep(10);

            retry = readFrame(&mCameraDevice, &frame,
                              wb.red, wb.green, wb.blue,
                              expComp, mCameraInfo.direction, mCameraInfo.orientation);
        } while ((retry > 0) && (getTimestamp() < timeout));

        if (retry > 0) {
            qemuClientReply(qc, false, "Unable to obtain video frame "
                                       "from the camera"sv);
        } else if (retry < 0) {
            qemuClientReplyASCIZ(qc, false, strerror(errno));
        }

        return retry;
    }

    void stopCapturingImpl() const {
        g_cameraCallbackDesc(mCameraInfo.vtbl->camera_source, false);
        camera_metrics_report_stop_session(mFrameCounter);

        const int res = (mCameraInfo.vtbl->stop_capturing)(&mCameraDevice);
        if (res) {
            derror("Can't stop the '%s' camera: %d.", mCameraInfo.display_name, res);
        }
    }

    void reportStartError(const ClientStartResult result) {
        camera_metrics_report_start_result(result);
        camera_metrics_report_stop_session(0);
    }

    void incrementFrameCounter() {
        ++mFrameCounter;
    }

    CameraInfo& mCameraInfo;
    CameraDevice& mCameraDevice;

private:
    QueryParser mQueryParser;
    uint64_t    mFrameCounter = 0;
};

constexpr std::string_view kQueryConnect         = "connect"sv;
constexpr std::string_view kQueryStart           = "start"sv;
constexpr std::string_view kQueryFrame           = "frame"sv;
constexpr std::string_view kQueryStop            = "stop"sv;
constexpr std::string_view kQueryDisconnect      = "disconnect"sv;

struct OldCamerasClient : public BaseCameraClient {
    OldCamerasClient(CameraInfo& ci, CameraDevice& cd)
            : BaseCameraClient(ci, cd)
    {}

    virtual ClientStartResult start(std::string_view params) = 0;
    virtual void capture(std::string_view params, QemudClient* qc) = 0;
    virtual void stop() = 0;

    void processQueryStart(const std::string_view params, QemudClient* qc) {
        const ClientStartResult startResult = start(params);
        if (startResult < 0) {
            reportStartError(startResult);
            return;
        }

        switch (startResult) {
        case CLIENT_START_RESULT_SUCCESS:
        case CLIENT_START_RESULT_ALREADY_STARTED:
            qemuClientReply(qc, true);
            break;
        case CLIENT_START_RESULT_PARAMETER_MISMATCH:
            qemuClientReply(qc, false, "Camera is already started with "
                                       "different capturing parameters"sv);
            break;
        case CLIENT_START_RESULT_UNKNOWN_PIXEL_FORMAT:
            qemuClientReply(qc, false, "Pixel format is unknown"sv);
            break;
        case CLIENT_START_RESULT_NO_PIXEL_CONVERSION:
            qemuClientReply(qc, false, "No conversion exist for the "
                                       "requested pixel format"sv);
            break;
        case CLIENT_START_RESULT_OUT_OF_MEMORY:
            qemuClientReply(qc, false, "Out of memory"sv);
            break;
        case CLIENT_START_RESULT_INCORRECT_PARAMS:
            qemuClientReply(qc, false, "Incorrect params"sv);
            break;
        default:
            dwarning("Unexpected startResult: %d.", startResult);
            [[fallthrough]];
        case CLIENT_START_RESULT_FAILED:
            qemuClientReply(qc, false, "Cannot start the camera");
            break;
        }
    }

    void processQuery(const std::string_view query,
                      const std::string_view params,
                      QemudClient* qc) override {
        if (query == kQueryFrame) {
            capture(params, qc);
        } else if (query == kQueryConnect) {
            qemuClientReply(qc, true);
        } else if (query == kQueryStart) {
            processQueryStart(params, qc);
        } else if ((query == kQueryStop) ||
                   (query == kQueryDisconnect)){
            stop();
            qemuClientReply(qc, true);
        } else if (query.empty()) {
            qemuClientReply(qc, false, "Empty query"sv);
        } else {
            qemuClientReply(qc, false, "Unknown query"sv);
        }
    }
};

constexpr std::string_view kParamDim             = "dim"sv;
constexpr std::string_view kParamPix             = "pix"sv;
constexpr std::string_view kParamOffset          = "offset"sv;
constexpr std::string_view kParamPreviewSize     = "preview"sv;
constexpr std::string_view kParamVideoSize       = "video"sv;
constexpr std::string_view kParamSendFrameTime   = "time"sv;
constexpr std::string_view kParamWhiteBalance    = "whiteb"sv;
constexpr std::string_view kParamExpComp         = "expcomp"sv;

constexpr WhiteBalance kDefaultWhiteBalance = { 1.0f, 1.0f, 1.0f };

struct SerialCameraClient : public OldCamerasClient {
    SerialCameraClient(CameraInfo& ci, CameraDevice& cd)
            : OldCamerasClient(ci, cd) {}

    ~SerialCameraClient() {
        stop();
    }

    static SerialCameraClient* create(CameraInfo& ci, CameraDevice& cd) {
        return new SerialCameraClient(ci, cd);
    }

    ClientStartResult start(const std::string_view params) override {
        uint32_t width, height;
        auto rect = std::tie(width, height);
        if (!getParamValue(rect, params, kParamDim, parseDim)) {
            return CLIENT_START_RESULT_INCORRECT_PARAMS;
        }
        uint32_t pixFormat;
        if (!getParamValue(pixFormat, params, kParamPix, parsePix)) {
            return CLIENT_START_RESULT_INCORRECT_PARAMS;
        }

        if (mStarted) {
            if ((width != mWidth) || (height != mHeight) ||
                    (pixFormat != mPixelFormat)) {
                return CLIENT_START_RESULT_PARAMETER_MISMATCH;
            } else {
                return CLIENT_START_RESULT_ALREADY_STARTED;
            }
        }

        return start3(width, height, pixFormat);
    }

    void capture(const std::string_view params, QemudClient* qc) override {
        constexpr size_t kZero = 0;

        size_t videoSize;
        if (!getParamValueV(videoSize, params, kParamVideoSize,
                            parseSize, kZero)) {
            qemuClientReply(qc, false, "Invalid 'video' parameter"sv);
            return;
        }

        size_t previewSize;
        if (!getParamValueV(previewSize, params, kParamPreviewSize,
                            parseSize, kZero)) {
            qemuClientReply(qc, false, "Invalid 'preview' parameter"sv);
            return;
        }

        if (!videoSize && !previewSize) {
            qemuClientReply(qc, false, "Nothing requested"sv);
            return;
        }

        if ((videoSize && (mVideoFrameBuffer.size() != videoSize)) ||
                (previewSize && (mPreviewFrameBuffer.size() != previewSize))) {
            qemuClientReply(qc, false, "Frame size mismatch"sv);
            return;
        }

        bool sendFrameTime;
        if (!getParamValueV(sendFrameTime, params, kParamSendFrameTime,
                            parseBool, false)) {
            qemuClientReply(qc, false, "Invalid 'time' parameter"sv);
            return;
        }

        WhiteBalance whiteBalance;
        if (!getParamValueV(whiteBalance, params, kParamWhiteBalance,
                            parseWhiteBalance, kDefaultWhiteBalance)) {
            qemuClientReply(qc, false, "Invalid 'whiteb' parameter"sv);
            return;
        }

        float expComp;
        if (!getParamValueV(expComp, params, kParamExpComp,
                            parseExpComp, 1.0f)) {
            qemuClientReply(qc, false, "Invalid 'expcomp' parameter"sv);
            return;
        }

        ClientFrameBuffer fbs[2] = {};
        uint32_t fbsNum = 0;
        if (videoSize) {
            fbs[fbsNum].pixel_format = mPixelFormat;
            fbs[fbsNum].width = mWidth;
            fbs[fbsNum].height = mHeight;
            fbs[fbsNum].framebuffer = mVideoFrameBuffer.data();
            fbsNum++;
        }
        if (previewSize) {
            fbs[fbsNum].pixel_format = V4L2_PIX_FMT_RGB32;
            fbs[fbsNum].width = mWidth;
            fbs[fbsNum].height = mHeight;
            fbs[fbsNum].framebuffer = mPreviewFrameBuffer.data();
            fbsNum++;
        }

        ClientFrame frame = {
            .framebuffers_count = fbsNum,
            .framebuffers = fbs,
            .staging_framebuffer = &mStagingFramebuffer,
            .staging_framebuffer_size = &mStagingFramebufferSize,
            .frame_time =
                    looper_nowNsWithClock(looper_getForThread(),
                                          LOOPER_CLOCK_VIRTUAL),
        };

        if (readFrameImpl(whiteBalance, expComp, frame, qc)) {
            return;
        }

        const size_t payloadSize = kReplyPrefixSize +
            (sendFrameTime ? sizeof(int64_t) : 0) + videoSize + previewSize;

        if (payloadSize > kReplyPrefixSize) {
            sendPayloadSize(qc, payloadSize);
            qemud_client_send(qc, kOkReplyData, kReplyPrefixSize);

            if (videoSize) {
                qemud_client_send(qc, mVideoFrameBuffer.data(), videoSize);
            }
            if (previewSize) {
                qemud_client_send(qc, mPreviewFrameBuffer.data(), previewSize);
            }
            if (sendFrameTime) {
                const int64_t adjusted_time = frame.frame_time +
                        android_sensors_get_time_offset();

                qemud_client_send(qc, (const uint8_t*) &adjusted_time,
                                  sizeof(adjusted_time));
            }
        } else {
            qemuClientReply(qc, true);
        }

        incrementFrameCounter();
    }

    void stop() override {
        if (!mStarted) {
            return;
        }

        stopCapturingImpl();

        mVideoFrameBuffer.clear();
        mPreviewFrameBuffer.clear();
        ::free(mStagingFramebuffer);
        mStagingFramebuffer = nullptr;
        mStagingFramebufferSize = 0;
        mWidth = 0;
        mHeight = 0;
        mPixelFormat = 0;
        mStarted = false;
    }

    void save(Stream* f) const override {
        OldCamerasClient::save(f);

        stream_put_byte(f, mStarted);
        if (mStarted) {
            stream_put_be32(f, mWidth);
            stream_put_be32(f, mHeight);
            stream_put_be32(f, mPixelFormat);
        }
    }

    int load(Stream* f) override {
        if (int r = OldCamerasClient::load(f)) {
            return r;
        }

        if (stream_get_byte(f) != 0) {
            const uint32_t width = stream_get_be32(f);
            const uint32_t height = stream_get_be32(f);
            const uint32_t pixelFormat = stream_get_be32(f);

            if (start3(width, height, pixelFormat) < 0) {
                return -EIO;
            }
        }

        return 0;
    }

private:
    ClientStartResult start3(const uint32_t width,
                             const uint32_t height,
                             const uint32_t pixFormat) {
        if (!has_converter(mCameraInfo.pixel_format, pixFormat) ||
            !has_converter(mCameraInfo.pixel_format, V4L2_PIX_FMT_RGB32)) {
            return CLIENT_START_RESULT_NO_PIXEL_CONVERSION;
        }

        size_t videoFrameSize;
        if (!calculate_framebuffer_size(pixFormat, width, height,
                                        &videoFrameSize)) {
            return CLIENT_START_RESULT_UNKNOWN_PIXEL_FORMAT;
        }

        if (!startCapturingImpl(width, height, pixFormat)) {
            return CLIENT_START_RESULT_FAILED;
        }

        mVideoFrameBuffer.resize(videoFrameSize);
        mPreviewFrameBuffer.resize(width * height * 4U);  // RGBA32
        mWidth = width;
        mHeight = height;
        mPixelFormat = pixFormat;
        mStarted = true;
        return CLIENT_START_RESULT_SUCCESS;
    }

    std::vector<uint8_t> mVideoFrameBuffer;
    std::vector<uint8_t> mPreviewFrameBuffer;
    uint8_t*    mStagingFramebuffer = nullptr;
    size_t      mStagingFramebufferSize = 0;
    uint32_t    mWidth = 0;
    uint32_t    mHeight = 0;
    uint32_t    mPixelFormat = 0;
    bool        mStarted = false;
};

struct GasCameraClient : public OldCamerasClient {
    GasCameraClient(CameraInfo& ci, CameraDevice& cd)
            : OldCamerasClient(ci, cd)
            , mGasGetHostPtr(get_address_space_device_control_ops()->get_host_ptr)
            , mGasPhysAddrStart(get_address_space_device_hw_funcs()->getPhysAddrStart())
    {}

    virtual ~GasCameraClient() {
        stop();
    }

    static GasCameraClient* create(CameraInfo& ci, CameraDevice& cd) {
        return new GasCameraClient(ci, cd);
    }

    ClientStartResult start(std::string_view) override {
        if (mStarted) {
            return CLIENT_START_RESULT_ALREADY_STARTED;
        }

        return start0();
    }

    void capture(const std::string_view params, QemudClient* qc) override {
        if (!mStarted) {
            qemuClientReply(qc, false, "Camera is not started");
            return;
        }

        uint32_t width;
        uint32_t height;
        auto rect = std::tie(width, height);
        if (!getParamValue(rect, params, kParamDim, parseDim)) {
            qemuClientReply(qc, false, "Invalid or missing 'dim' parameter"sv);
            return;
        }

        if ((width == 0) || (width > kMaxStreamDim) ||
            (height == 0) || (height > kMaxStreamDim)) {
            qemuClientReply(qc, false, "'dim' is out of bounds"sv);
            return;
        }

        uint32_t pixFormat;
        if (!getParamValue(pixFormat, params, kParamPix, parsePix)) {
            qemuClientReply(qc, false, "Invalid or missing 'pix' parameter"sv);
            return;
        }

        uint64_t offset;
        if (!getParamValue(offset, params, kParamOffset, parseOffset)) {
            qemuClientReply(qc, false, "Invalid or missing 'offset' parameter"sv);
            return;
        }

        bool sendFrameTime;
        if (!getParamValueV(sendFrameTime, params, kParamSendFrameTime,
                            parseBool, false)) {
            qemuClientReply(qc, false, "Invalid 'time' parameter"sv);
            return;
        }

        WhiteBalance whiteBalance;
        if (!getParamValueV(whiteBalance, params, kParamWhiteBalance,
                            parseWhiteBalance, kDefaultWhiteBalance)) {
            qemuClientReply(qc, false, "Invalid or missing 'whiteb' parameter"sv);
            return;
        }

        float expComp;
        if (!getParamValueV(expComp, params, kParamExpComp,
                            parseExpComp, 1.0f)) {
            qemuClientReply(qc, false, "Invalid or missing 'expcomp' parameter"sv);
            return;
        }

        const ClientFrameBuffer fb = {
            .pixel_format = pixFormat,
            .width = width,
            .height = height,
            .framebuffer = getGasBufferAddress(offset),
        };

        ClientFrame frame = {
            .framebuffers_count = 1,
            .framebuffers = &fb,
            .staging_framebuffer = &mStagingFramebuffer,
            .staging_framebuffer_size = &mStagingFramebufferSize,
            .frame_time =
                    looper_nowNsWithClock(looper_getForThread(),
                                          LOOPER_CLOCK_VIRTUAL),
        };

        if (readFrameImpl(whiteBalance, expComp, frame, qc)) {
            return;
        }

        if (sendFrameTime) {
            const int64_t adjusted_time = frame.frame_time +
                    android_sensors_get_time_offset();
            qemuClientReply(qc, true, &adjusted_time, sizeof(adjusted_time));
        } else {
            qemuClientReply(qc, true);
        }

        incrementFrameCounter();
    }

    void stop() override {
        if (!mStarted) {
            return;
        }

        stopCapturingImpl();

        ::free(mStagingFramebuffer);
        mStagingFramebuffer = nullptr;
        mStagingFramebufferSize = 0;
        mStarted = false;
    }

    void save(Stream* f) const override {
        OldCamerasClient::save(f);
        stream_put_byte(f, mStarted);
    }

    int load(Stream* f) override {
        if (int r = OldCamerasClient::load(f)) {
            return r;
        }

        if (stream_get_byte(f) != 0) {
            start0();
        }
        return 0;
    }

private:
    ClientStartResult start0() {
        const auto [width, height] = cameraClientGetMaxResolution(mCameraInfo);
        if (!startCapturingImpl(width, height, mCameraInfo.pixel_format)) {
            return CLIENT_START_RESULT_FAILED;
        }

        mStarted = true;
        return CLIENT_START_RESULT_SUCCESS;
    }

    void* getGasBufferAddress(const uint64_t offset) const {
        return mGasGetHostPtr(mGasPhysAddrStart + offset);
    }

    void*           (*const mGasGetHostPtr)(uint64_t);
    uint8_t*        mStagingFramebuffer = nullptr;
    size_t          mStagingFramebufferSize = 0;
    const uint64_t  mGasPhysAddrStart;
    bool            mStarted = false;
};

constexpr std::string_view kQueryConfigure = "configure"sv;
constexpr std::string_view kQueryCapture   = "capture"sv;
constexpr std::string_view kParamStreams   = "streams"sv;
constexpr std::string_view kParamBufs      = "bufs"sv;

struct MinigbmCameraClient : public BaseCameraClient {
    MinigbmCameraClient(CameraInfo& ci, CameraDevice& cd)
            : BaseCameraClient(ci, cd)
    {}

    ~MinigbmCameraClient() {
        ::free(mStagingFramebuffer);
    }

    static MinigbmCameraClient* create(CameraInfo& ci, CameraDevice& cd) {
        return new MinigbmCameraClient(ci, cd);
    }

    void processQuery(const std::string_view query,
                      const std::string_view params,
                      QemudClient* qc) override {
        if (query == kQueryCapture) {
            capture(params, qc);
        } else if (query == kQueryConfigure) {
            configure(params, qc);
        } else if (query.empty()) {
            qemuClientReply(qc, false, "Empty query"sv);
        } else {
            qemuClientReply(qc, false, "Unknown query"sv);
        }
    }

private:
    struct StreamState {
        std::vector<uint8_t> frameBuffer;
        int32_t id;
        uint32_t width;
        uint32_t height;
        uint32_t format;
    };

    using Streams = std::vector<StreamState>;

    // "configure streams=id:WxH@F,..."
    void configure(const std::string_view params, QemudClient* qc) {
        Streams streams;
        const bool parsed = getParamValue(streams, params, kParamStreams,
                [](const std::string_view str) -> std::optional<Streams> {
                    Streams streams;
                    if (tokenize(str, ',', false,
                                 [&streams](const std::string_view str){
                                     char strz[64];
                                     if (str.size() >= sizeof(strz)) {
                                         return false;
                                     }
                                     memcpy(strz, str.data(), str.size());
                                     strz[str.size()] = 0;

                                     StreamState ss;
                                     uint32_t androidFormat;
                                     if (4 != ::sscanf(strz, "%d:%ux%u@%X",
                                                       &ss.id, &ss.width, &ss.height, &androidFormat)) {
                                         return false;
                                     }

                                     if ((ss.width == 0) ||
                                         (ss.width > kMaxStreamDim) ||
                                         (ss.height == 0) ||
                                         (ss.height > kMaxStreamDim)) {
                                         return false;
                                     }

                                     const size_t numPixels = size_t(ss.width) *
                                                              size_t(ss.height);

                                     switch (androidFormat) {
                                         case kPixelFormat_RGBA_8888: {
                                             const size_t bufferSize =
                                                     numPixels *
                                                     sizeof(uint32_t);
                                             if (bufferSize > kMaxStreamBytes) {
                                                 return false;
                                             }

                                             ss.format = V4L2_PIX_FMT_RGB32;
                                             ss.frameBuffer.resize(bufferSize);
                                         } break;

                                         case kPixelFormat_YCBCR_420_888: {
                                             const size_t bufferSize =
                                                     align16(numPixels) +
                                                     align16(numPixels / 2);
                                             if (bufferSize > kMaxStreamBytes) {
                                                 return false;
                                             }

                                             ss.format = V4L2_PIX_FMT_NV12;
                                             ss.frameBuffer.resize(bufferSize);
                                         } break;

                                         default:
                                             return false;
                                     }

                                     streams.push_back(std::move(ss));
                                     return true;
                    })) {
                        return streams;
                    } else {
                        return std::nullopt;
                    }});
        if (!parsed) {
            camera_metrics_report_start_session(
                    mCameraInfo.vtbl->camera_source, mCameraInfo.direction,
                    0, 0, 0);
            reportStartError(CLIENT_START_RESULT_FAILED);
            qemuClientReply(qc, false, "Can't parse"sv);
            return;
        }
        if (streams.empty()) {
            camera_metrics_report_start_session(
                    mCameraInfo.vtbl->camera_source, mCameraInfo.direction,
                    0, 0, 0);
            reportStartError(CLIENT_START_RESULT_FAILED);
            qemuClientReply(qc, false, "No streams provided"sv);
            return;
        }

        if (!mStreams.empty()) {
            stopCapturingImpl();
        }

        const auto [width, height] = getMaxResolution(
                &*streams.begin(), &*streams.end());
        if (!startCapturingImpl(width, height, mCameraInfo.pixel_format)) {
            reportStartError(CLIENT_START_RESULT_FAILED);
            qemuClientReply(qc, false, "Can't start the camera"sv);
            return;
        }

        mStreams = std::move(streams);
        qemuClientReply(qc, true);
    }

    // "capture bufs=id:handle,..."
    void capture(const std::string_view params, QemudClient* qc) {
        using BufInfo = std::pair<StreamState*, uint32_t>;
        using Bufs = std::vector<BufInfo>;
        Bufs bufs;

        Streams& streamsRef = mStreams;
        const bool parsed = getParamValue(bufs, params, kParamBufs,
                [&streamsRef](const std::string_view str) -> std::optional<Bufs> {
                    Bufs bufs;
                    if (tokenize(str, ',', false,
                                 [&streamsRef, &bufs](const std::string_view str) -> bool {
                                     char strz[32];
                                     if (str.size() >= sizeof(strz)) {
                                         return false;
                                     }
                                     memcpy(strz, str.data(), str.size());
                                     strz[str.size()] = 0;

                                     int32_t id;
                                     uint32_t hostHandle;
                                     if (2 != ::sscanf(strz, "%d:%u", &id, &hostHandle)) {
                                         return false;
                                     }

                                     const auto si = std::find_if(streamsRef.begin(),
                                                                  streamsRef.end(),
                                                                  [id](const StreamState& ss) {
                                                                      return id == ss.id;
                                                                  });
                                     if (si == streamsRef.end()) {
                                         return false;
                                     }

                                     bufs.push_back({&*si, hostHandle});
                                     return true;
                    })) {
                        return bufs;
                    } else {
                        return std::nullopt;
                    }});
        if (!parsed) {
            qemuClientReply(qc, false, "Can't parse"sv);
            return;
        }
        const size_t bufsSize = bufs.size();
        if (!bufsSize) {
            qemuClientReply(qc, false, "No bufs provided"sv);
            return;
        }

        WhiteBalance whiteBalance;
        if (!getParamValueV(whiteBalance, params, kParamWhiteBalance,
                            parseWhiteBalance, kDefaultWhiteBalance)) {
            qemuClientReply(qc, false, "Invalid or missing 'whiteb' parameter"sv);
            return;
        }

        float expComp;
        if (!getParamValueV(expComp, params, kParamExpComp,
                            parseExpComp, 1.0f)) {
            qemuClientReply(qc, false, "Invalid or missing 'expcomp' parameter"sv);
            return;
        }

        std::vector<ClientFrameBuffer> fbs(bufsSize);
        for (size_t i = 0; i < bufsSize; ++i) {
            StreamState& ss = *bufs[i].first;
            ClientFrameBuffer& fb = fbs[i];
            fb.pixel_format = ss.format;
            fb.width = ss.width;
            fb.height = ss.height;
            fb.framebuffer = ss.frameBuffer.data();
            fb.framebuffer_size = ss.frameBuffer.size();
        }

        ClientFrame frame = {
            .framebuffers_count = bufsSize,
            .framebuffers = fbs.data(),
            .staging_framebuffer = &mStagingFramebuffer,
            .staging_framebuffer_size = &mStagingFramebufferSize,
            .frame_time =
                    looper_nowNsWithClock(looper_getForThread(),
                                          LOOPER_CLOCK_VIRTUAL),
        };

        if (readFrameImpl(whiteBalance, expComp, frame, qc)) {
            return;
        }

        for (const BufInfo& bi : bufs) {
            const StreamState& ss = *bi.first;

            stream_renderer_box box = {
                .x = 0, .y = 0, .z = 0,
                .w = ss.width, .h = ss.height, .d = 1,
            };

            struct iovec framebuffer = {
                .iov_base = const_cast<uint8_t*>(ss.frameBuffer.data()),
                .iov_len = ss.frameBuffer.size(),
            };

            stream_renderer_transfer_write_iov(bi.second, 0, 0, 0, 0,
                                               &box, 0, &framebuffer, 1);
        }

        qemuClientReply(qc, true);
        incrementFrameCounter();
    }

    void save(Stream* f) const override {
        BaseCameraClient::save(f);
        stream_put_be32(f, mStreams.size());
        stream_write(f, mStreams.data(),
                     mStreams.size() * sizeof(Streams::value_type));
    }

    int load(Stream* f) override {
        if (int r = BaseCameraClient::load(f)) {
            return r;
        }

        const uint32_t nStreams = stream_get_be32(f);
        Streams streams(nStreams);

        if (nStreams) {
            const ssize_t readSize =
                nStreams * sizeof(Streams::value_type);
            if (stream_read(f, streams.data(), readSize) != readSize) {
                return -EIO;
            }

            const auto [width, height] = getMaxResolution(
                    &*streams.begin(), &*streams.end());
            if (!startCapturingImpl(width, height,
                                    mCameraInfo.pixel_format)) {
                return -EIO;
            }
        }

        mStreams = std::move(streams);
        return 0;
    }

    Streams     mStreams;
    uint8_t*    mStagingFramebuffer = nullptr;
    size_t      mStagingFramebufferSize = 0;
};

enum class CameraClientProtocol : int {
    // Pixels are sent over the channel.
    SERIAL,

    // A guest-host shared memory (goldfish_address_space) offset
    // is sent over the channel, pixels are written to the
    // memory directly.
    GAS,

    // A virtio-gpu buffer handle is sent over the channel,
    // pixels are written to virtio-gpu directly.
    MINIGBM,
};

CameraClientProtocol getCameraClientProtocol(const std::string_view params) {
    if (feature_is_enabled(kFeature_Minigbm)) {
        return CameraClientProtocol::MINIGBM;
    }

    if (avdInfo_getApiLevel(getConsoleAgents()->settings->avdInfo()) > 29) {
        return CameraClientProtocol::GAS;
    }

    return CameraClientProtocol::SERIAL;
}

ICppQemudClient* CameraService::cameraClientCreate(const std::string_view params) {
    static constexpr std::string_view kParamName       = "name"sv;
    static constexpr std::string_view kParamInpChannel = "inp_channel"sv;

    std::optional<std::string_view> maybeDeviceName =
        getTokenValueStr(params, kParamName);
    if (!maybeDeviceName) {
        dwarning("Missing the '%s' parameter.", kParamName);
        return nullptr;
    }

    const std::string_view deviceName = maybeDeviceName.value();
    CameraInfo* ci = findCameraByDeviceName(deviceName);
    if (!ci) {
        dwarning("Camera name '%s' is not found in the list of "
                 "connected cameras.", deviceName);
        return nullptr;
    }

    if (ci->in_use) {
        dwarning("Can't open the '%s' camera, it is still in use.", deviceName);
        return nullptr;
    }
    if (!ci->frame_sizes_num || !ci->frame_sizes) {
        dwarning("Camera '%s' has no supported frame dimensions.", deviceName);
        return nullptr;
    }

    uint32_t inpChannel;
    if (!getParamValueV(inpChannel, params, kParamInpChannel, parseInpChannel, 0U)) {
        dwarning("Invalid '%s' parameter.", kParamInpChannel);
        return nullptr;
    }

    CameraDevice* cameraDevice = (ci->vtbl->open)(ci->device_name, inpChannel);
    if (!cameraDevice) {
        dwarning("Can't open a camera device: name='%s' channel=%u.",
                 deviceName, inpChannel);
        return nullptr;
    }

    ICppQemudClient* client;

    const CameraClientProtocol protocol = getCameraClientProtocol(params);
    switch (protocol) {
    case CameraClientProtocol::SERIAL:
        client = SerialCameraClient::create(*ci, *cameraDevice);
        break;
    case CameraClientProtocol::GAS:
        client = GasCameraClient::create(*ci, *cameraDevice);
        break;
    case CameraClientProtocol::MINIGBM:
        client = MinigbmCameraClient::create(*ci, *cameraDevice);
        break;
    default:
        derror("Unexpected protocol: %d.", protocol);
        client = nullptr;
        break;
    }

    if (!client) {
        (ci->vtbl->close)(cameraDevice);
    }
    return client;
}
}  // namespace

void android_camera_service_init(void) {
    // All the interesting things happen in the ctor.
    static CameraService s_cameraService;
}

void register_camera_status_change_callback(camera_callback_t cb,
                                            void* ctx,
                                            CameraSourceType src) {
    g_cameraCallbackDesc.set(cb, ctx, src);
}
