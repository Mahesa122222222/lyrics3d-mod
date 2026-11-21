#pragma once
#include <EGL/egl.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pl/Config.hpp>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
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
    bool isMainSurface(EGLDisplay dpy, EGLSurface surf);
    void drawFrame(EGLDisplay dpy, EGLSurface surf);
    void reloadConfigIfChanged(double timeSec);

    // --- Mod Menu (callback datang dari thread lain -> masuk antrean, diproses di thread render) ---
    enum class Cmd { Play, Pause, Toggle, Next, Prev };
    bool registerMenu();
    void unregisterMenu();
    void queueCmd(Cmd c);
    void queueSetting(std::string key, std::string value);
    void drainMenuQueue();
    void rebuildConfig(); // mCfg = mFileCfg + pengaturan dari menu
    std::filesystem::path configPath() const;

    ll::mod::NativeMod* mSelf = nullptr;

    std::optional<pl::config::ConfigFile<LyricsConfig>> mConfig;
    LyricsConfig mFileCfg; // isi config.json
    LyricsConfig mCfg;     // yang dipakai: mFileCfg + pengaturan dari menu
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

    // Aplikasi punya >1 surface (game + overlay Levi). Overlay lirik hanya di yang terbesar.
    std::mutex mSurfMutex;
    EGLSurface mMainSurf = nullptr;
    long long mMainArea = 0;
    double mMainSeen = 0.0;

    std::mutex mMenuMutex;
    std::vector<Cmd> mPendingCmds;
    std::vector<std::pair<std::string, std::string>> mPendingSettings;
    std::map<std::string, std::string> mMenuSettings; // hanya dipakai thread render
    std::atomic<bool> mMenuRegistered{false};
    std::atomic<bool> mMenuWasOff{false};

    std::atomic<bool> mActive{false};
    std::chrono::steady_clock::time_point mT0 = std::chrono::steady_clock::now();
    std::mutex mLogMutex;
    std::ofstream mLogFile; // <folder data mod>/lyrics3d.log, untuk diagnosa tanpa adb
};
