LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE     := taplus_intl_fix
LOCAL_SRC_FILES  := main.cpp
LOCAL_CPPFLAGS   := -std=c++17 -fno-exceptions -fno-rtti -O2
LOCAL_LDLIBS     := -llog
include $(BUILD_SHARED_LIBRARY)
