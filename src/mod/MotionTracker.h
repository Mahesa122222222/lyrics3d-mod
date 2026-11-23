#pragma once
// Estimates how far the game camera has turned by watching how the picture on screen moves.
// Pure CPU code: it gets a small grayscale copy of every frame (e.g. 200x90) and compares it
// with a "key frame". Because the comparison is always against the same key frame, a static
// camera stays perfectly still (no drift), and slow turns are measured exactly.
//
// Only two vertical strips on either side of the crosshair are compared. The HUD (joystick, buttons,
// hotbar, held item, crosshair) lives outside of them, so it cannot pull the result towards "no motion".
// Every frame is contrast-stretched first, so dark (night) scenes work too.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lyrics {

class MotionTracker {
public:
    void reset() {
        mHaveKey = false;
        mKeyYaw = mYaw;
        mKeyPitch = mPitch;
    }

    // Camera estimate in radians: yaw + = turned right, pitch + = looked up.
    float yaw() const { return mYaw; }
    float pitch() const { return mPitch; }
    bool lastValid() const { return mLastValid; }
    float lastShiftX() const { return mSx; }
    float lastShiftY() const { return mSy; }
    float lastTexture() const { return mTex; }

    // gray: w*h bytes, row 0 = top. focalPx = focal length of the game camera, in pixels of this image.
    // Returns true if a reliable measurement was made for this frame.
    bool update(const uint8_t* gray, int w, int h, float focalPx) {
        mLastValid = false;
        if (w < 64 || h < 32 || focalPx < 1.0f) return false;
        if (!normalize(gray, w, h, mCur)) { // flat picture (black screen, pure sky): nothing to track
            mHaveKey = false;
            return false;
        }
        if (!mHaveKey || w != mW || h != mH) {
            rebase(w, h);
            return false;
        }
        if (!mKeyTextured) {
            rebase(w, h);
            return false;
        }

        // --- coarse search on half-resolution copies ---
        half(mCur, w, h, mCur2);
        const int w2 = w / 2;
        Rects r2 = rects(w2, h / 2), r1 = rects(w, h);
        const int rx = 5, ry = 3;
        float best = 1e30f, sum = 0.0f;
        int bsx = 0, bsy = 0, cnt = 0;
        for (int sy = -ry; sy <= ry; ++sy)
            for (int sx = -rx; sx <= rx; ++sx) {
                const float s = sad(mCur2.data(), mKey2.data(), w2, r2, sx, sy);
                sum += s;
                ++cnt;
                if (s < best) best = s, bsx = sx, bsy = sy;
            }
        const float mean = sum / static_cast<float>(cnt);
        if (!(best < 0.85f * mean) || best > 60.0f) { // no clear match: scene changed or too flat
            rebase(w, h);
            return false;
        }

        // --- refine on full resolution around the coarse result ---
        float v[3][3];
        int fx = bsx * 2, fy = bsy * 2;
        float fbest = 1e30f;
        int ox = 0, oy = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const float s = sad(mCur.data(), mKey.data(), w, r1, fx + dx, fy + dy);
                v[dy + 1][dx + 1] = s;
                if (s < fbest) fbest = s, ox = dx, oy = dy;
            }
        fx += ox;
        fy += oy;
        // Parabolic sub-pixel refinement (only when the minimum is not on the edge of the 3x3 block).
        float subx = 0.0f, suby = 0.0f;
        if (ox == 0) subx = parab(v[oy + 1][0], v[oy + 1][1], v[oy + 1][2]);
        if (oy == 0) suby = parab(v[0][ox + 1], v[1][ox + 1], v[2][ox + 1]);
        mSx = static_cast<float>(fx) + subx;
        mSy = static_cast<float>(fy) + suby;
        mLastValid = true;

        // Content moved right/down by (sx, sy) => camera turned left / looked up.
        mYaw = mKeyYaw - std::atan(mSx / focalPx);
        mPitch = mKeyPitch + std::atan(mSy / focalPx);

        if (std::fabs(mSx) > 5.0f || std::fabs(mSy) > 3.5f) rebase(w, h); // keep the match range centred
        return true;
    }

private:
    struct Rects {
        int x0[2], x1[2], y0, y1;
    };
    static Rects rects(int w, int h) {
        Rects r;
        r.x0[0] = static_cast<int>(w * 0.30f);
        r.x1[0] = static_cast<int>(w * 0.47f);
        r.x0[1] = static_cast<int>(w * 0.53f);
        r.x1[1] = static_cast<int>(w * 0.70f);
        r.y0 = static_cast<int>(h * 0.20f);
        r.y1 = static_cast<int>(h * 0.78f);
        return r;
    }

    static float parab(float a, float b, float c) {
        const float d = a - 2.0f * b + c;
        if (d <= 1e-6f) return 0.0f;
        return std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / d));
    }

    static void half(const std::vector<uint8_t>& src, int w, int h, std::vector<uint8_t>& out) {
        const int w2 = w / 2, h2 = h / 2;
        out.resize(static_cast<size_t>(w2) * h2);
        for (int y = 0; y < h2; ++y)
            for (int x = 0; x < w2; ++x) {
                const int i = (2 * y) * w + 2 * x;
                out[static_cast<size_t>(y) * w2 + x] =
                    static_cast<uint8_t>((src[i] + src[i + 1] + src[i + w] + src[i + w + 1] + 2) >> 2);
            }
    }

    // Mean absolute difference between cur(x, y) and key(x - sx, y - sy) over the two strips.
    static float sad(const uint8_t* cur, const uint8_t* key, int stride, const Rects& r, int sx, int sy) {
        long total = 0;
        long count = 0;
        for (int k = 0; k < 2; ++k) {
            for (int y = r.y0; y < r.y1; ++y) {
                const uint8_t* c = cur + static_cast<size_t>(y) * stride;
                const uint8_t* kk = key + static_cast<size_t>(y - sy) * stride - sx;
                for (int x = r.x0[k]; x < r.x1[k]; ++x) total += std::abs(static_cast<int>(c[x]) - static_cast<int>(kk[x]));
            }
            count += static_cast<long>(r.x1[k] - r.x0[k]) * (r.y1 - r.y0);
        }
        return static_cast<float>(total) / static_cast<float>(count);
    }

    // Stretch the contrast of the strips (dark scenes!). Returns false if there is nothing to see.
    bool normalize(const uint8_t* src, int w, int h, std::vector<uint8_t>& out) {
        const Rects r = rects(w, h);
        long sum = 0, cnt = 0;
        for (int k = 0; k < 2; ++k)
            for (int y = r.y0; y < r.y1; ++y)
                for (int x = r.x0[k]; x < r.x1[k]; ++x) sum += src[static_cast<size_t>(y) * w + x], ++cnt;
        const float mean = static_cast<float>(sum) / static_cast<float>(cnt);
        float mad = 0.0f;
        for (int k = 0; k < 2; ++k)
            for (int y = r.y0; y < r.y1; ++y)
                for (int x = r.x0[k]; x < r.x1[k]; ++x) mad += std::fabs(static_cast<float>(src[static_cast<size_t>(y) * w + x]) - mean);
        mad /= static_cast<float>(cnt);
        mTex = mad;
        if (mad < 0.35f) return false;
        const float gain = std::max(1.0f, std::min(30.0f, 14.0f / mad));
        out.resize(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < out.size(); ++i) {
            const float v = 128.0f + (static_cast<float>(src[i]) - mean) * gain;
            out[i] = static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
        }
        return true;
    }

    void rebase(int w, int h) {
        mW = w;
        mH = h;
        mKey = mCur;
        half(mKey, w, h, mKey2);
        mKeyYaw = mYaw;
        mKeyPitch = mPitch;
        mHaveKey = true;
        // Is there enough fine detail to match against?
        const Rects r = rects(w, h);
        long g = 0, cnt = 0;
        for (int k = 0; k < 2; ++k)
            for (int y = r.y0; y < r.y1; ++y)
                for (int x = r.x0[k]; x < r.x1[k] - 1; ++x) {
                    const int i = y * w + x;
                    g += std::abs(static_cast<int>(mKey[i + 1]) - static_cast<int>(mKey[i])) +
                         std::abs(static_cast<int>(mKey[i + w]) - static_cast<int>(mKey[i]));
                    ++cnt;
                }
        mKeyTextured = static_cast<float>(g) / static_cast<float>(cnt) > 1.2f;
    }

    std::vector<uint8_t> mKey, mKey2, mCur, mCur2;
    int mW = 0, mH = 0;
    bool mHaveKey = false, mKeyTextured = false, mLastValid = false;
    float mYaw = 0, mPitch = 0, mKeyYaw = 0, mKeyPitch = 0, mSx = 0, mSy = 0, mTex = 0;
};

} // namespace lyrics
