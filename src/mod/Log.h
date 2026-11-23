#pragma once
// Small logger. Default: logcat (Android) / stderr (host test).
// LyricsMod replaces the sink so logs also go to Levi Launcher's logger.

namespace lyrics::log {

using Sink = void (*)(int level, const char* message);

void setSink(Sink sink);

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void write(int level, const char* fmt, ...);

} // namespace lyrics::log

#define LYR_LOGI(...) ::lyrics::log::write(0, __VA_ARGS__)
#define LYR_LOGW(...) ::lyrics::log::write(1, __VA_ARGS__)
#define LYR_LOGE(...) ::lyrics::log::write(2, __VA_ARGS__)
