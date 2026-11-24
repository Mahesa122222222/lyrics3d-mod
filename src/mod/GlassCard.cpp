#include "GlassCard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "Log.h"

namespace lyrics {

namespace {

const char* kVs = R"(#version 300 es
uniform vec2 uRes;
uniform vec4 uRect;
uniform float uPad;
out vec2 vP;
out vec2 vUV;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vec2 pos = uRect.xy - vec2(uPad) + c * (uRect.zw + 2.0 * vec2(uPad));
    vP = pos;
    vUV = c;
    gl_Position = vec4(pos.x / uRes.x * 2.0 - 1.0, 1.0 - pos.y / uRes.y * 2.0, 0.0, 1.0);
}
)";

const char* kGlassFs = R"(#version 300 es
precision highp float;
in vec2 vP;
in vec2 vUV;
uniform vec4 uRect;
uniform float uRadius, uTime, uGlass, uOpacity;
uniform int uMode;
uniform vec4 uA, uB;
out vec4 o;
float sdRR(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
void main() {
    vec2 hs = uRect.zw * 0.5;
    vec2 q = vP - (uRect.xy + hs);
    float d = sdRR(q, hs, uRadius);
    vec2 uv = (vP - uRect.xy) / uRect.zw;
    float inside = 1.0 - smoothstep(-1.0, 1.0, d);
    if (uMode == 1) { o = vec4(uA.rgb, uA.a * inside * uOpacity); return; }
    if (uMode == 2) {
        float a = uTime * 0.35;
        float g = dot(uv - 0.5, vec2(cos(a), sin(a))) + 0.5;
        vec3 col = mix(uA.rgb, uB.rgb, clamp(g, 0.0, 1.0));
        col += 0.14 * smoothstep(0.6, 0.0, length(uv - vec2(0.3, 0.25)));
        o = vec4(col, inside * uOpacity);
        return;
    }
    float shadow = (1.0 - inside) * exp(-max(d, 0.0) / 9.0) * 0.40;
    vec3 base = mix(vec3(0.05, 0.06, 0.09), vec3(0.17, 0.18, 0.24), uv.y * 0.7);
    float a = mix(0.14, 0.66, uGlass);
    vec2 p1 = vec2(0.5 + 0.42 * sin(uTime * 0.45), 0.5 + 0.38 * cos(uTime * 0.31));
    vec2 p2 = vec2(0.5 + 0.40 * cos(uTime * 0.37 + 1.7), 0.5 + 0.36 * sin(uTime * 0.52 + 0.6));
    float b1 = smoothstep(0.65, 0.0, length((uv - p1) * vec2(1.7, 1.0)));
    float b2 = smoothstep(0.60, 0.0, length((uv - p2) * vec2(1.7, 1.0)));
    vec3 col = base + uA.rgb * b1 * 0.55 + uB.rgb * b2 * 0.50;
    a = clamp(a + (b1 + b2) * 0.16, 0.0, 0.92);
    col += vec3(0.10) * smoothstep(0.55, 0.0, uv.y);
    float band = uv.x * 0.9 + uv.y * 0.45 - fract(uTime * 0.07) * 2.6 + 0.6;
    col += vec3(0.10) * exp(-band * band * 40.0);
    float rim = smoothstep(-2.2, -0.4, d) * (1.0 - smoothstep(-0.4, 0.9, d));
    float rimLight = mix(0.75, 0.14, clamp(uv.x * 0.45 + uv.y * 0.65, 0.0, 1.0));
    col += vec3(rim * rimLight);
    a = max(a, rim * (0.35 + 0.4 * rimLight));
    col += vec3(exp(min(d, 0.0) / 7.0) * (1.0 - rim) * 0.07);
    float alpha = max(inside * a, shadow) * uOpacity;
    o = vec4(col * inside, alpha);
}
)";

const char* kTextFs = R"(#version 300 es
precision highp float;
in vec2 vP;
in vec2 vUV;
uniform sampler2D uTex;
uniform vec3 uC0, uC1, uG;
uniform vec2 uU;
uniform float uProg, uOpacity;
uniform vec4 uFlat;
out vec4 o;
void main() {
    float t = texture(uTex, vUV).a;
    if (uProg < -0.5) { o = vec4(uFlat.rgb, t * uFlat.a * uOpacity); return; }
    float k = clamp((vUV.x - uU.x) / max(uU.y - uU.x, 0.0001), 0.0, 1.0);
    float s = 1.0 - smoothstep(uProg - 0.02, uProg + 0.02, k);
    vec3 sung = mix(uC0, uG, k);
    o = vec4(mix(uC1, sung, s), t * uOpacity);
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
        LYR_LOGE("Card shader failed to compile: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint link(const char* fs) {
    GLuint v = compile(GL_VERTEX_SHADER, kVs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        LYR_LOGE("Card program failed to link");
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void hexColor(const std::string& s, const float fb[3], float out[3]) {
    out[0] = fb[0], out[1] = fb[1], out[2] = fb[2];
    if (s.size() != 7 || s[0] != '#') return;
    char* end = nullptr;
    const long v = std::strtol(s.c_str() + 1, &end, 16);
    if (!end || *end) return;
    out[0] = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
    out[1] = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
    out[2] = static_cast<float>(v & 0xFF) / 255.0f;
}

float smooth01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

std::string clock(int64_t ms) {
    if (ms < 0) ms = 0;
    const int s = static_cast<int>(ms / 1000);
    char b[24];
    std::snprintf(b, sizeof(b), "%d:%02d", s / 60, s % 60);
    return b;
}

} // namespace

void GlassCard::invalidateGl() {
    mGlass = mText = mVao = 0;
    mTex.clear();
}

void GlassCard::release() {
    for (auto& kv : mTex)
        if (kv.second.id) glDeleteTextures(1, &kv.second.id);
    mTex.clear();
    if (mGlass) glDeleteProgram(mGlass);
    if (mText) glDeleteProgram(mText);
    if (mVao) glDeleteVertexArrays(1, &mVao);
    mGlass = mText = mVao = 0;
}

bool GlassCard::build() {
    if (mGlass && mText && mVao) return true;
    mGlass = link(kGlassFs);
    mText = link(kTextFs);
    if (!mGlass || !mText) return false;
    glGenVertexArrays(1, &mVao);
    const char* gn[] = {"uRes", "uRect", "uPad", "uRadius", "uTime", "uGlass", "uOpacity", "uMode", "uA", "uB"};
    for (int i = 0; i < 10; ++i) uG[i] = glGetUniformLocation(mGlass, gn[i]);
    const char* tn[] = {"uRes", "uRect", "uPad", "uTex", "uC0", "uC1", "uG", "uU", "uProg", "uOpacity"};
    for (int i = 0; i < 10; ++i) uT[i] = glGetUniformLocation(mText, tn[i]);
    uT[9] = glGetUniformLocation(mText, "uOpacity");
    return true;
}

const TextTexture* GlassCard::text(JNIEnv* env, TextFactory& f, const std::string& s, float px, bool bold, int maxW,
                                   int& budget) {
    if (s.empty()) return nullptr;
    const std::string key = std::to_string(static_cast<int>(px)) + (bold ? "b" : "n") + std::to_string(maxW) + "|" + s;
    auto it = mTex.find(key);
    if (it != mTex.end()) return it->second.id ? &it->second : nullptr;
    if (budget <= 0) return nullptr;
    --budget;
    TextStyle st;
    st.fontPx = px;
    st.bold = bold;
    st.family = mFamily;
    st.glowRadiusPx = 0.0f;
    st.maxWidthPx = maxW;
    TextTexture t;
    if (!f.make(env, s, st, t)) {
        mTex[key] = TextTexture{};
        return nullptr;
    }
    return &mTex.emplace(key, t).first->second;
}

void GlassCard::rect(float x, float y, float w, float h, float radius, int mode, const float a[4], const float b[4],
                     float opacity, float pad, float glass) {
    glUseProgram(mGlass);
    glUniform2f(uG[0], static_cast<float>(mW), static_cast<float>(mH));
    glUniform4f(uG[1], x, y, w, h);
    glUniform1f(uG[2], pad);
    glUniform1f(uG[3], radius);
    glUniform1f(uG[4], static_cast<float>(mTime));
    glUniform1f(uG[5], glass);
    glUniform1f(uG[6], opacity);
    glUniform1i(uG[7], mode);
    glUniform4f(uG[8], a[0], a[1], a[2], a[3]);
    glUniform4f(uG[9], b[0], b[1], b[2], b[3]);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void GlassCard::tex(const TextTexture& t, float x, float y, float scale, float prog, const float c0[3],
                    const float c1[3], const float g[3], float opacity, bool flat, const float flatColor[4]) {
    glUseProgram(mText);
    const float w = static_cast<float>(t.width) * scale, h = static_cast<float>(t.height) * scale;
    glUniform2f(uT[0], static_cast<float>(mW), static_cast<float>(mH));
    glUniform4f(uT[1], x, y, w, h);
    glUniform1f(uT[2], 0.0f);
    glUniform1i(uT[3], 0);
    glUniform3f(uT[4], c0[0], c0[1], c0[2]);
    glUniform3f(uT[5], c1[0], c1[1], c1[2]);
    glUniform3f(uT[6], g[0], g[1], g[2]);
    glUniform2f(uT[7], t.u0, t.u1);
    glUniform1f(uT[8], flat ? -1.0f : prog);
    glUniform1f(uT[9], opacity);
    const GLint fl = glGetUniformLocation(mText, "uFlat");
    glUniform4f(fl, flatColor[0], flatColor[1], flatColor[2], flatColor[3]);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void GlassCard::draw(JNIEnv* env, TextFactory& factory, const LyricsConfig& cfg, const ViewState& view, int W, int H,
                     double timeSec) {
    if (!cfg.cardShow || !view.lrc || W < 64 || H < 64) return;
    if (!build()) return;
    mW = W;
    mH = H;
    mTime = std::fmod(timeSec, 4000.0);
    if (cfg.fontFamily != mFamily) {
        mFamily = cfg.fontFamily;
        for (auto& kv : mTex)
            if (kv.second.id) glDeleteTextures(1, &kv.second.id);
        mTex.clear();
    }
    if (mTex.size() > 60) {
        for (auto& kv : mTex)
            if (kv.second.id) glDeleteTextures(1, &kv.second.id);
        mTex.clear();
    }

    const LrcData& lrc = *view.lrc;
    const int n = static_cast<int>(lrc.lines.size());
    const bool lyrics = lrc.timed && n > 0;
    const int idx = lyrics ? findLineIndex(lrc.lines, view.positionMs) : -1;
    if (view.trackId != mTrack) {
        mTrack = view.trackId;
        mIdx = idx;
        mPrevIdx = idx;
        mChangeT = -10;
    } else if (idx != mIdx) {
        mPrevIdx = mIdx;
        mIdx = idx;
        mChangeT = timeSec;
    }
    const float ease = smooth01(static_cast<float>((timeSec - mChangeT) / 0.38));

    auto lineText = [&](int i) -> std::string {
        if (i < 0 || i >= n) return std::string();
        const std::string& s = lrc.lines[static_cast<size_t>(i)].text;
        return s.empty() ? std::string("\xE2\x99\xAA") : s; // ♪ for pauses
    };

    // colors
    const float fbS[3] = {1.0f, 0.85f, 0.24f}, fbU[3] = {1, 1, 1}, fbG[3] = {1.0f, 0.31f, 0.85f};
    float cs[3], cu[3], cg[3];
    hexColor(cfg.sungColor, fbS, cs);
    hexColor(cfg.unsungColor, fbU, cu);
    hexColor(cfg.gradientColor, fbG, cg);
    if (cfg.colorMode == "solid") cg[0] = cs[0], cg[1] = cs[1], cg[2] = cs[2];
    // Dimmed "not yet sung" color: the unsung color at reduced strength.
    const float dim[3] = {cu[0] * 0.55f, cu[1] * 0.55f, cu[2] * 0.58f};

    const float s = std::clamp(cfg.cardScale, 0.5f, 2.0f) * static_cast<float>(H) / 720.0f * 0.85f;
    const float pad = 20.0f * s, cover = 64.0f * s, radius = 26.0f * s;
    float cardW = std::min(620.0f * s, static_cast<float>(W) - 24.0f);
    const float inner = cardW - 2.0f * pad;
    const bool bar = cfg.cardProgress && view.durationMs > 0;
    float cardH = pad + cover;
    if (lyrics) cardH += 12.0f * s + 46.0f * s + 30.0f * s;
    if (bar) cardH += 12.0f * s + 20.0f * s;
    cardH += pad;
    float x = cfg.cardX * (static_cast<float>(W) - cardW);
    x = std::clamp(x, 12.0f, std::max(12.0f, static_cast<float>(W) - cardW - 12.0f));
    float y = 14.0f * s + cfg.cardY * (static_cast<float>(H) - cardH - 28.0f * s);

    glViewport(0, 0, W, H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(mVao);
    glActiveTexture(GL_TEXTURE0);

    const float opacity = std::clamp(cfg.opacity, 0.1f, 1.0f);
    const float colA[4] = {cs[0], cs[1], cs[2], 1.0f}, colB[4] = {cg[0], cg[1], cg[2], 1.0f};
    // "breathing" while the song plays
    const float breath = view.playing ? 0.5f + 0.5f * std::sin(static_cast<float>(timeSec) * 2.2f) : 0.0f;
    const float cardA[4] = {cs[0], cs[1], cs[2], 1.0f};
    const float cardB[4] = {cg[0], cg[1], cg[2], 1.0f};
    rect(x, y, cardW, cardH, radius, 0, cardA, cardB, opacity, 30.0f * s, cfg.cardGlass);

    int budget = 3;
    const float zero4[4] = {0, 0, 0, 0};
    // --- header: cover + title/artist
    const float cx = x + pad, cy = y + pad;
    const float coverA[4] = {cs[0], cs[1], cs[2], 1.0f}, coverB[4] = {cg[0], cg[1], cg[2], 1.0f};
    const float coverPulse = 1.0f + 0.02f * breath;
    rect(cx - (coverPulse - 1.0f) * cover * 0.5f, cy - (coverPulse - 1.0f) * cover * 0.5f, cover * coverPulse,
         cover * coverPulse, 14.0f * s, 2, coverA, coverB, 1.0f, 0.0f, 0.0f);
    if (const TextTexture* note = text(env, factory, "\xE2\x99\xAB", 34.0f * s, true, 200, budget)) {
        const float shadow[4] = {0, 0, 0, 0.25f};
        tex(*note, cx + (cover - note->width) * 0.5f, cy + (cover - note->height) * 0.5f, 1.0f, 1.0f, cu, cu, cu, 0.95f,
            true, shadow);
        const float white[4] = {1, 1, 1, 0.92f};
        tex(*note, cx + (cover - note->width) * 0.5f, cy + (cover - note->height) * 0.5f - 1.0f * s, 1.0f, 1.0f, cu, cu,
            cu, 0.95f, true, white);
    }
    const float tx = cx + cover + 16.0f * s;
    const int textW = static_cast<int>(std::max(80.0f, x + cardW - pad - tx));
    std::string title = view.title.empty() ? std::string("Lyrics 3D") : view.title;
    if (const TextTexture* t = text(env, factory, title, 22.0f * s, true, textW, budget)) {
        const float sh[4] = {0, 0, 0, 0.35f};
        tex(*t, tx, cy + cover * 0.5f - t->height + 8.0f * s + 1.5f * s, 1.0f, 1.0f, cu, cu, cu, 1.0f, true, sh);
        const float white[4] = {1, 1, 1, 1};
        tex(*t, tx, cy + cover * 0.5f - t->height + 8.0f * s, 1.0f, 1.0f, cu, cu, cu, 1.0f, true, white);
    }
    if (!view.artist.empty()) {
        if (const TextTexture* t = text(env, factory, view.artist, 16.0f * s, false, textW, budget)) {
            const float soft[4] = {1, 1, 1, 0.62f};
            tex(*t, tx, cy + cover * 0.5f + 2.0f * s, 1.0f, 1.0f, cu, cu, cu, 1.0f, true, soft);
        }
    }

    float ly = cy + cover;
    // --- lyric lines
    if (lyrics) {
        ly += 12.0f * s;
        const int shown = idx;
        const std::string cur = shown >= 0 ? lineText(shown) : std::string();
        const std::string outgoing = (ease < 1.0f && mPrevIdx != mIdx) ? lineText(mPrevIdx) : std::string();
        const int64_t start = shown >= 0 ? lrc.lines[static_cast<size_t>(shown)].timeMs : 0;
        const int64_t end = (shown >= 0 && shown + 1 < n) ? lrc.lines[static_cast<size_t>(shown) + 1].timeMs : start + 4000;
        const float prog = shown >= 0 ? std::clamp(static_cast<float>(view.positionMs - start) /
                                                   static_cast<float>(std::max<int64_t>(end - start, 400)),
                                                   0.0f, 1.0f)
                                      : 0.0f;
        const float rowH = 46.0f * s;
        if (!cur.empty()) {
            if (const TextTexture* t = text(env, factory, cur, 33.0f * s, true, static_cast<int>(inner), budget)) {
                const float oy = (1.0f - ease) * 16.0f * s;
                const float sh[4] = {0, 0, 0, 0.35f};
                const float px = x + pad, py = ly + (rowH - t->height) * 0.5f + oy;
                tex(*t, px, py + 1.5f * s, 1.0f, 1.0f, cs, dim, cg, ease, true, sh);
                tex(*t, px, py, 1.0f, prog, cs, dim, cg, ease, false, zero4);
            }
        }
        if (!outgoing.empty() && ease < 1.0f) {
            if (const TextTexture* t = text(env, factory, outgoing, 33.0f * s, true, static_cast<int>(inner), budget)) {
                const float oy = -ease * 16.0f * s;
                tex(*t, x + pad, ly + (rowH - t->height) * 0.5f + oy, 1.0f, 1.0f, cs, dim, cg, 1.0f - ease, false,
                    zero4);
            }
        }
        ly += rowH;
        const std::string next = lineText(shown + 1);
        if (!next.empty()) {
            if (const TextTexture* t = text(env, factory, next, 19.0f * s, false, static_cast<int>(inner), budget)) {
                const float soft[4] = {1, 1, 1, 0.5f};
                tex(*t, x + pad, ly + (30.0f * s - t->height) * 0.5f + (1.0f - ease) * 8.0f * s, 1.0f, 1.0f, cu, cu, cu,
                    ease, true, soft);
            }
        }
        ly += 30.0f * s;
    }

    // --- progress bar
    if (bar) {
        ly += 12.0f * s;
        const float track[4] = {1, 1, 1, 0.22f}, fill[4] = {1, 1, 1, 0.95f};
        const float barY = ly + 3.0f * s, barH = 5.0f * s;
        const float frac = std::clamp(static_cast<float>(view.positionMs) / static_cast<float>(view.durationMs), 0.0f, 1.0f);
        const float timeW = 44.0f * s;
        const float bx = x + pad + timeW, bw = inner - 2.0f * timeW;
        rect(bx, barY, bw, barH, barH * 0.5f, 1, track, track, opacity, 0.0f, 0.0f);
        if (frac > 0.002f) {
            const float fillC[4] = {cs[0] * 0.5f + 0.5f, cs[1] * 0.5f + 0.5f, cs[2] * 0.5f + 0.5f, 1.0f};
            rect(bx, barY, std::max(barH, bw * frac), barH, barH * 0.5f, 1, fillC, fillC, opacity, 0.0f, 0.0f);
            rect(bx + bw * frac - barH, barY - barH * 0.35f, barH * 1.7f, barH * 1.7f, barH * 0.85f, 1, fill, fill,
                 opacity, 0.0f, 0.0f);
        }
        if (const TextTexture* t = text(env, factory, clock(view.positionMs), 13.0f * s, false, 200, budget)) {
            const float soft[4] = {1, 1, 1, 0.7f};
            tex(*t, x + pad, ly + (20.0f * s - t->height) * 0.5f, 1.0f, 1.0f, cu, cu, cu, 1.0f, true, soft);
        }
        if (const TextTexture* t = text(env, factory, clock(view.durationMs), 13.0f * s, false, 200, budget)) {
            const float soft[4] = {1, 1, 1, 0.7f};
            tex(*t, x + cardW - pad - t->width, ly + (20.0f * s - t->height) * 0.5f, 1.0f, 1.0f, cu, cu, cu, 1.0f, true,
                soft);
        }
    }
    (void)colA;
    (void)colB;
}

} // namespace lyrics
