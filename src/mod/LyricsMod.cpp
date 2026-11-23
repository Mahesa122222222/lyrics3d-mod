#include "LyricsMod.h"

#include <GLES3/gl3.h>
#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cctype>
#include <optional>
#include <cstring>
#include <exception>
#include <system_error>

#include "Jni.h"
#include "Log.h"

namespace fs = std::filesystem;

namespace {

// current() may return a pointer or a reference depending on the SDK version; support both.
template <class T>
T* asPtr(T& v) {
    return &v;
}
template <class T>
T* asPtr(T* v) {
    return v;
}

// true while this thread is inside our detour (prevents double drawing if
// eglSwapBuffersWithDamage calls eglSwapBuffers internally).
thread_local bool tInsideSwap = false;

const char* const kModuleId = "lyrics3d.main";
const char* const kBtnPlay = "lyrics3d.btn.playpause";
const char* const kBtnNext = "lyrics3d.btn.next";
const char* const kBtnPrev = "lyrics3d.btn.prev";

bool parseBool(std::string_view s, bool fallback) {
    if (s == "true" || s == "1" || s == "on" || s == "True" || s == "TRUE") return true;
    if (s == "false" || s == "0" || s == "off" || s == "False" || s == "FALSE") return false;
    return fallback;
}
bool parseNum(std::string_view s, double& out) {
    const std::string tmp(s);
    char* end = nullptr;
    const double v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str()) return false;
    out = v;
    return true;
}
// "#RRGGBB" (also accepts no '#', or 8-digit #AARRGGBB -> takes the RGB part).
std::optional<std::string> normalizeColor(std::string_view s) {
    if (!s.empty() && s.front() == '#') s.remove_prefix(1);
    if (s.size() == 8) s.remove_prefix(2);
    if (s.size() != 6) return std::nullopt;
    std::string out = "#";
    for (char ch : s) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return std::nullopt;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return out;
}
const char* const kFonts[] = {"sans-serif", "sans-serif-condensed", "serif", "monospace", "casual", "cursive"};
bool isKnownFont(std::string_view s) {
    for (const char* f : kFonts)
        if (s == f) return true;
    return false;
}
std::string fmtNum(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4g", v);
    return b;
}

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

std::atomic<bool> LyricsMod::sTouchRegistered{false};

LyricsMod& LyricsMod::instance() {
    static LyricsMod inst;
    return inst;
}

LyricsMod::LyricsMod() : mSelf(asPtr(ll::mod::NativeMod::current())) {}

void LyricsMod::logSink(int level, const char* message) {
    LyricsMod& m = instance();
    std::lock_guard<std::mutex> lk(m.mLogMutex);
    if (!m.mSelf) return;
    const char* prefix = level == 0 ? "" : (level == 1 ? "[WARN] " : "[ERROR] ");
    m.mSelf->getLogger().info("{}{}", prefix, message);
    if (m.mLogFile.is_open()) {
        m.mLogFile << prefix << message << '\n';
        m.mLogFile.flush();
    }
}

fs::path LyricsMod::realStatePath() const { return fs::path(mSelf->getDataDir()) / "realcam_state.txt"; }

fs::path LyricsMod::configPath() const { return fs::path(mSelf->getConfigDir()) / "config.json"; }

// ---------------------------------------------------------------- lifecycle

bool LyricsMod::load() {
    try {
        lyrics::log::setSink(&LyricsMod::logSink);
        lyrics::jni::setJavaVM(mSelf->getJavaVM());

        std::error_code ec;
        fs::create_directories(fs::path(mSelf->getDataDir()) / "songs", ec);
        {
            std::lock_guard<std::mutex> lk(mLogMutex);
            mLogFile.open(fs::path(mSelf->getDataDir()) / "lyrics3d.log", std::ios::out | std::ios::trunc);
        }

        mConfig.emplace();
        if (!mConfig->load()) LYR_LOGW("Failed to read config.json; using defaults");
        mFileCfg = mConfig->value();
        mCfg = mFileCfg;
        mCfgStamp = fs::last_write_time(configPath(), ec);
        loadMenuSettings();
        {
            std::ifstream js(realStatePath());
            std::string st;
            if (js && std::getline(js, st) && st.find("installing") != std::string::npos) {
                mRealBlocked = true;
                LYR_LOGW("Real camera is disabled: the game crashed while it was being installed last time "
                         "(delete realcam_state.txt to try again)");
            }
        }
        LYR_LOGI("Lyrics 3D loaded. Put songs + .lrc files in: %s",
                 (fs::path(mSelf->getDataDir()) / "songs").string().c_str());
        return true;
    } catch (const std::exception& e) {
        LYR_LOGE("load() failed: %s", e.what());
        return false;
    }
}

bool LyricsMod::enable() {
    try {
        // EGL functions live in the system libEGL.so, so this does not depend on the Minecraft version.
        void* lib = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) lib = dlopen("libEGL.so", RTLD_NOW);
        void* swapTarget = lib ? dlsym(lib, "eglSwapBuffers") : nullptr;
        if (!swapTarget) swapTarget = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
        if (!swapTarget) {
            LYR_LOGE("eglSwapBuffers not found; overlay cannot be installed");
            return false;
        }

        mT0 = std::chrono::steady_clock::now();
        mBroken = false;
        mActive.store(true, std::memory_order_release);

        mSwapHook = pl::memory::HookHandle(swapTarget, reinterpret_cast<void*>(&LyricsMod::swapDetour),
                                           reinterpret_cast<void**>(&mOrigSwap), pl::memory::HookPriority::Normal);
        if (!mSwapHook.installed()) {
            LYR_LOGE("Failed to install eglSwapBuffers hook");
            mActive.store(false);
            return false;
        }

        // Some apps use the damage-rect variant; hook it if present (optional).
        void* damageTarget = lib ? dlsym(lib, "eglSwapBuffersWithDamageKHR") : nullptr;
        if (!damageTarget && lib) damageTarget = dlsym(lib, "eglSwapBuffersWithDamageEXT");
        if (damageTarget) {
            mSwapDamageHook =
                pl::memory::HookHandle(damageTarget, reinterpret_cast<void*>(&LyricsMod::swapDamageDetour),
                                       reinterpret_cast<void**>(&mOrigSwapDamage), pl::memory::HookPriority::Normal);
            if (!mSwapDamageHook.installed()) LYR_LOGW("eglSwapBuffersWithDamage hook skipped");
        }
        LYR_LOGI("Lyrics overlay enabled");
        // The touch callback cannot be unregistered; it is registered once and gated by mActive.
        if (!sTouchRegistered.exchange(true)) {
            try {
                pl::input::registerTouchCallback(
                    [](const pl::input::TouchEvent& e) { return LyricsMod::instance().onTouch(e); });
                LYR_LOGI("Touch callback registered (for world mode)");
            } catch (const std::exception& ex) {
                LYR_LOGW("Touch callback failed: %s", ex.what());
            }
        }
        loadMenuPlan();
        if (mPlanStage <= 2) {
            if (!registerMenu()) LYR_LOGW("Mod Menu rejected the module; use control.txt / config.json instead");
        } else if (mPlanStage == 3) {
            mMenuDeferred = true; // registered from the render thread, see drawFrame()
        } else {
            LYR_LOGW("Mod Menu not used (all registration attempts failed). Use control.txt / config.json instead");
        }
        return true;
    } catch (const std::exception& e) {
        LYR_LOGE("enable() failed: %s", e.what());
        mActive.store(false);
        return false;
    }
}

bool LyricsMod::disable() {
    unregisterMenu();
    mActive.store(false, std::memory_order_release);
    lyrics::GameCamera::instance().uninstall();
    mSwapHook.reset();
    mSwapDamageHook.reset();

    std::lock_guard<std::mutex> lk(mFrameMutex);
    if (mRenderer) {
        mRenderer->invalidateGl(); // the GL context does not belong to this thread; do not call GL
        mRenderer->shutdown(lyrics::jni::env());
    }
    mRenderer.reset();
    mController.reset(); // stops the audio thread
    mCtx = EGL_NO_CONTEXT;
    LYR_LOGI("Lyrics overlay disabled");
    return true;
}

bool LyricsMod::unload() { return true; }

// ---------------------------------------------------------------- hook

EGLBoolean LyricsMod::swapDetour(EGLDisplay dpy, EGLSurface surf) {
    LyricsMod& m = instance();
    if (!tInsideSwap) {
        tInsideSwap = true;
        m.beforeSwap(dpy, surf);
        tInsideSwap = false;
    }
    return m.mOrigSwap ? m.mOrigSwap(dpy, surf) : EGL_FALSE;
}

EGLBoolean LyricsMod::swapDamageDetour(EGLDisplay dpy, EGLSurface surf, EGLint* rects, EGLint n) {
    LyricsMod& m = instance();
    if (!tInsideSwap) {
        tInsideSwap = true;
        m.beforeSwap(dpy, surf);
        tInsideSwap = false;
    }
    return m.mOrigSwapDamage ? m.mOrigSwapDamage(dpy, surf, rects, n) : EGL_FALSE;
}

bool LyricsMod::isMainSurface(EGLDisplay dpy, EGLSurface surf) {
    EGLint w = 0, h = 0;
    if (!eglQuerySurface(dpy, surf, EGL_WIDTH, &w) || !eglQuerySurface(dpy, surf, EGL_HEIGHT, &h)) return false;
    const long long area = static_cast<long long>(w) * static_cast<long long>(h);
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - mT0).count();

    std::lock_guard<std::mutex> lk(mSurfMutex);
    if (surf == mMainSurf) {
        mMainArea = area;
        mMainSeen = now;
        return true;
    }
    // A new surface replaces the old one if it is larger, or if the old one has not been seen for >1 second.
    if (mMainSurf == nullptr || area > mMainArea || now - mMainSeen > 1.0) {
        LYR_LOGI("Main surface: %dx%d", static_cast<int>(w), static_cast<int>(h));
        mMainSurf = surf;
        mMainArea = area;
        mMainSeen = now;
        mGlChecked = false; // new surface -> re-check GL version & context
        return true;
    }
    return false;
}

void LyricsMod::beforeSwap(EGLDisplay dpy, EGLSurface surf) {
    if (!mActive.load(std::memory_order_acquire) || mBroken) return;
    if (!isMainSurface(dpy, surf)) return;
    std::unique_lock<std::mutex> lk(mFrameMutex, std::try_to_lock);
    if (!lk.owns_lock()) return; // disable() is running
    try {
        drawFrame(dpy, surf);
    } catch (const std::exception& e) {
        LYR_LOGE("Overlay disabled because of an exception: %s", e.what());
        mBroken = true;
    } catch (...) {
        LYR_LOGE("Overlay disabled because of an unknown exception");
        mBroken = true;
    }
}

void LyricsMod::reloadConfigIfChanged(double timeSec) {
    if (timeSec < mNextCfgCheck || !mConfig) return;
    mNextCfgCheck = timeSec + 1.0;
    std::error_code ec;
    auto stamp = fs::last_write_time(configPath(), ec);
    if (ec || stamp == mCfgStamp) return;
    if (mConfig->load()) {
        mFileCfg = mConfig->value();
        rebuildConfig();
        LYR_LOGI("config.json reloaded");
    }
    mCfgStamp = fs::last_write_time(configPath(), ec); // load() may rewrite the file
}

void LyricsMod::drawFrame(EGLDisplay dpy, EGLSurface surf) {
    // Only draw on the active main surface (ignore other surfaces, e.g. offscreen).
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT || eglGetCurrentSurface(EGL_DRAW) != surf) return;

    JNIEnv* env = lyrics::jni::env();
    if (!env) return;

    if (!mGlChecked || ctx != mCtx) {
        const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        if (!ver || std::strstr(ver, "OpenGL ES 3") == nullptr) {
            LYR_LOGE("OpenGL ES 3.x required, found: %s. Overlay disabled.", ver ? ver : "?");
            mBroken = true;
            return;
        }
        if (mRenderer && mCtx != EGL_NO_CONTEXT && ctx != mCtx) {
            LYR_LOGI("GL context changed; rebuilding resources");
            mRenderer->invalidateGl();
        }
        mCtx = ctx;
        mGlChecked = true;
    }

    if (!mController) mController = std::make_unique<lyrics::LyricsController>(fs::path(mSelf->getDataDir()), mCfg);
    if (!mRenderer) {
        mRenderer = std::make_unique<lyrics::LyricsRenderer>();
        if (!mRenderer->init(env)) {
            LYR_LOGE("Renderer failed to initialize; overlay disabled");
            mBroken = true;
            return;
        }
    }

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - mT0).count();
    reloadConfigIfChanged(t);
    if (mMenuDeferred && t > 6.0) { // stage 3: once only, a few seconds after the game is running
        mMenuDeferred = false;
        registerMenu();
    }
    drainMenuQueue();
    if (mSettingsDirty && t >= mNextSettingsSave) {
        mSettingsDirty = false;
        mNextSettingsSave = t + 1.0;
        saveMenuSettings();
    }
    mController->tick(t);
    updateSchemaIfNeeded();
    if (!mCfg.enabled) return;
    mScrW.store(w);
    mScrH.store(h);
    mTSens.store(mCfg.camSens);
    mTZone.store(mCfg.camZoneLeft);
    mTInvX.store(mCfg.camInvertX);
    mTInvY.store(mCfg.camInvertY);
lyrics::CamInput cam;
    {
        std::lock_guard<std::mutex> lk(mCamMutex);
        cam.swipeYaw = mCamYaw;
        cam.swipePitch = mCamPitch;
        cam.dragX = mDragX;
        cam.dragY = mDragY;
        if (mJoyActive && t - mJoyT < 3.0) {
            const float R = 0.11f * static_cast<float>(h);
            const float jx = (mJoyX - mJoyCx) / R, jy = (mJoyY - mJoyCy) / R;
            const float len = std::sqrt(jx * jx + jy * jy);
            if (len > 0.25f) {
                const float mag = std::min(len, 1.0f);
                cam.joyFwd = -jy / len * mag;
                cam.joyStrafe = jx / len * mag;
            }
        }
    }
    if (mCfg.camTrack == "auto" && !mRealTried && !mRealBlocked && t > 8.0) { // late, once
        mRealTried = true;
        auto writeState = [this](const char* text) {
            std::ofstream o(realStatePath(), std::ios::out | std::ios::trunc);
            o << text << '\n';
        };
        writeState("installing"); // if the game crashes inside install(), the next launch sees this
        lyrics::GameCamera::instance().setScreen(w, h);
        const bool ok = lyrics::GameCamera::instance().install();
        writeState(ok ? "ok" : "failed");
    }
    if (lyrics::GameCamera::instance().installed()) {
        lyrics::GameCamera::instance().setScreen(w, h);
        const lyrics::GamePose gp = lyrics::GameCamera::instance().endFrame();
        cam.realOn = true;
        cam.realA = gp.a;
        cam.realB = gp.b;
        cam.realC = gp.c;
        cam.rAyaw = gp.ayaw;
        cam.rApitch = gp.apitch;
        cam.rByaw = gp.byaw;
        cam.rBpitch = gp.bpitch;
        cam.rCyaw = gp.cyaw;
        cam.rCpitch = gp.cpitch;
        cam.realPersp = gp.persp;
        cam.realFov = gp.fovV;
        cam.realCalls = gp.calls;
        cam.realRigid = gp.rigid;
    }
    mRenderer->render(env, mCfg, mController->view(), w, h, t, cam);
}

bool LyricsMod::onTouch(const pl::input::TouchEvent& e) {
    if (!mActive.load(std::memory_order_acquire)) return false;
    mTouchCount.fetch_add(1, std::memory_order_relaxed);
    if (mTouchLogged.load(std::memory_order_relaxed) < 12) {
        mTouchLogged.fetch_add(1, std::memory_order_relaxed);
        LYR_LOGI("touch: action=%d id=%d x=%.0f y=%.0f", e.action, e.pointerId, e.x, e.y);
    }
    const int a = e.action & 0xFF; // MotionEvent style: 0/5 down, 2 move, 1/6/3 up
    const bool isDown = a == 0 || a == 5, isUp = a == 1 || a == 6 || a == 3;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - mT0).count();
    const int sw = mScrW.load(std::memory_order_relaxed);
    const bool inZone = sw <= 0 || e.x > mTZone.load(std::memory_order_relaxed) * static_cast<float>(sw);
    std::lock_guard<std::mutex> lk(mCamMutex);
    auto it = mPtrs.find(e.pointerId);
    if (isUp) {
        if (mJoyActive && mJoyId == e.pointerId) mJoyActive = false;
        if (it != mPtrs.end()) mPtrs.erase(it);
        return false;
    }
    // A finger that has not been seen for 0.2 s counts as a new touch (the joystick finger may stay still).
    const bool fresh = it == mPtrs.end() || (!it->second.joy && now - it->second.t > 0.2);
    if (isDown || fresh) {
        TouchPtr p{e.x, e.y, inZone, false, now};
        if (!inZone && !mJoyActive) { // left side of the screen = the walking joystick
            p.joy = true;
            mJoyActive = true;
            mJoyId = e.pointerId;
            mJoyCx = mJoyX = e.x;
            mJoyCy = mJoyY = e.y;
            mJoyT = now;
        }
        mPtrs[e.pointerId] = p;
        return false;
    }
    if (it->second.joy) {
        mJoyX = e.x;
        mJoyY = e.y;
        mJoyT = now;
        return false;
    }
    const float dx = e.x - it->second.x, dy = e.y - it->second.y;
    it->second.x = e.x;
    it->second.y = e.y;
    it->second.t = now;
    if (!it->second.inZone || std::fabs(dx) > 150.0f || std::fabs(dy) > 150.0f) return false;
    mDragX += dx;
    mDragY += dy;
    const float k = mTSens.load(std::memory_order_relaxed) * 3.14159265f / 180.0f;
    mCamYaw += (mTInvX.load(std::memory_order_relaxed) ? -dx : dx) * k;
    mCamPitch += (mTInvY.load(std::memory_order_relaxed) ? dy : -dy) * k;
    const float lim = 89.0f * 3.14159265f / 180.0f;
    mCamPitch = std::max(-lim, std::min(lim, mCamPitch));
    const float twoPi = 6.2831853f;
    if (mCamYaw > twoPi || mCamYaw < -twoPi) mCamYaw = std::fmod(mCamYaw, twoPi);
    return false;
}

// ---------------------------------------------------------------- Mod Menu

// The launcher's Mod Menu once crashed the game inside registerModule() (tombstone: libpreloader.so).
// Because the exact cause was unknown, registration is done in stages and the result is remembered in
// menu_state_v2.txt: if the game crashes while registering, the next launch automatically uses a
// simpler method. Delete menu_state_v2.txt to start again from stage 1.
//   stage 1: full module (settings + callbacks) in enable()
//   stage 2: bare module (no settings/callbacks) in enable()
//   stage 3: bare module, registered from the render thread a few seconds after the game starts
//   stage 4+: Mod Menu is not used
fs::path LyricsMod::menuStatePath() const { return fs::path(mSelf->getDataDir()) / "menu_state_v2.txt"; }

void LyricsMod::loadMenuPlan() {
    mPlanStage = 1;
    mPlanButtons = true;
    std::string phase = "OK";
    std::ifstream in(menuStatePath());
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "stage") mPlanStage = std::atoi(v.c_str());
        else if (k == "buttons") mPlanButtons = (v == "1");
        else if (k == "phase") phase = v;
    }
    if (mPlanStage < 1) mPlanStage = 1;
    if (phase == "M") {
        LYR_LOGW("Previous launch crashed in registerModule (stage %d) -> moving to the next stage", mPlanStage);
        ++mPlanStage;
        mPlanButtons = true;
    } else if (phase == "B") {
        LYR_LOGW("Previous launch crashed in registerButton -> on-screen buttons disabled");
        mPlanButtons = false;
    }
    saveMenuPhase("OK");
}

void LyricsMod::saveMenuPhase(const char* phase) {
    std::ofstream out(menuStatePath(), std::ios::out | std::ios::trunc);
    out << "stage=" << mPlanStage << "\nbuttons=" << (mPlanButtons ? 1 : 0) << "\nphase=" << phase << "\n";
    out.flush(); // must be written to disk before calling Mod Menu (which may crash)
}

bool LyricsMod::registerMenu() {
    if (mMenuRegistered) return true;
    namespace mm = pl::modmenu;
    if (mPlanStage > 3) {
        LYR_LOGW("Mod Menu not used (all registration attempts failed). Use control.txt / config.json instead");
        return false;
    }
    // Use the real id held by the launcher (as in the docs example: getSelf().getId()).
    std::string modId = mSelf ? std::string(mSelf->getId()) : std::string();
    if (modId.empty()) modId = "lyrics3d";
    const bool full = (mPlanStage == 1);
    LYR_LOGI("Registering Mod Menu: stage %d, modId=\"%s\"", mPlanStage, modId.c_str());

    mm::ModuleBuilder mb(kModuleId, "Lyrics 3D");
    mb.modId(modId).description("Play songs from your phone and show the lyrics as 3D text in the game.").defaultEnabled(true);
    if (full) {
        mb.onToggle([](std::string_view, bool enabled) {
              LyricsMod& m = instance();
              m.queueSetting("enabled", enabled ? "true" : "false");
              // Off -> pause music. On again -> resume (only if it was turned off via the menu,
              // so that "autoStart" in config.json is still respected when the game just started).
              if (!enabled) {
                  m.mMenuWasOff.store(true);
                  m.queueCmd(Cmd::Pause);
              } else if (m.mMenuWasOff.exchange(false)) {
                  m.queueCmd(Cmd::Play);
              }
          })
            .onConfigChanged([](std::string_view, std::string_view key, std::string_view value) {
                LyricsMod& m = instance();
                if (key.rfind("act_", 0) == 0) { // menu button (action value = "true")
                    if (value != "true") return;
                    if (key == "act_prev") m.queueCmd(Cmd::Prev);
                    else if (key == "act_next") m.queueCmd(Cmd::Next);
                    else if (key == "act_toggle") m.queueCmd(Cmd::Toggle);
                    else if (key == "act_stop") m.queueCmd(Cmd::Stop);
                    else if (key == "act_rescan") m.queueCmd(Cmd::Rescan);
                } else if (key == "song") { // song list
                    const std::string v(value);
                    if (!v.empty()) m.queueCmd(Cmd::PlayIndex, std::atoi(v.c_str()));
                } else {
                    m.queueSetting(std::string(key), std::string(value));
                }
            })
            .config("volume", "Volume", mm::ConfigType::SliderFloat, "0.8", "0", "1")
            .config("fontSize", "Font size", mm::ConfigType::SliderFloat, "0.055", "0.02", "0.12")
            .config("positionY", "Vertical position", mm::ConfigType::SliderFloat, "0.55", "-1", "1")
            .config("opacity", "Opacity", mm::ConfigType::SliderFloat, "1", "0.1", "1")
            .config("lyricsOffsetMs", "Lyrics offset (ms)", mm::ConfigType::SliderInt, "0", "-2000", "2000")
            .config("effect3d", "3D effect (tilt + thickness)", mm::ConfigType::Toggle, "true")
            .config("showContext", "Previous/next lines", mm::ConfigType::Toggle, "true")
            .config("loop", "Repeat playlist", mm::ConfigType::Toggle, "true")
            .config("shuffle", "Shuffle", mm::ConfigType::Toggle, "false");
    }

    saveMenuPhase("M");
    const bool ok = mb.registerModule();
    saveMenuPhase("OK");
    LYR_LOGI("registerModule -> %s", ok ? "true" : "false");
    if (!ok) return false;
    mMenuRegistered = true;

    if (!mPlanButtons) {
        LYR_LOGI("On-screen buttons skipped");
        return true;
    }
    auto makeBtn = [&](const char* id, const char* name, const char* label, Cmd cmd) {
        saveMenuPhase("B");
        const bool b =
            mm::ButtonBuilder(id, name)
                .moduleId(kModuleId)
                .modId(modId)
                .label(label)
                .behavior(mm::ButtonBehavior::Click)
                .sizeScale(1.0f, 1.0f)
                .stylePreset(mm::ButtonStylePreset::Accent)
                .onEvent([cmd](std::string_view, mm::ButtonEvent ev, float) {
                    if (ev == mm::ButtonEvent::Click) instance().queueCmd(cmd);
                })
                .registerButton();
        saveMenuPhase("OK");
        LYR_LOGI("registerButton(%s) -> %s", id, b ? "true" : "false");
    };
    makeBtn(kBtnPrev, "Previous song", "Prev", Cmd::Prev);
    makeBtn(kBtnPlay, "Play / Pause", "Play", Cmd::Toggle);
    makeBtn(kBtnNext, "Next song", "Next", Cmd::Next);
    return true;
}

void LyricsMod::unregisterMenu() {
    if (!mMenuRegistered.exchange(false)) return;
    pl::modmenu::unregisterButton(kBtnPrev);
    pl::modmenu::unregisterButton(kBtnPlay);
    pl::modmenu::unregisterButton(kBtnNext);
    pl::modmenu::unregisterModule(kModuleId);
}

void LyricsMod::queueCmd(Cmd c, int arg) {
    std::lock_guard<std::mutex> lk(mMenuMutex);
    if (mPendingCmds.size() < 32) mPendingCmds.push_back({c, arg});
}

void LyricsMod::queueSetting(std::string key, std::string value) {
    std::lock_guard<std::mutex> lk(mMenuMutex);
    if (mPendingSettings.size() < 256) mPendingSettings.emplace_back(std::move(key), std::move(value));
}

void LyricsMod::drainMenuQueue() {
    std::vector<PendingCmd> cmds;
    std::vector<std::pair<std::string, std::string>> settings;
    {
        std::lock_guard<std::mutex> lk(mMenuMutex);
        cmds.swap(mPendingCmds);
        settings.swap(mPendingSettings);
    }
    if (!settings.empty()) {
        for (auto& kv : settings) {
            if (mLoggedChanges < 80) { // help diagnose the value format coming from the menu
                ++mLoggedChanges;
                LYR_LOGI("menu: %s = %s", kv.first.c_str(), kv.second.c_str());
            }
            mMenuSettings[kv.first] = kv.second;
        }
        mSettingsDirty = true;
        rebuildConfig();
    }
    if (!mController) return;
    for (const PendingCmd& pc : cmds) {
        switch (pc.c) {
        case Cmd::Play: mController->play(); break;
        case Cmd::Pause: mController->pause(); break;
        case Cmd::Toggle: mController->toggle(); break;
        case Cmd::Next: mController->next(); break;
        case Cmd::Prev: mController->prev(); break;
        case Cmd::Stop: mController->stop(); break;
        case Cmd::Rescan: mController->rescan(); break;
        case Cmd::PlayIndex:
            // The menu may resend the selected value when the list loads; ignore it if that song is already playing.
            if (pc.arg != mController->currentIndex()) mController->playIndex(pc.arg);
            break;
        }
    }
}

// Values from the menu override config.json while the game runs (config.json is still read for
// settings not in the menu).
void LyricsMod::rebuildConfig() {
    LyricsConfig c = mFileCfg;
    bool flat = false;
    for (const auto& [key, val] : mMenuSettings) {
        double n = 0.0;
        const bool isNum = parseNum(val, n);
        if (key == "enabled") c.enabled = parseBool(val, c.enabled);
        else if (key == "loop") c.loop = parseBool(val, c.loop);
        else if (key == "shuffle") c.shuffle = parseBool(val, c.shuffle);
        else if (key == "showContext") c.showContext = parseBool(val, c.showContext);
        else if (key == "effect3d") flat = !parseBool(val, true);
        else if (key == "volume" && isNum) c.volume = static_cast<float>(clampd(n, 0.0, 1.0));
        else if (key == "fontSize" && isNum) c.fontSize = static_cast<float>(clampd(n, 0.02, 0.12));
        else if (key == "positionY" && isNum) c.positionY = static_cast<float>(clampd(n, -1.0, 1.0));
        else if (key == "opacity" && isNum) c.opacity = static_cast<float>(clampd(n, 0.1, 1.0));
        else if (key == "lyricsOffsetMs" && isNum) c.lyricsOffsetMs = static_cast<int>(clampd(n, -5000.0, 5000.0));
        else if (key == "glow" && isNum) c.glow = static_cast<float>(clampd(n, 0.0, 2.0));
        else if (key == "bold") c.bold = parseBool(val, c.bold);
        else if (key == "fontFamily" && isKnownFont(val)) c.fontFamily = val;
        else if (key == "transitionMs" && isNum) c.transitionMs = static_cast<int>(clampd(n, 0.0, 2000.0));
        else if (key == "floatAmount" && isNum) c.floatAmount = static_cast<float>(clampd(n, 0.0, 0.1));
        else if (key == "maxYawDeg" && isNum) c.maxYawDeg = static_cast<float>(clampd(n, 0.0, 40.0));
        else if (key == "colorMode" && (val == "solid" || val == "gradient" || val == "rainbow")) c.colorMode = val;
        else if (key == "gradientColor") { if (auto col = normalizeColor(val)) c.gradientColor = *col; }
        else if (key == "rainbowSpeed" && isNum) c.rainbowSpeed = static_cast<float>(clampd(n, 0.0, 3.0));
        else if (key == "sparkle") c.sparkle = parseBool(val, c.sparkle);
        else if (key == "sparkleAmount" && isNum) c.sparkleAmount = static_cast<float>(clampd(n, 0.0, 1.0));
        else if (key == "partyNotes") c.partyNotes = parseBool(val, c.partyNotes);
        else if (key == "notesAmount" && isNum) c.notesAmount = static_cast<float>(clampd(n, 0.0, 1.0));
        else if (key == "layoutMode" && (val == "world" || val == "dynamic" || val == "screen")) c.layoutMode = val;
        else if (key == "dynFontSize" && isNum) c.dynFontSize = static_cast<float>(clampd(n, 0.02, 0.1));
        else if (key == "worldFontSize" && isNum) c.worldFontSize = static_cast<float>(clampd(n, 0.03, 0.15));
        else if (key == "worldDistance" && isNum) c.worldDistance = static_cast<float>(clampd(n, 2.0, 40.0));
        else if (key == "worldWalk") c.worldWalk = parseBool(val, c.worldWalk);
        else if (key == "walkSpeed" && isNum) c.walkSpeed = static_cast<float>(clampd(n, 0.5, 12.0));
        else if (key == "gameFovDeg" && isNum) c.gameFovDeg = static_cast<float>(clampd(n, 30.0, 110.0));
        else if (key == "camTrack" && (val == "motion" || val == "swipe" || val == "auto")) c.camTrack = val;
        else if (key == "worldCaps") c.worldCaps = parseBool(val, c.worldCaps);
        else if (key == "worldHoldMs" && isNum) c.worldHoldMs = static_cast<int>(clampd(n, 0.0, 6000.0));
        else if (key == "worldSpread" && isNum) c.worldSpread = static_cast<float>(clampd(n, 0.2, 1.6));
        else if (key == "camSens" && isNum) c.camSens = static_cast<float>(clampd(n, 0.02, 1.0));
        else if (key == "camInvertX") c.camInvertX = parseBool(val, c.camInvertX);
        else if (key == "camInvertY") c.camInvertY = parseBool(val, c.camInvertY);
        else if (key == "camZoneLeft" && isNum) c.camZoneLeft = static_cast<float>(clampd(n, 0.0, 0.7));
        else if (key == "sungColor") { if (auto col = normalizeColor(val)) c.sungColor = *col; }
        else if (key == "unsungColor") { if (auto col = normalizeColor(val)) c.unsungColor = *col; }
        else if (key == "extrudeColor") { if (auto col = normalizeColor(val)) c.extrudeColor = *col; }
    }
    if (flat) {
        c.maxYawDeg = 0.0f;
        c.pitchDeg = 0.0f;
        c.floatAmount = 0.0f;
        c.extrudeLayers = 0;
    }
    mCfg = c;
    if (mController) mController->applyConfig(mCfg);
}

// ---------------------------------------------------------------- save menu settings

fs::path LyricsMod::menuSettingsPath() const { return fs::path(mSelf->getDataDir()) / "menu_settings.txt"; }

void LyricsMod::loadMenuSettings() {
    std::ifstream in(menuSettingsPath());
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string k = line.substr(0, eq);
        if (k == "enabled") continue;
        mMenuSettings[k] = line.substr(eq + 1);
    }
    if (!mMenuSettings.empty()) {
        LYR_LOGI("Loaded %zu saved menu settings", mMenuSettings.size());
        rebuildConfig();
    }
}

void LyricsMod::saveMenuSettings() {
    std::ofstream out(menuSettingsPath(), std::ios::out | std::ios::trunc);
    for (const auto& [k, v] : mMenuSettings)
        if (k != "enabled") out << k << '=' << v << '\n';
}

// ---------------------------------------------------------------- menu schema (Mod Menu V2)

std::string LyricsMod::buildSchemaJson() const {
    namespace mm = pl::modmenu;
    const LyricsConfig d{}; // default values
    auto boolS = [](bool v) { return std::string(v ? "true" : "false"); };
    auto node = [](const char* key, const char* title, const char* cat, mm::ConfigControlTypeV2 type) {
        mm::ConfigNodeV2 n;
        n.id = key;
        n.key = key;
        n.category = cat;
        n.title = title;
        n.type = type;
        return n;
    };
    auto withValue = [](mm::ConfigNodeV2 n, std::string def, std::string cur) {
        n.defaultValue = std::move(def);
        n.currentValue = std::move(cur);
        n.hasCurrentValue = true;
        return n;
    };
    auto slider = [&](const char* key, const char* title, const char* cat, bool isInt, double lo, double hi,
                      double step, const char* unit, double def, double cur) {
        mm::ConfigNodeV2 n = node(key, title, cat, isInt ? mm::ConfigControlTypeV2::SliderInt : mm::ConfigControlTypeV2::SliderFloat);
        n.minValue = fmtNum(lo);
        n.maxValue = fmtNum(hi);
        n.step = fmtNum(step);
        n.unit = unit;
        return withValue(std::move(n), fmtNum(def), fmtNum(cur));
    };
    auto toggle = [&](const char* key, const char* title, const char* cat, bool def, bool cur) {
        return withValue(node(key, title, cat, mm::ConfigControlTypeV2::Toggle), boolS(def), boolS(cur));
    };
    auto button = [&](const char* key, const char* title, const char* cat) {
        mm::ConfigNodeV2 n = node(key, title, cat, mm::ConfigControlTypeV2::Button);
        n.actionValue = "true";
        return n;
    };
    auto color = [&](const char* key, const char* title, const std::string& def, const std::string& cur) {
        mm::ConfigNodeV2 n = node(key, title, "display", mm::ConfigControlTypeV2::Color);
        n.colorAlpha = false;
        return withValue(std::move(n), def, cur);
    };

    mm::ConfigSchemaBuilder sb;
    sb.category("player", "Player", "Song controls");
    sb.category("songs", "Song list", "Pick a song from the songs folder");
    sb.category("world", "World mode", "Lyrics fixed in the world (Floating words / Dynamic)");
    sb.category("display", "Lyrics display");
    sb.category("party", "Colors & party mode", "Gradient, rainbow, sparkles, music notes");
    sb.category("effects", "3D effects");
    sb.defaultCategory("player");

    // --- Player ---
    std::string nowPlaying = "No song is playing yet";
    std::vector<lyrics::LyricsController::TrackSummary> songs;
    int curIdx = -1;
    std::string songsDir;
    if (mController) {
        songs = mController->trackSummaries();
        curIdx = mController->currentIndex();
        songsDir = mController->songsDir().string();
        if (curIdx >= 0 && curIdx < static_cast<int>(songs.size())) nowPlaying = songs[curIdx].title;
    }
    {
        mm::ConfigNodeV2 n = node("now", "Now playing", "player", mm::ConfigControlTypeV2::Info);
        n.key.clear();
        n.description = nowPlaying;
        sb.node(std::move(n));
    }
    sb.node(button("act_prev", "Previous", "player"));
    sb.node(button("act_toggle", "Play / Pause", "player"));
    sb.node(button("act_next", "Next", "player"));
    sb.node(button("act_stop", "Stop", "player"));
    sb.node(button("act_rescan", "Rescan songs", "player"));
    sb.node(slider("volume", "Volume", "player", false, 0.0, 1.0, 0.05, "", d.volume, mCfg.volume));
    sb.node(toggle("loop", "Repeat playlist", "player", d.loop, mCfg.loop));
    sb.node(toggle("shuffle", "Shuffle", "player", d.shuffle, mCfg.shuffle));

    // --- Song list ---
    if (songs.empty()) {
        mm::ConfigNodeV2 n = node("nosongs", "No songs yet", "songs", mm::ConfigControlTypeV2::Info);
        n.key.clear();
        n.description = "Put song files (.mp3 .ogg .m4a .wav .flac) and .lrc lyric files with the same name in: " + songsDir +
                        " then press 'Rescan songs' in the Player tab.";
        sb.node(std::move(n));
    } else {
        mm::ConfigNodeV2 n = node("song", "Choose song", "songs", mm::ConfigControlTypeV2::Choice);
        n.choiceStyle = mm::ConfigChoiceStyleV2::Dropdown;
        n.searchable = true;
        n.description = std::to_string(songs.size()) + " songs found";
        for (size_t i = 0; i < songs.size(); ++i) {
            mm::ConfigOptionV2 o;
            o.value = std::to_string(i);
            o.label = songs[i].title + (songs[i].hasLrc ? "" : "  (no lyrics)");
            n.options.push_back(std::move(o));
        }
        n = withValue(std::move(n), "", curIdx >= 0 ? std::to_string(curIdx) : std::string());
        n.hasCurrentValue = curIdx >= 0;
        sb.node(std::move(n));
    }

    // --- World mode ---
    {
        mm::ConfigNodeV2 n = node("layoutMode", "Display mode", "world", mm::ConfigControlTypeV2::Choice);
        n.choiceStyle = mm::ConfigChoiceStyleV2::Dropdown;
        const std::pair<const char*, const char*> modes[] = {{"world", "Floating words"}, {"dynamic", "Dynamic (whole lines)"}, {"screen", "Lines on screen"}};
        for (const auto& m : modes) {
            mm::ConfigOptionV2 o;
            o.value = m.first;
            o.label = m.second;
            n.options.push_back(std::move(o));
        }
        sb.node(withValue(std::move(n), d.layoutMode, mCfg.layoutMode));
    }
    {
        mm::ConfigNodeV2 n = node("worldhelp", "How it works", "world", mm::ConfigControlTypeV2::Info);
        n.key.clear();
        n.description =
            "Floating words: words appear one by one. Dynamic: each whole lyric line becomes a sign standing in the world at an angle. Either way, they stay put when you look around and are left behind when you walk. "
            "The mod follows your camera by watching how the picture moves ('Screen motion'), or tries to read the game's real camera ('Auto', experimental: it falls back to screen motion by itself, see lyrics3d.log). "
            "If words drift, set 'Game FOV' to the value in Minecraft's video settings. Needs songs with timed "
            "lyrics (.lrc).";
        sb.node(std::move(n));
    }
    {
        mm::ConfigNodeV2 n = node("camTrack", "Camera tracking", "world", mm::ConfigControlTypeV2::Choice);
        n.choiceStyle = mm::ConfigChoiceStyleV2::Dropdown;
        const std::pair<const char*, const char*> modes[] = {{"motion", "Screen motion"}, {"swipe", "Finger swipes"}, {"auto", "Auto: real game camera (experimental)"}};
        for (const auto& m : modes) {
            mm::ConfigOptionV2 o;
            o.value = m.first;
            o.label = m.second;
            n.options.push_back(std::move(o));
        }
        sb.node(withValue(std::move(n), d.camTrack, mCfg.camTrack));
    }
    sb.node(slider("gameFovDeg", "Game FOV", "world", false, 30, 110, 1, "deg", d.gameFovDeg, mCfg.gameFovDeg));
    sb.node(slider("worldDistance", "Word distance (blocks)", "world", false, 2, 40, 0.5, "blocks", d.worldDistance, mCfg.worldDistance));
    sb.node(toggle("worldWalk", "Leave words behind when walking", "world", d.worldWalk, mCfg.worldWalk));
    sb.node(slider("walkSpeed", "Walking speed (blocks/s)", "world", false, 0.5, 12, 0.1, "blocks/s", d.walkSpeed, mCfg.walkSpeed));
    sb.node(slider("dynFontSize", "Line size (Dynamic mode)", "world", false, 0.02, 0.1, 0.0025, "", d.dynFontSize, mCfg.dynFontSize));
    sb.node(slider("worldFontSize", "Word size", "world", false, 0.03, 0.15, 0.005, "", d.worldFontSize, mCfg.worldFontSize));
    sb.node(toggle("worldCaps", "UPPERCASE", "world", d.worldCaps, mCfg.worldCaps));
    sb.node(slider("worldHoldMs", "Word hold time", "world", true, 0, 6000, 100, "ms", d.worldHoldMs, mCfg.worldHoldMs));
    sb.node(slider("worldSpread", "Word spread", "world", false, 0.2, 1.6, 0.05, "", d.worldSpread, mCfg.worldSpread));
    {
        mm::ConfigNodeV2 n = slider("camSens", "Camera turn sensitivity", "world", false, 0.02, 1.0, 0.01, "deg/px", d.camSens, mCfg.camSens);
        sb.node(std::move(n));
    }
    {
        mm::ConfigNodeV2 n = toggle("camInvertX", "Invert left-right", "world", d.camInvertX, mCfg.camInvertX);
        sb.node(std::move(n));
    }
    {
        mm::ConfigNodeV2 n = toggle("camInvertY", "Invert up-down", "world", d.camInvertY, mCfg.camInvertY);
        sb.node(std::move(n));
    }
    {
        mm::ConfigNodeV2 n = slider("camZoneLeft", "Ignored joystick zone", "world", false, 0.0, 0.7, 0.05, "", d.camZoneLeft, mCfg.camZoneLeft);
        sb.node(std::move(n));
    }

    // --- Display ---
    sb.node(slider("fontSize", "Font size", "display", false, 0.02, 0.12, 0.005, "", d.fontSize, mCfg.fontSize));
    sb.node(slider("positionY", "Vertical position", "display", false, -1.0, 1.0, 0.05, "", d.positionY, mCfg.positionY));
    sb.node(slider("opacity", "Opacity", "display", false, 0.1, 1.0, 0.05, "", d.opacity, mCfg.opacity));
    sb.node(slider("lyricsOffsetMs", "Lyrics offset", "display", true, -2000, 2000, 50, "ms", d.lyricsOffsetMs, mCfg.lyricsOffsetMs));
    sb.node(toggle("showContext", "Previous/next lines", "display", d.showContext, mCfg.showContext));
    sb.node(toggle("bold", "Bold text", "display", d.bold, mCfg.bold));
    {
        mm::ConfigNodeV2 n = node("fontFamily", "Font", "display", mm::ConfigControlTypeV2::Choice);
        n.choiceStyle = mm::ConfigChoiceStyleV2::Dropdown;
        for (const char* f : kFonts) {
            mm::ConfigOptionV2 o;
            o.value = f;
            o.label = f;
            n.options.push_back(std::move(o));
        }
        sb.node(withValue(std::move(n), d.fontFamily, mCfg.fontFamily));
    }
    sb.node(color("sungColor", "Sung color", d.sungColor, mCfg.sungColor));
    sb.node(color("unsungColor", "Unsung color", d.unsungColor, mCfg.unsungColor));
    sb.node(color("extrudeColor", "3D side color", d.extrudeColor, mCfg.extrudeColor));

    // --- Colors & party mode ---
    {
        mm::ConfigNodeV2 n = node("colorMode", "Color mode", "party", mm::ConfigControlTypeV2::Choice);
        n.choiceStyle = mm::ConfigChoiceStyleV2::Segmented;
        const std::pair<const char*, const char*> modes[] = {{"solid", "Solid"}, {"gradient", "Gradient"}, {"rainbow", "Rainbow"}};
        for (const auto& m : modes) {
            mm::ConfigOptionV2 o;
            o.value = m.first;
            o.label = m.second;
            n.options.push_back(std::move(o));
        }
        sb.node(withValue(std::move(n), d.colorMode, mCfg.colorMode));
    }
    {
        mm::ConfigNodeV2 n = color("gradientColor", "Gradient end color", d.gradientColor, mCfg.gradientColor);
        n.category = "party";
        n.visibleWhen.push_back({"colorMode", mm::ConfigConditionOpV2::Equals, "gradient"});
        sb.node(std::move(n));
    }
    {
        mm::ConfigNodeV2 n = slider("rainbowSpeed", "Rainbow speed", "party", false, 0.0, 2.0, 0.05, "", d.rainbowSpeed, mCfg.rainbowSpeed);
        n.visibleWhen.push_back({"colorMode", mm::ConfigConditionOpV2::Equals, "rainbow"});
        sb.node(std::move(n));
    }
    sb.node(toggle("sparkle", "Sparkles", "party", d.sparkle, mCfg.sparkle));
    {
        mm::ConfigNodeV2 n = slider("sparkleAmount", "Sparkle amount", "party", false, 0.0, 1.0, 0.05, "", d.sparkleAmount, mCfg.sparkleAmount);
        n.enabledWhen.push_back({"sparkle", mm::ConfigConditionOpV2::Truthy, ""});
        sb.node(std::move(n));
    }
    sb.node(toggle("partyNotes", "Music notes (party mode)", "party", d.partyNotes, mCfg.partyNotes));
    {
        mm::ConfigNodeV2 n = slider("notesAmount", "Music notes amount", "party", false, 0.0, 1.0, 0.05, "", d.notesAmount, mCfg.notesAmount);
        n.enabledWhen.push_back({"partyNotes", mm::ConfigConditionOpV2::Truthy, ""});
        sb.node(std::move(n));
    }

    // --- 3D effects ---
    const auto it = mMenuSettings.find("effect3d");
    const bool effect3d = (it == mMenuSettings.end()) ? true : parseBool(it->second, true);
    sb.node(toggle("effect3d", "3D effect (tilt + thickness)", "effects", true, effect3d));
    sb.node(slider("glow", "Glow", "effects", false, 0.0, 2.0, 0.1, "", d.glow, mCfg.glow));
    sb.node(slider("maxYawDeg", "Left-right sway", "effects", false, 0.0, 30.0, 1.0, "degrees", d.maxYawDeg, mFileCfg.maxYawDeg));
    sb.node(slider("floatAmount", "Floating effect", "effects", false, 0.0, 0.08, 0.005, "", d.floatAmount, mFileCfg.floatAmount));
    sb.node(slider("transitionMs", "Line change animation time", "effects", true, 100, 1200, 50, "ms", d.transitionMs, mCfg.transitionMs));
    return sb.toJson();
}

void LyricsMod::updateSchemaIfNeeded() {
    if (!mMenuRegistered || !mController || mPlanStage != 1) return;
    const uint32_t lib = mController->libraryVersion();
    const int idx = mController->currentIndex();
    if (mSchemaSent && lib == mSchemaLib && idx == mSchemaIdx) return;
    mSchemaSent = true;
    mSchemaLib = lib;
    mSchemaIdx = idx;
    const bool ok = pl::modmenu::setConfigSchemaJson(kModuleId, buildSchemaJson());
    LYR_LOGI("setConfigSchemaJson (songs=%zu, current=%d) -> %s", mController->trackCount(), idx, ok ? "true" : "false");
}
