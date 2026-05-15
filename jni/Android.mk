LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := CoreTurboScheduler
LOCAL_SRC_FILES := ../src/main.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../include
LOCAL_CPPFLAGS := -std=c++23 -O3 -flto -ffast-math -funroll-loops \
    -finline-functions -fomit-frame-pointer -Wall -Wextra -Wshadow \
    -fno-rtti -fno-threadsafe-statics -fvisibility=hidden \
    -ffunction-sections -fdata-sections
LOCAL_LDLIBS := -llog
LOCAL_STATIC_LIBRARIES := c++_static

include $(BUILD_EXECUTABLE)
