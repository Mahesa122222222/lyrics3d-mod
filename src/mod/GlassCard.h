#pragma once
// Spotify-style "liquid glass" lyric card drawn on top of the game (screen space).
#include <GLES3/gl3.h>
#include <jni.h>

#include <string>
#include <unordered_map>

#include "Config.h"
#include "LyricsRenderer.h"
#include "TextTexture.h"

namespace lyrics {

class GlassCard {
public:
    // GL context lost: forget handles without calling GL.
    void invalidateGl();
    void release();
    // Must be called on the render thread with a GL context; the caller saves/restores the GL state.
    void draw(JNIEnv* env, TextFactory& factory, const LyricsConfig& cfg, const ViewState& view, int W, int H,
              double timeSec);

private:
    bool build();
    const TextTexture* text(JNIEnv* env, TextFactory& f, const std::string& s, float px, bool bold, int maxW, int& budget);
    void rect(float x, float y, float w, float h, float radius, int mode, const float a[4], const float b[4], float opacity,
              float pad, float glass);
    void tex(const TextTexture& t, float x, float y, float scale, float prog, const float c0[3], const float c1[3],
             const float g[3], float opacity, bool flat, const float flatColor[4]);

    GLuint mGlass = 0, mText = 0, mVao = 0;
    GLint uG[10] = {}, uT[10] = {};
    int mW = 1, mH = 1;
    double mTime = 0;
    std::unordered_map<std::string, TextTexture> mTex;
    int mIdx = -9;
    int64_t mTrack = -1;
    double mChangeT = -10;
    int mPrevIdx = -9;
    std::string mFamily;
};

} // namespace lyrics
