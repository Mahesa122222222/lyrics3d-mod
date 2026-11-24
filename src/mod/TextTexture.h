#pragma once
#include <GLES3/gl3.h>
#include <jni.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lyrics {

struct TextTexture {
    GLuint id = 0;
    int width = 0;   // pixels, including padding for the glow
    int height = 0;
    float u0 = 0.0f; // U range containing the text (without padding), for progress highlighting
    float u1 = 1.0f;
};

struct TextStyle {
    float fontPx = 48.0f;
    int maxWidthPx = 2048;       // text wider than this is shrunk automatically
    float glowRadiusPx = 6.0f;   // 0 = no glow
    bool bold = true;
    std::string family = "sans-serif";
};

// Draws text into an Android Bitmap (Canvas/Paint via JNI) and uploads it as a GL texture.
// This supports all scripts (CJK, Arabic, emoji) without shipping our own font files.
// MUST be called from the render thread with an active GL context.
class TextFactory {
public:
    ~TextFactory();

    bool init(JNIEnv* env);
    bool ready() const { return mReady; }
    // Delete global refs (without touching GL). Call before the JVM shuts down / on disable.
    void shutdown(JNIEnv* env);

    // Text is rendered WHITE with alpha; coloring is done in the shader.
    bool make(JNIEnv* env, const std::string& text, const TextStyle& style, TextTexture& out);

    // Same drawing, but returns the alpha channel on the CPU (no GL). baseline = pixel row of the text baseline.
    bool makeAlpha(JNIEnv* env, const std::string& text, const TextStyle& style, std::vector<uint8_t>& alpha, int& w,
                   int& h, int& baseline);

private:
    bool mReady = false;

    jclass cPaint = nullptr, cCanvas = nullptr, cBitmap = nullptr, cConfig = nullptr;
    jclass cTypeface = nullptr, cGlUtils = nullptr, cMetrics = nullptr;
    jmethodID mPaintCtor = nullptr, mSetTextSize = nullptr, mSetColor = nullptr, mSetTypeface = nullptr;
    jmethodID mMeasureText = nullptr, mGetFontMetrics = nullptr, mSetShadow = nullptr;
    jmethodID mCanvasCtor = nullptr, mDrawText = nullptr;
    jmethodID mCreateBitmap = nullptr, mRecycle = nullptr;
    jmethodID mTypefaceCreate = nullptr, mTexImage2D = nullptr, mGetPixels = nullptr;
    jfieldID fAscent = nullptr, fDescent = nullptr;
    jobject oArgb8888 = nullptr; // Bitmap.Config.ARGB_8888 (global ref)
};

} // namespace lyrics
