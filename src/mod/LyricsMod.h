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
#include <pl/Input.hpp>
#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>
#include <pl/memory/Hook.hpp>

#include "LyricsRenderer.h"
#include "Config.h"
#include "LyricsController.h"
#include "GameCamera.h"

class LyricsMod {
public:
    static LyricsMod& instance();

    LyricsMod();

    bool load();
    bool enable();
    bool disable();
    bool unload();

    // Called from the input thread. Always returns false (does not swallow touches; the game still receives them).
    bool onTouch(const pl::input::TouchEvent& e);

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

    // --- Mod Menu (callbacks arrive on other threads -> queued, processed on the render thread) ---
    enum class Cmd { Play, Pause, Toggle, Next, Prev, Stop, Rescan, PlayIndex };
    struct PendingCmd {
        Cmd c;
        int arg;
    };
    bool registerMenu();
    std::filesystem::path menuStatePath() const;
    void loadMenuPlan();
    void saveMenuPhase(const char* phase);
    void unregisterMenu();
    void queueCmd(Cmd c, int arg = 0);
    void queueSetting(std::string key, std::string value);
    void drainMenuQueue();
    void rebuildConfig(); // mCfg = mFileCfg + settings from the menu
    std::filesystem::path menuSettingsPath() const;
    void loadMenuSettings();
    void saveMenuSettings();
    std::string buildSchemaJson() const;
    void updateSchemaIfNeeded();
    std::filesystem::path configPath() const;

    ll::mod::NativeMod* mSelf = nullptr;

    std::optional<pl::config::ConfigFile<LyricsConfig>> mConfig;
    LyricsConfig mFileCfg; // contents of config.json
    LyricsConfig mCfg;     // effective: mFileCfg + settings from the menu
    std::filesystem::file_time_type mCfgStamp{};
    double mNextCfgCheck = 0.0;

    pl::memory::HookHandle mSwapHook;
    pl::memory::HookHandle mSwapDamageHook;
    SwapFn mOrigSwap = nullptr;
    SwapDamageFn mOrigSwapDamage = nullptr;

    std::mutex mFrameMutex; // protects the controller/renderer from a concurrent disable()
    std::unique_ptr<lyrics::LyricsController> mController;
    std::unique_ptr<lyrics::LyricsRenderer> mRenderer;
    EGLContext mCtx = EGL_NO_CONTEXT;
    bool mBroken = false;
    bool mGlChecked = false;

    // The app has >1 surface (game + Levi overlay). The lyrics overlay is drawn only on the largest.
    std::mutex mSurfMutex;
    EGLSurface mMainSurf = nullptr;
    long long mMainArea = 0;
    double mMainSeen = 0.0;

    std::mutex mMenuMutex;
    std::vector<PendingCmd> mPendingCmds;
    std::vector<std::pair<std::string, std::string>> mPendingSettings;
    std::map<std::string, std::string> mMenuSettings; // render thread only
    std::atomic<bool> mMenuRegistered{false};
    int mPlanStage = 1;
    bool mPlanButtons = true;
    bool mMenuDeferred = false;
    std::atomic<bool> mMenuWasOff{false};
    bool mSettingsDirty = false;
    double mNextSettingsSave = 0.0;
    bool mSchemaSent = false;
    uint32_t mSchemaLib = 0;
    int mSchemaIdx = -2;
    int mLoggedChanges = 0;

    // --- camera view direction (estimated from finger swipes) for world mode ---
    struct TouchPtr {
        float x = 0, y = 0;
        bool inZone = true; // true = counts for camera turning (right side of the screen)
        bool joy = false;   // this finger holds the virtual joystick
        double t = 0;
    };
    std::mutex mCamMutex;
    float mCamYaw = 0.0f, mCamPitch = 0.0f; // radians; yaw + = turn right, pitch + = look up
    std::map<int, TouchPtr> mPtrs;
    float mDragX = 0.0f, mDragY = 0.0f; // total finger drag in pixels (camera side of the screen)
    bool mJoyActive = false;
    int mJoyId = -1;
    float mJoyCx = 0, mJoyCy = 0, mJoyX = 0, mJoyY = 0;
    double mJoyT = 0;
    std::atomic<float> mTSens{0.2f}, mTZone{0.3f};
    std::atomic<bool> mTInvX{false}, mTInvY{false};
    std::atomic<int> mScrW{0}, mScrH{0};
    std::atomic<int> mTouchLogged{0};
    std::atomic<long> mTouchCount{0};
    static std::atomic<bool> sTouchRegistered;

    // Java mode (reads the game camera from GL matrix uploads). Installed late and only once; a marker file makes
    // sure that a crash while installing disables it for good instead of crashing every launch.
    std::filesystem::path javaStatePath() const;
    bool mJavaBlocked = false, mJavaTried = false;

    std::atomic<bool> mActive{false};
    std::chrono::steady_clock::time_point mT0 = std::chrono::steady_clock::now();
    std::mutex mLogMutex;
    std::ofstream mLogFile; // <mod data folder>/lyrics3d.log, for diagnosis without adb
};
