#include "GameCamera.h"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#include <fstream>
#include <cstring>

#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "Log.h"

namespace lyrics {

namespace {
using UniformMatrix4fvFn = void (*)(GLint, GLsizei, GLboolean, const GLfloat*);
using Uniform4fvFn = void (*)(GLint, GLsizei, const GLfloat*);
using BufferSubDataFn = void (*)(GLenum, intptr_t, intptr_t, const void*);
using BufferDataFn = void (*)(GLenum, intptr_t, const void*, GLenum);
using MapRangeFn = void* (*)(GLenum, intptr_t, intptr_t, GLbitfield);
using UnmapFn = GLboolean (*)(GLenum);
using FlushFn = void (*)(GLenum, intptr_t, intptr_t);
BufferSubDataFn gOrigSub = nullptr;
BufferDataFn gOrigData = nullptr;
MapRangeFn gOrigMap = nullptr;
UnmapFn gOrigUnmap = nullptr;
FlushFn gOrigFlush = nullptr;
constexpr unsigned kMapWrite = 0x0002, kMapFlushExplicit = 0x0010, kMapPersistent = 0x0040;
constexpr intptr_t kMaxBlob = 256 * 1024;
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

void detSub(GLenum target, intptr_t off, intptr_t size, const void* data) {
    if (data && size > 0) GameCamera::instance().onBlob(data, static_cast<size_t>(size), target, 0);
    if (gOrigSub) gOrigSub(target, off, size, data);
}
void detData(GLenum target, intptr_t size, const void* data, GLenum usage) {
    if (data && size > 0) GameCamera::instance().onBlob(data, static_cast<size_t>(size), target, 1);
    if (gOrigData) gOrigData(target, size, data, usage);
}
void* detMap(GLenum target, intptr_t off, intptr_t len, GLbitfield access) {
    void* p = gOrigMap ? gOrigMap(target, off, len, access) : nullptr;
    GameCamera::MapInfo m;
    m.ptr = p;
    m.len = len;
    m.access = access;
    GameCamera::instance().setMap(target, m);
    return p;
}
GLboolean detUnmap(GLenum target) {
    GameCamera::MapInfo m;
    if (GameCamera::instance().getMap(target, m) && m.ptr && (m.access & kMapWrite) && !(m.access & kMapFlushExplicit) &&
        m.len > 0)
        GameCamera::instance().onBlob(m.ptr, static_cast<size_t>(m.len), target, 2);
    GameCamera::MapInfo none;
    GameCamera::instance().setMap(target, none);
    return gOrigUnmap ? gOrigUnmap(target) : 0;
}
void detFlush(GLenum target, intptr_t off, intptr_t len) {
    GameCamera::MapInfo m;
    if (GameCamera::instance().getMap(target, m) && m.ptr && len > 0)
        GameCamera::instance().onBlob(static_cast<char*>(m.ptr) + off, static_cast<size_t>(len), target, 3);
    if (gOrigFlush) gOrigFlush(target, off, len);
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
        LYR_LOGW("Real camera: GLES library not found");
        return false;
    }
    void* tm = dlsym(lib, "glUniformMatrix4fv");
    void* tv = dlsym(lib, "glUniform4fv");
    bool any = false;
    if (tm) {
        mHookM = pl::memory::HookHandle(tm, reinterpret_cast<void*>(&detM), reinterpret_cast<void**>(&gOrigM),
                                        pl::memory::HookPriority::Normal);
        if (mHookM.installed()) any = true;
        else LYR_LOGW("Real camera: glUniformMatrix4fv hook failed");
    }
    if (tv) {
        mHookV = pl::memory::HookHandle(tv, reinterpret_cast<void*>(&detV), reinterpret_cast<void**>(&gOrigV),
                                        pl::memory::HookPriority::Normal);
        if (mHookV.installed()) any = true;
        else LYR_LOGW("Real camera: glUniform4fv hook failed");
    }
    auto hookOne = [&](const char* name, void* detour, void** orig, pl::memory::HookHandle& hnd) {
        void* t = dlsym(lib, name);
        if (!t) {
            LYR_LOGW("Real camera: %s not found", name);
            return;
        }
        hnd = pl::memory::HookHandle(t, detour, orig, pl::memory::HookPriority::Normal);
        if (hnd.installed()) any = true;
        else LYR_LOGW("Real camera: %s hook failed", name);
    };
    hookOne("glBufferSubData", reinterpret_cast<void*>(&detSub), reinterpret_cast<void**>(&gOrigSub), mHookSub);
    hookOne("glBufferData", reinterpret_cast<void*>(&detData), reinterpret_cast<void**>(&gOrigData), mHookData);
    hookOne("glMapBufferRange", reinterpret_cast<void*>(&detMap), reinterpret_cast<void**>(&gOrigMap), mHookMap);
    hookOne("glUnmapBuffer", reinterpret_cast<void*>(&detUnmap), reinterpret_cast<void**>(&gOrigUnmap), mHookUnmap);
    hookOne("glFlushMappedBufferRange", reinterpret_cast<void*>(&detFlush), reinterpret_cast<void**>(&gOrigFlush),
            mHookFlush);
    mInstalled.store(any);
    LYR_LOGI("Real camera: GL hooks installed=%d (matrix=%d, vec4=%d)", any ? 1 : 0, mHookM.installed() ? 1 : 0,
             mHookV.installed() ? 1 : 0);
    return any;
}

void GameCamera::uninstall() {
    mInstalled.store(false);
    mHookM.reset();
    mHookV.reset();
    mHookSub.reset();
    mHookData.reset();
    mHookMap.reset();
    mHookUnmap.reset();
    mHookFlush.reset();
}

void GameCamera::setMap(unsigned target, const MapInfo& m) {
    std::lock_guard<std::mutex> lk(mMutex);
    if (m.ptr) {
        mNMap.fetch_add(1);
        if (m.access & kMapPersistent) mNPersist.fetch_add(1);
        mMaps[target] = m;
    } else {
        mMaps.erase(target);
        mNUnmap.fetch_add(1);
    }
}

bool GameCamera::getMap(unsigned target, MapInfo& m) {
    std::lock_guard<std::mutex> lk(mMutex);
    auto it = mMaps.find(target);
    if (it == mMaps.end()) return false;
    m = it->second;
    return true;
}

// Scans a blob of uploaded data for 4x4 matrices (every float position). The game sends its camera matrices to the GPU
// in uniform buffers, so this is where the real camera lives.
void GameCamera::onBlob(const void* data, size_t bytes, unsigned target, int kind) {
    if (kind == 0) mNSub.fetch_add(1);
    else if (kind == 1) mNData.fetch_add(1);
    else mNFlush.fetch_add(1);
    if (bytes < 64 || static_cast<intptr_t>(bytes) > kMaxBlob) {
        if (bytes >= 64) mBigSkipped.fetch_add(1);
        return;
    }
    if (target == 0x8893 /*ELEMENT_ARRAY*/) return;
    long left = 400 * 1024 - mBudget.load();
    if (left <= 0) return;
    size_t n = std::min<size_t>(bytes, static_cast<size_t>(left));
    mBudget.fetch_add(static_cast<long>(n));
    mBytes.fetch_add(static_cast<long>(n));
    const unsigned char* p = static_cast<const unsigned char*>(data);
    const size_t floats = n / 4;
    if (floats < 16) return;
    // copy to an aligned local buffer (the source may be unaligned or write-combined memory)
    std::vector<float> buf(floats);
    std::memcpy(buf.data(), p, floats * 4);
    for (size_t i = 0; i + 16 <= floats; ++i) {
        const float* m = buf.data() + i;
        // quick rejects: a matrix has a 0 or 1 or -1 somewhere in its last element / projection terms
        const float a15 = m[15];
        const bool rigidLike = std::fabs(a15 - 1.0f) < 1e-3f;
        const bool projLike = std::fabs(a15) < 1e-3f;
        const float len = m[3] * m[3] + m[7] * m[7] + m[11] * m[11];
        const float len2 = m[12] * m[12] + m[13] * m[13] + m[14] * m[14];
        if (!rigidLike && !projLike && !(std::fabs(len - 1.0f) < 0.03f) && !(std::fabs(len2 - 1.0f) < 0.03f)) continue;
        analyze(m);
    }
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

void GameCamera::record(int kind, float yaw, float pitch) {
    // identity rotation = chunk/world offsets, not a camera
    if (std::fabs(yaw) < 0.0035f && std::fabs(pitch) < 0.0035f) return;
    const int qy = static_cast<int>(std::lround(yaw * 57.2958f * 4.0f));
    const int qp = static_cast<int>(std::lround(pitch * 57.2958f * 4.0f));
    const int key = qy * 1000 + qp;
    std::lock_guard<std::mutex> lk(mMutex);
    auto& cnt = kind == 0 ? mCntA : (kind == 1 ? mCntB : mCntC);
    auto& val = kind == 0 ? mValA : (kind == 1 ? mValB : mValC);
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
        // --- view * projection (the usual "WorldViewProj"): its last row is the camera's forward direction
        {
            const float fx = M[3], fy = M[7], fz = M[11];
            const float len = fx * fx + fy * fy + fz * fz;
            if (near(len, 1.0f, 0.03f) && std::fabs(M[15]) < 1.0e5f) {
                const float il = 1.0f / std::sqrt(len);
                record(2, std::atan2(fx * il, -fz * il), std::asin(std::max(-1.0f, std::min(1.0f, fy * il))));
            }
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
            record(0, std::atan2(fx, -fz), std::asin(std::max(-1.0f, std::min(1.0f, fy))));
        }
        // camera -> world: forward = -(column 2)
        {
            const float fx = -c2x, fy = -c2y, fz = -c2z;
            record(1, std::atan2(fx, -fz), std::asin(std::max(-1.0f, std::min(1.0f, fy))));
        }
    }
}

GamePose GameCamera::endFrame(double timeNow) {
    mBudget.store(0);
    if (mLogN < 120 && timeNow - mLogT > 2.0) {
        mLogT = timeNow;
        ++mLogN;
        LYR_LOGI("realcam-buffers: subData=%d bufData=%d map=%d unmap=%d flush=%d persistent=%d scannedKB=%ld tooBig=%d",
                 mNSub.exchange(0), mNData.exchange(0), mNMap.exchange(0), mNUnmap.exchange(0), mNFlush.exchange(0),
                 mNPersist.exchange(0), mBytes.exchange(0) / 1024, mBigSkipped.exchange(0));
    }
    GamePose p;
    std::lock_guard<std::mutex> lk(mMutex);
    // Many different matrices are uploaded each frame. Keep following the one that was picked last time (the camera
    // moves smoothly); when there is none close enough, take the most frequent one.
    auto pick = [&](int kind, const std::unordered_map<int, int>& cnt,
                    const std::unordered_map<int, std::pair<float, float>>& val, bool& ok, float& yaw, float& pitch) {
        int bestKey = 0, best = 0;
        int nearKey = 0, nearCnt = 0;
        float nearD = 0.35f;
        for (const auto& kv : cnt) {
            if (kv.second > best || (kv.second == best && kv.first < bestKey)) best = kv.second, bestKey = kv.first;
            if (kv.second >= 2 && mHave[kind]) {
                const auto& v = val.at(kv.first);
                float dy = std::fabs(v.first - mLastYaw[kind]);
                if (dy > 3.14159f) dy = 6.28318f - dy;
                const float d = dy + std::fabs(v.second - mLastPitch[kind]);
                if (d < nearD) nearD = d, nearKey = kv.first, nearCnt = kv.second;
            }
        }
        if (nearCnt > 0) bestKey = nearKey, best = nearCnt;
        if (best >= 2) {
            ok = true;
            yaw = val.at(bestKey).first;
            pitch = val.at(bestKey).second;
            mHave[kind] = true;
            mLastYaw[kind] = yaw;
            mLastPitch[kind] = pitch;
        }
    };
    pick(0, mCntA, mValA, p.a, p.ayaw, p.apitch);
    pick(1, mCntB, mValB, p.b, p.byaw, p.bpitch);
    pick(2, mCntC, mValC, p.c, p.cyaw, p.cpitch);
    p.persp = mPersp;
    p.fovV = mFov;
    p.calls = mCalls;
    p.rigid = mRigid;
    mCntA.clear();
    mCntB.clear();
    mCntC.clear();
    mValA.clear();
    mValB.clear();
    mValC.clear();
    mPersp = false;
    mCalls = mRigid = 0;
    return p;
}

} // namespace lyrics

namespace lyrics {

namespace {
struct ScoutCtx {
    const char* base = nullptr;
    const ElfW(Dyn)* dyn = nullptr;
    std::string path;
    bool found = false;
};
int scoutCb(struct dl_phdr_info* info, size_t, void* data) {
    auto* c = static_cast<ScoutCtx*>(data);
    if (!info->dlpi_name || !std::strstr(info->dlpi_name, "libminecraftpe.so")) return 0;
    c->found = true;
    c->base = reinterpret_cast<const char*>(info->dlpi_addr);
    c->path = info->dlpi_name;
    for (int i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
            c->dyn = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    return 1;
}
} // namespace

int dumpGameSymbols(const std::string& outPath) {
    ScoutCtx ctx;
    dl_iterate_phdr(scoutCb, &ctx);
    std::ofstream out(outPath, std::ios::out | std::ios::trunc);
    if (!out) return -1;
    if (!ctx.found || !ctx.dyn) {
        out << "libminecraftpe.so not found\n";
        return -1;
    }
    const char* strtab = nullptr;
    const ElfW(Sym)* symtab = nullptr;
    size_t strsz = 0;
    for (const ElfW(Dyn)* d = ctx.dyn; d->d_tag != DT_NULL; ++d) {
        auto ptr = [&](ElfW(Addr) v) { return v >= reinterpret_cast<ElfW(Addr)>(ctx.base) ? v : reinterpret_cast<ElfW(Addr)>(ctx.base) + v; };
        if (d->d_tag == DT_STRTAB) strtab = reinterpret_cast<const char*>(ptr(d->d_un.d_ptr));
        else if (d->d_tag == DT_SYMTAB) symtab = reinterpret_cast<const ElfW(Sym)*>(ptr(d->d_un.d_ptr));
        else if (d->d_tag == DT_STRSZ) strsz = d->d_un.d_val;
    }
    out << "module: " << ctx.path << "\n";
    if (!strtab || !symtab || strtab <= reinterpret_cast<const char*>(symtab)) {
        out << "no dynamic symbol table\n";
        return -1;
    }
    size_t count = static_cast<size_t>(strtab - reinterpret_cast<const char*>(symtab)) / sizeof(ElfW(Sym));
    if (count > 3000000) count = 3000000;
    out << "dynamic symbols: " << count << " (strtab " << strsz << " bytes)\n";
    static const char* kKeys[] = {"getLocalPlayer", "ClientInstance", "Camera", "ViewMatrix", "ProjMatrix", "ProjectionMatrix",
                                  "WorldToScreen", "LevelRendererPlayer", "GameRenderer", "getHeadRot", "getRotation",
                                  "PlayerRot", "getViewport", "ScreenContext", "MatrixStack", "FOV", "Fov"};
    int lines = 0;
    for (const char* key : kKeys) {
        int perKey = 0;
        out << "## " << key << "\n";
        for (size_t i = 0; i < count && perKey < 400; ++i) {
            const ElfW(Sym)& s = symtab[i];
            if (s.st_name == 0 || s.st_name >= strsz || s.st_shndx == SHN_UNDEF) continue;
            const char* nm = strtab + s.st_name;
            if (nm[0] != '_' || nm[1] != 'Z' || !std::strstr(nm, key)) continue;
            const char* t = (ELF64_ST_TYPE(s.st_info) == STT_FUNC) ? "F" : "O";
            out << t << ' ' << nm << '\n';
            ++perKey;
            ++lines;
        }
    }
    return lines;
}

} // namespace lyrics
