#pragma once
#include <cmath>

namespace lyrics {

// Matriks 4x4 column-major (urutan OpenGL): elemen(baris r, kolom c) = m[c * 4 + r].
struct Mat4 {
    float m[16];
};

inline Mat4 identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

inline Mat4 mul(const Mat4& a, const Mat4& b) { // a * b
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

inline Mat4 translate(float x, float y, float z) {
    Mat4 r = identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

inline Mat4 scale(float x, float y, float z) {
    Mat4 r = identity();
    r.m[0] = x;
    r.m[5] = y;
    r.m[10] = z;
    return r;
}

inline Mat4 rotateX(float rad) {
    Mat4 r = identity();
    float c = std::cos(rad), s = std::sin(rad);
    r.m[5] = c;
    r.m[9] = -s;
    r.m[6] = s;
    r.m[10] = c;
    return r;
}

inline Mat4 rotateY(float rad) {
    Mat4 r = identity();
    float c = std::cos(rad), s = std::sin(rad);
    r.m[0] = c;
    r.m[8] = s;
    r.m[2] = -s;
    r.m[10] = c;
    return r;
}

inline Mat4 perspective(float fovyRad, float aspect, float nearZ, float farZ) {
    Mat4 r{};
    float f = 1.0f / std::tan(fovyRad * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (farZ + nearZ) / (nearZ - farZ);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
    return r;
}

struct Vec4 {
    float x, y, z, w;
};

inline Vec4 transform(const Mat4& a, float x, float y, float z, float w = 1.0f) {
    return {a.m[0] * x + a.m[4] * y + a.m[8] * z + a.m[12] * w,
            a.m[1] * x + a.m[5] * y + a.m[9] * z + a.m[13] * w,
            a.m[2] * x + a.m[6] * y + a.m[10] * z + a.m[14] * w,
            a.m[3] * x + a.m[7] * y + a.m[11] * z + a.m[15] * w};
}

} // namespace lyrics
