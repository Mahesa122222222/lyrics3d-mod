#pragma once
#include <GLES3/gl3.h>
#include <jni.h>

#include <cstdint>
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

    void render(JNIEnv* env, const LyricsConfig& cfg, const ViewState& view, int screenW, int screenH,
                double timeSec);

private:
    struct Color {
        float r, g, b;
    };

    bool buildProgram();
    void releaseTextures();
    const TextTexture* texFor(JNIEnv* env, const LyricsConfig& cfg, const LrcData& lrc, int idx,
                              int& budget);
    void drawQuad(const Mat4& mvp, const TextTexture& t, Color sung, Color unsung, float progress,
                  float alpha, bool solid);

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
