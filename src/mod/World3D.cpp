// Real in-game 3D lyrics. See World3D.h for credits and the license note (GPL-3.0-only).
#include "World3D.h"

#include <link.h>
#include <pl/memory/Hook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "Log.h"
#include "World3DBindings.h"

namespace lyrics::w3d {

namespace {

namespace B = ::lyrics::w3d_bind;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float len2(Vec3 a) { return a.x * a.x + a.y * a.y + a.z * a.z; }
Vec3 normalized(Vec3 a) {
    const float d = std::sqrt(len2(a));
    return d > 1e-5f ? a * (1.0f / d) : Vec3{1, 0, 0};
}
bool plausible(void* p) {
    const auto n = reinterpret_cast<uintptr_t>(p);
    return n > 0x10000 && n % 8 == 0;
}
template <class T>
T readAt(void* p, size_t off) {
    T v{};
    std::memcpy(&v, static_cast<char*>(p) + off, sizeof(T));
    return v;
}
float smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ------------------------------------------------------------------ shared state
std::atomic<State> gState{State::Idle};
std::atomic<bool> gEnabled{false}, gInstalled{false};
std::atomic<int64_t> gLastDraw{0};
std::mutex gLifecycle, gDataMutex, gPoseMutex;
std::thread gInstaller;
const B::Profile* gProfile = nullptr;

struct Pose {
    float pitch = 0, yaw = 0;
    uintptr_t player = 0, world = 0;
    int64_t time = 0;
    Vec3 position{};
};
Pose gPose;

std::shared_ptr<const Cue> gCue; // guarded by gDataMutex
Settings gSettings;              // guarded by gDataMutex
int64_t gPosMs = 0;
bool gPlaying = false;
int64_t gPosSampleAt = 0;

// ------------------------------------------------------------------ game functions
using TickFn = void (*)(void*);
using RenderFn = void (*)(void*, void*, void*);
using TessBegin = void (*)(void*, void*, int, int, int);
using TessColor = void (*)(void*, float, float, float, float);
using TessVertex = void (*)(void*, float, float, float);
using RenderMesh = void (*)(void*, void*, void*, char*);
TickFn gOrigTick = nullptr;
RenderFn gOrigRender = nullptr;
TessBegin gTessBegin = nullptr;
TessColor gTessColor = nullptr;
TessVertex gTessVertex = nullptr;
RenderMesh gRenderMesh = nullptr;
void* gTickTarget = nullptr;
void* gRenderTarget = nullptr;
void* gMaterialGroup = nullptr;
bool gTickHooked = false, gRenderHooked = false;

// Same memory layout as the game's HashedString (hash, string, last match).
struct HashedString {
    uint64_t hash = 0;
    std::string value;
    mutable const HashedString* lastMatch = nullptr;
    explicit HashedString(std::string_view s) : value(s) {
        if (s.empty()) return;
        hash = 0xCBF29CE484222325ULL;
        for (unsigned char c : s) hash = static_cast<uint64_t>(c) ^ (hash * 0x100000001B3ULL);
    }
};
using MaterialPtr = std::shared_ptr<void>;
MaterialPtr gMaterial;
int64_t gMaterialRetry = 0;

void resolveMaterial(int64_t now) {
    if (gMaterial || now < gMaterialRetry || !plausible(gMaterialGroup)) return;
    gMaterialRetry = now + 1000;
    auto** table = readAt<void**>(gMaterialGroup, 0);
    if (!plausible(table) || !table[2]) return;
    using Fn = MaterialPtr (*)(void*, const HashedString*);
    auto fn = reinterpret_cast<Fn>(table[2]);
    for (const char* name : {"name_tag_depth_tested", "name_tag_depth_tested_with_backface", "selection_box"}) {
        HashedString key(name);
        gMaterial = fn(gMaterialGroup, &key);
        if (gMaterial) break;
    }
}

// ------------------------------------------------------------------ player tick: camera angles and position
float gEyeOffset = 0.0f; // camera height above the position stored for the player (measured in first person)

void tickHook(void* player) {
    if (gOrigTick) gOrigTick(player);
    if (!gEnabled.load(std::memory_order_relaxed) || !gProfile || !plausible(player)) return;
    auto* rotation = readAt<void*>(player, gProfile->rotation);
    if (!plausible(rotation)) return;
    const float pitch = readAt<float>(rotation, 0), yaw = readAt<float>(rotation, 4);
    if (!std::isfinite(pitch) || !std::isfinite(yaw) || std::fabs(pitch) > 180.0f) return;
    // The position component sits 0x10 before the rotation component in the audited player layouts.
    auto* state = readAt<void*>(player, gProfile->rotation - 0x10);
    if (!plausible(state)) return;
    const Vec3 pos = readAt<Vec3>(state, 0);
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || len2(pos) > 1e14f) return;
    Pose next;
    next.pitch = pitch;
    next.yaw = std::remainder(yaw, 360.0f);
    next.player = reinterpret_cast<uintptr_t>(player);
    next.world = readAt<uintptr_t>(player, gProfile->level);
    next.time = nowMs();
    next.position = pos;
    std::unique_lock<std::mutex> lock(gPoseMutex, std::try_to_lock);
    if (lock) gPose = next;
}

// ------------------------------------------------------------------ scene
struct Panel {
    int part = 0;
    bool appeared = false;
    int64_t appearedAt = 0;
    Vec3 anchor{}, right{}, facing{};
    float width = 0; // blocks
};
struct Scene {
    uint64_t serial = 0;
    uintptr_t world = 0, player = 0, renderer = 0;
    int64_t lastFrame = 0, prevFadeStart = 0;
    std::shared_ptr<const Cue> cue, prevCue;
    std::vector<Panel> panels, previous;
    float clock = 0; // song position, smoothed
};
Scene gScene; // render thread only

uint64_t hash64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}
float rnd(uint64_t seed) { return static_cast<float>(hash64(seed) % 10001) / 10000.0f; }

// How far apart two panels look from the camera (>= 1 = no overlap).
float clearance(Vec3 a, float wa, Vec3 cam, Vec3 b, float wb) {
    Vec3 da = a - cam, db = b - cam;
    const float la = std::sqrt(len2(da)), lb = std::sqrt(len2(db));
    if (la < 0.01f || lb < 0.01f) return -1.0f;
    const float horizontal = std::hypot(da.x / la - db.x / lb, da.z / la - db.z / lb);
    const float vertical = std::fabs(da.y / la - db.y / lb);
    const float minH = wa * 0.5f / la + wb * 0.5f / lb + 0.07f;
    const float minV = 0.22f / la + 0.22f / lb + 0.035f;
    return std::max(horizontal / minH, vertical / minV);
}

void hsv(float h, float s, float v, float out[3]) {
    h = h - std::floor(h);
    const float c = v * s, x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f)), m = v - c;
    float r = 0, g = 0, b = 0;
    const int k = static_cast<int>(h * 6.0f);
    switch (k % 6) {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x; break;
    }
    out[0] = r + m;
    out[1] = g + m;
    out[2] = b + m;
}

struct ColorGuard {
    float* holder;
    float saved[4];
    explicit ColorGuard(float* p) : holder(p) {
        std::memcpy(saved, p, 16);
        for (int i = 0; i < 4; ++i) p[i] = 1.0f;
    }
    ~ColorGuard() { std::memcpy(holder, saved, 16); }
};

// A few diagnostic lines (one every 5 s, at most 40) telling why nothing is drawn.
void diag(const char* why, size_t quads = 0) {
    static int64_t last = 0;
    static int lines = 0;
    const int64_t now = nowMs();
    if (lines >= 40 || now - last < 5000) return;
    last = now;
    ++lines;
    LYR_LOGI("Real 3D: %s (quads=%zu)", why, quads);
}

void renderHook(void* renderer, void* ctx, void* arg) {
    if (gOrigRender) gOrigRender(renderer, ctx, arg);
    if (!gEnabled.load(std::memory_order_relaxed) || !gInstalled.load(std::memory_order_acquire) || !gProfile) return;
    if (!plausible(renderer) || !plausible(ctx)) {
        diag("render arguments look wrong");
        return;
    }

    const int64_t now = nowMs();
    Pose view;
    {
        std::unique_lock<std::mutex> lock(gPoseMutex, std::try_to_lock);
        if (!lock) return;
        view = gPose;
    }
    if (!view.world || !view.time || now - view.time > 1500) {
        diag("waiting for the player (no tick data)");
        return;
    }

    std::shared_ptr<const Cue> cue;
    Settings cfg;
    int64_t posMs = 0, sampleAt = 0;
    bool playing = false;
    {
        std::unique_lock<std::mutex> lock(gDataMutex, std::try_to_lock);
        if (!lock) return;
        cue = gCue;
        cfg = gSettings;
        posMs = gPosMs;
        playing = gPlaying;
        sampleAt = gPosSampleAt;
    }

    auto* rp = readAt<void*>(renderer, gProfile->rendererPlayer);
    auto* tess = readAt<void*>(ctx, gProfile->tessellator);
    auto* colors = readAt<float*>(ctx, 0x30);
    if (!plausible(rp) || !plausible(tess) || !plausible(colors)) {
        diag("renderer objects not found at the expected offsets");
        return;
    }
    const Vec3 cam = readAt<Vec3>(rp, gProfile->camera);
    if (!std::isfinite(cam.x) || !std::isfinite(cam.y) || !std::isfinite(cam.z) ||
        len2(cam - view.position) > 1024.0f) {
        diag("camera position is not valid");
        return;
    }
    resolveMaterial(now);
    if (!gMaterial) {
        diag("game material not available");
        return;
    }

    Scene& sc = gScene;
    const bool reset = sc.world != view.world || sc.player != view.player ||
                       sc.renderer != reinterpret_cast<uintptr_t>(rp) || now - sc.lastFrame > 1500;
    if (reset) {
        sc.panels.clear();
        sc.previous.clear();
        sc.cue.reset();
        sc.prevCue.reset();
        sc.serial = 0;
        sc.world = view.world;
        sc.player = view.player;
        sc.renderer = reinterpret_cast<uintptr_t>(rp);
    }
    const float dt = static_cast<float>(std::clamp<int64_t>(now - sc.lastFrame, 0, 200));
    sc.lastFrame = now;

    // First person: the camera is at the player's eyes. Measure how high that is above the stored position.
    const bool firstPerson = std::hypot(cam.x - view.position.x, cam.z - view.position.z) < 0.4f;
    if (firstPerson) {
        const float off = cam.y - view.position.y;
        if (off > -0.5f && off < 2.0f) gEyeOffset += (off - gEyeOffset) * 0.1f;
    }

    // Song clock: follow the player's position, smoothly, without drifting away.
    const int64_t age = std::clamp<int64_t>(now - sampleAt, 0, 1200);
    const float observed = static_cast<float>(posMs) + (playing ? static_cast<float>(age) : 0.0f);
    if (!playing || std::fabs(observed - sc.clock) > 1200.0f) sc.clock = observed;
    else {
        const float next = sc.clock + dt;
        sc.clock = next + std::clamp(observed - next, -dt * 0.18f, dt * 0.18f);
    }
    const float position = sc.clock;

    // A new line: the old panels fade out, the new line starts hidden.
    const uint64_t serial = cue ? cue->serial : 0;
    if (serial != sc.serial) {
        sc.previous = std::move(sc.panels);
        sc.prevCue = sc.cue;
        sc.prevFadeStart = now;
        sc.panels.clear();
        sc.cue = cue;
        sc.serial = serial;
        if (cue) {
            sc.panels.resize(cue->parts.size());
            for (size_t i = 0; i < sc.panels.size(); ++i) sc.panels[i].part = static_cast<int>(i);
        }
    }
    if (sc.previous.size() && now - sc.prevFadeStart > 900) {
        sc.previous.clear();
        sc.prevCue.reset();
    }

    const float cell = cfg.textBlocks / std::max(1.0f, sc.cue ? sc.cue->cellsPerEm : 32.0f);

    // Reveal parts when they are sung and give each one its own fixed place in the world.
    if (sc.cue) {
        const Cue& c = *sc.cue;
        const float duration = static_cast<float>(std::max<int64_t>(c.endMs - c.startMs, 600));
        for (auto& p : sc.panels) {
            if (p.appeared) continue;
            const Part& part = c.parts[static_cast<size_t>(p.part)];
            if (position < static_cast<float>(c.startMs) + part.f0 * duration) continue;
            p.width = part.widthCells * cell;
            const float eyeY = view.position.y + gEyeOffset + cfg.heightBlocks;
            Vec3 best{view.position.x, eyeY, view.position.z};
            float bestScore = -1e9f;
            for (int attempt = 0; attempt < 48; ++attempt) {
                const uint64_t seed = c.serial * 733ULL + static_cast<uint64_t>(p.part + 1) * 977ULL +
                                      static_cast<uint64_t>(attempt) * 137ULL;
                const float yawDeg = firstPerson ? view.yaw + (rnd(seed) - 0.5f) * cfg.scatterDeg
                                                 : rnd(seed) * 360.0f;
                const float yawRad = yawDeg * 0.01745329252f;
                const Vec3 forward{-std::sin(yawRad), 0.0f, std::cos(yawRad)};
                const float dist = cfg.distance * (0.8f + 0.4f * rnd(seed + 7));
                // Aim a little toward where the player is looking up or down.
                const float aim = firstPerson ? -std::tan(view.pitch * 0.01745329252f) * dist * 0.5f : 0.0f;
                const float dy = std::clamp(aim, -1.5f, 1.5f) + (rnd(seed + 13) - 0.5f) * 1.8f;
                const Vec3 cand = Vec3{view.position.x, eyeY + dy, view.position.z} + forward * dist;
                float score = 100.0f;
                if (!firstPerson) {
                    // keep it where the camera can see it: roughly in front of the camera, not behind the player
                    const Vec3 ray = normalized(Vec3{cand.x - cam.x, 0, cand.z - cam.z});
                    const Vec3 gaze = normalized(Vec3{view.position.x - cam.x, 0, view.position.z - cam.z});
                    const float al = ray.x * gaze.x + ray.z * gaze.z;
                    if (al < 0.3f) score = -5.0f;
                    else if (al > 0.95f && std::sqrt(len2(cand - cam)) > std::sqrt(len2(view.position - cam)))
                        score = -4.0f; // the player would hide it
                }
                for (const auto& o : sc.panels)
                    if (o.appeared) score = std::min(score, clearance(cand, p.width, cam, o.anchor, o.width));
                for (const auto& o : sc.previous)
                    if (o.appeared) score = std::min(score, clearance(cand, p.width, cam, o.anchor, o.width));
                if (score > bestScore) bestScore = score, best = cand;
                if (score >= 1.3f) break;
            }
            p.anchor = best;
            const Vec3 face = normalized(Vec3{cam.x - best.x, 0.0f, cam.z - best.z});
            // turn it a little away from the camera, so that it stands in the world with real perspective
            const float j = (rnd(c.serial * 31ULL + static_cast<uint64_t>(p.part) * 17ULL) - 0.5f) * 2.0f * cfg.jitterRad;
            const float cs = std::cos(j), sn = std::sin(j);
            const Vec3 faceJ{face.x * cs + face.z * sn, 0.0f, -face.x * sn + face.z * cs};
            p.right = Vec3{faceJ.z, 0.0f, -faceJ.x};
            p.facing = faceJ;
            p.appeared = true;
            p.appearedAt = now;
        }
    }

    // Count what has to be drawn.
    const float lineEnd = sc.cue ? static_cast<float>(sc.cue->endMs) : 0.0f;
    const float holdFade = 1.0f - smooth((position - lineEnd - static_cast<float>(cfg.holdMs)) / 600.0f);
    size_t count = 0;
    auto countPanels = [&](const std::vector<Panel>& ps, const std::shared_ptr<const Cue>& cu) {
        if (!cu) return;
        for (const auto& p : ps)
            if (p.appeared) {
                const Part& part = cu->parts[static_cast<size_t>(p.part)];
                count += part.fill.size() + (cfg.outline ? part.outline.size() : 0);
            }
    };
    if (holdFade > 0.0f) countPanels(sc.panels, sc.cue);
    countPanels(sc.previous, sc.prevCue);
    if (!count) {
        diag("nothing to draw right now");
        return;
    }
    diag("drawing", count);

    const float scale = cell;
    ColorGuard guard(colors);
    gTessBegin(tess, nullptr, 1, static_cast<int>(count * 4), 0);

    auto drawPanels = [&](const std::vector<Panel>& ps, const std::shared_ptr<const Cue>& cu, bool leaving) {
        if (!cu) return;
        const Cue& c = *cu;
        const float duration = static_cast<float>(std::max<int64_t>(c.endMs - c.startMs, 600));
        for (const auto& p : ps) {
            if (!p.appeared) continue;
            const Part& part = c.parts[static_cast<size_t>(p.part)];
            const float enter = smooth(static_cast<float>(now - p.appearedAt) / 320.0f);
            float alpha = enter * cfg.opacity;
            alpha *= leaving ? 1.0f - smooth(static_cast<float>(now - gScene.prevFadeStart) / 700.0f) : holdFade;
            if (alpha <= 0.01f) continue;
            // color: the part being sung is highlighted, finished parts settle to the normal color
            const float t0 = static_cast<float>(c.startMs) + part.f0 * duration;
            const float t1 = static_cast<float>(c.startMs) + part.f1 * duration;
            const float prog = leaving ? 1.0f : std::clamp((position - t0) / std::max(t1 - t0, 80.0f), 0.0f, 1.0f);
            float base[3];
            if (cfg.colorMode == 2) {
                hsv(static_cast<float>(p.part) * 0.13f + static_cast<float>(now % 100000) * 0.001f * cfg.rainbowSpeed,
                    0.55f, 1.0f, base);
            } else if (cfg.colorMode == 1) {
                const float k = c.parts.size() > 1 ? static_cast<float>(p.part) / static_cast<float>(c.parts.size() - 1) : 0.0f;
                for (int i = 0; i < 3; ++i) base[i] = cfg.sung[i] + (cfg.grad[i] - cfg.sung[i]) * k;
            } else {
                for (int i = 0; i < 3; ++i) base[i] = cfg.sung[i];
            }
            const float settle = smooth(prog >= 1.0f ? (position - t1) / 900.0f : 0.0f);
            float rgb[3];
            for (int i = 0; i < 3; ++i) rgb[i] = base[i] + (cfg.unsung[i] - base[i]) * settle * 0.6f;
            // a gentle rise while it is sung
            const float rise = 0.06f * smooth(static_cast<float>(now - p.appearedAt) / 900.0f);
            const float pop = 0.82f + 0.2f * enter;
            const float s = scale * pop;
            const Vec3 right = p.right;
            const Vec3 up{0, 1, 0};
            const Vec3 origin = p.anchor - cam + up * rise;
            auto quad = [&](Vec3 tl) {
                const Vec3 tr = tl + right * (s * 0.98f), bl = tl - up * (s * 0.98f), br = tr - up * (s * 0.98f);
                for (auto q : {bl, br, tr, tl}) gTessVertex(tess, q.x, q.y, q.z);
            };
            if (cfg.outline)
                for (const auto& cl : part.outline) {
                    gTessColor(tess, 0.0f, 0.0f, 0.0f, alpha * static_cast<float>(cl.coverage) / 255.0f);
                    quad(origin + right * (cl.x * s) + up * (cl.y * s));
                }
            // the colored letters sit a hair in front of their outline, so that both are visible
            const Vec3 front = origin + p.facing * 0.002f;
            for (const auto& cl : part.fill) {
                gTessColor(tess, rgb[0], rgb[1], rgb[2], alpha * static_cast<float>(cl.coverage) / 255.0f);
                quad(front + right * (cl.x * s) + up * (cl.y * s));
            }
        }
    };
    drawPanels(sc.previous, sc.prevCue, true);
    if (holdFade > 0.0f) drawPanels(sc.panels, sc.cue, false);

    alignas(8) char texture[0x58]{};
    gRenderMesh(ctx, tess, &gMaterial, texture);
    if (gEnabled.load()) gLastDraw = now;
}

// ------------------------------------------------------------------ finding the game
struct Image {
    uintptr_t base = 0;
    const ElfW(Phdr)* headers = nullptr;
    size_t count = 0;
    const B::Profile* selected = nullptr;
    bool contains(uintptr_t at, size_t size, unsigned flags) const {
        for (size_t i = 0; i < count; ++i) {
            const auto& h = headers[i];
            if (h.p_type != PT_LOAD || (h.p_flags & flags) != flags) continue;
            const auto start = base + h.p_vaddr;
            if (at >= start && at - start <= h.p_memsz && size <= h.p_memsz - (at - start)) return true;
        }
        return false;
    }
};

Image findImage() {
    Image result;
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* arg) {
            if (!info->dlpi_name) return 0;
            std::string_view name(info->dlpi_name);
            if (name.substr(name.find_last_of('/') + 1) != "libminecraftpe.so") return 0;
            auto& image = *static_cast<Image*>(arg);
            image.base = info->dlpi_addr;
            image.headers = info->dlpi_phdr;
            image.count = info->dlpi_phnum;
            for (size_t i = 0; i < image.count; ++i) {
                const auto& h = image.headers[i];
                const auto start = image.base + h.p_vaddr;
                if (h.p_type != PT_NOTE || !image.contains(start, h.p_memsz, PF_R)) continue;
                for (const auto& prof : B::profiles)
                    if (B::matchesBuildId({reinterpret_cast<const uint8_t*>(start), size_t(h.p_memsz)}, prof.buildId))
                        image.selected = &prof;
            }
            return 1;
        },
        &result);
    return result;
}

void prepare() {
    Image image = findImage();
    if (!image.headers) {
        LYR_LOGW("Real 3D: libminecraftpe.so not found");
        gState = State::Unsupported;
        return;
    }
    if (!image.selected) {
        LYR_LOGW("Real 3D: this Minecraft build is not one of the supported ones (1.26.52 / 1.26.60 / 1.26.62.1)");
        gState = State::Unsupported;
        return;
    }
    const B::Profile& prof = *image.selected;
    std::array<uintptr_t, B::BindingCount> addr{};
    for (size_t i = 0; i < addr.size(); ++i) {
        const auto& b = prof.bindings[i];
        const uintptr_t at = image.base + b.rva;
        if (!image.contains(at, b.pattern.size(), PF_R | PF_X)) {
            LYR_LOGW("Real 3D: binding %zu is outside the game code", i);
            gState = State::BindingMismatch;
            return;
        }
        if (!B::matchesPattern({reinterpret_cast<const uint8_t*>(at), B::patternBytes(b.pattern)}, b.pattern)) {
            // Another mod may already have hooked this function (its first 16 bytes are then a trampoline).
            bool ok = false;
            if (i != B::MaterialGroup) {
                std::string_view tail = b.pattern;
                bool enough = true;
                for (int skip = 0; skip < 16 && enough; ++skip) {
                    const auto sp = tail.find(' ');
                    if (sp == std::string_view::npos) enough = false;
                    else tail.remove_prefix(sp + 1);
                }
                ok = enough && image.contains(at + 16, B::patternBytes(tail), PF_R | PF_X) &&
                     B::matchesPattern({reinterpret_cast<const uint8_t*>(at + 16), B::patternBytes(tail)}, tail);
            }
            if (!ok) {
                LYR_LOGW("Real 3D: binding %zu does not match this build", i);
                gState = State::BindingMismatch;
                return;
            }
        }
        addr[i] = at;
    }
    // The material table is found from the ADRP/ADD pair at the start of the common-material function.
    const uintptr_t at = addr[B::MaterialGroup];
    uint32_t a = 0, b2 = 0;
    std::memcpy(&a, reinterpret_cast<void*>(at), 4);
    std::memcpy(&b2, reinterpret_cast<void*>(at + 4), 4);
    if ((a & 0x9F00001F) != 0x90000000 || (b2 & 0xFF8003FF) != 0x91000000) {
        gState = State::BindingMismatch;
        return;
    }
    int64_t imm = static_cast<int64_t>(((a >> 5) & 0x7FFFF) * 4 + ((a >> 29) & 3));
    if (imm & (1 << 20)) imm -= (1 << 21);
    const uintptr_t group = static_cast<uintptr_t>(static_cast<int64_t>(at & ~uintptr_t(0xfff)) + imm * 4096) +
                            (((b2 >> 10) & 0xfff) << ((b2 & 0x400000) ? 12 : 0)) + 32;
    if (!image.contains(group, sizeof(void*), PF_R)) {
        gState = State::BindingMismatch;
        return;
    }
    gProfile = &prof;
    gMaterialGroup = reinterpret_cast<void*>(group);
    gTessBegin = reinterpret_cast<TessBegin>(addr[B::Begin]);
    gTessColor = reinterpret_cast<TessColor>(addr[B::Color]);
    gTessVertex = reinterpret_cast<TessVertex>(addr[B::Vertex]);
    gRenderMesh = reinterpret_cast<RenderMesh>(addr[B::Mesh]);
    gTickTarget = reinterpret_cast<void*>(addr[B::Tick]);
    gRenderTarget = reinterpret_cast<void*>(addr[B::Render]);
    if (pl::memory::hook(gTickTarget, reinterpret_cast<void*>(&tickHook), reinterpret_cast<void**>(&gOrigTick)) != 0) {
        gState = State::HookFailed;
        return;
    }
    gTickHooked = true;
    if (pl::memory::hook(gRenderTarget, reinterpret_cast<void*>(&renderHook), reinterpret_cast<void**>(&gOrigRender)) !=
        0) {
        gState = State::HookFailed;
        return;
    }
    gRenderHooked = true;
    gInstalled.store(true, std::memory_order_release);
    gState = State::Ready;
    LYR_LOGI("Real 3D: ready on Minecraft %s (exact build)", std::string(prof.name).c_str());
}

} // namespace

// ------------------------------------------------------------------ public API
void request(bool enable) {
    gEnabled.store(enable);
    if (!enable) return;
    std::lock_guard<std::mutex> lock(gLifecycle);
    if (gState.load() != State::Idle) return;
    gState = State::Preparing;
    gInstaller = std::thread([] {
        try {
            prepare();
        } catch (...) {
            gState = State::HookFailed;
        }
        if (gState.load() != State::Ready) LYR_LOGW("Real 3D: renderer unavailable (state %d)", static_cast<int>(gState.load()));
    });
}

State state() { return gState.load(); }

bool drawing() {
    const int64_t n = gLastDraw.load();
    return gEnabled.load() && n > 0 && nowMs() - n < 500;
}

const char* statusText() {
    switch (gState.load()) {
    case State::Idle:
    case State::Preparing: return "Preparing the 3D renderer";
    case State::Unsupported: return "3D not available for this Minecraft build";
    case State::BindingMismatch: return "3D renderer does not match this build";
    case State::HookFailed: return "3D renderer hook failed";
    case State::Ready: return drawing() ? "3D lyrics active" : "3D ready, waiting for the world";
    }
    return "";
}

void publishCue(Cue&& cue) {
    auto sp = std::make_shared<const Cue>(std::move(cue));
    std::lock_guard<std::mutex> lock(gDataMutex);
    gCue = std::move(sp);
}

void setPlayback(int64_t positionMs, bool playing) {
    std::lock_guard<std::mutex> lock(gDataMutex);
    gPosMs = positionMs;
    gPlaying = playing;
    gPosSampleAt = nowMs();
}

void setSettings(const Settings& s) {
    std::lock_guard<std::mutex> lock(gDataMutex);
    gSettings = s;
}

void unload() {
    gEnabled.store(false);
    {
        std::lock_guard<std::mutex> lock(gLifecycle);
        if (gInstaller.joinable()) gInstaller.join();
    }
    if (gRenderHooked) pl::memory::unhook(gRenderTarget, reinterpret_cast<void*>(&renderHook));
    if (gTickHooked) pl::memory::unhook(gTickTarget, reinterpret_cast<void*>(&tickHook));
    gRenderHooked = gTickHooked = false;
    gInstalled = false;
}

} // namespace lyrics::w3d
