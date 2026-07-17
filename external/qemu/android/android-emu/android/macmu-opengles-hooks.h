// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdint.h>

#include "aemu/base/c_header.h"
#include "aemu/base/export.h"

#ifndef USING_ANDROID_BP
ANDROID_BEGIN_HEADER
#endif

// MacMu-private renderer hooks used by the qemu glue and MultiDisplay bridge.
// Keep these out of host-common/opengles.h so hardware/google/aemu can stay at
// its upstream submodule revision.
AEMU_EXPORT void android_notifyDisplayColorBufferChanged(uint32_t displayId,
                                                         uint32_t colorBufferHandle);
AEMU_EXPORT void android_exportDisplayFrame(uint32_t displayId);
AEMU_EXPORT void android_setDisplayExportEnabled(uint32_t displayId, int enabled);
AEMU_EXPORT void android_clearDisplayExportFrame(uint32_t displayId);
AEMU_EXPORT void android_resetDisplayExportSubscriptions(void);

#ifndef USING_ANDROID_BP
ANDROID_END_HEADER
#endif
