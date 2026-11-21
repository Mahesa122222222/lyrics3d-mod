#pragma once
#include <EGL/egl.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

#include <pl/Config.hpp>
#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

#include "LyricsRenderer.h"
#include "Config.h"
#include "LyricsController.h"

class LyricsMod {
public:
    static LyricsMod& instance();

    LyricsMod();

    bool load();
    bool enable();
    bool disable();
    bool unload();

private:
    using SwapFn = EGLBoolean (*)(EGLDisplay, EGLSurface);
    using SwapDamageFn = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint*, EGLint);

    static EGLBoolean swapDetour(EGLDisplay dpy, EGLSurface surf);
    static EGLBoolean swapDamageDetour(EGLDisplay dpy, EGLSurface surf, EGLint* rects, EGLint n);
    static void logSink(int level, const char* message);

    void beforeSwap(EGLDisplay dpy, EGLSurface surf);
    void drawFrame(EGLDisplay dpy, EGLSurface surf);
    void reloadConfigIfChanged(double timeSec);
    std::filesystem::path configPath() const;

    ll::mod::NativeMod* mSelf = nullptr;

    std::optional<pl::config::ConfigFile<LyricsConfig>> mConfig;
    LyricsConfig mCfg;
    std::filesystem::file_time_type mCfgStamp{};
    double mNextCfgCheck = 0.0;

    pl::memory::HookHandle mSwapHook;
    pl::memory::HookHandle mSwapDamageHook;
    SwapFn mOrigSwap = nullptr;
    SwapDamageFn mOrigSwapDamage = nullptr;

    std::mutex mFrameMutex; // melindungi controller/renderer dari disable() yang berjalan bersamaan
    std::unique_ptr<lyrics::LyricsController> mController;
    std::unique_ptr<lyrics::LyricsRenderer> mRenderer;
    EGLContext mCtx = EGL_NO_CONTEXT;
    bool mBroken = false;
    bool mGlChecked = false;

    std::atomic<bool> mActive{false};
    std::chrono::steady_clock::time_point mT0 = std::chrono::steady_clock::now();
    std::mutex mLogMutex;
};
