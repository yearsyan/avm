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

#include "webcam_source.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/types/optional.h"

#include "../fourcc_utils.h"
#include "aemu/base/files/ScopedFd.h"
// For V4L2 constants
#include "android/camera/camera-common.h"
#include "android/utils/debug.h"
#include "android/utils/eintr_wrapper.h"
#include "android/utils/system.h"

using android::base::ScopedFd;

namespace android {
namespace ver {

namespace {

int _xioctl(int fd, int request, void* arg) {
    return HANDLE_EINTR(ioctl(fd, request, arg));
}

std::vector<WebcamSource::WebcamResolutions> _enumerate_resolutions(
        int fd,
        uint32_t pixel_format) {
    std::vector<WebcamSource::WebcamResolutions> resolutions;
    struct v4l2_frmsizeenum frmsize;
    memset(&frmsize, 0, sizeof(frmsize));
    frmsize.pixel_format = pixel_format;

    for (int i = 0;; ++i) {
        frmsize.index = i;
        if (_xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) < 0) {
            break;
        }

        switch (frmsize.type) {
            case V4L2_FRMSIZE_TYPE_DISCRETE: {
                resolutions.push_back(
                        {frmsize.discrete.width, frmsize.discrete.height, {}});
                break;
            }
            case V4L2_FRMSIZE_TYPE_STEPWISE: {
                /* Limit this to a maximum of 25 entries (5x5) by stepping
                 * multiples of step size if necessary. We ensure both min and
                 * max are always included. */
                uint32_t step_width = std::max(frmsize.stepwise.step_width, 1u);
                uint32_t step_height =
                        std::max(frmsize.stepwise.step_height, 1u);

                uint32_t total_steps_w = (frmsize.stepwise.max_width -
                                          frmsize.stepwise.min_width) /
                                         step_width;
                uint32_t total_steps_h = (frmsize.stepwise.max_height -
                                          frmsize.stepwise.min_height) /
                                         step_height;

                uint32_t stride_width =
                        step_width * std::max((total_steps_w + 3) / 4, 1u);
                uint32_t stride_height =
                        step_height * std::max((total_steps_h + 3) / 4, 1u);

                uint32_t w = frmsize.stepwise.min_width;
                do {
                    uint32_t h = frmsize.stepwise.min_height;
                    do {
                        resolutions.push_back({{w, h}, {}});
                        if (h == frmsize.stepwise.max_height) {
                            break;
                        }
                        h = std::min(h + stride_height,
                                     frmsize.stepwise.max_height);
                    } while (true);
                    if (w == frmsize.stepwise.max_width) {
                        break;
                    }
                    w = std::min(w + stride_width, frmsize.stepwise.max_width);
                } while (true);
                break;
            }
            case V4L2_FRMSIZE_TYPE_CONTINUOUS: {
                /* Special stepwise case, when steps are set to 1. We still need
                 * to flatten this for the guest, but the array may be too big.
                 * Fortunately, we don't need to be fancy, so three sizes would
                 * be sufficient here: min, max, and one in the middle.
                 */
                uint32_t min_w = frmsize.stepwise.min_width;
                uint32_t min_h = frmsize.stepwise.min_height;
                uint32_t max_w = frmsize.stepwise.max_width;
                uint32_t max_h = frmsize.stepwise.max_height;

                resolutions.push_back({{min_w, min_h}, {}});
                if (max_w != min_w || max_h != min_h) {
                    resolutions.push_back({{min_w + (max_w - min_w) / 2,
                                            min_h + (max_h - min_h) / 2},
                                           {}});
                    resolutions.push_back({{max_w, max_h}, {}});
                }
            }
            default: {
                LOG(WARNING) << "Unhandled V4L2_FRMSIZE_TYPE? " << frmsize.type;
                break;
            }
        }
    }
    return resolutions;
}

}  // namespace

std::vector<std::shared_ptr<WebcamSource::WebcamInfo>>
WebcamSource::EnumerateWebcams() {
    std::vector<std::shared_ptr<WebcamSource::WebcamInfo>> webcams;
    std::map<ino_t, std::string> by_id_map;

    // Build up a map of stable aliases for /dev/video nodes if available
    std::string by_id_dir_path = "/dev/v4l/by-id";
    DIR* dir = opendir(by_id_dir_path.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            std::string full_path = by_id_dir_path + "/" + entry->d_name;
            struct stat st;
            if (stat(full_path.c_str(), &st) == 0) {
                by_id_map[st.st_ino] = full_path;
            }
        }
        closedir(dir);
    }

    for (int i = 0; i < 64; ++i) {
        std::string dev_path = "/dev/video" + std::to_string(i);
        ScopedFd fd(HANDLE_EINTR(open(dev_path.c_str(), O_RDWR | O_NONBLOCK)));
        if (!fd.valid()) {
            if (errno != ENOENT) {
                derror("Failed to open camera device %s: %s", dev_path.c_str(),
                       strerror(errno));
            }
            continue;
        }

        struct v4l2_capability caps = {};
        if (_xioctl(fd.get(), VIDIOC_QUERYCAP, &caps) < 0) {
            continue;
        }

        if (!(caps.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            continue;
        }

        std::shared_ptr<WebcamInfo> info = std::make_shared<WebcamInfo>();
        const char* card_name = reinterpret_cast<const char*>(caps.card);
        const char* card_name_end =
                std::find(card_name, card_name + sizeof(caps.card), '\0');
        info->friendly_name = std::string(card_name, card_name_end);
        info->os_name = dev_path;

        struct stat st;

        if (fstat(fd.get(), &st) == 0) {
            auto it = by_id_map.find(st.st_ino);
            if (it != by_id_map.end()) {
                info->os_alias = it->second;
            }
        }
        if (info->os_alias.empty()) {
            info->os_alias = dev_path;
        }
        info->preferred_format_index = -1;

        struct v4l2_fmtdesc fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        for (int j = 0;; ++j) {
            fmt.index = j;
            if (_xioctl(fd.get(), VIDIOC_ENUM_FMT, &fmt) < 0) {
                break;
            }

            WebcamPixelFormat wpfmt;
            wpfmt.pixel_format = fmt.pixelformat;
            wpfmt.compressed = fmt.flags & V4L2_FMT_FLAG_COMPRESSED;
            wpfmt.resolutions =
                    _enumerate_resolutions(fd.get(), fmt.pixelformat);
            /* Sort resolutions by pixel count */
            std::sort(
                    wpfmt.resolutions.begin(), wpfmt.resolutions.end(),
                    [](const WebcamResolutions& a, const WebcamResolutions& b) {
                        return a.resolution.width * a.resolution.height <
                               b.resolution.width * b.resolution.height;
                    });
            if (!wpfmt.resolutions.empty()) {
                info->supported_formats.push_back(std::move(wpfmt));
            }
        }

        if (!info->supported_formats.empty()) {
            info->preferred_format_index =
                    GetPreferredFormatIndex(info->supported_formats);
            webcams.push_back(std::move(info));
        }
    }

    return webcams;
}

enum class CameraIoType {
    CAMERA_IO_MEMMAP,
    CAMERA_IO_USERPTR,
    CAMERA_IO_DIRECT
};

class LinuxImpl : public WebcamSource::Impl {
public:
    explicit LinuxImpl(std::shared_ptr<const WebcamSource::WebcamInfo> info)
        : webcam_info_(std::move(info)) {}
    ~LinuxImpl() override { StopLocked(); }

    int StartLocked(uint32_t pixel_format,
                    int frame_width,
                    int frame_height) override {
        if (pixel_format != V4L2_PIX_FMT_RGB32) {
            derror("Scene system currently only supports V4L2_PIX_FMT_RGB32");
            return -1;
        }
        if (fd_.valid()) {
            return 0;  // Already started
        }

        fd_ = ScopedFd(HANDLE_EINTR(
                open(webcam_info_->os_name.c_str(), O_RDWR | O_NONBLOCK)));
        if (!fd_.valid()) {
            derror("Cannot open camera device '%s': %s",
                   webcam_info_->friendly_name.c_str(), strerror(errno));
            return -1;
        }

        struct v4l2_capability caps;
        if (_xioctl(fd_.get(), VIDIOC_QUERYCAP, &caps) < 0) {
            derror("Unable to query capabilities for camera device '%s'",
                   webcam_info_->friendly_name.c_str());
            StopLocked();
            return -1;
        }

        if (!(caps.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            derror("Camera '%s' is not a video capture device",
                   webcam_info_->friendly_name.c_str());
            StopLocked();
            return -1;
        }

        struct v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        // Check if we directly support the requested format. Otherwise use the
        // already chosen preferred format.
        WebcamSource::WebcamPixelFormat format =
                webcam_info_->supported_formats
                        [webcam_info_->preferred_format_index];

        for (const WebcamSource::WebcamPixelFormat& supported :
             webcam_info_->supported_formats) {
            if (pixel_format == supported.pixel_format) {
                format = supported;
            }
        }
        WebcamSource::Resolution res = {frame_width, frame_height};
        res = format.FindBestMatchForResolution(res);
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = res.width;
        fmt.fmt.pix.height = res.height;
        fmt.fmt.pix.pixelformat = format.pixel_format;
        fmt.fmt.pix.field = V4L2_FIELD_ANY;

        if (_xioctl(fd_.get(), VIDIOC_S_FMT, &fmt) < 0) {
            derror("Camera '%s' does not support pixel format %s with dimensions %dx%d",
                   webcam_info_->friendly_name.c_str(),
                   FourccToString(pixel_format), res.width, res.height);
            StopLocked();
            return -1;
        }

        actual_pixel_format_ = fmt.fmt.pix;

        // Try MMAP first
        if (InitMmap() == 0) {
            io_type_ = CameraIoType::CAMERA_IO_MEMMAP;
        } else if (InitUserPtr() == 0) {
            io_type_ = CameraIoType::CAMERA_IO_USERPTR;
        } else {
            if (!(caps.capabilities & V4L2_CAP_READWRITE)) {
                derror("Don't know how to access frames on device '%s'",
                       webcam_info_->friendly_name.c_str());
                StopLocked();
                return -1;
            }
            if (InitDirect() == 0) {
                io_type_ = CameraIoType::CAMERA_IO_DIRECT;
            } else {
                StopLocked();
                return -1;
            }
        }

        if (io_type_ != CameraIoType::CAMERA_IO_DIRECT) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (_xioctl(fd_.get(), VIDIOC_STREAMON, &type) < 0) {
                derror("VIDIOC_STREAMON failed: %s", strerror(errno));
                StopLocked();
                return -1;
            }
        }

        streaming_ = true;
        requested_pixel_format_ = pixel_format;
        return 0;
    }

    int StopLocked() override {
        if (streaming_ && io_type_ != CameraIoType::CAMERA_IO_DIRECT) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            _xioctl(fd_.get(), VIDIOC_STREAMOFF, &type);
        }
        streaming_ = false;

        buffers_.clear();
        staging_buffer_ = std::vector<uint8_t>();

        if (fd_.valid()) {
            fd_.close();
        }
        return 0;
    }

    absl::StatusOr<bool> FetchNextFrame(
            std::function<absl::Status(const RawImageBufferView*)> new_frame_cb)
            override {
        if (!streaming_) {
            return absl::InternalError("Camera not streaming");
        }

        if (io_type_ == CameraIoType::CAMERA_IO_DIRECT) {
            void* buff = buffers_[0].start;
            ssize_t read_bytes = HANDLE_EINTR(
                    read(fd_.get(), buff, actual_pixel_format_.sizeimage));
            if (read_bytes < 0) {
                if (errno == EAGAIN) {
                    return false;
                }
                return absl::InternalError(std::string("Read failed: ") +
                                           strerror(errno));
            }
            if (read_bytes == 0) {
                return absl::InternalError("Read failed: EOF");
            }
            size_t total_read_bytes = static_cast<size_t>(read_bytes);

            RawImageBufferViewFourCC view = {
                    static_cast<const uint8_t*>(buff), total_read_bytes,
                    actual_pixel_format_.pixelformat,
                    actual_pixel_format_.width, actual_pixel_format_.height};
            if (view.pixel_format != requested_pixel_format_) {
                if (ConvertBufferToRGB32(view, conversion_storage_,
                                         staging_buffer_) != 0) {
                    return absl::InternalError("Format conversion failed");
                }
            }
            auto ver_view = RawImageBufferViewFourCCBridge(&view);
            absl::Status status = new_frame_cb(&ver_view);
            if (!status.ok()) {
                return status;
            }
            return true;
        } else {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = (io_type_ == CameraIoType::CAMERA_IO_MEMMAP)
                                 ? V4L2_MEMORY_MMAP
                                 : V4L2_MEMORY_USERPTR;

            if (_xioctl(fd_.get(), VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN) {
                    return false;
                }
                return absl::InternalError(
                        std::string("VIDIOC_DQBUF failed: ") + strerror(errno));
            }

            if (buf.flags & V4L2_BUF_FLAG_ERROR) {
                _xioctl(fd_.get(), VIDIOC_QBUF, &buf);  // Recycle error frame
                return absl::InvalidArgumentError(absl::StrFormat(
                        "Webcam %s reported an error with the returned buffer",
                        webcam_info_->friendly_name));
            }

            RawImageBufferViewFourCC view;
            view.buffer = static_cast<const uint8_t*>(
                    (io_type_ == CameraIoType::CAMERA_IO_MEMMAP)
                            ? buffers_[buf.index].start
                            : reinterpret_cast<void*>(buf.m.userptr));

            view.buffer_size = buf.bytesused;
            view.pixel_format = actual_pixel_format_.pixelformat;
            view.width = actual_pixel_format_.width;
            view.height = actual_pixel_format_.height;
            if (view.pixel_format != requested_pixel_format_) {
                if (ConvertBufferToRGB32(view, conversion_storage_,
                                         staging_buffer_) != 0) {
                    return absl::InternalError("Format conversion failed");
                }
            }
            auto ver_view = RawImageBufferViewFourCCBridge(&view);
            absl::Status status = new_frame_cb(&ver_view);

            if (_xioctl(fd_.get(), VIDIOC_QBUF, &buf) < 0) {
                dwarning("VIDIOC_QBUF failed: %s", strerror(errno));
            }

            if (!status.ok()) {
                return status;
            }
            return true;
        }
    }

    struct FrameBuffer {
        void* start = nullptr;
        size_t length = 0;
        std::function<void(void*, size_t)> deleter;

        FrameBuffer() = default;
        FrameBuffer(void* s, size_t l, std::function<void(void*, size_t)> d)
            : start(s), length(l), deleter(std::move(d)) {}
        ~FrameBuffer() {
            if (start && deleter) {
                deleter(start, length);
            }
        }
        FrameBuffer(const FrameBuffer&) = delete;
        FrameBuffer& operator=(const FrameBuffer&) = delete;
        FrameBuffer(FrameBuffer&& other) noexcept
            : start(other.start),
              length(other.length),
              deleter(std::move(other.deleter)) {
            other.start = nullptr;
        }
        FrameBuffer& operator=(FrameBuffer&& other) noexcept {
            if (this != &other) {
                if (start && deleter) {
                    deleter(start, length);
                }
                start = other.start;
                length = other.length;
                deleter = std::move(other.deleter);
                other.start = nullptr;
            }
            return *this;
        }
    };

    int InitMmap() {
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (_xioctl(fd_.get(), VIDIOC_REQBUFS, &req) < 0) {
            return -1;
        }

        for (uint32_t i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (_xioctl(fd_.get(), VIDIOC_QUERYBUF, &buf) < 0) {
                return -1;
            }
            void* start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd_.get(), buf.m.offset);
            if (start == MAP_FAILED) {
                return -1;
            }
            buffers_.emplace_back(start, buf.length,
                                  [](void* s, size_t l) { munmap(s, l); });
            if (_xioctl(fd_.get(), VIDIOC_QBUF, &buf) < 0) {
                return -1;
            }
        }
        return 0;
    }

    int InitUserPtr() {
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_USERPTR;
        if (_xioctl(fd_.get(), VIDIOC_REQBUFS, &req) < 0) {
            return -1;
        }
        for (uint32_t i = 0; i < req.count; ++i) {
            void* start = malloc(actual_pixel_format_.sizeimage);
            if (!start) {
                return -1;
            }
            buffers_.emplace_back(start, actual_pixel_format_.sizeimage,
                                  [](void* s, size_t l) { free(s); });
            struct v4l2_buffer buf;
            memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_USERPTR;
            buf.index = i;
            buf.m.userptr = reinterpret_cast<unsigned long>(start);
            buf.length = actual_pixel_format_.sizeimage;
            if (_xioctl(fd_.get(), VIDIOC_QBUF, &buf) < 0) {
                return -1;
            }
        }
        return 0;
    }

    int InitDirect() {
        void* start = malloc(actual_pixel_format_.sizeimage);
        if (!start) {
            return -1;
        }
        buffers_.emplace_back(start, actual_pixel_format_.sizeimage,
                              [](void* s, size_t l) { free(s); });
        return 0;
    }

    std::shared_ptr<const WebcamSource::WebcamInfo> webcam_info_;
    ScopedFd fd_;
    CameraIoType io_type_;
    std::vector<FrameBuffer> buffers_;
    struct v4l2_pix_format actual_pixel_format_;
    uint32_t requested_pixel_format_ = 0;
    bool streaming_ = false;

    // Buffers to convert to RGB32
    std::vector<uint8_t> conversion_storage_;
    std::vector<uint8_t> staging_buffer_;
};

std::unique_ptr<WebcamSource::Impl> CreatePlatformWebcamImpl(
        std::shared_ptr<const WebcamSource::WebcamInfo> info) {
    return std::make_unique<LinuxImpl>(std::move(info));
}

}  // namespace ver
}  // namespace android
