// Copyright 2026 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#import <AVFoundation/AVFoundation.h>
#import <Accelerate/Accelerate.h>
#include "webcam_source.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/types/optional.h"

#include "../fourcc_utils.h"
// For V4L2 constants
#include "android/camera/camera-common.h"
#include "android/hw-sensors.h"
#include "android/utils/debug.h"
#include "android/utils/system.h"

namespace android {
namespace ver {

/* Converts CoreVideo pixel format to our internal FOURCC value. */
static uint32_t FourCCToInternal(uint32_t cm_pix_format) {
    switch (cm_pix_format) {
        case kCVPixelFormatType_24RGB:
            return V4L2_PIX_FMT_RGB24;
        case kCVPixelFormatType_24BGR:
            return V4L2_PIX_FMT_BGR24;
        case kCVPixelFormatType_32ARGB:
            return V4L2_PIX_FMT_ARGB32;
        case kCVPixelFormatType_32RGBA:
            return V4L2_PIX_FMT_RGB32;
        case kCVPixelFormatType_32BGRA:
            return V4L2_PIX_FMT_BGR32;
        case kCVPixelFormatType_422YpCbCr8:
            return V4L2_PIX_FMT_UYVY;
        case kCVPixelFormatType_420YpCbCr8Planar:
            return V4L2_PIX_FMT_YUV420;
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
            return V4L2_PIX_FMT_NV12;
        case kCVPixelFormatType_422YpCbCr8_yuvs:
            return V4L2_PIX_FMT_YUYV;
        default:
            return 0;
    }
}

std::vector<std::shared_ptr<WebcamSource::WebcamInfo>>
WebcamSource::EnumerateWebcams() {
    std::vector<std::shared_ptr<WebcamSource::WebcamInfo>> webcams;

    NSArray* captureDeviceType = @[
        AVCaptureDeviceTypeBuiltInWideAngleCamera,
        AVCaptureDeviceTypeExternalUnknown,
    ];

    AVCaptureDeviceDiscoverySession* deviceDiscoverySession = [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:captureDeviceType
                                  mediaType:AVMediaTypeVideo
                                   position:AVCaptureDevicePositionUnspecified];

    NSArray* videoDevicesUnsorted = deviceDiscoverySession.devices;
    if (!videoDevicesUnsorted) {
        return webcams;
    }

    NSArray* videoDevices =
            [videoDevicesUnsorted sortedArrayUsingComparator:^(id lhs, id rhs) {
              const int r = strcmp([[lhs uniqueID] UTF8String],
                                   [[rhs uniqueID] UTF8String]);
              if (r > 0) {
                  return (NSComparisonResult)NSOrderedDescending;
              } else if (r < 0) {
                  return (NSComparisonResult)NSOrderedAscending;
              } else {
                  return (NSComparisonResult)NSOrderedSame;
              }
            }];

    for (AVCaptureDevice* videoDevice in videoDevices) {
        std::shared_ptr<WebcamInfo> info = std::make_shared<WebcamInfo>();
        info->friendly_name = [[videoDevice localizedName] UTF8String];
        info->os_name = [[videoDevice uniqueID] UTF8String];
        info->os_alias = info->os_name;
        info->preferred_format_index = -1;

        std::map<uint32_t, WebcamPixelFormat> format_map;
        for (AVCaptureDeviceFormat* vFormat in [videoDevice formats]) {
            CMFormatDescriptionRef description = vFormat.formatDescription;
            uint32_t pixel_format = FourCCToInternal(
                    CMFormatDescriptionGetMediaSubType(description));
            if (pixel_format == 0) {
                continue;
            }

            WebcamPixelFormat& wpfmt = format_map[pixel_format];
            wpfmt.pixel_format = pixel_format;
            wpfmt.compressed = false;
            CMVideoDimensions dims =
                    CMVideoFormatDescriptionGetDimensions(description);

            bool duplicate = false;
            for (const auto& res : wpfmt.resolutions) {
                if (res.resolution.width == (unsigned int)dims.width &&
                    res.resolution.height == (unsigned int)dims.height) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                wpfmt.resolutions.push_back({(unsigned int)dims.width,
                                             (unsigned int)dims.height,
                                             {}});
            }
        }

        for (auto& entry : format_map) {
            WebcamPixelFormat& wpfmt = entry.second;
            /* Sort resolutions by pixel count */
            std::sort(
                    wpfmt.resolutions.begin(), wpfmt.resolutions.end(),
                    [](const WebcamResolutions& a, const WebcamResolutions& b) {
                        return a.resolution.width * a.resolution.height <
                               b.resolution.width * b.resolution.height;
                    });
            info->supported_formats.push_back(std::move(wpfmt));
        }

        if (!info->supported_formats.empty()) {
            info->preferred_format_index =
                    GetPreferredFormatIndex(info->supported_formats);
            webcams.push_back(std::move(info));
        }
    }

    return webcams;
}

}  // namespace ver
}  // namespace android

#include <os/lock.h>

@interface VerMacCamera
    : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
    CGColorSpaceRef imageColorSpace_;
    vImage_CGImageFormat imageDesiredFormat_;

    int desiredWidth_;
    int desiredHeight_;

    AVCaptureDevice* captureDevice_;
    AVCaptureVideoDataOutput* outputDevice_;
    dispatch_queue_t outputQueue_;
    AVCaptureSession* captureSession_;

    os_unfair_lock outputBufferLock_;
    BOOL outputFrameUpdated_;
    BOOL shadowBufferInUse_;
    vImage_Buffer readFrameBuffer_;
    vImage_Buffer readFrameShadowBuffer_;

    // processCameraFrame state
    vImage_Buffer inputFrame_;
    vImage_Buffer rotatedBuffer_;
    vImage_Buffer outputFrame_;
    void* scaleTempBuffer_;
}

- (instancetype)init;
- (void)stopCapture;
- (void)dealloc;
- (int)startCapturing:(const char*)device_name
                width:(int)width
               height:(int)height;
- (int)readFrameWithUpdater:
        (std::function<absl::Status(const android::ver::RawImageBufferView*)>&)
                updater;

@end

@implementation VerMacCamera

static void swapImages(vImage_Buffer* lhs, vImage_Buffer* rhs) {
    vImage_Buffer tmp = *lhs;
    *lhs = *rhs;
    *rhs = tmp;
}

static vImage_Buffer shallowCropToAspectRatio(const vImage_Buffer* src,
                                              const int desiredWidth,
                                              const int desiredHeight) {
    const float frameWidth = src->width;
    const float frameHeight = src->height;
    const float currAspect = frameWidth / frameHeight;
    const float desiredAspect = ((float)desiredWidth) / ((float)desiredHeight);
    int cropX0, cropY0, cropW, cropH;

    if (desiredAspect < currAspect) {
        cropW = (desiredAspect / currAspect) * frameWidth;
        cropH = frameHeight;
        cropX0 = (frameWidth - cropW) / 2;
        cropY0 = 0;
    } else {
        cropW = frameWidth;
        cropH = (currAspect / desiredAspect) * frameHeight;
        cropX0 = 0;
        cropY0 = (frameHeight - cropH) / 2;
    }

    vImage_Buffer cropped = *src;
    cropped.data =
            (uint8_t*)cropped.data + (cropY0 * cropped.rowBytes) + cropX0 * 4;
    cropped.width = cropW;
    cropped.height = cropH;

    return cropped;
}

- (instancetype)init {
    if (!(self = [super init])) {
        return nil;
    }

    imageColorSpace_ = NULL;
    desiredWidth_ = 0;
    desiredHeight_ = 0;
    captureDevice_ = NULL;
    outputDevice_ = NULL;
    outputQueue_ = NULL;
    captureSession_ = NULL;

    outputBufferLock_ = OS_UNFAIR_LOCK_INIT;
    outputFrameUpdated_ = NO;
    shadowBufferInUse_ = NO;

    return self;
}

- (int)startCapturing:(const char*)device_name
                width:(int)width
               height:(int)height {
    AVAuthorizationStatus status =
            [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (status == AVAuthorizationStatusDenied ||
        status == AVAuthorizationStatusRestricted) {
        derror("Camera access denied or restricted.");
        return -1;
    }

    desiredWidth_ = width;
    desiredHeight_ = height;

    AVCaptureDevice* captureDevice;
    if (device_name == NULL || *device_name == '\0') {
        captureDevice =
                [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    } else {
        NSString* deviceName = [NSString stringWithFormat:@"%s", device_name];
        captureDevice = [AVCaptureDevice deviceWithUniqueID:deviceName];
    }

    if (!captureDevice) {
        derror("There are no available video devices found.");
        return -1;
    }

    AVCaptureSession* captureSession = [AVCaptureSession new];
    NSError* videoDeviceError;
    AVCaptureDeviceInput* input_device =
            [[AVCaptureDeviceInput alloc] initWithDevice:captureDevice
                                                   error:&videoDeviceError];
    if ([captureSession canAddInput:input_device]) {
        [captureSession addInput:input_device];
        [input_device release];
    } else {
        [input_device release];
        [captureSession release];
        derror("cannot add camera capture input device (%s)",
               [[videoDeviceError localizedDescription] UTF8String]);
        return -1;
    }

    AVCaptureVideoDataOutput* outputDevice = [AVCaptureVideoDataOutput new];
    if ([captureSession canAddOutput:outputDevice]) {
        [captureSession addOutput:outputDevice];
    } else {
        [outputDevice release];
        [captureSession release];
        derror("could not add video out");
        return -1;
    }

    outputQueue_ = dispatch_queue_create(
            "com.google.goldfish.mac.camera.capture", DISPATCH_QUEUE_SERIAL);
    outputDevice.videoSettings = @{
        (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32ARGB),
    };
    [outputDevice setSampleBufferDelegate:self queue:outputQueue_];

    imageColorSpace_ = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    imageDesiredFormat_ = (vImage_CGImageFormat){
            .bitsPerComponent = 8,
            .bitsPerPixel = 32,
            .colorSpace = imageColorSpace_,
            .bitmapInfo = (CGBitmapInfo)(kCGImageByteOrder32Big |
                                         kCGImageAlphaNoneSkipLast),
            .version = 0,
            .decode = nil,
            .renderingIntent = kCGRenderingIntentDefault};

    captureDevice_ = [captureDevice retain];
    outputDevice_ = outputDevice;
    captureSession_ = captureSession;

    [captureSession startRunning];
    return 0;
}

- (void)stopCapture {
    if (captureSession_) {
        [captureSession_ stopRunning];
        for (AVCaptureInput* input in captureSession_.inputs) {
            [captureSession_ removeInput:input];
        }
        for (AVCaptureOutput* output in captureSession_.outputs) {
            [captureSession_ removeOutput:output];
        }
        [outputDevice_ setSampleBufferDelegate:nil queue:nil];
        [captureSession_ release];
        captureSession_ = NULL;
        [captureDevice_ release];
        captureDevice_ = NULL;
        [outputDevice_ release];
        outputDevice_ = NULL;
        if (outputQueue_) {
            [outputQueue_ release];
            outputQueue_ = NULL;
        }
        if (imageColorSpace_) {
            CGColorSpaceRelease(imageColorSpace_);
            imageColorSpace_ = NULL;
        }
    }
}

- (void)dealloc {
    [self stopCapture];
    free(readFrameBuffer_.data);
    free(readFrameShadowBuffer_.data);
    free(inputFrame_.data);
    free(rotatedBuffer_.data);
    free(outputFrame_.data);
    free(scaleTempBuffer_);
    [super dealloc];
}

- (void)processCameraFrame:(CMSampleBufferRef)inputFrameSample {
    CVImageBufferRef srcFrame = CMSampleBufferGetImageBuffer(inputFrameSample);
    vImageCVImageFormatRef srcFormat =
            vImageCVImageFormat_CreateWithCVPixelBuffer(srcFrame);
    vImageCVImageFormat_SetColorSpace(srcFormat, imageColorSpace_);

    vImagePixelCount srcWidth = CVPixelBufferGetWidth(srcFrame);
    vImagePixelCount srcHeight = CVPixelBufferGetHeight(srcFrame);

    if (inputFrame_.width != srcWidth || inputFrame_.height != srcHeight) {
        free(inputFrame_.data);
        inputFrame_.data = NULL;
        inputFrame_.width = 0;
        inputFrame_.height = 0;
    }

    vImage_Error error = vImageBuffer_InitWithCVPixelBuffer(
            &inputFrame_, &imageDesiredFormat_, srcFrame, srcFormat, nil,
            inputFrame_.data ? kvImageNoAllocate : kvImageNoFlags);
    vImageCVImageFormat_Release(srcFormat);

    if (error != kvImageNoError) {
        return;
    }

    // Restore rotation logic from camera-capture-mac.m
    // TODO: Support dynamic sensor orientation. Hardcoding 90 for now as in old
    // code's callback.
    const AndroidCoarseOrientation orientation =
            ANDROID_COARSE_REVERSE_LANDSCAPE;  // 90 deg
    const int rotation =
            (5 - orientation) % 4;  // Assuming forward camera for now.

    vImagePixelCount rotatedWidth, rotatedHeight;
    if (orientation == ANDROID_COARSE_PORTRAIT ||
        orientation == ANDROID_COARSE_REVERSE_PORTRAIT) {
        rotatedWidth = inputFrame_.height;
        rotatedHeight = inputFrame_.width;
    } else {
        rotatedWidth = inputFrame_.width;
        rotatedHeight = inputFrame_.height;
    }

    if (rotatedBuffer_.width != rotatedWidth ||
        rotatedBuffer_.height != rotatedHeight) {
        free(rotatedBuffer_.data);
        rotatedBuffer_.data = NULL;
        rotatedBuffer_.height = 0;
    }
    if (!rotatedBuffer_.data) {
        vImageBuffer_Init(&rotatedBuffer_, rotatedHeight, rotatedWidth, 32,
                          kvImageNoFlags);
    }

    const Pixel_8888 backColor = {0, 0, 0, 0};
    vImageRotate90_ARGB8888(&inputFrame_, &rotatedBuffer_, rotation, backColor,
                            kvImageNoFlags);

    vImage_Buffer cropped = shallowCropToAspectRatio(
            &rotatedBuffer_, desiredWidth_, desiredHeight_);

    if (desiredWidth_ != outputFrame_.width ||
        desiredHeight_ != outputFrame_.height) {
        free(outputFrame_.data);
        outputFrame_.data = NULL;
        outputFrame_.height = 0;
        free(scaleTempBuffer_);
        scaleTempBuffer_ = NULL;
    }

    if (!outputFrame_.data) {
        vImageBuffer_Init(&outputFrame_, desiredHeight_, desiredWidth_, 32,
                          kvImageNoFlags);
    }

    if (!scaleTempBuffer_) {
        scaleTempBuffer_ = malloc(vImageScale_ARGB8888(
                &cropped, &outputFrame_, NULL, kvImageGetTempBufferSize));
    }
    vImageScale_ARGB8888(&cropped, &outputFrame_, scaleTempBuffer_,
                         kvImageNoFlags);

    os_unfair_lock_lock(&outputBufferLock_);
    swapImages(&readFrameBuffer_, &outputFrame_);
    outputFrameUpdated_ = YES;
    os_unfair_lock_unlock(&outputBufferLock_);
}

- (void)captureOutput:(AVCaptureOutput*)captureOutput
        didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
               fromConnection:(AVCaptureConnection*)connection {
    CFRetain(sampleBuffer);
    dispatch_async(outputQueue_, ^(void) {
      [self processCameraFrame:sampleBuffer];
      CFRelease(sampleBuffer);
    });
}

- (int)readFrameWithUpdater:
        (std::function<absl::Status(const android::ver::RawImageBufferView*)>&)
                updater {
    os_unfair_lock_lock(&outputBufferLock_);

    if (shadowBufferInUse_) {
        os_unfair_lock_unlock(&outputBufferLock_);
        return 1;  // EAGAIN
    }

    if (!outputFrameUpdated_) {
        os_unfair_lock_unlock(&outputBufferLock_);
        return 1;  // EAGAIN
    }

    swapImages(&readFrameBuffer_, &readFrameShadowBuffer_);
    outputFrameUpdated_ = NO;

    BOOL hasData = (readFrameShadowBuffer_.data != NULL);
    if (hasData) {
        shadowBufferInUse_ = YES;
    }

    os_unfair_lock_unlock(&outputBufferLock_);

    if (hasData) {
        android::ver::RawImageBufferViewFourCC view;
        view.buffer = (const uint8_t*)readFrameShadowBuffer_.data;
        view.buffer_size =
                readFrameShadowBuffer_.rowBytes * readFrameShadowBuffer_.height;
        view.pixel_format = V4L2_PIX_FMT_RGB32;
        view.width = readFrameShadowBuffer_.width;
        view.height = readFrameShadowBuffer_.height;

        auto ver_view = RawImageBufferViewFourCCBridge(&view);
        absl::Status status = updater(&ver_view);

        os_unfair_lock_lock(&outputBufferLock_);
        shadowBufferInUse_ = NO;
        os_unfair_lock_unlock(&outputBufferLock_);

        return status.ok() ? 0 : -1;
    } else {
        return 1;  // EAGAIN
    }
}

@end

namespace android {
namespace ver {

class MacImpl : public WebcamSource::Impl {
public:
    explicit MacImpl(std::shared_ptr<const WebcamSource::WebcamInfo> info)
        : webcam_info_(std::move(info)) {
        camera_ = [[VerMacCamera alloc] init];
    }
    ~MacImpl() override { [camera_ release]; }

    int StartLocked(uint32_t pixel_format,
                    int frame_width,
                    int frame_height) override {
        return [camera_ startCapturing:webcam_info_->os_name.c_str()
                                 width:frame_width
                                height:frame_height];
    }

    int StopLocked() override {
        [camera_ stopCapture];
        return 0;
    }

    absl::StatusOr<bool> FetchNextFrame(
            std::function<absl::Status(const RawImageBufferView*)> new_frame_cb)
            override {
        int res = [camera_ readFrameWithUpdater:new_frame_cb];
        if (res == 0) {
            return true;
        } else if (res == 1) {
            return false;
        } else {
            return absl::InternalError("Failed to read frame");
        }
    }

private:
    std::shared_ptr<const WebcamSource::WebcamInfo> webcam_info_;
    VerMacCamera* camera_;
};

std::unique_ptr<WebcamSource::Impl> CreatePlatformWebcamImpl(
        std::shared_ptr<const WebcamSource::WebcamInfo> info) {
    return std::make_unique<MacImpl>(std::move(info));
}

}  // namespace ver
}  // namespace android
