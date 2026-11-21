#include "LyricsRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "Log.h"
#include "Mat4.h"

namespace lyrics {

namespace {

constexpr float kPi = 3.14159265358979f;
inline float rad(float deg) { return deg * kPi / 180.0f; }
inline float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

const char* kVs = R"(#version 300 es
layout(location = 0) in vec2 aPos;
uniform mat4 uMvp;
out vec2 vUV;
void main() {
    vUV = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
    gl_Position = uMvp * vec4(aPos, 0.0, 1.0);
}
)";

const char* kFs = R"(#version 300 es
precision highp float;
in vec2 vUV;
uniform sampler2D uTex;
uniform vec3 uSung;
uniform vec3 uUnsung;
uniform float uProgress;   // 0..1 sepanjang teks
uniform vec2 uSpan;        // rentang U teks (tanpa padding)
uniform float uAlpha;
uniform float uSolid;      // 1 = warna tunggal (sisi extrude)
out vec4 oColor;
void main() {
    float a = texture(uTex, vUV).a * uAlpha;
    vec3 col = uSung;
    if (uSolid < 0.5) {
        float u = (vUV.x - uSpan.x) / max(uSpan.y - uSpan.x, 0.0001);
        col = mix(uSung, uUnsung, smoothstep(uProgress - 0.015, uProgress + 0.015, u));
    }
    oColor = vec4(col * a, a); // premultiplied alpha
}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        LYR_LOGE("Shader gagal dikompilasi: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

// "#RRGGBB" -> 0..1. Jika format salah, kembalikan fallback.
void parseColor(const std::string& s, float fb[3], float out[3]) {
    out[0] = fb[0];
    out[1] = fb[1];
    out[2] = fb[2];
    if (s.size() != 7 || s[0] != '#') return;
    char* end = nullptr;
    long v = std::strtol(s.c_str() + 1, &end, 16);
    if (!end || *end != '\0') return;
    out[0] = ((v >> 16) & 0xFF) / 255.0f;
    out[1] = ((v >> 8) & 0xFF) / 255.0f;
    out[2] = (v & 0xFF) / 255.0f;
}

// Simpan & pulihkan state GL yang kita sentuh, supaya renderer game tidak terganggu.
struct GlStateGuard {
    GLint program, vao, arrayBuf, activeTex, tex0, fbo, viewport[4], unpackBuf, unpackAlign, unpackRow;
    GLint blendSrcRgb, blendDstRgb, blendSrcA, blendDstA, blendEqRgb, blendEqA;
    GLboolean blend, depth, cull, scissor, stencil, depthMask, colorMask[4];

    GlStateGuard() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuf);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuf);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlign);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRow);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEqRgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEqA);
        blend = glIsEnabled(GL_BLEND);
        depth = glIsEnabled(GL_DEPTH_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);

        // Keadaan netral untuk menggambar & mengunggah tekstur.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDepthMask(GL_FALSE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    ~GlStateGuard() {
        auto set = [](GLenum cap, GLboolean on) {
            if (on) glEnable(cap);
            else glDisable(cap);
        };
        set(GL_BLEND, blend);
        set(GL_DEPTH_TEST, depth);
        set(GL_CULL_FACE, cull);
        set(GL_SCISSOR_TEST, scissor);
        set(GL_STENCIL_TEST, stencil);
        glDepthMask(depthMask);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glBlendEquationSeparate(blendEqRgb, blendEqA);
        glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcA, blendDstA);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRow);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlign);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuf);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex0);
        glActiveTexture(activeTex);
        glBindBuffer(GL_ARRAY_BUFFER, arrayBuf);
        glBindVertexArray(vao);
        glUseProgram(program);
        while (glGetError() != GL_NO_ERROR) {
        } // jangan tinggalkan error milik kita untuk game
    }
};

} // namespace

LyricsRenderer::~LyricsRenderer() = default;

bool LyricsRenderer::init(JNIEnv* env) {
    mInitDone = mFactory.init(env);
    return mInitDone;
}

void LyricsRenderer::invalidateGl() {
    mProgram = mVao = mVbo = 0;
    mTex.clear();
    mTrackId = -1;
    mStyleKey.clear();
}

void LyricsRenderer::shutdown(JNIEnv* env) {
    mTex.clear();
    mFactory.shutdown(env);
    mInitDone = false;
}

void LyricsRenderer::releaseTextures() {
    for (auto& kv : mTex)
        if (kv.second.id) glDeleteTextures(1, &kv.second.id);
    mTex.clear();
}

bool LyricsRenderer::buildProgram() {
    GLuint vs = compile(GL_VERTEX_SHADER, kVs);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFs);
    if (!vs || !fs) return false;
    mProgram = glCreateProgram();
    glAttachShader(mProgram, vs);
    glAttachShader(mProgram, fs);
    glLinkProgram(mProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(mProgram, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(mProgram, sizeof(log), nullptr, log);
        LYR_LOGE("Program shader gagal di-link: %s", log);
        glDeleteProgram(mProgram);
        mProgram = 0;
        return false;
    }
    uMvp = glGetUniformLocation(mProgram, "uMvp");
    uTex = glGetUniformLocation(mProgram, "uTex");
    uSung = glGetUniformLocation(mProgram, "uSung");
    uUnsung = glGetUniformLocation(mProgram, "uUnsung");
    uProgress = glGetUniformLocation(mProgram, "uProgress");
    uSpan = glGetUniformLocation(mProgram, "uSpan");
    uAlpha = glGetUniformLocation(mProgram, "uAlpha");
    uSolid = glGetUniformLocation(mProgram, "uSolid");

    static const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenVertexArrays(1, &mVao);
    glGenBuffers(1, &mVbo);
    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    return true;
}

const TextTexture* LyricsRenderer::texFor(JNIEnv* env, const LyricsConfig& cfg, const LrcData& lrc, int idx,
                                          int& budget) {
    if (idx < 0 || idx >= static_cast<int>(lrc.lines.size())) return nullptr;
    auto it = mTex.find(idx);
    if (it != mTex.end()) return it->second.id ? &it->second : nullptr;
    if (lrc.lines[idx].text.empty()) return nullptr;
    if (budget <= 0) return nullptr; // batasi pembuatan tekstur per frame agar tidak patah-patah
    --budget;

    TextStyle st;
    st.fontPx = static_cast<float>(mFontPxBuilt);
    st.glowRadiusPx = cfg.glow > 0.0f ? cfg.glow * st.fontPx * 0.12f : 0.0f;
    st.bold = cfg.bold;
    st.family = cfg.fontFamily;
    st.maxWidthPx = 2048;

    TextTexture t;
    if (!mFactory.make(env, lrc.lines[idx].text, st, t)) {
        mTex[idx] = TextTexture{}; // tandai gagal agar tidak dicoba tiap frame
        return nullptr;
    }
    auto ins = mTex.emplace(idx, t);
    return &ins.first->second;
}

void LyricsRenderer::drawQuad(const Mat4& mvp, const TextTexture& t, Color sung, Color unsung, float progress,
                              float alpha, bool solid) {
    glUniformMatrix4fv(uMvp, 1, GL_FALSE, mvp.m);
    glUniform3f(uSung, sung.r, sung.g, sung.b);
    glUniform3f(uUnsung, unsung.r, unsung.g, unsung.b);
    glUniform1f(uProgress, progress);
    glUniform2f(uSpan, t.u0, t.u1);
    glUniform1f(uAlpha, alpha);
    glUniform1f(uSolid, solid ? 1.0f : 0.0f);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void LyricsRenderer::render(JNIEnv* env, const LyricsConfig& cfg, const ViewState& view, int screenW,
                            int screenH, double timeSec) {
    if (!mInitDone || !view.lrc || view.lrc->lines.empty() || screenW < 16 || screenH < 16) return;

    GlStateGuard guard;

    if (!mProgram && !buildProgram()) {
        mInitDone = false; // jangan coba terus-menerus
        return;
    }

    // --- tekstur: bangun ulang jika ukuran/gaya/lagu berubah ---
    const int fontPx = std::clamp(static_cast<int>(std::lround(screenH * cfg.fontSize)), 16, 220);
    const std::string styleKey = cfg.fontFamily + (cfg.bold ? "|b|" : "|n|") + std::to_string(cfg.glow);
    if (view.trackId != mTrackId || fontPx != mFontPxBuilt || styleKey != mStyleKey) {
        releaseTextures();
        mTrackId = view.trackId;
        mFontPxBuilt = fontPx;
        mStyleKey = styleKey;
        mLastIdx = -2;
    }

    const LrcData& lrc = *view.lrc;
    const int n = static_cast<int>(lrc.lines.size());
    const int idx = findLineIndex(lrc.lines, view.positionMs);

    // --- animasi pindah baris ---
    if (idx != mLastIdx) {
        mShiftDir = (mLastIdx == -2) ? 1 : (idx >= mLastIdx ? 1 : -1);
        mIdxChangeTime = (mLastIdx == -2) ? -1e9 : timeSec;
        mLastIdx = idx;
    }
    const float tr = cfg.transitionMs > 0
                         ? clamp01(static_cast<float>((timeSec - mIdxChangeTime) * 1000.0 / cfg.transitionMs))
                         : 1.0f;
    const float ease = 1.0f - (1.0f - tr) * (1.0f - tr) * (1.0f - tr);
    const float shift = static_cast<float>(mShiftDir) * (1.0f - ease);

    // --- kamera & tata letak ---
    const float fov = rad(std::clamp(cfg.fovDeg, 20.0f, 100.0f));
    const float depth = std::max(0.5f, cfg.depth);
    const float aspect = static_cast<float>(screenW) / static_cast<float>(screenH);
    const float halfH = depth * std::tan(fov * 0.5f);
    const float worldPerPx = 2.0f * halfH / static_cast<float>(screenH);
    const float lineH = static_cast<float>(fontPx) * cfg.lineSpacing * worldPerPx;
    const float y0 = std::clamp(cfg.positionY, -1.0f, 1.0f) * halfH;
    const Mat4 proj = perspective(fov, aspect, 0.1f, 50.0f);

    const float t = static_cast<float>(timeSec);
    const float yaw = rad(cfg.maxYawDeg) * std::sin(t * 0.7f) + rad(18.0f) * shift * -1.0f;
    const float pitch = rad(cfg.pitchDeg) * std::sin(t * 0.5f + 1.0f);
    const float bob = cfg.floatAmount * halfH * std::sin(t * 1.3f);
    const Mat4 group = mul(translate(0.0f, y0 + bob, -depth), mul(rotateY(yaw), rotateX(pitch)));

    float fbSung[3] = {1.0f, 0.85f, 0.24f}, fbUnsung[3] = {1, 1, 1}, fbExt[3] = {0.16f, 0.12f, 0.3f};
    float cs[3], cu[3], ce[3];
    parseColor(cfg.sungColor, fbSung, cs);
    parseColor(cfg.unsungColor, fbUnsung, cu);
    parseColor(cfg.extrudeColor, fbExt, ce);
    const Color sung{cs[0], cs[1], cs[2]}, unsung{cu[0], cu[1], cu[2]}, extr{ce[0], ce[1], ce[2]};

    glUseProgram(mProgram);
    glBindVertexArray(mVao);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uTex, 0);
    glViewport(0, 0, screenW, screenH);

    // Gambar dari yang paling jauh dari fokus ke yang paling dekat (baris aktif di atas).
    int order[5] = {-2, 2, -1, 1, 0};
    int budget = 2;
    // Bangun tekstur menurut prioritas (baris aktif dulu), lalu gambar dengan budget 0 (hanya cache).
    for (int rel : {0, 1, -1, 2, -2}) texFor(env, cfg, lrc, idx + rel, budget);
    budget = 0;
    const int layers = std::clamp(cfg.extrudeLayers, 0, 16);
    const float step = layers > 0 ? cfg.extrudeDepth / static_cast<float>(layers) : 0.0f;

    for (int ri = 0; ri < 5; ++ri) {
        const int rel = order[ri];
        const int i = idx + rel;
        if (i < 0 || i >= n) continue;

        const float arel = static_cast<float>(rel) + shift; // posisi animasi (satuan baris)
        const float aabs = std::fabs(arel);
        float alpha = aabs <= 1.0f ? 1.0f - 0.65f * aabs : 0.35f * std::max(0.0f, 2.0f - aabs);
        if (!cfg.showContext) alpha *= clamp01(1.0f - aabs * 1.6f);
        alpha *= clamp01(cfg.opacity);
        if (alpha <= 0.01f) continue;

        const TextTexture* tex = texFor(env, cfg, lrc, i, budget);
        if (!tex) continue;

        // Progres warna "sudah dinyanyikan".
        float progress = i < idx ? 1.0f : 0.0f;
        if (i == idx) {
            const int64_t t0 = lrc.lines[i].timeMs;
            const int64_t t1 = (i + 1 < n) ? lrc.lines[i + 1].timeMs : t0 + 4000;
            const double dur = std::clamp<double>(static_cast<double>(t1 - t0), 600.0, 8000.0) * 0.9;
            progress = lrc.timed ? clamp01(static_cast<float>((view.positionMs - t0) / dur)) : 0.0f;
        }

        const float s = 1.0f - 0.18f * std::min(aabs, 1.0f);
        const float hx = static_cast<float>(tex->width) * 0.5f * worldPerPx * s;
        const float hy = static_cast<float>(tex->height) * 0.5f * worldPerPx * s;
        const float ly = -arel * lineH;

        // Sisi tebal: lapisan gelap bertumpuk ke belakang, ikut berputar bersama pelat depan.
        for (int k = layers; k >= 1; --k) {
            const float dz = -step * static_cast<float>(k);
            const float off = 0.35f * step * static_cast<float>(k);
            Mat4 model = mul(group, mul(translate(off, ly - off, dz), scale(hx, hy, 1.0f)));
            drawQuad(mul(proj, model), *tex, extr, extr, progress, alpha * 0.9f, true);
        }
        Mat4 model = mul(group, mul(translate(0.0f, ly, 0.0f), scale(hx, hy, 1.0f)));
        drawQuad(mul(proj, model), *tex, sung, unsung, progress, alpha, false);
    }

    // Buang tekstur yang jauh dari baris aktif.
    for (auto it = mTex.begin(); it != mTex.end();) {
        if (std::abs(it->first - idx) > 3) {
            if (it->second.id) glDeleteTextures(1, &it->second.id);
            it = mTex.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace lyrics
