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
#include "TextTexture.h"

namespace lyrics {

struct ViewState {
    const LrcData* lrc = nullptr; // null = tidak ada yang ditampilkan
    int64_t positionMs = 0;       // sudah termasuk offset konfigurasi
    int64_t trackId = 0;          // berubah tiap lagu ganti (membuang cache tekstur)
    bool playing = false;
};

// Menggambar lirik pseudo-3D sebagai overlay di atas frame game.
// Dipanggil dari hook eglSwapBuffers, di thread render dengan konteks GL aktif.
class LyricsRenderer {
public:
    ~LyricsRenderer();

    bool init(JNIEnv* env);
    // Konteks GL berganti/hilang: buang handle tanpa memanggil GL (sudah tidak valid).
    void invalidateGl();
    void shutdown(JNIEnv* env);

    // camYawRad/camPitchRad: perkiraan arah pandang kamera game (dari geseran jari). Dipakai mode dunia.
    void render(JNIEnv* env, const LyricsConfig& cfg, const ViewState& view, int screenW, int screenH,
                double timeSec, float camYawRad = 0.0f, float camPitchRad = 0.0f);

private:
    struct Color {
        float r, g, b;
    };

    // --- mode dunia ---
    struct Chunk {
        std::string text;
        float f0 = 0.0f, f1 = 1.0f; // bagian waktu baris (0..1)
    };
    struct Anchor {
        float yaw = 0, pitch = 0, tilt = 0, spawn = 0;
    };
    const std::vector<Chunk>& chunksFor(const LrcData& lrc, int idx, bool caps);
    const TextTexture* chunkTex(JNIEnv* env, const LyricsConfig& cfg, int key, const std::string& text, int& budget);
    void renderWorld(JNIEnv* env, const LyricsConfig& cfg, const ViewState& view, int screenW, int screenH,
                     double timeSec, float camYaw, float camPitch, int fontPx);
    std::unordered_map<int, std::vector<Chunk>> mChunks; // indeks baris -> potongan kata
    std::unordered_map<int, TextTexture> mCTex;          // indeks baris * 64 + potongan -> tekstur
    std::unordered_map<int, Anchor> mAnch;               // posisi tetap di dunia

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

    // Pengaturan warna yang dipakai drawQuad untuk teks utama.
    struct Fx {
        float mode = 0.0f; // 0 solid, 1 gradien, 2 pelangi
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

    std::unordered_map<int, TextTexture> mTex; // indeks baris -> tekstur
    int64_t mTrackId = -1;
    int mFontPxBuilt = 0;
    std::string mStyleKey;

    int mLastIdx = -2;
    int mShiftDir = 1;
    double mIdxChangeTime = 0.0;
};

} // namespace lyrics
