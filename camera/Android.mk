include $(all-subdir-makefiles)

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
LOCAL_SRC_FILES:= \
	CameraHal_Module.cpp \
        V4L2Camera.cpp \
        CameraHardware.cpp \
        converter.cpp

LOCAL_C_INCLUDES += \
    $(LOCAL_PATH)/inc/ \
    frameworks/base/include/ui \
    frameworks/base/include/utils \
    frameworks/base/include/media/stagefright \
    frameworks/base/include/media/stagefright/openmax \
    external/jpeg \
    external/jhead

LOCAL_SHARED_LIBRARIES:= \
    libui \
    libbinder \
    libutils \
    libcutils \
    libcamera_client \
    libcameraservice \
    libgui \
    libjpeg \
    libexif

LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE:= camera.$(TARGET_BOARD_PLATFORM)
LOCAL_MODULE_TAGS:= optional

include $(BUILD_SHARED_LIBRARY)
