#pragma once
#include <GLES3/gl3.h>
#include <jni.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "LrcParser.h"
#include "Config.h"
#include "Mat4.h"
#include "MotionTracker.h"
#include "TextTexture.h"

namespace lyrics {

struct ViewState {
    const LrcData* lrc = nullptr; // null = nothing is shown
    int64_t positionMs = 0;       // already includes the configured offset
    int64_t trackId = 0;          // changes with every song change (drops the texture cache)
    bool playing = false;
};

// Draws pseudo-3D lyrics as an overlay on top of the game frame.
// Called from the eglSwapBuffers hook, on the render thread with an active GL context.
class LyricsRenderer {
public:
    ~LyricsRenderer();

    bool init(JNIEnv* env);
    // GL context changed/lost: drop handles without calling GL (no longer valid).
    void invalidateGl();
    void shutdown(JNIEnv* env);

    // camYawRad/camPitchRad: estimated game camera view direction (from finger swipes). Used by world mode.
    void render(JNIEnv* env, const LyricsConfig& cfg, const ViewState& view, int screenW, int screenH,
                double timeSec, float camYawRad = 0.0f, float camPitchRad = 0.0f);

private:
    struct Color {
        float r, g, b;
    };

    // --- world mode ---
    struct Chunk {
        std::string text;
        float f0 = 0.0f, f1 = 1.0f; // portion of the line time (0..1)
    };
    struct Anchor {
        float yaw = 0, pitch = 0, tilt = 0, spawn = 0;
        float hw = 0, hh = 0;  // half size as seen from the camera (radians)
        int64_t expire = 0;    // song position (ms) after which the word is gone
    };
    const std::vector<Chunk>& chunksFor(const LrcData& lrc, int idx, bool caps);
    const TextTexture* chunkTex(JNIEnv* env, const LyricsConfig& cfg, int key, const std::string& text, int& budget);
    void renderWorld(JNIEnv* env, const LyricsConfig& cfg, const ViewState& view, int screenW, int screenH,
                     double timeSec, float camYaw, float camPitch, int fontPx);
    std::unordered_map<int, std::vector<Chunk>> mChunks; // line index -> word chunks
    std::unordered_map<int, TextTexture> mCTex;          // line index * 64 + chunk -> texture
    std::unordered_map<int, Anchor> mAnch;               // fixed position in the world

    // --- screen-motion camera tracking (small copy of every frame, read back asynchronously) ---
    bool ensureGrab(int W, int H);
    void releaseGrab();
    void grabFrame(int W, int H, double timeSec, float gameFovRad);
    MotionTracker mTracker;
    GLuint mGFbo[3] = {0, 0, 0}, mGTex[3] = {0, 0, 0}, mPbo[2] = {0, 0};
    int mGW[3] = {0, 0, 0}, mGH[3] = {0, 0, 0};
    int mGScrW = 0, mGScrH = 0, mPboN = 0, mGBad = 0;
    bool mGFailed = false, mGPending = false;
    double mGLastT = -1.0, mGLogT = 0.0;
    long mGFrames = 0;
    std::vector<uint8_t> mGray;

    bool buildProgram();
    void releaseTextures();
    const TextTexture* texFor(JNIEnv* env, const LyricsConfig& cfg, const LrcData& lrc, int idx,
                              int& budget);
    void drawQuad(const Mat4& mvp, const TextTexture& t, Color sung, Color unsung, float progress,
                  float alpha, bool solid);
    const TextTexture* noteTex(JNIEnv* env, const LyricsConfig& cfg, int k, int& budget);
    void drawNotes(JNIEnv* env, const LyricsConfig& cfg, const Mat4& proj, float t, float halfH, float aspect,
                   float depth, float worldPerPx, Color base, int& budget);
    void drawSparkles(const Mat4& proj, const Mat4& group, const LyricsConfig& cfg, float t, float ly, float hx,
                      float hy, float headX, float sizeWorld, Color base);

    // Color settings used by drawQuad for the main text.
    struct Fx {
        float mode = 0.0f; // 0 solid, 1 gradient, 2 rainbow
        Color grad2{1, 0.3f, 0.85f};
        float time = 0.0f;
        float speed = 0.3f;
    } mFx;

    GLuint mSparkProg = 0;
    GLint uSMvp = -1, uSColor = -1, uSAlpha = -1;
    GLint uMode = -1, uGrad2 = -1, uTime = -1, uRainSpeed = -1;
    TextTexture mNoteTex[3];
    bool mNoteFailed[3] = {false, false, false};

    TextFactory mFactory;
    bool mInitDone = false;

    GLuint mProgram = 0, mVao = 0, mVbo = 0;
    GLint uMvp = -1, uTex = -1, uSung = -1, uUnsung = -1, uProgress = -1, uSpan = -1, uAlpha = -1,
          uSolid = -1;

    std::unordered_map<int, TextTexture> mTex; // line index -> texture
    int64_t mTrackId = -1;
    int mFontPxBuilt = 0;
    std::string mStyleKey;

    int mLastIdx = -2;
    int mShiftDir = 1;
    double mIdxChangeTime = 0.0;
};

} // namespace lyrics
