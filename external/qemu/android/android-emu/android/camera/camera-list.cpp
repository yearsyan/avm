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

#include "android/camera/camera-list.h"

#ifdef AEMU_CORE_ONLY

#include <stdio.h>

void android_camera_list_webcams(bool verbose) {
    (void)verbose;
}

#else

#include "ver/virtual_environment_renderer.h"

#include <stdio.h>
#include <string>

void android_camera_list_webcams(bool verbose) {
    uint32_t count = ver_get_webcam_count();
    if (count == 0) {
        return;
    }

    printf("List of web cameras connected to the computer:\n");
    for (uint32_t i = 0; i < count; ++i) {
        VerWebcamHandle cam = ver_get_webcam_info(i);
        if (!cam) {
            continue;
        }

        int preferred_format_index =
                ver_webcam_info_get_preferred_format_index(cam);
        std::string label = "webcam" + std::to_string(i);

        if (preferred_format_index != -1) {
            uint32_t pixel_format = ver_webcam_info_get_pixel_format_fourcc(
                    cam, static_cast<uint32_t>(preferred_format_index));
            char format_str[5];
            format_str[0] = pixel_format & 0xff;
            format_str[1] = (pixel_format >> 8) & 0xff;
            format_str[2] = (pixel_format >> 16) & 0xff;
            format_str[3] = (pixel_format >> 24) & 0xff;
            format_str[4] = '\0';

            printf(" Camera '%s' can be specified by label as '%s' or by id as '%s' and will use pixel format '%s'\n",
                   ver_webcam_info_get_user_facing_name(cam), label.c_str(),
                   ver_webcam_info_get_id(cam), format_str);
        } else {
            printf(" Camera '%s' is unsupported \n",
                   ver_webcam_info_get_user_facing_name(cam));
        }

        if (verbose) {
            printf(" This camera reports support for the following formats and resolutions:\n");
            uint32_t format_count = ver_webcam_info_get_format_count(cam);
            for (uint32_t f = 0; f < format_count; ++f) {
                uint32_t pixel_format =
                        ver_webcam_info_get_pixel_format_fourcc(cam, f);
                char format_str[5];
                format_str[0] = pixel_format & 0xff;
                format_str[1] = (pixel_format >> 8) & 0xff;
                format_str[2] = (pixel_format >> 16) & 0xff;
                format_str[3] = (pixel_format >> 24) & 0xff;
                format_str[4] = '\0';

                printf("   %s: ", format_str);
                uint32_t res_count =
                        ver_webcam_info_get_format_resolution_count(cam, f);
                for (uint32_t r = 0; r < res_count; ++r) {
                    int w, h;
                    if (ver_webcam_info_get_format_resolution(cam, f, r, &w,
                                                              &h)) {
                        printf(" %dx%d", w, h);
                    }
                }
                printf("\n");
            }
        }
        ver_free_webcam_info(cam);
    }
}

#endif
