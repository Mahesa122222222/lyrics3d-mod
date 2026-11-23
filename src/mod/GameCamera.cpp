#include "GameCamera.h"

#include <dlfcn.h>

#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>

#include "Log.h"

namespace lyrics {

namespace {
using UniformMatrix4fvFn = void (*)(GLint, GLsizei, GLboolean, const GLfloat*);
using Uniform4fvFn = void (*)(GLint, GLsizei, const GLfloat*);
UniformMatrix4fvFn gOrigM = nullptr;
Uniform4fvFn gOrigV = nullptr;

void detM(GLint loc, GLsizei count, GLboolean tr, const GLfloat* v) {
    GameCamera::instance().onMatrices(v, static_cast<int>(count), tr != 0);
    if (gOrigM) gOrigM(loc, count, tr, v);
}
void detV(GLint loc, GLsizei count, const GLfloat* v) {
    GameCamera::instance().onVec4(v, static_cast<int>(count));
    if (gOrigV) gOrigV(loc, count, v);
}

inline bool near(float a, float b, float eps) { return std::fabs(a - b) < eps; }
} // namespace

GameCamera& GameCamera::instance() {
    static GameCamera g;
    return g;
}

bool GameCamera::install() {
    if (mInstalled.load()) return true;
    void* lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen("libGLESv3.so", RTLD_NOW);
    if (!lib) {
        LYR_LOGW("Java mode: GLES library not found");
        return false;
    }
    void* tm = dlsym(lib, "glUniformMatrix4fv");
    void* tv = dlsym(lib, "glUniform4fv");
    bool any = false;
    if (tm) {
        mHookM = pl::memory::HookHandle(tm, reinterpret_cast<void*>(&detM), reinterpret_cast<void**>(&gOrigM),
                                        pl::memory::HookPriority::Normal);
        if (mHookM.installed()) any = true;
        else LYR_LOGW("Java mode: glUniformMatrix4fv hook failed");
    }
    if (tv) {
        mHookV = pl::memory::HookHandle(tv, reinterpret_cast<void*>(&detV), reinterpret_cast<void**>(&gOrigV),
                                        pl::memory::HookPriority::Normal);
        if (mHookV.installed()) any = true;
        else LYR_LOGW("Java mode: glUniform4fv hook failed");
    }
    mInstalled.store(any);
    LYR_LOGI("Java mode: GL hooks installed=%d (matrix=%d, vec4=%d)", any ? 1 : 0, mHookM.installed() ? 1 : 0,
             mHookV.installed() ? 1 : 0);
    return any;
}

void GameCamera::uninstall() {
    mInstalled.store(false);
    mHookM.reset();
    mHookV.reset();
}

void GameCamera::onMatrices(const float* v, int count, bool transposed) {
    if (!v || count <= 0) return;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mCalls += 1;
    }
    if (count > 8) return;
    for (int i = 0; i < count; ++i) {
        const float* p = v + 16 * i;
        if (transposed) {
            float t[16];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) t[c * 4 + r] = p[r * 4 + c];
            analyze(t);
        } else {
            analyze(p);
        }
    }
}

void GameCamera::onVec4(const float* v, int count) {
    if (!v || count < 4 || count > 16 || (count % 4) != 0) return;
    for (int i = 0; i + 4 <= count; i += 4) analyze(v + 4 * i);
}

void GameCamera::record(bool viewLike, float yaw, float pitch) {
    // identity rotation = chunk/world offsets, not a camera
    if (std::fabs(yaw) < 0.0035f && std::fabs(pitch) < 0.0035f) return;
    const int qy = static_cast<int>(std::lround(yaw * 57.2958f * 4.0f));
    const int qp = static_cast<int>(std::lround(pitch * 57.2958f * 4.0f));
    const int key = qy * 1000 + qp;
    std::lock_guard<std::mutex> lk(mMutex);
    auto& cnt = viewLike ? mCntA : mCntB;
    auto& val = viewLike ? mValA : mValB;
    if (cnt.size() > 4000) return; // safety
    cnt[key] += 1;
    val[key] = {yaw, pitch};
}

// m is column-major (what OpenGL would multiply with column vectors). The data is analysed as given and also
// transposed, because we cannot know which convention the game's shaders use.
void GameCamera::analyze(const float* m0) {
    float m[16], t[16];
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m0[i]) || std::fabs(m0[i]) > 1.0e7f) return;
        m[i] = m0[i];
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t[c * 4 + r] = m[r * 4 + c];

    const float aspect = mAspect.load();
    const float* variants[2] = {m, t};
    for (const float* M : variants) {
        // --- perspective projection: m[11] = -1 (or +1), m[15] = 0, no off-diagonal terms
        if ((near(M[11], -1.0f, 1e-3f) || near(M[11], 1.0f, 1e-3f)) && near(M[15], 0.0f, 1e-3f) &&
            near(M[1], 0, 1e-4f) && near(M[2], 0, 1e-4f) && near(M[3], 0, 1e-4f) && near(M[4], 0, 1e-4f) &&
            near(M[6], 0, 1e-4f) && near(M[7], 0, 1e-4f) && M[5] > 0.3f && M[0] > 0.1f && aspect > 0.5f &&
            near(M[5] / M[0], aspect, 0.06f * aspect)) {
            std::lock_guard<std::mutex> lk(mMutex);
            mPersp = true;
            mFov = 2.0f * std::atan(1.0f / M[5]);
            continue;
        }
        // --- rigid transform (rotation + translation): last row 0,0,0,1 and orthonormal 3x3
        if (!(near(M[3], 0, 1e-3f) && near(M[7], 0, 1e-3f) && near(M[11], 0, 1e-3f) && near(M[15], 1.0f, 1e-3f))) continue;
        const float c0x = M[0], c0y = M[1], c0z = M[2], c1x = M[4], c1y = M[5], c1z = M[6], c2x = M[8], c2y = M[9],
                    c2z = M[10];
        const float l0 = c0x * c0x + c0y * c0y + c0z * c0z, l1 = c1x * c1x + c1y * c1y + c1z * c1z,
                    l2 = c2x * c2x + c2y * c2y + c2z * c2z;
        if (!(near(l0, 1.0f, 0.02f) && near(l1, 1.0f, 0.02f) && near(l2, 1.0f, 0.02f))) continue;
        if (std::fabs(c0x * c1x + c0y * c1y + c0z * c1z) > 0.02f || std::fabs(c0x * c2x + c0y * c2y + c0z * c2z) > 0.02f ||
            std::fabs(c1x * c2x + c1y * c2y + c1z * c2z) > 0.02f)
            continue;
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mRigid += 1;
        }
        // world -> camera (view matrix): the camera looks along -Z of camera space, i.e. world direction -(row 2)
        {
            const float fx = -c0z, fy = -c1z, fz = -c2z;
            record(true, std::atan2(fx, -fz), std::asin(std::max(-1.0f, std::min(1.0f, fy))));
        }
        // camera -> world: forward = -(column 2)
        {
            const float fx = -c2x, fy = -c2y, fz = -c2z;
            record(false, std::atan2(fx, -fz), std::asin(std::max(-1.0f, std::min(1.0f, fy))));
        }
    }
}

GamePose GameCamera::endFrame() {
    GamePose p;
    std::lock_guard<std::mutex> lk(mMutex);
    auto pick = [](const std::unordered_map<int, int>& cnt, const std::unordered_map<int, std::pair<float, float>>& val,
                   bool& ok, float& yaw, float& pitch) {
        int bestKey = 0, best = 0;
        for (const auto& kv : cnt)
            if (kv.second > best || (kv.second == best && kv.first < bestKey)) best = kv.second, bestKey = kv.first;
        if (best >= 2) {
            ok = true;
            yaw = val.at(bestKey).first;
            pitch = val.at(bestKey).second;
        }
    };
    pick(mCntA, mValA, p.a, p.ayaw, p.apitch);
    pick(mCntB, mValB, p.b, p.byaw, p.bpitch);
    p.persp = mPersp;
    p.fovV = mFov;
    p.calls = mCalls;
    p.rigid = mRigid;
    mCntA.clear();
    mCntB.clear();
    mValA.clear();
    mValB.clear();
    mPersp = false;
    mCalls = mRigid = 0;
    return p;
}

} // namespace lyrics
