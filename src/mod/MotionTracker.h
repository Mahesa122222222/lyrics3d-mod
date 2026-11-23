#pragma once
// Estimates how far the game camera has turned by watching how the picture on screen moves.
// Pure CPU code: it gets a small grayscale copy of every frame (e.g. 200x90) and compares it
// with a "key frame". Because the comparison is always against the same key frame, a static
// camera stays perfectly still (no drift), and slow turns are measured exactly.
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

    // gray: w*h bytes, row 0 = top. focalPx = focal length of the game camera, in pixels of this image.
    // Returns true if a reliable measurement was made for this frame.
    bool update(const uint8_t* gray, int w, int h, float focalPx) {
        if (w < 64 || h < 32 || focalPx < 1.0f) return false;
        if (!mHaveKey || w != mW || h != mH) {
            rebase(gray, w, h);
            mLastValid = false;
            return false;
        }
        // Region used for matching: central part (the HUD sits at the edges and never moves).
        const int x0 = w / 8, x1 = w - w / 8, y0 = h / 8, y1 = h - h / 4;

        if (!mKeyTextured) { // flat key (sky, black screen): keep replacing it
            rebase(gray, w, h);
            mLastValid = false;
            return false;
        }

        // --- coarse search on half-resolution copies ---
        std::vector<uint8_t> cur2;
        half(gray, w, h, cur2);
        const int w2 = w / 2;
        const int cx0 = x0 / 2, cx1 = x1 / 2, cy0 = y0 / 2, cy1 = y1 / 2;
        const int rx = 5, ry = 3;
        float best = 1e30f, sum = 0.0f;
        int bsx = 0, bsy = 0, cnt = 0;
        for (int sy = -ry; sy <= ry; ++sy)
            for (int sx = -rx; sx <= rx; ++sx) {
                const float s = sad(cur2.data(), mKey2.data(), w2, cx0, cx1, cy0, cy1, sx, sy);
                sum += s;
                ++cnt;
                if (s < best) best = s, bsx = sx, bsy = sy;
            }
        const float mean = sum / static_cast<float>(cnt);
        if (!(best < 0.85f * mean) || best > 45.0f) { // no clear match: scene changed or too flat
            rebase(gray, w, h);
            mLastValid = false;
            return false;
        }

        // --- refine on full resolution around the coarse result ---
        float v[3][3];
        int fx = bsx * 2, fy = bsy * 2;
        float fbest = 1e30f;
        int ox = 0, oy = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const float s = sad(gray, mKey.data(), w, x0, x1, y0, y1, fx + dx, fy + dy);
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

        if (std::fabs(mSx) > 5.0f || std::fabs(mSy) > 3.5f) rebase(gray, w, h); // keep the match range centred
        return true;
    }

private:
    static float parab(float a, float b, float c) {
        const float d = a - 2.0f * b + c;
        if (d <= 1e-6f) return 0.0f;
        return std::max(-0.5f, std::min(0.5f, 0.5f * (a - c) / d));
    }

    static void half(const uint8_t* src, int w, int h, std::vector<uint8_t>& out) {
        const int w2 = w / 2, h2 = h / 2;
        out.resize(static_cast<size_t>(w2) * h2);
        for (int y = 0; y < h2; ++y)
            for (int x = 0; x < w2; ++x) {
                const int i = (2 * y) * w + 2 * x;
                out[static_cast<size_t>(y) * w2 + x] =
                    static_cast<uint8_t>((src[i] + src[i + 1] + src[i + w] + src[i + w + 1] + 2) >> 2);
            }
    }

    // Mean absolute difference between cur(x, y) and key(x - sx, y - sy) over the region.
    static float sad(const uint8_t* cur, const uint8_t* key, int stride, int x0, int x1, int y0, int y1, int sx, int sy) {
        long total = 0;
        for (int y = y0; y < y1; ++y) {
            const uint8_t* c = cur + static_cast<size_t>(y) * stride;
            const uint8_t* k = key + static_cast<size_t>(y - sy) * stride - sx;
            for (int x = x0; x < x1; ++x) total += std::abs(static_cast<int>(c[x]) - static_cast<int>(k[x]));
        }
        return static_cast<float>(total) / static_cast<float>((x1 - x0) * (y1 - y0));
    }

    void rebase(const uint8_t* gray, int w, int h) {
        mW = w;
        mH = h;
        mKey.assign(gray, gray + static_cast<size_t>(w) * h);
        half(mKey.data(), w, h, mKey2);
        mKeyYaw = mYaw;
        mKeyPitch = mPitch;
        mHaveKey = true;
        // Is there enough texture to match against?
        const int x0 = w / 8, x1 = w - w / 8, y0 = h / 8, y1 = h - h / 4;
        long g = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1 - 1; ++x) {
                const int i = y * w + x;
                g += std::abs(static_cast<int>(mKey[i + 1]) - static_cast<int>(mKey[i])) +
                     std::abs(static_cast<int>(mKey[i + w]) - static_cast<int>(mKey[i]));
            }
        mKeyTextured = static_cast<float>(g) / static_cast<float>((x1 - x0) * (y1 - y0)) > 1.6f;
    }

    std::vector<uint8_t> mKey, mKey2;
    int mW = 0, mH = 0;
    bool mHaveKey = false, mKeyTextured = false, mLastValid = false;
    float mYaw = 0, mPitch = 0, mKeyYaw = 0, mKeyPitch = 0, mSx = 0, mSy = 0;
};

} // namespace lyrics
