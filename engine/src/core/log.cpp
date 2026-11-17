#include "mge/log.h"
#include <cstdio>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace mge {
void logf(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
#if defined(__ANDROID__)
    static const int prio[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    __android_log_vprint(prio[static_cast<int>(level)], "MGE", fmt, args);
#else
    static const char* tag[] = {"D", "I", "W", "E"};
    std::fprintf(stderr, "[MGE/%s] ", tag[static_cast<int>(level)]);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
#endif
    va_end(args);
}
}
