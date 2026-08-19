LOCAL_PATH := $(call my-dir)

# LSPosed/Dobby fork：LSPlant InitInfo 需要的 inline hooker 實作。
include $(CLEAR_VARS)
LOCAL_MODULE := dobby
LOCAL_SRC_FILES := vendor/dobby/lib/libdobby.a
include $(PREBUILT_STATIC_LIBRARY)

XZ_DIR := vendor/xz/src/linux/lib/xz

include $(CLEAR_VARS)
LOCAL_MODULE     := taplus_intl_fix
LOCAL_SRC_FILES  := main.cpp dualwake.cpp art_resolver.cpp \
    $(XZ_DIR)/xz_crc32.c \
    $(XZ_DIR)/xz_crc64.c \
    $(XZ_DIR)/xz_dec_stream.c \
    $(XZ_DIR)/xz_dec_lzma2.c
LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/vendor/dobby/include \
    $(LOCAL_PATH)/vendor/lsplant/include \
    $(LOCAL_PATH)/vendor/xz/src/linux/include/linux \
    $(LOCAL_PATH)/vendor/xz/src/userspace \
    $(LOCAL_PATH)/$(XZ_DIR)
LOCAL_CPPFLAGS   := -std=c++17 -fno-exceptions -fno-rtti -O2 -fvisibility=hidden
LOCAL_CFLAGS     := -DXZ_USE_CRC64 -fvisibility=hidden
LOCAL_STATIC_LIBRARIES := dobby
# 官方 Zygisk 規範要求模組不把 STL/第三方符號洩漏到宿主進程的動態符號表；
# entry point 在 zygisk.hpp 內以 visibility("default") 保留，其餘全部隱藏。
LOCAL_LDFLAGS    := -Wl,--exclude-libs,ALL
LOCAL_LDLIBS     := -llog -ldl
include $(BUILD_SHARED_LIBRARY)
