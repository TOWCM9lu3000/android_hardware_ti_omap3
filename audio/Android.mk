# Copyright (C) 2011 Texas Instruments
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

ifneq (,$(findstring omap3evm, $(TARGET_PRODUCT)))
    LOCAL_MODULE := audio.primary.omap3evm
else ifneq (,$(findstring beagleboard, $(TARGET_PRODUCT)))
    LOCAL_MODULE := audio.primary.beagleboard
else ifneq (,$(findstring flashboard, $(TARGET_PRODUCT)))
    LOCAL_MODULE := audio.primary.flashboard
else ifneq (,$(findstring am335xevm, $(TARGET_PRODUCT)))
    LOCAL_MODULE := audio.primary.am335xevm
    LOCAL_CFLAGS += -DAM335XEVM
else
    LOCAL_MODULE := audio.primary.generic
endif

#ifeq ($(strip $(BOARD_USES_TI_OMAP_MODEM_AUDIO)),true)
#	LOCAL_CFLAGS := -DUSE_RIL
#endif

LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_SRC_FILES := audio_hw.c

#ifeq ($(strip $(BOARD_USES_TI_OMAP_MODEM_AUDIO)),true)
#	LOCAL_SRC_FILES += ril_interface.c
#endif
LOCAL_C_INCLUDES += \
	external/tinyalsa/include \
	system/media/audio_utils/include \
	system/media/audio_effects/include
LOCAL_SHARED_LIBRARIES := liblog libcutils libtinyalsa libaudioutils libdl
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)
