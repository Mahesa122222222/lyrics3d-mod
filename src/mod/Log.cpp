#include "Log.h"

#include <cstdarg>
#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace lyrics::log {

static Sink gSink = nullptr;

void setSink(Sink sink) { gSink = sink; }

void write(int level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

#if defined(__ANDROID__)
    int prio = level == 0 ? ANDROID_LOG_INFO : (level == 1 ? ANDROID_LOG_WARN : ANDROID_LOG_ERROR);
    __android_log_write(prio, "LyricsMod", buf);
#else
    std::fprintf(stderr, "[LyricsMod:%d] %s\n", level, buf);
#endif

    if (gSink) gSink(level, buf);
}

} // namespace lyrics::log
