#pragma once
// Host stub: Android logcat calls become no-ops in tests.
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
#define __android_log_print(...) ((void)0)
