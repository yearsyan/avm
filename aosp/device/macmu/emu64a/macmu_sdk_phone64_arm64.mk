#
# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Reuse the complete AOSP arm64 phone emulator definition. MacMu only adds a
# product-level package exclusion module; the goldfish device remains pristine.
$(call inherit-product, device/generic/goldfish/64bitonly/product/sdk_phone64_arm64.mk)

PRODUCT_SOONG_NAMESPACES += device/macmu/emu64a
PRODUCT_PACKAGES += MacMuAppExcludes

PRODUCT_BRAND := MacMu
PRODUCT_NAME := macmu_sdk_phone64_arm64
PRODUCT_DEVICE := emu64a
PRODUCT_MODEL := MacMu Android 16 Emulator
PRODUCT_MANUFACTURER := MacMu
