#include "LyricsMod.h"

#include <GLES3/gl3.h>
#include <dlfcn.h>

#include <cstdlib>
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
double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

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
    mController->tick(t);
    if (!mCfg.enabled) return;
    mRenderer->render(env, mCfg, mController->view(), w, h, t);
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
                instance().queueSetting(std::string(key), std::string(value));
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

void LyricsMod::queueCmd(Cmd c) {
    std::lock_guard<std::mutex> lk(mMenuMutex);
    if (mPendingCmds.size() < 32) mPendingCmds.push_back(c);
}

void LyricsMod::queueSetting(std::string key, std::string value) {
    std::lock_guard<std::mutex> lk(mMenuMutex);
    if (mPendingSettings.size() < 256) mPendingSettings.emplace_back(std::move(key), std::move(value));
}

void LyricsMod::drainMenuQueue() {
    std::vector<Cmd> cmds;
    std::vector<std::pair<std::string, std::string>> settings;
    {
        std::lock_guard<std::mutex> lk(mMenuMutex);
        cmds.swap(mPendingCmds);
        settings.swap(mPendingSettings);
    }
    if (!settings.empty()) {
        for (auto& kv : settings) mMenuSettings[kv.first] = kv.second;
        rebuildConfig();
    }
    if (!mController) return;
    for (Cmd c : cmds) {
        switch (c) {
        case Cmd::Play: mController->play(); break;
        case Cmd::Pause: mController->pause(); break;
        case Cmd::Toggle: mController->toggle(); break;
        case Cmd::Next: mController->next(); break;
        case Cmd::Prev: mController->prev(); break;
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
