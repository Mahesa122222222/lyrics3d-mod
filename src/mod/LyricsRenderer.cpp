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
uniform float uMode;       // 0 solid, 1 gradien, 2 pelangi (hanya untuk teks utama)
uniform vec3 uGrad2;
uniform float uTime;
uniform float uRainSpeed;
out vec4 oColor;
vec3 hsv2rgb(vec3 c) {
    vec3 p = abs(fract(c.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}
void main() {
    float a = texture(uTex, vUV).a * uAlpha;
    vec3 col = uSung;
    if (uSolid < 0.5) {
        float u = (vUV.x - uSpan.x) / max(uSpan.y - uSpan.x, 0.0001);
        vec3 sc = uSung;
        if (uMode > 0.5 && uMode < 1.5) sc = mix(uSung, uGrad2, clamp(u, 0.0, 1.0));
        else if (uMode >= 1.5) sc = hsv2rgb(vec3(fract(u * 0.8 - uTime * uRainSpeed), 0.65, 1.0));
        col = mix(sc, uUnsung, smoothstep(uProgress - 0.015, uProgress + 0.015, u));
    }
    oColor = vec4(col * a, a); // premultiplied alpha
}
)";

// Bintang kilau 4 sudut + inti bercahaya, digambar procedural (tanpa tekstur).
const char* kFsSpark = R"(#version 300 es
precision highp float;
in vec2 vUV;
uniform vec3 uColor;
uniform float uAlpha;
out vec4 oColor;
void main() {
    vec2 p = vUV * 2.0 - 1.0;
    float r2 = dot(p, p);
    float cross = exp(-14.0 * abs(p.x * p.y)) * max(0.0, 1.0 - sqrt(r2));
    float core = exp(-7.0 * r2);
    float a = clamp(cross * 1.2 + core * 0.7, 0.0, 1.0) * uAlpha;
    vec3 col = mix(uColor, vec3(1.0), core);
    oColor = vec4(col * a, a);
}
)";

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline float rand01(uint32_t seed) { return static_cast<float>(hash32(seed) & 0xFFFFFFu) / 16777216.0f; }

// Hue 0..1 -> RGB (jenuh sedang, terang).
void hsvToRgb(float h, float s, float v, float out[3]) {
    h = h - std::floor(h);
    const float k[3] = {0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
    for (int i = 0; i < 3; ++i) {
        float x = std::fabs((h + k[i] - std::floor(h + k[i])) * 6.0f - 3.0f);
        float c = std::min(1.0f, std::max(0.0f, x - 1.0f));
        out[i] = v * (1.0f - s + s * c);
    }
}

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
    mProgram = mVao = mVbo = mSparkProg = 0;
    mTex.clear();
    mCTex.clear();
    mChunks.clear();
    mAnch.clear();
    for (int k = 0; k < 3; ++k) {
        mNoteTex[k] = TextTexture{};
        mNoteFailed[k] = false;
    }
    mTrackId = -1;
    mStyleKey.clear();
}

void LyricsRenderer::shutdown(JNIEnv* env) {
    mTex.clear();
    mCTex.clear();
    mChunks.clear();
    mAnch.clear();
    for (int k = 0; k < 3; ++k) mNoteTex[k] = TextTexture{};
    mFactory.shutdown(env);
    mInitDone = false;
}

void LyricsRenderer::releaseTextures() {
    for (auto& kv : mTex)
        if (kv.second.id) glDeleteTextures(1, &kv.second.id);
    mTex.clear();
    for (auto& kv : mCTex)
        if (kv.second.id) glDeleteTextures(1, &kv.second.id);
    mCTex.clear();
    mChunks.clear();
    mAnch.clear();
    for (int k = 0; k < 3; ++k) {
        if (mNoteTex[k].id) glDeleteTextures(1, &mNoteTex[k].id);
        mNoteTex[k] = TextTexture{};
        mNoteFailed[k] = false;
    }
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
    uMode = glGetUniformLocation(mProgram, "uMode");
    uGrad2 = glGetUniformLocation(mProgram, "uGrad2");
    uTime = glGetUniformLocation(mProgram, "uTime");
    uRainSpeed = glGetUniformLocation(mProgram, "uRainSpeed");

    // Program kedua (kilau). Kalau gagal, fitur kilau saja yang mati.
    {
        GLuint svs = compile(GL_VERTEX_SHADER, kVs);
        GLuint sfs = compile(GL_FRAGMENT_SHADER, kFsSpark);
        if (svs && sfs) {
            mSparkProg = glCreateProgram();
            glAttachShader(mSparkProg, svs);
            glAttachShader(mSparkProg, sfs);
            glLinkProgram(mSparkProg);
            GLint sok = 0;
            glGetProgramiv(mSparkProg, GL_LINK_STATUS, &sok);
            if (!sok) {
                glDeleteProgram(mSparkProg);
                mSparkProg = 0;
                LYR_LOGW("Program kilau gagal di-link; efek kilau dimatikan");
            } else {
                uSMvp = glGetUniformLocation(mSparkProg, "uMvp");
                uSColor = glGetUniformLocation(mSparkProg, "uColor");
                uSAlpha = glGetUniformLocation(mSparkProg, "uAlpha");
            }
        }
        if (svs) glDeleteShader(svs);
        if (sfs) glDeleteShader(sfs);
    }

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
    glUniform1f(uMode, solid ? 0.0f : mFx.mode);
    glUniform3f(uGrad2, mFx.grad2.r, mFx.grad2.g, mFx.grad2.b);
    glUniform1f(uTime, mFx.time);
    glUniform1f(uRainSpeed, mFx.speed);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

const TextTexture* LyricsRenderer::noteTex(JNIEnv* env, const LyricsConfig& cfg, int k, int& budget) {
    if (mNoteTex[k].id) return &mNoteTex[k];
    if (mNoteFailed[k] || budget <= 0) return nullptr;
    --budget;
    static const char* const glyphs[3] = {"\xE2\x99\xAA", "\xE2\x99\xAB", "\xE2\x99\xAC"}; // ♪ ♫ ♬
    TextStyle st;
    st.fontPx = static_cast<float>(mFontPxBuilt) * 1.3f;
    st.glowRadiusPx = cfg.glow > 0.0f ? cfg.glow * st.fontPx * 0.12f : 0.0f;
    st.bold = true;
    st.family = "sans-serif";
    st.maxWidthPx = 512;
    if (!mFactory.make(env, glyphs[k], st, mNoteTex[k])) {
        mNoteTex[k] = TextTexture{};
        mNoteFailed[k] = true;
        return nullptr;
    }
    return &mNoteTex[k];
}

// Not musik naik dari bawah layar ke atas, bergoyang pelan. Digambar di belakang lirik.
void LyricsRenderer::drawNotes(JNIEnv* env, const LyricsConfig& cfg, const Mat4& proj, float t, float halfH,
                               float aspect, float depth, float worldPerPx, Color base, int& budget) {
    const float amount = clamp01(cfg.notesAmount);
    const int count = 3 + static_cast<int>(std::lround(amount * 9.0f));
    const bool rainbow = (mFx.mode >= 1.5f);
    for (int j = 0; j < count; ++j) {
        const uint32_t sj = static_cast<uint32_t>(j) * 7919u + 17u;
        const float life = 4.0f + 3.0f * rand01(sj);
        const float shifted = t + rand01(sj + 1) * life;
        const uint32_t cyc = static_cast<uint32_t>(shifted / life);
        const float ph = shifted / life - std::floor(shifted / life);
        const uint32_t seed = hash32(sj * 31u + cyc * 977u);

        const TextTexture* tex = noteTex(env, cfg, static_cast<int>((seed >> 8) % 3u), budget);
        if (!tex) continue;

        const float xRange = aspect * halfH * 0.92f;
        const float x = (rand01(seed + 2) * 2.0f - 1.0f) * xRange + std::sin(ph * 6.0f + static_cast<float>(j)) * 0.05f * halfH;
        const float y = (-1.1f + 2.2f * ph) * halfH;
        const float rot = std::sin(ph * 5.0f + static_cast<float>(j)) * 0.4f;
        const float s = 0.7f + 0.6f * rand01(seed + 3);
        const float alpha = std::sin(ph * kPi) * 0.85f * clamp01(cfg.opacity);
        if (alpha <= 0.01f) continue;

        float rgb[3] = {base.r, base.g, base.b};
        if (rainbow) hsvToRgb(rand01(seed + 4) + t * mFx.speed * 0.3f, 0.65f, 1.0f, rgb);
        const Color col{rgb[0], rgb[1], rgb[2]};

        const float hx = static_cast<float>(tex->width) * 0.5f * worldPerPx * s;
        const float hy = static_cast<float>(tex->height) * 0.5f * worldPerPx * s;
        Mat4 model = mul(translate(x, y, -depth), mul(rotateZ(rot), scale(hx, hy, 1.0f)));
        drawQuad(mul(proj, model), *tex, col, col, 1.0f, alpha, true);
    }
}

// Bintang kilau di sekitar baris aktif, sebagian berkumpul di "kepala" warna yang sedang berjalan.
void LyricsRenderer::drawSparkles(const Mat4& proj, const Mat4& group, const LyricsConfig& cfg, float t, float ly,
                                  float hx, float hy, float headX, float sizeWorld, Color base) {
    if (!mSparkProg) return;
    glUseProgram(mSparkProg);
    const float amount = clamp01(cfg.sparkleAmount);
    const int count = 6 + static_cast<int>(std::lround(amount * 30.0f));
    const bool rainbow = (mFx.mode >= 1.5f);
    for (int i = 0; i < count; ++i) {
        const uint32_t si = static_cast<uint32_t>(i) * 104729u + 5u;
        const float life = 0.9f + 1.2f * rand01(si);
        const float shifted = t + rand01(si + 1) * life;
        const uint32_t cyc = static_cast<uint32_t>(shifted / life);
        const float ph = shifted / life - std::floor(shifted / life);
        const uint32_t seed = hash32(si * 31u + cyc * 131u);

        float x, y;
        if ((i & 1) == 0) { // dekat kepala warna
            x = headX + (rand01(seed + 2) - 0.5f) * hx * 0.35f;
            y = (rand01(seed + 3) - 0.5f) * hy * 1.7f;
        } else { // tersebar di sepanjang teks
            x = (rand01(seed + 2) * 2.0f - 1.0f) * hx * 0.95f;
            y = (rand01(seed + 3) * 2.0f - 1.0f) * hy * 1.3f;
        }
        y += ph * hy * 0.5f;
        const float pulse = std::sin(ph * kPi);
        const float sz = sizeWorld * (0.3f + 0.6f * rand01(seed + 4)) * pulse;
        const float alpha = pulse * clamp01(cfg.opacity);
        if (sz <= 0.0005f || alpha <= 0.02f) continue;

        float rgb[3] = {base.r * 0.5f + 0.5f, base.g * 0.5f + 0.5f, base.b * 0.5f + 0.5f};
        if (rainbow) hsvToRgb(rand01(seed + 5) + t * mFx.speed * 0.3f, 0.55f, 1.0f, rgb);

        Mat4 model = mul(group, mul(translate(x, ly + y, 0.03f), scale(sz, sz, 1.0f)));
        const Mat4 mvp = mul(proj, model);
        glUniformMatrix4fv(uSMvp, 1, GL_FALSE, mvp.m);
        glUniform3f(uSColor, rgb[0], rgb[1], rgb[2]);
        glUniform1f(uSAlpha, alpha);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glUseProgram(mProgram);
}

void LyricsRenderer::render(JNIEnv* env, const LyricsConfig& cfg, const ViewState& view, int screenW,
                            int screenH, double timeSec, float camYawRad, float camPitchRad) {
    if (!mInitDone || !view.lrc || view.lrc->lines.empty() || screenW < 16 || screenH < 16) return;

    GlStateGuard guard;

    if (!mProgram && !buildProgram()) {
        mInitDone = false; // jangan coba terus-menerus
        return;
    }

    // --- tekstur: bangun ulang jika ukuran/gaya/lagu berubah ---
    const bool world = cfg.layoutMode == "world" && view.lrc->timed;
    const int fontPx =
        std::clamp(static_cast<int>(std::lround(screenH * (world ? cfg.worldFontSize : cfg.fontSize))), 16, 220);
    const std::string styleKey = cfg.fontFamily + (cfg.bold ? "|b|" : "|n|") + std::to_string(cfg.glow) +
                                 (world ? (cfg.worldCaps ? "|W1" : "|W0") : "|S");
    if (view.trackId != mTrackId || fontPx != mFontPxBuilt || styleKey != mStyleKey) {
        releaseTextures();
        mTrackId = view.trackId;
        mFontPxBuilt = fontPx;
        mStyleKey = styleKey;
        mLastIdx = -2;
    }

    if (world) {
        renderWorld(env, cfg, view, screenW, screenH, timeSec, camYawRad, camPitchRad, fontPx);
        return;
    }

    const LrcData& lrc = *view.lrc;
    const int n = static_cast<int>(lrc.lines.size());
    const int idx = findLineIndex(lrc.lines, view.positionMs);

    // --- animasi pindah baris ---
    if (idx != mLastIdx) {
        const bool forward = (mLastIdx != -2) && idx > mLastIdx;
        mShiftDir = 1;
        mIdxChangeTime = forward ? timeSec : -1e9; // mundur/awal: langsung pindah tanpa animasi
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

    float fbG[3] = {1.0f, 0.31f, 0.85f}, cg[3];
    parseColor(cfg.gradientColor, fbG, cg);
    mFx.mode = cfg.colorMode == "rainbow" ? 2.0f : (cfg.colorMode == "gradient" ? 1.0f : 0.0f);
    mFx.grad2 = Color{cg[0], cg[1], cg[2]};
    mFx.time = std::fmod(t, 1000.0f);
    mFx.speed = std::clamp(cfg.rainbowSpeed, 0.0f, 3.0f);

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
    // Not musik (di belakang lirik).
    if (cfg.partyNotes && view.playing) {
        int noteBudget = 1;
        drawNotes(env, cfg, proj, t, halfH, aspect, depth, worldPerPx, sung, noteBudget);
    }
    struct SparkSpot {
        bool valid = false;
        float ly = 0, hx = 0, hy = 0, headX = 0;
    } spark;

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

        if (rel == 0) {
            const float localHead = (tex->u0 + progress * (tex->u1 - tex->u0)) * 2.0f - 1.0f;
            spark = {true, ly, hx, hy, localHead * hx};
        }
    }

    // Kilau di atas lirik aktif.
    if (cfg.sparkle && view.playing && spark.valid)
        drawSparkles(proj, group, cfg, t, spark.ly, spark.hx, spark.hy, spark.headX,
                     static_cast<float>(fontPx) * worldPerPx * 0.55f, sung);

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

// ---------------------------------------------------------------- mode dunia

namespace {

size_t utf8Len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1; }
int cpCount(const std::string& s) {
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

} // namespace

// Pecah satu baris jadi potongan 1-3 kata (maks ~10 huruf), lengkap dengan perkiraan waktunya.
const std::vector<LyricsRenderer::Chunk>& LyricsRenderer::chunksFor(const LrcData& lrc, int idx, bool caps) {
    auto it = mChunks.find(idx);
    if (it != mChunks.end()) return it->second;

    std::string text = lrc.lines[idx].text;
    if (caps)
        for (char& c : text)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');

    // kata
    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : text) {
            if (c == ' ' || c == '\t') {
                if (!cur.empty()) words.push_back(cur), cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) words.push_back(cur);
    }
    // kata yang kepanjangan (mis. bahasa tanpa spasi) dipotong per ~8 karakter
    std::vector<std::string> parts;
    for (const auto& w : words) {
        if (cpCount(w) <= 14) {
            parts.push_back(w);
            continue;
        }
        std::string cur;
        int cnt = 0;
        for (size_t i = 0; i < w.size();) {
            const size_t l = std::min(utf8Len(static_cast<unsigned char>(w[i])), w.size() - i);
            cur.append(w, i, l);
            i += l;
            if (++cnt >= 8) parts.push_back(cur), cur.clear(), cnt = 0;
        }
        if (!cur.empty()) parts.push_back(cur);
    }

    std::vector<Chunk> out;
    std::string cur;
    for (const auto& w : parts) {
        if (!cur.empty() && cpCount(cur) + 1 + cpCount(w) > 10) {
            out.push_back({cur, 0, 0});
            cur.clear();
        }
        if (!cur.empty()) cur += ' ';
        cur += w;
    }
    if (!cur.empty()) out.push_back({cur, 0, 0});
    while (out.size() > 60) { // pengaman
        out[out.size() - 2].text += " " + out.back().text;
        out.pop_back();
    }

    int total = 0;
    for (const auto& c : out) total += cpCount(c.text) + 1;
    int acc = 0;
    for (auto& c : out) {
        c.f0 = total > 0 ? static_cast<float>(acc) / static_cast<float>(total) : 0.0f;
        acc += cpCount(c.text) + 1;
        c.f1 = total > 0 ? static_cast<float>(acc) / static_cast<float>(total) : 1.0f;
    }
    return mChunks.emplace(idx, std::move(out)).first->second;
}

const TextTexture* LyricsRenderer::chunkTex(JNIEnv* env, const LyricsConfig& cfg, int key, const std::string& text,
                                            int& budget) {
    auto it = mCTex.find(key);
    if (it != mCTex.end()) return it->second.id ? &it->second : nullptr;
    if (budget <= 0) return nullptr;
    --budget;
    TextStyle st;
    st.fontPx = static_cast<float>(mFontPxBuilt);
    st.glowRadiusPx = cfg.glow > 0.0f ? cfg.glow * st.fontPx * 0.12f : 0.0f;
    st.bold = cfg.bold;
    st.family = cfg.fontFamily;
    st.maxWidthPx = 2048;
    TextTexture t;
    if (!mFactory.make(env, text, st, t)) {
        mCTex[key] = TextTexture{};
        return nullptr;
    }
    return &mCTex.emplace(key, t).first->second;
}

void LyricsRenderer::renderWorld(JNIEnv* env, const LyricsConfig& cfg, const ViewState& view, int screenW,
                                 int screenH, double timeSec, float camYaw, float camPitch, int /*fontPx*/) {
    const LrcData& lrc = *view.lrc;
    const int n = static_cast<int>(lrc.lines.size());
    const int idx = findLineIndex(lrc.lines, view.positionMs);
    const float t = static_cast<float>(timeSec);

    const float fov = rad(std::clamp(cfg.fovDeg, 20.0f, 100.0f));
    const float depth = std::max(0.5f, cfg.depth);
    const float aspect = static_cast<float>(screenW) / static_cast<float>(screenH);
    const float halfH = depth * std::tan(fov * 0.5f);
    const float worldPerPx = 2.0f * halfH / static_cast<float>(screenH);
    const Mat4 proj = perspective(fov, aspect, 0.1f, 50.0f);
    const float halfFovV = fov * 0.5f;
    const float halfFovH = std::atan(aspect * std::tan(halfFovV));

    // Pandangan kamera: kamera menoleh ke kanan = benda bergeser ke kiri.
    const Mat4 view4 = mul(rotateX(-camPitch), rotateY(camYaw));

    float fbSung[3] = {1.0f, 0.85f, 0.24f}, fbUnsung[3] = {1, 1, 1}, fbExt[3] = {0.16f, 0.12f, 0.3f};
    float cs[3], cu[3], ce[3];
    parseColor(cfg.sungColor, fbSung, cs);
    parseColor(cfg.unsungColor, fbUnsung, cu);
    parseColor(cfg.extrudeColor, fbExt, ce);
    const Color sung{cs[0], cs[1], cs[2]}, unsung{cu[0], cu[1], cu[2]}, extr{ce[0], ce[1], ce[2]};
    float fbG[3] = {1.0f, 0.31f, 0.85f}, cg[3];
    parseColor(cfg.gradientColor, fbG, cg);
    mFx.mode = cfg.colorMode == "rainbow" ? 2.0f : (cfg.colorMode == "gradient" ? 1.0f : 0.0f);
    mFx.grad2 = Color{cg[0], cg[1], cg[2]};
    mFx.time = std::fmod(t, 1000.0f);
    mFx.speed = std::clamp(cfg.rainbowSpeed, 0.0f, 3.0f);

    glUseProgram(mProgram);
    glBindVertexArray(mVao);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uTex, 0);
    glViewport(0, 0, screenW, screenH);

    int budget = 2;
    if (cfg.partyNotes && view.playing) {
        int noteBudget = 1;
        drawNotes(env, cfg, proj, t, halfH, aspect, depth, worldPerPx, sung, noteBudget);
    }

    const int64_t pos = view.positionMs;
    const int layers = std::clamp(cfg.extrudeLayers, 0, 16);
    const float step = layers > 0 ? cfg.extrudeDepth / static_cast<float>(layers) : 0.0f;
    const float spread = std::clamp(cfg.worldSpread, 0.2f, 1.6f);

    // 3 kolom x 3 baris posisi; urutannya selalu berpindah sisi supaya kata berurutan tidak bertumpuk.
    static const float kCol[3] = {-0.62f, 0.0f, 0.62f};
    static const float kRow[3] = {0.5f, 0.05f, -0.42f};
    static const int kSlots[9][2] = {{0, 0}, {2, 1}, {1, 2}, {2, 0}, {0, 1}, {1, 0}, {0, 2}, {2, 2}, {1, 1}};

    // Bangun tekstur dulu (baris aktif, potongan berurutan).
    for (int li = idx; li <= std::min(idx + 1, n - 1); ++li) {
        if (li < 0 || lrc.lines[li].text.empty()) continue;
        const auto& ch = chunksFor(lrc, li, cfg.worldCaps);
        for (size_t k = 0; k < ch.size() && budget > 0; ++k) chunkTex(env, cfg, li * 64 + static_cast<int>(k), ch[k].text, budget);
    }
    budget = 0;

    struct Spot {
        bool valid = false;
        Mat4 group;
        float hx = 0, hy = 0, headX = 0;
    } spot;

    const int holdMs = std::clamp(cfg.worldHoldMs, 0, 6000);
    const float fadeMs = 600.0f;
    for (int li = std::max(0, idx - 3); li <= idx && li < n; ++li) {
        if (li < 0 || lrc.lines[li].text.empty()) continue;
        const int64_t t0 = lrc.lines[li].timeMs;
        const int64_t t1 = (li + 1 < n) ? lrc.lines[li + 1].timeMs : t0 + 4000;
        const double dur = std::clamp<double>(static_cast<double>(t1 - t0), 600.0, 8000.0) * 0.9;
        const auto& ch = chunksFor(lrc, li, cfg.worldCaps);
        const uint32_t lineSeed = hash32(static_cast<uint32_t>(li) * 2654435761u + 7u);
        const int slotBase = static_cast<int>(lineSeed % 9u);

        for (size_t k = 0; k < ch.size(); ++k) {
            const int64_t cStart = t0 + static_cast<int64_t>(dur * ch[k].f0);
            const int64_t cEnd = t0 + static_cast<int64_t>(dur * ch[k].f1);
            if (pos < cStart) continue;

            // Habis masa tayang?
            const int64_t lineEnd = std::max<int64_t>(t1, t0 + static_cast<int64_t>(dur));
            const float over = static_cast<float>(pos - (lineEnd + holdMs));
            float fadeOut = 1.0f - clamp01(over / fadeMs);
            if (fadeOut <= 0.01f) continue;

            const int key = li * 64 + static_cast<int>(k);
            const TextTexture* tex = chunkTex(env, cfg, key, ch[k].text, budget);
            if (!tex) continue;

            // Jangkar di dunia: ditetapkan saat kata pertama kali tampil, relatif ke arah pandang saat itu.
            auto ait = mAnch.find(key);
            if (ait == mAnch.end()) {
                const uint32_t seed = hash32(lineSeed + static_cast<uint32_t>(k) * 40503u);
                const int* slot = kSlots[(slotBase + static_cast<int>(k)) % 9];
                const float jx = (rand01(seed + 1) - 0.5f) * 0.22f;
                const float jy = (rand01(seed + 2) - 0.5f) * 0.18f;
                Anchor a;
                a.yaw = -camYaw - (kCol[slot[0]] + jx) * spread * halfFovH;
                a.pitch = camPitch + (kRow[slot[1]] + jy) * spread * halfFovV;
                a.tilt = (rand01(seed + 3) - 0.5f) * 0.3f;
                a.spawn = t;
                ait = mAnch.emplace(key, a).first;
            }
            const Anchor& an = ait->second;

            // Animasi muncul: membesar sedikit melampaui lalu mengendap.
            const float age = t - an.spawn;
            const float pi = clamp01(age / 0.28f);
            const float back = 1.0f + 2.2f * std::pow(pi - 1.0f, 3.0f) + 1.2f * std::pow(pi - 1.0f, 2.0f);
            const float pop = 0.55f + 0.45f * back;
            const float fadeIn = clamp01(age / 0.18f);
            const float alpha = fadeIn * fadeOut * clamp01(cfg.opacity);
            if (alpha <= 0.01f) continue;

            // Kemajuan warna
            float progress;
            if (pos >= cEnd) progress = 1.0f;
            else progress = clamp01(static_cast<float>(pos - cStart) / static_cast<float>(std::max<int64_t>(1, cEnd - cStart)));

            const float bob = 0.012f * std::sin(t * 1.3f + static_cast<float>(key));
            const float rise = 0.01f * std::min(age, 6.0f); // naik pelan
            const float s = pop;
            const float hx = static_cast<float>(tex->width) * 0.5f * worldPerPx * s;
            const float hy = static_cast<float>(tex->height) * 0.5f * worldPerPx * s;

            const Mat4 place = mul(view4, mul(rotateY(an.yaw), mul(rotateX(an.pitch + bob + rise),
                                                                   mul(translate(0.0f, 0.0f, -depth), rotateZ(an.tilt)))));
            for (int L = layers; L >= 1; --L) {
                const float dz = -step * static_cast<float>(L);
                const float off = 0.35f * step * static_cast<float>(L);
                Mat4 model = mul(place, mul(translate(off, -off, dz), scale(hx, hy, 1.0f)));
                drawQuad(mul(proj, model), *tex, extr, extr, progress, alpha * 0.9f, true);
            }
            drawQuad(mul(proj, mul(place, scale(hx, hy, 1.0f))), *tex, sung, unsung, progress, alpha, false);

            if (li == idx && pos < cEnd + 400) {
                const float localHead = (tex->u0 + progress * (tex->u1 - tex->u0)) * 2.0f - 1.0f;
                spot = {true, place, hx, hy, localHead * hx};
            }
        }
    }

    if (cfg.sparkle && view.playing && spot.valid)
        drawSparkles(proj, spot.group, cfg, t, 0.0f, spot.hx, spot.hy, spot.headX,
                     static_cast<float>(mFontPxBuilt) * worldPerPx * 0.55f, sung);

    // Buang yang sudah jauh.
    for (auto it = mCTex.begin(); it != mCTex.end();) {
        const int li = it->first / 64;
        if (li < idx - 4 || li > idx + 2) {
            if (it->second.id) glDeleteTextures(1, &it->second.id);
            it = mCTex.erase(it);
        } else ++it;
    }
    for (auto it = mAnch.begin(); it != mAnch.end();) {
        const int li = it->first / 64;
        if (li < idx - 4 || li > idx) it = mAnch.erase(it);
        else ++it;
    }
    for (auto it = mChunks.begin(); it != mChunks.end();) {
        if (it->first < idx - 4 || it->first > idx + 2) it = mChunks.erase(it);
        else ++it;
    }
}

} // namespace lyrics
