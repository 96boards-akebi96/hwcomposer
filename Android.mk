# Copyright (C) 2008 The Android Open Source Project
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


LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_PRELINK_MODULE := false

LOCAL_CFLAGS :=

GRALLOC_DIR := frameworks/native/include/media/openmax
GRALLOC_DIR += vendor/arm/gralloc/driver/product/android/gralloc/src
GRALLOC_DIR += vendor/socionext/sc1401aj1/OpenMAX/libomxil-prox/include
# Additional CFLAGS for "gralloc_priv.h"
LOCAL_CFLAGS += -DMALI_ION=1
LOCAL_CFLAGS += -DMALI_AFBC_GRALLOC=1
LOCAL_CFLAGS += -DENABLE_WAIT_VEC_UPDATE=1 -DWAIT_VEC_UPDATE_TIME=6000

# HAL module implemenation stored in
# hw/<OVERLAY_HARDWARE_MODULE_ID>.<ro.product.board>.so

LOCAL_C_INCLUDES += $(GRALLOC_DIR)
LOCAL_C_INCLUDES += $(TOP)/system/core/libion

LOCAL_SHARED_LIBRARIES += libbase libnativewindow
LOCAL_SHARED_LIBRARIES += liblog libcutils libutils libhardware libbinder
LOCAL_SHARED_LIBRARIES += libomxil-bellagio
LOCAL_SHARED_LIBRARIES += libomxprox.$(TARGET_BOOTLOADER_BOARD_NAME)
LOCAL_SHARED_LIBRARIES += \
    libOmxDisplayManager libOmxDisplayController \
    libDisplayManagerClient

LOCAL_SHARED_LIBRARIES += \
    libEGL \
    libGLESv1_CM \
    libGLESv2 \
    libui

LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_MODULE := hwcomposer.$(TARGET_BOOTLOADER_BOARD_NAME)
LOCAL_CFLAGS += -DLOG_TAG=\"hwcomposer\"
LOCAL_SRC_FILES := hwcomposer_omx.cpp

LOCAL_MODULE_TAGS := optional
ifeq ($(BOARD_VNDK_VERSION),current)
LOCAL_VENDOR_MODULE := true
endif
include $(BUILD_SHARED_LIBRARY)

