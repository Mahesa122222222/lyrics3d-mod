#include "PlayerPanel.h"

#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include "Log.h"

namespace lyrics {
namespace {

namespace mm = pl::modmenu;

// ------------------------------------------------------------------ small helpers

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool has(float px, float py) const { return px >= x && px <= x + w && py >= y && py <= y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    float r() const { return x + w; }
    float b() const { return y + h; }
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

constexpr uint32_t argb(int r, int g, int b, int a) {
    return (static_cast<uint32_t>(a & 255) << 24) | (static_cast<uint32_t>(r & 255) << 16) |
           (static_cast<uint32_t>(g & 255) << 8) | static_cast<uint32_t>(b & 255);
}
// multiply the alpha of a color
inline uint32_t mulA(uint32_t c, float k) {
    const float a = static_cast<float>(c >> 24) * clampf(k, 0.f, 1.f);
    return (static_cast<uint32_t>(a + 0.5f) << 24) | (c & 0x00FFFFFFu);
}
inline uint32_t withA(uint32_t c, int a) { return (static_cast<uint32_t>(a & 255) << 24) | (c & 0x00FFFFFFu); }

void hsv(float h, float s, float v, int& r, int& g, int& b) {
    h = std::fmod(h, 360.f);
    if (h < 0) h += 360.f;
    const float c = v * s, x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f)), m = v - c;
    float rr = 0, gg = 0, bb = 0;
    if (h < 60) { rr = c; gg = x; }
    else if (h < 120) { rr = x; gg = c; }
    else if (h < 180) { gg = c; bb = x; }
    else if (h < 240) { gg = x; bb = c; }
    else if (h < 300) { rr = x; bb = c; }
    else { rr = c; bb = x; }
    r = static_cast<int>((rr + m) * 255.f + 0.5f);
    g = static_cast<int>((gg + m) * 255.f + 0.5f);
    b = static_cast<int>((bb + m) * 255.f + 0.5f);
}

uint32_t keyHash(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}
float hueOf(uint32_t h) { return static_cast<float>(h % 360u); }
uint32_t accentOf(uint32_t h) {
    int r, g, b;
    hsv(hueOf(h), 0.62f, 1.0f, r, g, b);
    return argb(r, g, b, 255);
}

// Rough text width (the overlay cannot measure text): narrow / normal / wide characters.
float textWidth(const std::string& s, float size) {
    float w = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        float k;
        if (c < 0x80) {
            if (c == ' ' || c == 'i' || c == 'l' || c == 'j' || c == '.' || c == ',' || c == '\'' || c == ':' || c == ';' ||
                c == '!' || c == '|' || c == 'I' || c == 't' || c == 'f' || c == 'r')
                k = 0.34f;
            else if ((c >= 'A' && c <= 'Z') || c == 'm' || c == 'w' || c == 'M' || c == 'W')
                k = 0.64f;
            else
                k = 0.54f;
        } else {
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else len = 4;
            k = (c < 0xE0) ? 0.58f : 1.0f; // Latin supplement is narrow, CJK etc. is wide
        }
        w += k * size;
        i += len;
    }
    return w;
}

std::string cutTo(const std::string& s, size_t bytes) {
    if (s.size() <= bytes) return s;
    size_t n = bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) --n;
    return s.substr(0, n);
}

// Shorten with "..." so that the estimated width fits.
std::string fit(const std::string& s, float size, float maxW) {
    if (maxW <= 0) return {};
    if (textWidth(s, size) <= maxW) return s;
    std::string t = s;
    while (!t.empty() && textWidth(t, size) + size * 1.6f > maxW) {
        size_t n = t.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(t[n]) & 0xC0u) == 0x80u) --n;
        t.resize(n);
    }
    return t + "...";
}

std::string mmss(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t s = ms / 1000;
    char b[32];
    std::snprintf(b, sizeof(b), "%lld:%02lld", static_cast<long long>(s / 60), static_cast<long long>(s % 60));
    return b;
}

// ------------------------------------------------------------------ icons (software rasterized, white or black)

struct Gray {
    static constexpr int N = 64;
    std::vector<float> a = std::vector<float>(N * N, 0.f);
    // `inside` works in a 24 x 24 design space.
    template <class F>
    void add(F inside) {
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                int hit = 0;
                for (int sy = 0; sy < 3; ++sy)
                    for (int sx = 0; sx < 3; ++sx) {
                        const float px = (static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / 3.f) * 24.f / N;
                        const float py = (static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / 3.f) * 24.f / N;
                        if (inside(px, py)) ++hit;
                    }
                float& p = a[static_cast<size_t>(y * N + x)];
                p = std::max(p, static_cast<float>(hit) / 9.f);
            }
    }
};

bool inTri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy) {
    auto s = [](float x1, float y1, float x2, float y2, float x3, float y3) {
        return (x1 - x3) * (y2 - y3) - (x2 - x3) * (y1 - y3);
    };
    const float d1 = s(px, py, ax, ay, bx, by), d2 = s(px, py, bx, by, cx, cy), d3 = s(px, py, cx, cy, ax, ay);
    const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}
bool inRect(float px, float py, float x0, float y0, float x1, float y1) { return px >= x0 && px <= x1 && py >= y0 && py <= y1; }
bool inCircle(float px, float py, float cx, float cy, float r) { return (px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r; }
bool nearSeg(float px, float py, float x0, float y0, float x1, float y1, float half) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / l2 : 0.f;
    t = clampf(t, 0.f, 1.f);
    const float qx = x0 + t * dx, qy = y0 + t * dy;
    return (px - qx) * (px - qx) + (py - qy) * (py - qy) <= half * half;
}

void shapeFor(Gray& g, const std::string& name) {
    if (name == "play") {
        g.add([](float x, float y) { return inTri(x, y, 7.5f, 4.2f, 20.2f, 12.f, 7.5f, 19.8f); });
    } else if (name == "pause") {
        g.add([](float x, float y) { return inRect(x, y, 6.f, 4.5f, 10.4f, 19.5f) || inRect(x, y, 13.6f, 4.5f, 18.f, 19.5f); });
    } else if (name == "next") {
        g.add([](float x, float y) {
            return inTri(x, y, 5.f, 5.f, 16.f, 12.f, 5.f, 19.f) || inRect(x, y, 16.4f, 5.f, 19.2f, 19.f);
        });
    } else if (name == "prev") {
        g.add([](float x, float y) {
            return inTri(x, y, 19.f, 5.f, 8.f, 12.f, 19.f, 19.f) || inRect(x, y, 4.8f, 5.f, 7.6f, 19.f);
        });
    } else if (name == "close") {
        g.add([](float x, float y) {
            return nearSeg(x, y, 6.f, 6.f, 18.f, 18.f, 1.25f) || nearSeg(x, y, 18.f, 6.f, 6.f, 18.f, 1.25f);
        });
    } else if (name == "settings") {
        g.add([](float x, float y) {
            return nearSeg(x, y, 3.f, 6.f, 21.f, 6.f, 1.0f) || nearSeg(x, y, 3.f, 12.f, 21.f, 12.f, 1.0f) ||
                   nearSeg(x, y, 3.f, 18.f, 21.f, 18.f, 1.0f) || inCircle(x, y, 8.f, 6.f, 2.7f) || inCircle(x, y, 16.f, 12.f, 2.7f) ||
                   inCircle(x, y, 10.f, 18.f, 2.7f);
        });
    } else if (name == "note") {
        g.add([](float x, float y) {
            return inCircle(x, y, 8.f, 17.5f, 3.6f) || inRect(x, y, 10.6f, 4.5f, 12.4f, 17.5f) ||
                   inTri(x, y, 12.4f, 4.5f, 19.f, 8.f, 12.4f, 10.5f);
        });
    }
}

bool registerIcon(const std::string& name, bool black) {
    Gray g;
    shapeFor(g, name);
    std::vector<unsigned char> px(static_cast<size_t>(Gray::N * Gray::N * 4));
    for (int i = 0; i < Gray::N * Gray::N; ++i) {
        const unsigned char v = black ? 12 : 255;
        px[static_cast<size_t>(i) * 4 + 0] = v;
        px[static_cast<size_t>(i) * 4 + 1] = v;
        px[static_cast<size_t>(i) * 4 + 2] = v;
        px[static_cast<size_t>(i) * 4 + 3] = static_cast<unsigned char>(clampf(g.a[static_cast<size_t>(i)], 0.f, 1.f) * 255.f + 0.5f);
    }
    return mm::registerImage("lyr3d.ic." + name + (black ? ".k" : ".w"), std::span<const unsigned char>(px.data(), px.size()),
                             Gray::N, Gray::N);
}

// Generated album art: a gradient with a note, colors from the song name.
std::string makeCover(const std::string& id, uint32_t key) {
    constexpr int N = 128;
    std::vector<unsigned char> px(static_cast<size_t>(N * N * 4));
    int r1, g1, b1, r2, g2, b2;
    hsv(hueOf(key), 0.70f, 0.95f, r1, g1, b1);
    hsv(hueOf(key) + 55.f, 0.85f, 0.45f, r2, g2, b2);
    const float rad = 18.f;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const float t = clampf((static_cast<float>(x) * 0.45f + static_cast<float>(y) * 0.55f) / static_cast<float>(N), 0.f, 1.f);
            float r = lerpf(static_cast<float>(r1), static_cast<float>(r2), t);
            float g = lerpf(static_cast<float>(g1), static_cast<float>(g2), t);
            float b = lerpf(static_cast<float>(b1), static_cast<float>(b2), t);
            // soft light spot top-left
            const float dx = static_cast<float>(x) / N - 0.28f, dy = static_cast<float>(y) / N - 0.22f;
            const float spot = std::exp(-(dx * dx + dy * dy) * 9.f) * 0.28f;
            r += (255.f - r) * spot;
            g += (255.f - g) * spot;
            b += (255.f - b) * spot;
            // note, in the 24 design space centered
            float nc = 0;
            for (int sy = 0; sy < 2; ++sy)
                for (int sx = 0; sx < 2; ++sx) {
                    const float fx = ((static_cast<float>(x) + 0.25f + 0.5f * static_cast<float>(sx)) / N) * 24.f;
                    const float fy = ((static_cast<float>(y) + 0.25f + 0.5f * static_cast<float>(sy)) / N) * 24.f;
                    // map the middle 14 units of the design space onto the cover
                    const float px2 = 12.f + (fx - 12.f) * 1.9f, py2 = 12.f + (fy - 12.f) * 1.9f;
                    const bool in = inCircle(px2, py2, 8.f, 17.5f, 3.6f) || inRect(px2, py2, 10.6f, 4.5f, 12.4f, 17.5f) ||
                                    inTri(px2, py2, 12.4f, 4.5f, 19.f, 8.f, 12.4f, 10.5f);
                    if (in) nc += 0.25f;
                }
            r = lerpf(r, 255.f, nc * 0.88f);
            g = lerpf(g, 255.f, nc * 0.88f);
            b = lerpf(b, 255.f, nc * 0.88f);
            // rounded corners with 1 px antialiasing
            const float qx = std::fabs(static_cast<float>(x) + 0.5f - N * 0.5f) - (N * 0.5f - rad);
            const float qy = std::fabs(static_cast<float>(y) + 0.5f - N * 0.5f) - (N * 0.5f - rad);
            const float d = std::sqrt(std::max(qx, 0.f) * std::max(qx, 0.f) + std::max(qy, 0.f) * std::max(qy, 0.f)) - rad;
            const float alpha = clampf(0.5f - d, 0.f, 1.f);
            const size_t i = static_cast<size_t>(y * N + x) * 4;
            px[i + 0] = static_cast<unsigned char>(clampf(r, 0.f, 255.f));
            px[i + 1] = static_cast<unsigned char>(clampf(g, 0.f, 255.f));
            px[i + 2] = static_cast<unsigned char>(clampf(b, 0.f, 255.f));
            px[i + 3] = static_cast<unsigned char>(alpha * 255.f);
        }
    if (!mm::registerImage(id, std::span<const unsigned char>(px.data(), px.size()), N, N)) return {};
    return id;
}

// ------------------------------------------------------------------ layout (used by drawing and touch)

struct Layout {
    float u = 1, w = 0, h = 0;
    Rect mini, miniBtn, outer, close, left, right, cover, seek, bPrev, bPlay, bNext, bShuf, bLoop, vol, tab[3], content;
    Rect libHeader, rescan, list;
    float rowH = 42;
    float setRowH = 44;
};

Layout makeLayout(float w, float h, const PanelConfig& c) {
    Layout L;
    L.w = w;
    L.h = h;
    const float u = clampf(h / 540.f, 0.65f, 2.5f);
    L.u = u;
    const float f = clampf(c.scale, 0.6f, 1.6f);
    const float miniW = std::min(262.f * u * f, w * 0.45f), miniH = 50.f * u * f;
    L.mini = {clampf((w - miniW) * clampf(c.x, 0.f, 1.f), 4 * u, w - miniW - 4 * u),
              clampf((h - miniH) * clampf(c.y, 0.f, 1.f), 4 * u, h - miniH - 4 * u), miniW, miniH};
    const float side = 34.f * u * f;
    L.miniBtn = {L.mini.r() - side - 8 * u * f, L.mini.y + (miniH - side) / 2, side, side};

    L.outer = {w * 0.045f, h * 0.12f, w * 0.91f, h * 0.86f};
    L.close = {L.outer.r() - 42 * u, L.outer.y + 12 * u, 30 * u, 30 * u};
    const float contentY = L.outer.y + 56 * u, contentH = L.outer.h - 68 * u;
    const float cx = L.outer.x + 14 * u, cw = L.outer.w - 28 * u, gut = 10 * u, leftW = cw * 0.38f;
    L.left = {cx, contentY, leftW, contentH};
    L.right = {cx + leftW + gut, contentY, cw - leftW - gut, contentH};

    const float pad = 16 * u;
    const float cov = std::min(L.left.w - 2 * pad, L.left.h * 0.40f);
    L.cover = {L.left.x + (L.left.w - cov) / 2, L.left.y + pad, cov, cov};
    L.seek = {L.left.x + pad, L.left.b() - 134 * u, L.left.w - 2 * pad, 6 * u};
    const float cy = L.left.b() - 74 * u, mid = L.left.cx();
    L.bPlay = {mid - 26 * u, cy - 26 * u, 52 * u, 52 * u};
    L.bPrev = {mid - 26 * u - 16 * u - 38 * u, cy - 19 * u, 38 * u, 38 * u};
    L.bNext = {mid + 26 * u + 16 * u, cy - 19 * u, 38 * u, 38 * u};
    const float vy = L.left.b() - 38 * u;
    L.bShuf = {L.left.x + pad, vy, 52 * u, 24 * u};
    L.bLoop = {L.bShuf.r() + 8 * u, vy, 52 * u, 24 * u};
    L.vol = {L.bLoop.r() + 16 * u, vy + 9 * u, L.left.r() - pad - (L.bLoop.r() + 16 * u), 6 * u};

    const float tgap = 6 * u, tw = (L.right.w - 24 * u - 2 * tgap) / 3.f;
    for (int i = 0; i < 3; ++i) L.tab[i] = {L.right.x + 12 * u + static_cast<float>(i) * (tw + tgap), L.right.y + 10 * u, tw, 30 * u};
    L.content = {L.right.x + 12 * u, L.right.y + 52 * u, L.right.w - 24 * u, L.right.h - 62 * u};
    L.libHeader = {L.content.x, L.content.y, L.content.w, 34 * u};
    L.rescan = {L.content.r() - 78 * u, L.content.y + 4 * u, 78 * u, 26 * u};
    L.list = {L.content.x, L.content.y + 40 * u, L.content.w, L.content.h - 40 * u};
    L.rowH = 42 * u;
    L.setRowH = std::min(44.f * u, L.content.h / 8.2f);
    return L;
}

Rect settingRow(const Layout& L, int i) {
    return {L.content.x, L.content.y + static_cast<float>(i) * L.setRowH, L.content.w, L.setRowH - 5 * L.u};
}
Rect segRect(const Rect& row, int i, float u) {
    const float sw = 78 * u, gap = 4 * u;
    const float x0 = row.r() - 12 * u - 3 * sw - 2 * gap;
    return {x0 + static_cast<float>(i) * (sw + gap), row.y + 6 * u, sw, row.h - 12 * u};
}
Rect pillRect(const Rect& row, float u) { return {row.r() - 12 * u - 52 * u, row.y + (row.h - 26 * u) / 2, 52 * u, 26 * u}; }
Rect minusRect(const Rect& row, float u) { return {row.r() - 12 * u - 128 * u, row.y + (row.h - 28 * u) / 2, 30 * u, 28 * u}; }
Rect plusRect(const Rect& row, float u) { return {row.r() - 12 * u - 30 * u, row.y + (row.h - 28 * u) / 2, 30 * u, 28 * u}; }

const char* const kModes[3] = {"dynamic", "world", "screen"};
const char* const kModeNames[3] = {"Real 3D", "Floating", "Screen"};

std::string num(float v, int decimals) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.*f", decimals, static_cast<double>(v));
    return b;
}

constexpr uint32_t kText = 0xFFFFFFFFu, kSub = 0xFFB3B3B3u, kDim = 0xFF7A7A7Au;
constexpr uint32_t kGreen = 0xFF1ED760u;

} // namespace

// ------------------------------------------------------------------ input thread

bool PlayerPanel::isOpen() const {
    std::lock_guard<std::mutex> lk(mMu);
    return mOpen;
}

bool PlayerPanel::onTouch(int action, int pointerId, float x, float y) {
    const int a = action & 0xFF;
    const bool down = a == 0 || a == 5, up = a == 1 || a == 6 || a == 3, move = a == 2;
    const mm::HudSurfaceSize sz = mm::getHudSurfaceSize();
    std::lock_guard<std::mutex> lk(mMu);
    if (sz.width <= 0 || sz.height <= 0) return false;
    const Layout L = makeLayout(sz.width, sz.height, mCfg);
    const float u = L.u;

    // ---- a finger that this panel already owns
    if (mCap != Cap::None && mCapId == pointerId) {
        if (mCap == Cap::Seek) {
            if (move || down) {
                mSeekFrac = clampf((x - L.seek.x) / L.seek.w, 0.f, 1.f);
                mSeeking = true;
            } else if (up) {
                if (mDurMs > 0) {
                    Ev e;
                    e.k = Ev::Seek;
                    e.ms = static_cast<int64_t>(static_cast<double>(mDurMs) * static_cast<double>(mSeekFrac));
                    mEvents.push_back(e);
                }
                mSeeking = false;
                mCap = Cap::None;
            }
        } else if (mCap == Cap::Volume) {
            if (move || down) {
                mVolDrag = clampf((x - L.vol.x) / L.vol.w, 0.f, 1.f);
                Ev e;
                e.k = Ev::Set;
                e.a = "volume";
                e.b = num(mVolDrag, 2);
                mEvents.push_back(e);
            } else if (up) {
                mVolDrag = -1.f;
                mCap = Cap::None;
            }
        } else if (mCap == Cap::List) {
            if (move) {
                const float dy = y - mCapStartY;
                if (std::fabs(dy) > 8 * u) mCapMoved = true;
                if (mCapMoved) {
                    const float maxScroll = std::max(0.f, static_cast<float>(mLibCount) * L.rowH - L.list.h);
                    mScroll = clampf(mCapStartScroll - dy, 0.f, maxScroll);
                }
            } else if (up) {
                if (!mCapMoved) {
                    const int idx = static_cast<int>(std::floor((mCapStartY - L.list.y + mScroll) / L.rowH));
                    if (idx >= 0 && idx < mLibCount) {
                        Ev e;
                        e.k = Ev::Play;
                        e.i = idx;
                        mEvents.push_back(e);
                    }
                }
                mCap = Cap::None;
            }
        } else { // Swallow
            if (up) mCap = Cap::None;
        }
        return true;
    }

    if (!down) return false; // moves / ups of fingers that the game owns

    auto own = [&](Cap c) {
        mCap = c;
        mCapId = pointerId;
        mCapStartX = x;
        mCapStartY = y;
        mCapStartScroll = mScroll;
        mCapMoved = false;
        return true;
    };
    auto push = [&](Ev::K k) {
        Ev e;
        e.k = k;
        mEvents.push_back(e);
    };

    // ---- closed: only the pill
    if (!mOpen) {
        if (!mCfg.show || !L.mini.has(x, y)) return false;
        if (L.miniBtn.has(x, y)) {
            push(Ev::Toggle);
        } else {
            mOpen = true;
            mToggleT = -1.0; // render thread fixes the time
        }
        return own(Cap::Swallow);
    }

    // ---- open
    if (L.close.has(x, y) || !L.outer.has(x, y)) {
        mOpen = false;
        mToggleT = -1.0;
        return own(Cap::Swallow);
    }
    if (L.seek.x - 6 * u <= x && x <= L.seek.r() + 6 * u && std::fabs(y - L.seek.cy()) <= 16 * u) {
        mSeekFrac = clampf((x - L.seek.x) / L.seek.w, 0.f, 1.f);
        mSeeking = true;
        return own(Cap::Seek);
    }
    if (L.vol.x - 6 * u <= x && x <= L.vol.r() + 6 * u && std::fabs(y - L.vol.cy()) <= 14 * u) {
        mVolDrag = clampf((x - L.vol.x) / L.vol.w, 0.f, 1.f);
        Ev e;
        e.k = Ev::Set;
        e.a = "volume";
        e.b = num(mVolDrag, 2);
        mEvents.push_back(e);
        return own(Cap::Volume);
    }
    if (L.bPlay.has(x, y)) { push(Ev::Toggle); return own(Cap::Swallow); }
    if (L.bPrev.has(x, y)) { push(Ev::Prev); return own(Cap::Swallow); }
    if (L.bNext.has(x, y)) { push(Ev::Next); return own(Cap::Swallow); }
    if (L.bShuf.has(x, y)) {
        Ev e;
        e.k = Ev::Set;
        e.a = "shuffle";
        e.b = "toggle";
        mEvents.push_back(e);
        return own(Cap::Swallow);
    }
    if (L.bLoop.has(x, y)) {
        Ev e;
        e.k = Ev::Set;
        e.a = "loop";
        e.b = "toggle";
        mEvents.push_back(e);
        return own(Cap::Swallow);
    }
    for (int i = 0; i < 3; ++i)
        if (L.tab[i].has(x, y)) {
            mTab = static_cast<Tab>(i);
            return own(Cap::Swallow);
        }
    if (!L.content.has(x, y)) return own(Cap::Swallow);

    if (mTab == Tab::Library) {
        if (L.rescan.has(x, y)) {
            push(Ev::Rescan);
            return own(Cap::Swallow);
        }
        if (L.list.has(x, y)) return own(Cap::List);
        return own(Cap::Swallow);
    }
    if (mTab == Tab::Lyrics) {
        // tap a line = jump to it
        const float step = 36 * u;
        const float centerY = L.content.cy();
        const int idx = static_cast<int>(std::lround(mLyrScroll + (y - centerY) / step));
        if (idx >= 0 && idx < static_cast<int>(mLineTimes.size()) && mLineTimes[static_cast<size_t>(idx)] >= 0) {
            Ev e;
            e.k = Ev::SeekLine;
            e.ms = mLineTimes[static_cast<size_t>(idx)];
            mEvents.push_back(e);
        }
        return own(Cap::Swallow);
    }
    // settings
    auto set = [&](const char* k, std::string v) {
        Ev e;
        e.k = Ev::Set;
        e.a = k;
        e.b = std::move(v);
        mEvents.push_back(e);
    };
    {
        const Rect r0 = settingRow(L, 0);
        for (int i = 0; i < 3; ++i)
            if (segRect(r0, i, u).has(x, y)) set("layoutMode", kModes[i]);
    }
    if (pillRect(settingRow(L, 1), u).has(x, y)) set("cardShow", mSetCard ? "false" : "true");
    if (pillRect(settingRow(L, 5), u).has(x, y)) set("r3Outline", mSetOutline ? "false" : "true");
    if (minusRect(settingRow(L, 2), u).has(x, y)) set("r3Size", num(clampf(mSetSize - 0.05f, 0.2f, 1.6f), 2));
    if (plusRect(settingRow(L, 2), u).has(x, y)) set("r3Size", num(clampf(mSetSize + 0.05f, 0.2f, 1.6f), 2));
    if (minusRect(settingRow(L, 3), u).has(x, y)) set("r3Scatter", num(clampf(mSetScatter - 10.f, 20.f, 160.f), 0));
    if (plusRect(settingRow(L, 3), u).has(x, y)) set("r3Scatter", num(clampf(mSetScatter + 10.f, 20.f, 160.f), 0));
    if (minusRect(settingRow(L, 4), u).has(x, y)) set("r3Height", num(clampf(mSetHeight - 0.25f, -3.f, 3.f), 2));
    if (plusRect(settingRow(L, 4), u).has(x, y)) set("r3Height", num(clampf(mSetHeight + 0.25f, -3.f, 3.f), 2));
    {
        const Rect r7 = settingRow(L, 7);
        const Rect b{r7.r() - 12 * u - 130 * u, r7.y + 3 * u, 130 * u, r7.h - 6 * u};
        if (b.has(x, y)) push(Ev::OpenMenu);
    }
    return own(Cap::Swallow);
}

// ------------------------------------------------------------------ render thread

void PlayerPanel::shutdown(const std::string_view moduleId) {
    if (mDrewSomething) {
        mm::submitDrawCommands(moduleId, std::span<const mm::DrawCommand>());
        mDrewSomething = false;
    }
    std::lock_guard<std::mutex> lk(mMu);
    mOpen = false;
    mCap = Cap::None;
    mEvents.clear();
}

void PlayerPanel::frame(double t, bool ready, const PanelConfig& cfg, const PanelModel& m, const std::string_view moduleId,
                        const PanelHost& host) {
    if (!ready) return;
    if (!mIconsDone) {
        mIconsDone = true;
        bool ok = true;
        for (const char* n : {"play", "pause", "next", "prev", "close", "settings", "note"}) {
            ok = registerIcon(n, false) && ok;
            ok = registerIcon(n, true) && ok;
        }
        LYR_LOGI("Player panel: icons %s", ok ? "registered" : "FAILED");
    }
    const mm::HudSurfaceSize sz = mm::getHudSurfaceSize();
    if (sz.width <= 0 || sz.height <= 0) return;

    // ---- take the UI state, hand over the model, run the actions
    std::vector<Ev> evs;
    bool open;
    Tab tab;
    float scroll, seekFrac, volDrag;
    bool seeking;
    double toggleT;
    {
        std::lock_guard<std::mutex> lk(mMu);
        mCfg = cfg;
        mSurfW = sz.width;
        mSurfH = sz.height;
        mLibCount = m.songTitles ? static_cast<int>(m.songTitles->size()) : 0;
        mDurMs = m.durMs;
        mHasTrack = m.hasTrack;
        mSetCard = m.cardShow;
        mSetOutline = m.r3Outline;
        mSetSize = m.r3Size;
        mSetScatter = m.r3Scatter;
        mSetHeight = m.r3Height;
        if (mToggleT < 0.0) mToggleT = t;
        if (m.lrc != mLrcSeen || (m.lrc && m.lrc->lines.size() != mLineTimes.size())) {
            mLrcSeen = m.lrc;
            mLineTimes.clear();
            if (m.lrc)
                for (const auto& ln : m.lrc->lines) mLineTimes.push_back(m.lrc->timed ? ln.timeMs : -1);
            mLyrScroll = 0.f;
        }
        evs.swap(mEvents);
        open = mOpen;
        tab = mTab;
        scroll = mScroll;
        seeking = mSeeking;
        seekFrac = mSeekFrac;
        volDrag = mVolDrag;
        toggleT = mToggleT;
    }
    for (const Ev& e : evs) {
        switch (e.k) {
        case Ev::Toggle: if (host.toggle) host.toggle(); break;
        case Ev::Next: if (host.next) host.next(); break;
        case Ev::Prev: if (host.prev) host.prev(); break;
        case Ev::Rescan: if (host.rescan) host.rescan(); break;
        case Ev::OpenMenu: if (host.openMenu) host.openMenu(); break;
        case Ev::Play: if (host.playIndex) host.playIndex(e.i); break;
        case Ev::Seek: if (host.seekMs) host.seekMs(e.ms); break;
        case Ev::SeekLine: if (host.seekMs) host.seekMs(e.ms + m.lyricOffsetMs); break;
        case Ev::Set:
            if (host.setSetting) {
                std::string v = e.b;
                if (v == "toggle") v = (e.a == "shuffle" ? !m.shuffle : !m.loop) ? "true" : "false";
                host.setSetting(e.a, v);
            }
            break;
        }
    }

    const double dt = std::clamp(t - mLastFrame, 0.0, 0.25);
    mLastFrame = t;

    // ---- animation state
    const float prog = clampf(static_cast<float>((t - toggleT) / 0.22), 0.f, 1.f);
    const float ease = prog * prog * (3.f - 2.f * prog);
    const float fade = open ? ease : 1.f - ease;
    const bool panelVisible = open || fade > 0.001f;

    int active = -1;
    if (m.lrc && !m.lrc->lines.empty()) active = m.lrc->timed ? findLineIndex(m.lrc->lines, m.lyricPosMs) : 0;
    {
        std::lock_guard<std::mutex> lk(mMu);
        const float target = static_cast<float>(active);
        if (std::fabs(target - mLyrScroll) > 7.f) mLyrScroll = target;
        else mLyrScroll += (target - mLyrScroll) * static_cast<float>(1.0 - std::exp(-dt * 9.0));
    }
    float lyrScroll;
    {
        std::lock_guard<std::mutex> lk(mMu);
        lyrScroll = mLyrScroll;
    }

    // Idle and nothing to show: clear once.
    if (!cfg.show && !panelVisible) {
        if (mDrewSomething) {
            mm::submitDrawCommands(moduleId, std::span<const mm::DrawCommand>());
            mDrewSomething = false;
        }
        return;
    }
    if (t - mLastSubmit < 0.033) return;
    mLastSubmit = t;

    // ---- cover + accent for this song
    const std::string songKey = m.title + "|" + m.artist;
    const uint32_t key = keyHash(songKey);
    const uint32_t accent = m.hasTrack ? accentOf(key) : kGreen;
    std::string coverId;
    if (m.hasTrack && !m.title.empty()) {
        auto it = mCoverIds.find(songKey);
        if (it != mCoverIds.end()) coverId = it->second;
        else if (mCoverIds.size() < 80) {
            coverId = makeCover("lyr3d.cover." + std::to_string(key), key);
            mCoverIds[songKey] = coverId;
        }
    }

    const Layout L = makeLayout(sz.width, sz.height, cfg);
    const float u = L.u;
    const float dy = (1.f - fade) * 16.f * u;
    const float al = fade;

    std::vector<mm::DrawCommand> out;
    out.reserve(220);
    auto box = [&](Rect r, uint32_t color, float radius, float a = 1.f, bool slide = true) {
        mm::DrawCommand c;
        c.type = mm::DrawCommandType::RectFilled;
        c.x = r.x;
        c.y = r.y + (slide ? dy : 0.f);
        c.w = r.w;
        c.h = r.h;
        c.x3 = radius;
        c.color = mulA(color, a);
        out.push_back(std::move(c));
    };
    auto disc = [&](float cx, float cy, float rad, uint32_t color, float a = 1.f, bool slide = true) {
        mm::DrawCommand c;
        c.type = mm::DrawCommandType::CircleFilled;
        c.x = cx;
        c.y = cy + (slide ? dy : 0.f);
        c.size = rad;
        c.color = mulA(color, a);
        out.push_back(std::move(c));
    };
    auto txt = [&](float x, float y, const std::string& s, float size, uint32_t color, float a = 1.f, bool slide = true) {
        if (s.empty()) return;
        mm::DrawCommand c;
        c.type = mm::DrawCommandType::Text;
        c.x = x;
        c.y = y + (slide ? dy : 0.f);
        c.size = size;
        c.color = mulA(color, a);
        c.text = s;
        out.push_back(std::move(c));
    };
    auto ctxt = [&](float cx, float y, const std::string& s, float size, uint32_t color, float a = 1.f) {
        txt(cx - textWidth(s, size) / 2.f, y, s, size, color, a);
    };
    auto rtxt = [&](float rx, float y, const std::string& s, float size, uint32_t color, float a = 1.f) {
        txt(rx - textWidth(s, size), y, s, size, color, a);
    };
    auto img = [&](const std::string& id, Rect r, float a = 1.f, bool slide = true) {
        mm::DrawCommand c;
        c.type = mm::DrawCommandType::Image;
        c.x = r.x;
        c.y = r.y + (slide ? dy : 0.f);
        c.w = r.w;
        c.h = r.h;
        c.color = mulA(0xFFFFFFFFu, a);
        c.imageId = id;
        out.push_back(std::move(c));
    };
    auto icon = [&](const char* name, bool black, Rect r, float a = 1.f, bool slide = true) {
        img(std::string("lyr3d.ic.") + name + (black ? ".k" : ".w"), r, a, slide);
    };
    auto eq = [&](float x, float y, float w, float h, uint32_t color, float a, bool slide) {
        for (int i = 0; i < 3; ++i) {
            const float k = m.playing ? (0.25f + 0.75f * std::fabs(std::sin(static_cast<float>(t) * 3.6f + static_cast<float>(i) * 1.37f))) : 0.3f;
            const float bh = h * k, bw = w * 0.24f;
            box({x + static_cast<float>(i) * w * 0.38f, y + h - bh, bw, bh}, color, bw * 0.4f, a, slide);
        }
    };
    auto pill = [&](Rect r, uint32_t fill, float a, bool slide = true) { box(r, fill, r.h * 0.5f, a, slide); };

    // =============================================================== mini pill (always, when enabled)
    if (cfg.show && !open) {
        const float f = clampf(cfg.scale, 0.6f, 1.6f);
        const Rect p = L.mini;
        box({p.x + 2 * u, p.y + 4 * u, p.w, p.h}, 0x66000000u, 16 * u * f, 1.f, false);
        box(p, argb(255, 255, 255, 48), 16 * u * f, 1.f, false);                    // glass rim
        box({p.x + 1.2f * u, p.y + 1.2f * u, p.w - 2.4f * u, p.h - 2.4f * u}, 0xEC15171Cu, 15 * u * f, 1.f, false);
        box({p.x + 1.2f * u, p.y + 1.2f * u, p.w - 2.4f * u, p.h * 0.5f}, withA(accent, 26), 15 * u * f, 1.f, false); // tint
        const float cs = 36 * u * f;
        const Rect cv{p.x + 7 * u * f, p.y + (p.h - cs) / 2, cs, cs};
        if (!coverId.empty()) img(coverId, cv, 1.f, false);
        else {
            box(cv, 0xFF2A2C31u, 9 * u * f, 1.f, false);
            icon("note", false, {cv.x + cs * 0.2f, cv.y + cs * 0.2f, cs * 0.6f, cs * 0.6f}, 0.8f, false);
        }
        const float tx = cv.r() + 9 * u * f;
        const float tw = L.miniBtn.x - tx - 6 * u * f;
        const float fs = std::max(10.f, 12.5f * u * f);
        std::string title = m.hasTrack || !m.title.empty() ? m.title : std::string("Nothing playing");
        txt(tx, p.y + p.h * 0.42f, fit(title, fs, tw), fs, kText, 1.f, false);
        std::string sub = !m.artist.empty() ? m.artist : (m.hasTrack ? std::string("Tap to open") : std::string("Tap to pick a song"));
        txt(tx, p.y + p.h * 0.72f, fit(sub, fs * 0.78f, tw), fs * 0.78f, kSub, 1.f, false);
        // play / pause button
        disc(L.miniBtn.cx(), L.miniBtn.cy(), L.miniBtn.w * 0.5f, 0xFFFFFFFFu, 1.f, false);
        const float ic = L.miniBtn.w * 0.56f;
        icon(m.playing ? "pause" : "play", true, {L.miniBtn.cx() - ic / 2 + (m.playing ? 0.f : ic * 0.05f), L.miniBtn.cy() - ic / 2, ic, ic}, 1.f, false);
        if (m.durMs > 0) {
            const float bx = p.x + 12 * u * f, bw = p.w - 24 * u * f, by = p.b() - 6 * u * f;
            box({bx, by, bw, 2 * u * f}, 0x33FFFFFFu, u, 1.f, false);
            box({bx, by, bw * clampf(static_cast<float>(m.posMs) / static_cast<float>(m.durMs), 0.f, 1.f), 2 * u * f}, accent, u, 1.f, false);
        }
    }

    // =============================================================== full panel
    if (panelVisible) {
        box({0, 0, L.w, L.h}, 0xA0050608u, 0.f, al, false); // scrim
        const Rect o = L.outer;
        box({o.x, o.y + 6 * u, o.w, o.h}, 0x66000000u, 24 * u, al);              // shadow
        box(o, argb(255, 255, 255, 56), 24 * u, al);                              // glass rim
        box({o.x + 1.4f * u, o.y + 1.4f * u, o.w - 2.8f * u, o.h - 2.8f * u}, 0xF2101216u, 23 * u, al);
        box({o.x + 1.4f * u, o.y + 1.4f * u, o.w - 2.8f * u, 70 * u}, withA(accent, 22), 23 * u, al); // ambient tint on top

        // header
        box({o.x + 20 * u, o.y + 16 * u, 30 * u, 30 * u}, withA(accent, 60), 9 * u, al);
        icon("note", false, {o.x + 25 * u, o.y + 21 * u, 20 * u, 20 * u}, al);
        txt(o.x + 60 * u, o.y + 33 * u, "Lyrics 3D", 16 * u, kText, al);
        txt(o.x + 60 * u, o.y + 47 * u, "Music player", 9.5f * u, kSub, al);
        box(L.close, 0x26FFFFFFu, 15 * u, al);
        icon("close", false, {L.close.x + 5 * u, L.close.y + 5 * u, 20 * u, 20 * u}, al);

        // ---------------- left: now playing
        box(L.left, 0x12FFFFFFu, 18 * u, al);
        box({L.left.x, L.left.y, L.left.w, L.left.h}, withA(accent, 30), 18 * u, al);
        if (!coverId.empty()) img(coverId, L.cover, al);
        else {
            box(L.cover, 0xFF2A2C31u, 16 * u, al);
            icon("note", false, {L.cover.cx() - L.cover.w * 0.22f, L.cover.cy() - L.cover.w * 0.22f, L.cover.w * 0.44f, L.cover.w * 0.44f}, 0.7f * al);
        }
        const float lw = L.left.w - 32 * u;
        const float titleY = L.cover.b() + 28 * u;
        txt(L.left.x + 16 * u, titleY, fit(m.hasTrack || !m.title.empty() ? m.title : std::string("Nothing playing"), 17 * u, lw), 17 * u, kText, al);
        txt(L.left.x + 16 * u, titleY + 21 * u,
            fit(!m.artist.empty() ? m.artist : std::string(m.hasTrack ? "Unknown artist" : "Pick a song in Library"), 12 * u, lw), 12 * u, kSub, al);

        // progress
        const float shown = seeking ? seekFrac : (m.durMs > 0 ? clampf(static_cast<float>(m.posMs) / static_cast<float>(m.durMs), 0.f, 1.f) : 0.f);
        box(L.seek, 0x33FFFFFFu, 3 * u, al);
        box({L.seek.x, L.seek.y, std::max(L.seek.w * shown, 0.f), L.seek.h}, accent, 3 * u, al);
        disc(L.seek.x + L.seek.w * shown, L.seek.cy(), seeking ? 8 * u : 6 * u, 0xFFFFFFFFu, al);
        const int64_t shownMs = seeking ? static_cast<int64_t>(static_cast<double>(m.durMs) * seekFrac) : m.posMs;
        txt(L.seek.x, L.seek.b() + 18 * u, mmss(shownMs), 9.8f * u, kSub, al);
        rtxt(L.seek.r(), L.seek.b() + 18 * u, m.durMs > 0 ? mmss(m.durMs) : "--:--", 9.8f * u, kSub, al);

        // transport
        disc(L.bPlay.cx(), L.bPlay.cy(), L.bPlay.w * 0.5f, 0xFFFFFFFFu, al);
        const float pic = L.bPlay.w * 0.5f;
        icon(m.playing ? "pause" : "play", true, {L.bPlay.cx() - pic / 2 + (m.playing ? 0.f : pic * 0.05f), L.bPlay.cy() - pic / 2, pic, pic}, al);
        icon("prev", false, L.bPrev, al);
        icon("next", false, L.bNext, al);

        // shuffle / repeat / volume
        auto toggleChip = [&](Rect r, const char* label, bool on) {
            pill(r, on ? accent : 0x26FFFFFFu, al);
            ctxt(r.cx(), r.cy() + 3.6f * u, label, 10.f * u, on ? 0xFF101216u : kSub, al);
        };
        toggleChip(L.bShuf, "Shuffle", m.shuffle);
        toggleChip(L.bLoop, "Repeat", m.loop);
        const float vol = volDrag >= 0.f ? volDrag : clampf(m.volume, 0.f, 1.f);
        box(L.vol, 0x33FFFFFFu, 3 * u, al);
        box({L.vol.x, L.vol.y, L.vol.w * vol, L.vol.h}, 0xFFFFFFFFu, 3 * u, al);
        disc(L.vol.x + L.vol.w * vol, L.vol.cy(), 5.5f * u, 0xFFFFFFFFu, al);
        rtxt(L.vol.r(), L.vol.y - 8 * u, "Volume " + std::to_string(static_cast<int>(vol * 100.f + 0.5f)) + "%", 8.8f * u, kDim, al);

        // ---------------- right: tabs
        box(L.right, 0x12FFFFFFu, 18 * u, al);
        static const char* tabNames[3] = {"Lyrics", "Library", "Settings"};
        for (int i = 0; i < 3; ++i) {
            const bool on = static_cast<int>(tab) == i;
            pill(L.tab[i], on ? 0xFFFFFFFFu : 0x22FFFFFFu, al);
            ctxt(L.tab[i].cx(), L.tab[i].cy() + 4.2f * u, tabNames[i], 11.5f * u, on ? 0xFF101216u : kSub, al);
        }

        const Rect C = L.content;
        if (tab == Tab::Lyrics) {
            if (!m.lrc || m.lrc->lines.empty()) {
                ctxt(C.cx(), C.cy(), "Nothing is playing", 15 * u, kText, al);
                ctxt(C.cx(), C.cy() + 22 * u, "Pick a song in Library", 11.5f * u, kSub, al);
            } else {
                const float step = 36 * u;
                const int n = static_cast<int>(m.lrc->lines.size());
                const int lo = std::max(0, static_cast<int>(std::floor(lyrScroll)) - 5), hi = std::min(n - 1, static_cast<int>(std::ceil(lyrScroll)) + 5);
                for (int i = lo; i <= hi; ++i) {
                    const float d = std::fabs(static_cast<float>(i) - lyrScroll);
                    const float y = C.cy() + (static_cast<float>(i) - lyrScroll) * step;
                    if (y < C.y + 14 * u || y > C.b() - 6 * u) continue;
                    const float k = clampf(1.f - d, 0.f, 1.f); // 1 = active
                    float size = lerpf(14.f, 21.f, k) * u;
                    std::string s = m.lrc->lines[static_cast<size_t>(i)].text;
                    if (s.empty()) s = "...";
                    const float maxW = C.w - 24 * u;
                    if (textWidth(s, size) > maxW) size = std::max(size * 0.78f, size * maxW / textWidth(s, size));
                    s = fit(s, size, maxW);
                    const float fadeD = clampf(1.f - (d - 1.f) / 3.2f, 0.f, 1.f);
                    const float a2 = lerpf(0.30f, 1.f, k) * (d < 1.f ? 1.f : fadeD) * al;
                    const uint32_t col = k > 0.5f ? 0xFFFFFFFFu : kSub;
                    ctxt(C.cx(), y + size * 0.35f, s, size, col, a2);
                }
                if (!m.lrc->timed) ctxt(C.cx(), C.b() - 10 * u, "This song has no timed lyrics (.lrc)", 9.5f * u, kDim, al);
            }
        } else if (tab == Tab::Library) {
            const int count = m.songTitles ? static_cast<int>(m.songTitles->size()) : 0;
            txt(C.x + 4 * u, C.y + 21 * u, std::to_string(count) + (count == 1 ? " song" : " songs"), 12.f * u, kText, al);
            pill(L.rescan, 0x26FFFFFFu, al);
            ctxt(L.rescan.cx(), L.rescan.cy() + 3.8f * u, "Rescan", 10.5f * u, kText, al);
            if (count == 0) {
                const float fs = 11.f * u;
                ctxt(C.cx(), C.cy(), "No songs yet", 15 * u, kText, al);
                const std::string path = fit("Put .mp3 / .ogg / .m4a files and .lrc files in:", fs, C.w - 20 * u);
                ctxt(C.cx(), C.cy() + 22 * u, path, fs, kSub, al);
                ctxt(C.cx(), C.cy() + 40 * u, fit(m.songsDir, 10 * u, C.w - 20 * u), 10 * u, kDim, al);
            } else {
                const float maxScroll = std::max(0.f, static_cast<float>(count) * L.rowH - L.list.h);
                const float sc = clampf(scroll, 0.f, maxScroll);
                for (int i = 0; i < count; ++i) {
                    const float ry = L.list.y + static_cast<float>(i) * L.rowH - sc;
                    if (ry < L.list.y - 1.f || ry + L.rowH > L.list.b() + 1.f) continue;
                    const bool cur = i == m.currentSong;
                    const Rect row{L.list.x, ry, L.list.w - 10 * u, L.rowH - 4 * u};
                    if (cur) box(row, withA(accent, 52), 11 * u, al);
                    const std::string& title = (*m.songTitles)[static_cast<size_t>(i)];
                    const uint32_t kk = keyHash(title);
                    const Rect art{row.x + 8 * u, row.y + (row.h - 30 * u) / 2, 30 * u, 30 * u};
                    int r1, g1, b1;
                    hsv(hueOf(kk), 0.65f, 0.80f, r1, g1, b1);
                    box(art, argb(r1, g1, b1, 255), 8 * u, al);
                    if (cur) eq(art.x + 7 * u, art.y + 7 * u, 16 * u, 16 * u, 0xFFFFFFFFu, al, true);
                    else icon("note", false, {art.x + 6 * u, art.y + 6 * u, 18 * u, 18 * u}, 0.85f * al);
                    const bool hasLrc = m.songHasLrc && static_cast<size_t>(i) < m.songHasLrc->size() && (*m.songHasLrc)[static_cast<size_t>(i)];
                    const float chipW = 46 * u;
                    const float tx = art.r() + 11 * u;
                    txt(tx, row.cy() + 4.5f * u, fit(title, 12.5f * u, row.r() - chipW - 16 * u - tx), 12.5f * u, cur ? 0xFFFFFFFFu : 0xFFE6E6E6u, al);
                    if (hasLrc) {
                        const Rect chip{row.r() - chipW - 8 * u, row.cy() - 9 * u, chipW, 18 * u};
                        pill(chip, withA(accent, 70), al);
                        ctxt(chip.cx(), chip.cy() + 3.2f * u, "LRC", 8.6f * u, kText, al);
                    } else {
                        rtxt(row.r() - 10 * u, row.cy() + 3.4f * u, "no lyrics", 8.8f * u, kDim, al);
                    }
                }
                if (maxScroll > 0.f) {
                    const float th = std::max(24 * u, L.list.h * L.list.h / (L.list.h + maxScroll));
                    const float ty = L.list.y + (L.list.h - th) * (sc / maxScroll);
                    box({L.list.r() - 4 * u, ty, 3 * u, th}, 0x55FFFFFFu, 2 * u, al);
                }
            }
        } else { // Settings
            auto label = [&](const Rect& row, const char* name, const char* hint) {
                box(row, 0x14FFFFFFu, 12 * u, al);
                txt(row.x + 14 * u, row.cy() + (hint ? 0.f : 4.5f * u), name, 12.5f * u, kText, al);
                if (hint) txt(row.x + 14 * u, row.cy() + 13 * u, hint, 8.8f * u, kDim, al);
            };
            auto stepper = [&](const Rect& row, const std::string& value) {
                const Rect mn = minusRect(row, u), pl = plusRect(row, u);
                pill(mn, 0x2CFFFFFFu, al);
                pill(pl, 0x2CFFFFFFu, al);
                ctxt(mn.cx(), mn.cy() + 5.5f * u, "-", 16 * u, kText, al);
                ctxt(pl.cx(), pl.cy() + 5.5f * u, "+", 16 * u, kText, al);
                ctxt((mn.r() + pl.x) / 2.f, row.cy() + 4.2f * u, value, 11.5f * u, kText, al);
            };
            auto toggle = [&](const Rect& row, bool on) {
                const Rect p = pillRect(row, u);
                pill(p, on ? accent : 0x40FFFFFFu, al);
                disc(on ? p.r() - 13 * u : p.x + 13 * u, p.cy(), 9.5f * u, 0xFFFFFFFFu, al);
            };
            {
                const Rect r = settingRow(L, 0);
                label(r, "Display mode", nullptr);
                for (int i = 0; i < 3; ++i) {
                    const Rect s = segRect(r, i, u);
                    const bool on = m.layoutMode == kModes[i];
                    pill(s, on ? 0xFFFFFFFFu : 0x26FFFFFFu, al);
                    ctxt(s.cx(), s.cy() + 3.8f * u, kModeNames[i], 10.5f * u, on ? 0xFF101216u : kSub, al);
                }
            }
            { const Rect r = settingRow(L, 1); label(r, "Lyric card", "Glass card on the screen"); toggle(r, m.cardShow); }
            { const Rect r = settingRow(L, 2); label(r, "3D text size", "Height of the letters in blocks"); stepper(r, num(m.r3Size, 2)); }
            { const Rect r = settingRow(L, 3); label(r, "3D spread", "How wide the words are scattered"); stepper(r, num(m.r3Scatter, 0) + " deg"); }
            { const Rect r = settingRow(L, 4); label(r, "3D height", "Move the words up or down"); stepper(r, num(m.r3Height, 2)); }
            { const Rect r = settingRow(L, 5); label(r, "3D outline", "Dark edge around the letters"); toggle(r, m.r3Outline); }
            {
                const Rect r = settingRow(L, 6);
                txt(r.x + 14 * u, r.cy() + 4.f * u, fit("Real 3D: " + m.statusText, 10.2f * u, r.w - 28 * u), 10.2f * u, kSub, al);
            }
            {
                const Rect r = settingRow(L, 7);
                const Rect b{r.r() - 12 * u - 130 * u, r.y + 3 * u, 130 * u, r.h - 6 * u};
                pill(b, 0xFFFFFFFFu, al);
                ctxt(b.cx(), b.cy() + 3.9f * u, "All settings", 10.8f * u, 0xFF101216u, al);
            }
        }
    }

    mm::submitDrawCommands(moduleId, out);
    mDrewSomething = true;
}

} // namespace lyrics
