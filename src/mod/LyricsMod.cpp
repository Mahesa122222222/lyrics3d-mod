#include "LyricsMod.h"

#include <GLES3/gl3.h>
#include <dlfcn.h>

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
}

fs::path LyricsMod::configPath() const { return fs::path(mSelf->getConfigDir()) / "config.json"; }

// ---------------------------------------------------------------- lifecycle

bool LyricsMod::load() {
    try {
        lyrics::log::setSink(&LyricsMod::logSink);
        lyrics::jni::setJavaVM(mSelf->getJavaVM());

        std::error_code ec;
        fs::create_directories(fs::path(mSelf->getDataDir()) / "songs", ec);

        mConfig.emplace();
        if (!mConfig->load()) LYR_LOGW("Gagal membaca config.json; memakai nilai bawaan");
        mCfg = mConfig->value();
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
        return true;
    } catch (const std::exception& e) {
        LYR_LOGE("enable() gagal: %s", e.what());
        mActive.store(false);
        return false;
    }
}

bool LyricsMod::disable() {
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

void LyricsMod::beforeSwap(EGLDisplay dpy, EGLSurface surf) {
    if (!mActive.load(std::memory_order_acquire) || mBroken) return;
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
        mCfg = mConfig->value();
        if (mController) mController->applyConfig(mCfg);
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
    mController->tick(t);
    if (!mCfg.enabled) return;
    mRenderer->render(env, mCfg, mController->view(), w, h, t);
}
