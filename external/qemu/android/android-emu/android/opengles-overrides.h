/* Copyright (C) 2026 The Android Open Source Project
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

#include "host-common/opengles.h"

/**
 * @struct AndroidOpenglesFuncs
 * @brief A dispatch table for OpenGLES emulation functions.
 *
 * This struct allows different backends (like the standard gfxstream backend
 * or a minimal fishtank backend) to provide their own implementations of
 * OpenGLES-related operations.
 */
struct AndroidOpenglesFuncs {
    int (*prepareOpenglesEmulation)(void);
    int (*setOpenglesEmulation)(void* renderLib, void* eglDispatch, void* glesv2Dispatch);
    int (*initOpenglesEmulation)(void);
    int (*startOpenglesRenderer)(int width, int height,
                                         bool isPhone, int guestApiLevel,
                                         const QAndroidVmOperations *vm_operations,
                                         const QAndroidEmulatorWindowAgent *window_agent,
                                         const QAndroidMultiDisplayAgent *multi_display_agent,
                                         const void* gfxstreamFeatures,
                                         int* glesMajorVersion_out,
                                         int* glesMinorVersion_out);
    bool (*asyncReadbackSupported)(void);
    void (*setPostCallback)(OnPostFunc onPost,
                                    void* onPostContext,
                                    bool useBgraReadback,
                                    uint32_t displayId);
    ReadPixelsFunc (*getReadPixelsFunc)(void);
    FlushReadPixelPipeline (*getFlushReadPixelPipeline)(void);
    void (*getOpenglesHardwareStrings)(char** vendor,
                                               char** renderer,
                                               char** version);
    void (*getOpenglesVersion)(int* maj, int* min);
    int (*showOpenglesWindow)(void* window,
                                      int wx,
                                      int wy,
                                      int ww,
                                      int wh,
                                      int fbw,
                                      int fbh,
                                      float dpr,
                                      float rotation,
                                      bool deleteExisting,
                                      bool hideWindow);
    int (*hideOpenglesWindow)(void);
    void (*setOpenglesTranslation)(float px, float py);
    void (*setOpenglesScreenMask)(int width, int height, const uint8_t* rgbaData);
    void (*setOpenglesScreenBackground)(int width, int height,
                                                const uint8_t* rgbaData);
    void (*setOpenglesDisplayLayout)(int screenWidth, int screenHeight,
                                             int displayPosX, int displayPosY,
                                             int displayWidth, int displayHeight);
    void (*redrawOpenglesWindow)(void);
    void (*setShouldSkipDraw)(bool skip);
    bool (*getShouldSkipDraw)(void);
    bool (*hasGuestPostedAFrame)(void);
    void (*resetGuestPostedAFrame)(void);
    void (*registerScreenshotFunc)(ScreenshotFunc f);
    bool (*screenShot)(const char* dirname, uint32_t displayId);
    void (*stopOpenglesRenderer)(bool wait);
    void (*finishOpenglesRenderer)(void);
    void (*onGuestGraphicsProcessCreate)(uint64_t puid);
    void (*cleanupProcGLObjects)(uint64_t puid);
    void (*waitForOpenglesProcessCleanup)(void);
    struct AndroidVirtioGpuOps* (*getVirtioGpuOps)(void);
    const void* (*getEGLDispatch)(void);
    const void* (*getGLESv2Dispatch)(void);
    void (*setVsyncHz)(int vsyncHz);
    void (*setOpenglesDisplayConfigs)(int configId, int w, int h,
                                              int dpiX, int dpiY);
    void (*setOpenglesDisplayActiveConfig)(int configId);
};

/**
 * @brief Sets the active OpenGLES function dispatch table.
 *
 * This function allows a backend to override the default OpenGLES behavior
 * by providing its own set of function pointers. If not called, a default
 * implementation based on the gfxstream backend will be used.
 *
 * @param funcs Pointer to the AndroidOpenglesFuncs struct containing the overrides.
 */
void android_setOpenglesFuncs(struct AndroidOpenglesFuncs* funcs);