// Host-side mock of <android/log.h>. The test executable defines
// __android_log_print and records every message that survives the module's
// debug gate.
#pragma once

#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5

int __android_log_print(int prio, const char *tag, const char *fmt, ...);
