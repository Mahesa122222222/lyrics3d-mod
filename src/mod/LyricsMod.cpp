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

// current() bisa mengembalikan pointer atau referensi tergantung versi SDK; dukung keduanya.
template <class T>
T* asPtr(T& v) {
    return &v;
}
template <class T>
T* asPtr(T* v) {
    return v;
}

// true saat thread ini sedang berada di dalam detour kita (cegah gambar ganda jika
// eglSwapBuffersWithDamage memanggil eglSwapBuffers di dalamnya).
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
// "#RRGGBB" (juga menerima tanpa '#', atau 8 digit #AARRGGBB -> ambil RGB-nya).
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
        if (!mConfig->load()) LYR_LOGW("Gagal membaca config.json; memakai nilai bawaan");
        mFileCfg = mConfig->value();
        mCfg = mFileCfg;
        mCfgStamp = fs::last_write_time(configPath(), ec);
        loadMenuSettings();
        LYR_LOGI("Lyrics 3D dimuat. Taruh lagu + .lrc di: %s",
                 (fs::path(mSelf->getDataDir()) / "songs").string().c_str());
        return true;
    } catch (const std::exception& e) {
        LYR_LOGE("load() gagal: %s", e.what());
        return false;
    }
}

bool LyricsMod::enable() {
    try {
        // Fungsi EGL ada di libEGL.so milik sistem, jadi tidak bergantung versi Minecraft.
        void* lib = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
        if (!lib) lib = dlopen("libEGL.so", RTLD_NOW);
        void* swapTarget = lib ? dlsym(lib, "eglSwapBuffers") : nullptr;
        if (!swapTarget) swapTarget = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
        if (!swapTarget) {
            LYR_LOGE("eglSwapBuffers tidak ditemukan; overlay tidak bisa dipasang");
            return false;
        }

        mT0 = std::chrono::steady_clock::now();
        mBroken = false;
        mActive.store(true, std::memory_order_release);

        mSwapHook = pl::memory::HookHandle(swapTarget, reinterpret_cast<void*>(&LyricsMod::swapDetour),
                                           reinterpret_cast<void**>(&mOrigSwap), pl::memory::HookPriority::Normal);
        if (!mSwapHook.installed()) {
            LYR_LOGE("Hook eglSwapBuffers gagal dipasang");
            mActive.store(false);
            return false;
        }

        // Sebagian aplikasi memakai varian dengan damage rect; hook kalau ada (tidak wajib).
        void* damageTarget = lib ? dlsym(lib, "eglSwapBuffersWithDamageKHR") : nullptr;
        if (!damageTarget && lib) damageTarget = dlsym(lib, "eglSwapBuffersWithDamageEXT");
        if (damageTarget) {
            mSwapDamageHook =
                pl::memory::HookHandle(damageTarget, reinterpret_cast<void*>(&LyricsMod::swapDamageDetour),
                                       reinterpret_cast<void**>(&mOrigSwapDamage), pl::memory::HookPriority::Normal);
            if (!mSwapDamageHook.installed()) LYR_LOGW("Hook eglSwapBuffersWithDamage dilewati");
        }
        LYR_LOGI("Overlay lirik aktif");
        // Callback sentuh tidak bisa dicabut; didaftarkan sekali dan dicek lewat mActive.
        if (!sTouchRegistered.exchange(true)) {
            try {
                pl::input::registerTouchCallback(
                    [](const pl::input::TouchEvent& e) { return LyricsMod::instance().onTouch(e); });
                LYR_LOGI("Callback sentuh terdaftar (untuk mode dunia)");
            } catch (const std::exception& ex) {
                LYR_LOGW("Callback sentuh gagal: %s", ex.what());
            }
        }
        loadMenuPlan();
        if (mPlanStage <= 2) {
            if (!registerMenu()) LYR_LOGW("Mod Menu menolak modul; kontrol lewat control.txt / config.json");
        } else if (mPlanStage == 3) {
            mMenuDeferred = true; // didaftarkan dari thread render, lihat drawFrame()
        } else {
            LYR_LOGW("Mod Menu tidak dipakai (semua cara pendaftaran bermasalah). Kontrol: control.txt / config.json");
        }
        return true;
    } catch (const std::exception& e) {
        LYR_LOGE("enable() gagal: %s", e.what());
        mActive.store(false);
        return false;
    }
}

bool LyricsMod::disable() {
    unregisterMenu();
    mActive.store(false, std::memory_order_release);
    mSwapHook.reset();
    mSwapDamageHook.reset();

    std::lock_guard<std::mutex> lk(mFrameMutex);
    if (mRenderer) {
        mRenderer->invalidateGl(); // konteks GL bukan milik thread ini; jangan panggil GL
        mRenderer->shutdown(lyrics::jni::env());
    }
    mRenderer.reset();
    mController.reset(); // menghentikan thread audio
    mCtx = EGL_NO_CONTEXT;
    LYR_LOGI("Overlay lirik dimatikan");
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
    // Surface baru menggantikan yang lama jika lebih besar, atau yang lama sudah >1 detik tak terlihat.
    if (mMainSurf == nullptr || area > mMainArea || now - mMainSeen > 1.0) {
        LYR_LOGI("Surface utama: %dx%d", static_cast<int>(w), static_cast<int>(h));
        mMainSurf = surf;
        mMainArea = area;
        mMainSeen = now;
        mGlChecked = false; // surface baru -> periksa ulang versi GL & konteks
        return true;
    }
    return false;
}

void LyricsMod::beforeSwap(EGLDisplay dpy, EGLSurface surf) {
    if (!mActive.load(std::memory_order_acquire) || mBroken) return;
    if (!isMainSurface(dpy, surf)) return;
    std::unique_lock<std::mutex> lk(mFrameMutex, std::try_to_lock);
    if (!lk.owns_lock()) return; // disable() sedang berjalan
    try {
        drawFrame(dpy, surf);
    } catch (const std::exception& e) {
        LYR_LOGE("Overlay dimatikan karena exception: %s", e.what());
        mBroken = true;
    } catch (...) {
        LYR_LOGE("Overlay dimatikan karena exception tak dikenal");
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
        LYR_LOGI("config.json dimuat ulang");
    }
    mCfgStamp = fs::last_write_time(configPath(), ec); // load() bisa menulis ulang file
}

void LyricsMod::drawFrame(EGLDisplay dpy, EGLSurface surf) {
    // Hanya gambar di surface utama yang sedang aktif (abaikan surface lain, mis. offscreen).
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT || eglGetCurrentSurface(EGL_DRAW) != surf) return;

    JNIEnv* env = lyrics::jni::env();
    if (!env) return;

    if (!mGlChecked || ctx != mCtx) {
        const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        if (!ver || std::strstr(ver, "OpenGL ES 3") == nullptr) {
            LYR_LOGE("Butuh OpenGL ES 3.x, ditemukan: %s. Overlay dimatikan.", ver ? ver : "?");
            mBroken = true;
            return;
        }
        if (mRenderer && mCtx != EGL_NO_CONTEXT && ctx != mCtx) {
            LYR_LOGI("Konteks GL berganti; membangun ulang resource");
            mRenderer->invalidateGl();
        }
        mCtx = ctx;
        mGlChecked = true;
    }

    if (!mController) mController = std::make_unique<lyrics::LyricsController>(fs::path(mSelf->getDataDir()), mCfg);
    if (!mRenderer) {
        mRenderer = std::make_unique<lyrics::LyricsRenderer>();
        if (!mRenderer->init(env)) {
            LYR_LOGE("Renderer gagal diinisialisasi; overlay dimatikan");
            mBroken = true;
            return;
        }
    }

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - mT0).count();
    reloadConfigIfChanged(t);
    if (mMenuDeferred && t > 6.0) { // tahap 3: sekali saja, setelah game berjalan beberapa detik
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
    float cy, cp;
    {
        std::lock_guard<std::mutex> lk(mCamMutex);
        cy = mCamYaw;
        cp = mCamPitch;
    }
    mRenderer->render(env, mCfg, mController->view(), w, h, t, cy, cp);
}

bool LyricsMod::onTouch(const pl::input::TouchEvent& e) {
    if (!mActive.load(std::memory_order_acquire)) return false;
    mTouchCount.fetch_add(1, std::memory_order_relaxed);
    if (mTouchLogged.load(std::memory_order_relaxed) < 12) {
        mTouchLogged.fetch_add(1, std::memory_order_relaxed);
        LYR_LOGI("touch: action=%d id=%d x=%.0f y=%.0f", e.action, e.pointerId, e.x, e.y);
    }
    const int a = e.action & 0xFF; // gaya MotionEvent: 0/5 turun, 2 geser, 1/6/3 angkat
    const int sw = mScrW.load(std::memory_order_relaxed);
    const bool inZone = sw <= 0 || e.x > mTZone.load(std::memory_order_relaxed) * static_cast<float>(sw);
    std::lock_guard<std::mutex> lk(mCamMutex);
    if (a == 0 || a == 5) {
        mPtrs[e.pointerId] = TouchPtr{e.x, e.y, inZone};
        return false;
    }
    if (a == 1 || a == 6 || a == 3) {
        mPtrs.erase(e.pointerId);
        return false;
    }
    if (a != 2) return false;
    auto it = mPtrs.find(e.pointerId);
    if (it == mPtrs.end()) {
        mPtrs[e.pointerId] = TouchPtr{e.x, e.y, inZone};
        return false;
    }
    const float dx = e.x - it->second.x, dy = e.y - it->second.y;
    it->second.x = e.x;
    it->second.y = e.y;
    if (!it->second.inZone || std::fabs(dx) > 400.0f || std::fabs(dy) > 400.0f) return false;
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

// Mod Menu milik launcher pernah membuat game crash di dalam registerModule() (tombstone: libpreloader.so).
// Karena penyebab pastinya belum diketahui, pendaftaran dilakukan bertahap dan hasilnya diingat di
// menu_state_v2.txt: kalau game crash saat mendaftar, peluncuran berikutnya otomatis memakai cara yang
// lebih sederhana. Hapus menu_state_v2.txt untuk mengulang dari tahap 1.
//   tahap 1: modul lengkap (pengaturan + callback) di enable()
//   tahap 2: modul polos (tanpa pengaturan/callback) di enable()
//   tahap 3: modul polos, didaftarkan dari thread render beberapa detik setelah game jalan
//   tahap 4+: Mod Menu tidak dipakai
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
        LYR_LOGW("Peluncuran sebelumnya crash saat registerModule (tahap %d) -> naik ke tahap berikutnya", mPlanStage);
        ++mPlanStage;
        mPlanButtons = true;
    } else if (phase == "B") {
        LYR_LOGW("Peluncuran sebelumnya crash saat registerButton -> tombol layar dimatikan");
        mPlanButtons = false;
    }
    saveMenuPhase("OK");
}

void LyricsMod::saveMenuPhase(const char* phase) {
    std::ofstream out(menuStatePath(), std::ios::out | std::ios::trunc);
    out << "stage=" << mPlanStage << "\nbuttons=" << (mPlanButtons ? 1 : 0) << "\nphase=" << phase << "\n";
    out.flush(); // harus sudah tertulis ke disk sebelum memanggil Mod Menu (yang mungkin crash)
}

bool LyricsMod::registerMenu() {
    if (mMenuRegistered) return true;
    namespace mm = pl::modmenu;
    if (mPlanStage > 3) {
        LYR_LOGW("Mod Menu tidak dipakai (semua cara pendaftaran bermasalah). Kontrol: control.txt / config.json");
        return false;
    }
    // Pakai id asli yang dipegang launcher (sesuai contoh di dokumentasi: getSelf().getId()).
    std::string modId = mSelf ? std::string(mSelf->getId()) : std::string();
    if (modId.empty()) modId = "lyrics3d";
    const bool full = (mPlanStage == 1);
    LYR_LOGI("Mendaftarkan Mod Menu: tahap %d, modId=\"%s\"", mPlanStage, modId.c_str());

    mm::ModuleBuilder mb(kModuleId, "Lyrics 3D");
    mb.modId(modId).description("Putar lagu dari HP dan tampilkan liriknya sebagai teks 3D di layar.").defaultEnabled(true);
    if (full) {
        mb.onToggle([](std::string_view, bool enabled) {
              LyricsMod& m = instance();
              m.queueSetting("enabled", enabled ? "true" : "false");
              // Mati -> jeda musik. Hidup lagi -> lanjut (hanya jika tadi dimatikan lewat menu,
              // supaya "Putar otomatis" di config.json tetap dihormati saat game baru mulai).
              if (!enabled) {
                  m.mMenuWasOff.store(true);
                  m.queueCmd(Cmd::Pause);
              } else if (m.mMenuWasOff.exchange(false)) {
                  m.queueCmd(Cmd::Play);
              }
          })
            .onConfigChanged([](std::string_view, std::string_view key, std::string_view value) {
                LyricsMod& m = instance();
                if (key.rfind("act_", 0) == 0) { // tombol di menu (nilai aksi = "true")
                    if (value != "true") return;
                    if (key == "act_prev") m.queueCmd(Cmd::Prev);
                    else if (key == "act_next") m.queueCmd(Cmd::Next);
                    else if (key == "act_toggle") m.queueCmd(Cmd::Toggle);
                    else if (key == "act_stop") m.queueCmd(Cmd::Stop);
                    else if (key == "act_rescan") m.queueCmd(Cmd::Rescan);
                } else if (key == "song") { // daftar lagu
                    const std::string v(value);
                    if (!v.empty()) m.queueCmd(Cmd::PlayIndex, std::atoi(v.c_str()));
                } else {
                    m.queueSetting(std::string(key), std::string(value));
                }
            })
            .config("volume", "Volume", mm::ConfigType::SliderFloat, "0.8", "0", "1")
            .config("fontSize", "Ukuran huruf", mm::ConfigType::SliderFloat, "0.055", "0.02", "0.12")
            .config("positionY", "Posisi vertikal", mm::ConfigType::SliderFloat, "0.55", "-1", "1")
            .config("opacity", "Transparansi", mm::ConfigType::SliderFloat, "1", "0.1", "1")
            .config("lyricsOffsetMs", "Geser lirik (ms)", mm::ConfigType::SliderInt, "0", "-2000", "2000")
            .config("effect3d", "Efek 3D (miring + tebal)", mm::ConfigType::Toggle, "true")
            .config("showContext", "Baris sebelum/sesudah", mm::ConfigType::Toggle, "true")
            .config("loop", "Ulangi playlist", mm::ConfigType::Toggle, "true")
            .config("shuffle", "Acak urutan", mm::ConfigType::Toggle, "false");
    }

    saveMenuPhase("M");
    const bool ok = mb.registerModule();
    saveMenuPhase("OK");
    LYR_LOGI("registerModule -> %s", ok ? "true" : "false");
    if (!ok) return false;
    mMenuRegistered = true;

    if (!mPlanButtons) {
        LYR_LOGI("Tombol layar dilewati");
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
    makeBtn(kBtnPrev, "Lagu sebelumnya", "Prev", Cmd::Prev);
    makeBtn(kBtnPlay, "Putar / Jeda", "Play", Cmd::Toggle);
    makeBtn(kBtnNext, "Lagu berikutnya", "Next", Cmd::Next);
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
            if (mLoggedChanges < 80) { // bantu diagnosa format nilai dari menu
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
            // Menu bisa mengirim ulang nilai pilihan saat daftar dimuat; abaikan kalau itu lagu yang sedang jalan.
            if (pc.arg != mController->currentIndex()) mController->playIndex(pc.arg);
            break;
        }
    }
}

// Nilai dari menu menimpa config.json selama game berjalan (config.json tetap dibaca untuk
// pengaturan yang tidak ada di menu, mis. warna dan font).
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
        else if (key == "layoutMode" && (val == "world" || val == "screen")) c.layoutMode = val;
        else if (key == "worldFontSize" && isNum) c.worldFontSize = static_cast<float>(clampd(n, 0.03, 0.15));
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

// ---------------------------------------------------------------- simpan pengaturan menu

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
        LYR_LOGI("Memuat %zu pengaturan menu yang tersimpan", mMenuSettings.size());
        rebuildConfig();
    }
}

void LyricsMod::saveMenuSettings() {
    std::ofstream out(menuSettingsPath(), std::ios::out | std::ios::trunc);
    for (const auto& [k, v] : mMenuSettings)
        if (k != "enabled") out << k << '=' << v << '\n';
}

// ---------------------------------------------------------------- skema menu (Mod Menu V2)

std::string LyricsMod::buildSchemaJson() const {
    namespace mm = pl::modmenu;
    const LyricsConfig d{}; // nilai bawaan
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
    sb.category("player", "Pemutar", "Kontrol lagu");
    sb.category("songs", "Daftar lagu", "Pilih lagu dari folder songs");
    sb.category("world", "Mode dunia", "Kata melayang di dunia seperti mod Java");
    sb.category("display", "Tampilan lirik");
    sb.category("party", "Warna & mode pesta", "Gradien, pelangi, kilau, not musik");
    sb.category("effects", "Efek 3D");
    sb.defaultCategory("player");

    // --- Pemutar ---
    std::string nowPlaying = "Belum ada lagu yang diputar";
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
        mm::ConfigNodeV2 n = node("now", "Sedang diputar", "player", mm::ConfigControlTypeV2::Info);
        n.key.clear();
        n.description = nowPlaying;
        sb.node(std::move(n));
    }
    sb.node(button("act_prev", "Sebelumnya", "player"));
    sb.node(button("act_toggle", "Putar / Jeda", "player"));
    sb.node(button("act_next", "Berikutnya", "player"));
    sb.node(button("act_stop", "Berhenti", "player"));
    sb.node(button("act_rescan", "Pindai ulang lagu", "player"));
    sb.node(slider("volume", "Volume", "player", false, 0.0, 1.0, 0.05, "", d.volume, mCfg.volume));
    sb.node(toggle("loop", "Ulangi playlist", "player", d.loop, mCfg.loop));
    sb.node(toggle("shuffle", "Acak urutan", "player", d.shuffle, mCfg.shuffle));

    // --- Daftar lagu ---
    if (songs.empty()) {
        mm::ConfigNodeV2 n = node("nosongs", "Belum ada lagu", "songs", mm::ConfigControlTypeV2::Info);
        n.key.clear();
        n.description = "Taruh file lagu (.mp3 .ogg .m4a .wav .flac) dan lirik .lrc dengan nama yang sama di: " + songsDir +
                        " lalu tekan 'Pindai ulang lagu' di tab Pemutar.";
        sb.node(std::move(n));
    } else {
        mm::ConfigNodeV2 n = node("song", "Pilih lagu", "songs", mm::ConfigControlTypeV2::Choice);
        n.choiceStyle = mm::ConfigChoiceStyleV2::Dropdown;
        n.searchable = true;
        n.description = std::to_string(songs.size()) + " lagu ditemukan";
        for (size_t i = 0; i < songs.size(); ++i) {
            mm::ConfigOptionV2 o;
            o.value = std::to_string(i);
            o.label = songs[i].title + (songs[i].hasLrc ? "" : "  (tanpa lirik)");
            n.options.push_back(std::move(o));
        }
        n = withValue(std::move(n), "", curIdx >= 0 ? std::to_string(curIdx) : std::string());
        n.hasCurrentValue = curIdx >= 0;
        sb.node(std::move(n));
    }

    // --- Mode dunia ---
    {
        mm::ConfigNodeV2 n = node("layoutMode", "Mode tampilan", "world", mm::ConfigControlTypeV2::Choice);
        n.choiceStyle = mm::ConfigChoiceStyleV2::Segmented;
        const std::pair<const char*, const char*> modes[] = {{"world", "Melayang di dunia"}, {"screen", "Baris di layar"}};
        for (const auto& m : modes) {
            mm::ConfigOptionV2 o;
            o.value = m.first;
            o.label = m.second;
            n.options.push_back(std::move(o));
        }
        sb.node(withValue(std::move(n), d.layoutMode, mCfg.layoutMode));
    }
    {
        mm::ConfigNodeV2 n = node("worldhelp", "Cara kerja", "world", mm::ConfigControlTypeV2::Info);
        n.key.clear();
        n.description =
            "Kata muncul satu-satu di depanmu lalu tetap di tempatnya saat kamu memutar kamera. Arah kamera "
            "diperkirakan dari geseran jari di layar, jadi atur 'Sensitivitas putar kamera' sampai kata terasa menempel "
            "di dunia. Kalau bergerak ke arah yang salah, nyalakan 'Balik arah'. Hanya berlaku untuk lagu yang liriknya "
            "bertanda waktu (.lrc).";
        sb.node(std::move(n));
    }
    sb.node(slider("worldFontSize", "Ukuran kata", "world", false, 0.03, 0.15, 0.005, "", d.worldFontSize, mCfg.worldFontSize));
    sb.node(toggle("worldCaps", "HURUF BESAR", "world", d.worldCaps, mCfg.worldCaps));
    sb.node(slider("worldHoldMs", "Lama kata bertahan", "world", true, 0, 6000, 100, "ms", d.worldHoldMs, mCfg.worldHoldMs));
    sb.node(slider("worldSpread", "Sebaran kata", "world", false, 0.2, 1.6, 0.05, "", d.worldSpread, mCfg.worldSpread));
    sb.node(slider("camSens", "Sensitivitas putar kamera", "world", false, 0.02, 1.0, 0.01, "derajat/px", d.camSens, mCfg.camSens));
    sb.node(toggle("camInvertX", "Balik arah kiri-kanan", "world", d.camInvertX, mCfg.camInvertX));
    sb.node(toggle("camInvertY", "Balik arah atas-bawah", "world", d.camInvertY, mCfg.camInvertY));
    sb.node(slider("camZoneLeft", "Zona joystick diabaikan", "world", false, 0.0, 0.7, 0.05, "", d.camZoneLeft, mCfg.camZoneLeft));

    // --- Tampilan ---
    sb.node(slider("fontSize", "Ukuran huruf", "display", false, 0.02, 0.12, 0.005, "", d.fontSize, mCfg.fontSize));
    sb.node(slider("positionY", "Posisi vertikal", "display", false, -1.0, 1.0, 0.05, "", d.positionY, mCfg.positionY));
    sb.node(slider("opacity", "Transparansi", "display", false, 0.1, 1.0, 0.05, "", d.opacity, mCfg.opacity));
    sb.node(slider("lyricsOffsetMs", "Geser lirik", "display", true, -2000, 2000, 50, "ms", d.lyricsOffsetMs, mCfg.lyricsOffsetMs));
    sb.node(toggle("showContext", "Baris sebelum/sesudah", "display", d.showContext, mCfg.showContext));
    sb.node(toggle("bold", "Huruf tebal", "display", d.bold, mCfg.bold));
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
    sb.node(color("sungColor", "Warna sudah dinyanyikan", d.sungColor, mCfg.sungColor));
    sb.node(color("unsungColor", "Warna belum dinyanyikan", d.unsungColor, mCfg.unsungColor));
    sb.node(color("extrudeColor", "Warna sisi tebal 3D", d.extrudeColor, mCfg.extrudeColor));

    // --- Warna & mode pesta ---
    {
        mm::ConfigNodeV2 n = node("colorMode", "Mode warna", "party", mm::ConfigControlTypeV2::Choice);
        n.choiceStyle = mm::ConfigChoiceStyleV2::Segmented;
        const std::pair<const char*, const char*> modes[] = {{"solid", "Satu warna"}, {"gradient", "Gradien"}, {"rainbow", "Pelangi"}};
        for (const auto& m : modes) {
            mm::ConfigOptionV2 o;
            o.value = m.first;
            o.label = m.second;
            n.options.push_back(std::move(o));
        }
        sb.node(withValue(std::move(n), d.colorMode, mCfg.colorMode));
    }
    {
        mm::ConfigNodeV2 n = color("gradientColor", "Warna ujung gradien", d.gradientColor, mCfg.gradientColor);
        n.category = "party";
        n.visibleWhen.push_back({"colorMode", mm::ConfigConditionOpV2::Equals, "gradient"});
        sb.node(std::move(n));
    }
    {
        mm::ConfigNodeV2 n = slider("rainbowSpeed", "Kecepatan pelangi", "party", false, 0.0, 2.0, 0.05, "", d.rainbowSpeed, mCfg.rainbowSpeed);
        n.visibleWhen.push_back({"colorMode", mm::ConfigConditionOpV2::Equals, "rainbow"});
        sb.node(std::move(n));
    }
    sb.node(toggle("sparkle", "Kilau", "party", d.sparkle, mCfg.sparkle));
    {
        mm::ConfigNodeV2 n = slider("sparkleAmount", "Jumlah kilau", "party", false, 0.0, 1.0, 0.05, "", d.sparkleAmount, mCfg.sparkleAmount);
        n.enabledWhen.push_back({"sparkle", mm::ConfigConditionOpV2::Truthy, ""});
        sb.node(std::move(n));
    }
    sb.node(toggle("partyNotes", "Not musik (mode pesta)", "party", d.partyNotes, mCfg.partyNotes));
    {
        mm::ConfigNodeV2 n = slider("notesAmount", "Jumlah not musik", "party", false, 0.0, 1.0, 0.05, "", d.notesAmount, mCfg.notesAmount);
        n.enabledWhen.push_back({"partyNotes", mm::ConfigConditionOpV2::Truthy, ""});
        sb.node(std::move(n));
    }

    // --- Efek 3D ---
    const auto it = mMenuSettings.find("effect3d");
    const bool effect3d = (it == mMenuSettings.end()) ? true : parseBool(it->second, true);
    sb.node(toggle("effect3d", "Efek 3D (miring + tebal)", "effects", true, effect3d));
    sb.node(slider("glow", "Cahaya", "effects", false, 0.0, 2.0, 0.1, "", d.glow, mCfg.glow));
    sb.node(slider("maxYawDeg", "Ayunan kiri-kanan", "effects", false, 0.0, 30.0, 1.0, "derajat", d.maxYawDeg, mFileCfg.maxYawDeg));
    sb.node(slider("floatAmount", "Efek melayang", "effects", false, 0.0, 0.08, 0.005, "", d.floatAmount, mFileCfg.floatAmount));
    sb.node(slider("transitionMs", "Kecepatan animasi pindah baris", "effects", true, 100, 1200, 50, "ms", d.transitionMs, mCfg.transitionMs));
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
    LYR_LOGI("setConfigSchemaJson (lagu=%zu, sekarang=%d) -> %s", mController->trackCount(), idx, ok ? "true" : "false");
}
