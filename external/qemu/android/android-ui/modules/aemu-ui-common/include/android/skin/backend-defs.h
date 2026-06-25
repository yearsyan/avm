
/* Copyright (C) 2022 The Android Open Source Project
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
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DECL_WinsysPreferredGlesBackend
#define DECL_WinsysPreferredGlesBackend
enum WinsysPreferredGlesBackend {
    WINSYS_GLESBACKEND_PREFERENCE_AUTO = 0,
    WINSYS_GLESBACKEND_PREFERENCE_ANGLE_DEPRECATED = 1,
    WINSYS_GLESBACKEND_PREFERENCE_ANGLE9_DEPRECATED = 2,
    WINSYS_GLESBACKEND_PREFERENCE_SWIFTSHADER_DEPRECATED = 3,
    WINSYS_GLESBACKEND_PREFERENCE_NATIVEGL_DEPRECATED = 4,
    WINSYS_GLESBACKEND_PREFERENCE_SOFTWARE = 5,
    WINSYS_GLESBACKEND_PREFERENCE_HARDWARE = 6,
    WINSYS_GLESBACKEND_PREFERENCE_NUM = 7,
};

enum WinsysPreferredGlesApiLevel {
    WINSYS_GLESAPILEVEL_PREFERENCE_AUTO = 0,
    WINSYS_GLESAPILEVEL_PREFERENCE_MAX = 1,
    WINSYS_GLESAPILEVEL_PREFERENCE_COMPAT = 2,
    WINSYS_GLESAPILEVEL_PREFERENCE_NUM = 3,
};
#endif

#ifndef DECL_WinsysGuestGlesDriverPreference
#define DECL_WinsysGuestGlesDriverPreference
enum WinsysGuestGlesDriverPreference {
    WINSYS_GUEST_GLES_DRIVER_PREFERENCE_AUTO = 0,
    WINSYS_GUEST_GLES_DRIVER_PREFERENCE_GUESTANGLE = 1,
    WINSYS_GUEST_GLES_DRIVER_PREFERENCE_NATIVE = 2,
};
#endif

#ifdef __cplusplus
}
#endif
