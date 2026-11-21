#pragma once
#include <GLES3/gl3.h>
#include <jni.h>

#include <string>

namespace lyrics {

struct TextTexture {
    GLuint id = 0;
    int width = 0;   // piksel, termasuk padding untuk glow
    int height = 0;
    float u0 = 0.0f; // rentang U yang berisi teks (tanpa padding), untuk highlight progres
    float u1 = 1.0f;
};

struct TextStyle {
    float fontPx = 48.0f;
    int maxWidthPx = 2048;       // teks lebih lebar dari ini diperkecil otomatis
    float glowRadiusPx = 6.0f;   // 0 = tanpa glow
    bool bold = true;
    std::string family = "sans-serif";
};

// Menggambar teks ke Bitmap Android (Canvas/Paint lewat JNI) lalu mengunggahnya jadi tekstur GL.
// Cara ini memberi dukungan semua aksara (CJK, Arab, emoji) tanpa membawa file font sendiri.
// HARUS dipanggil dari thread render dengan konteks GL aktif.
class TextFactory {
public:
    ~TextFactory();

    bool init(JNIEnv* env);
    bool ready() const { return mReady; }
    // Hapus global ref (tanpa menyentuh GL). Panggil sebelum JVM berhenti / saat disable.
    void shutdown(JNIEnv* env);

    // Teks dirender PUTIH dengan alpha; pewarnaan dilakukan di shader.
    bool make(JNIEnv* env, const std::string& text, const TextStyle& style, TextTexture& out);

private:
    bool mReady = false;

    jclass cPaint = nullptr, cCanvas = nullptr, cBitmap = nullptr, cConfig = nullptr;
    jclass cTypeface = nullptr, cGlUtils = nullptr, cMetrics = nullptr;
    jmethodID mPaintCtor = nullptr, mSetTextSize = nullptr, mSetColor = nullptr, mSetTypeface = nullptr;
    jmethodID mMeasureText = nullptr, mGetFontMetrics = nullptr, mSetShadow = nullptr;
    jmethodID mCanvasCtor = nullptr, mDrawText = nullptr;
    jmethodID mCreateBitmap = nullptr, mRecycle = nullptr;
    jmethodID mTypefaceCreate = nullptr, mTexImage2D = nullptr;
    jfieldID fAscent = nullptr, fDescent = nullptr;
    jobject oArgb8888 = nullptr; // Bitmap.Config.ARGB_8888 (global ref)
};

} // namespace lyrics
