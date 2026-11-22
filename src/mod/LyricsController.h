#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "AudioPlayer.h"
#include "LrcParser.h"
#include "LyricsRenderer.h"
#include "Config.h"

namespace lyrics {

struct Track {
    std::filesystem::path audio;
    std::filesystem::path lrc; // kosong jika tidak ada
    std::string title;
};

// Mengatur playlist, memuat .lrc, dan menyiapkan ViewState untuk renderer.
// Semua method dipanggil dari satu thread (thread render), kecuali constructor.
class LyricsController {
public:
    LyricsController(const std::filesystem::path& dataDir, const LyricsConfig& cfg);

    void applyConfig(const LyricsConfig& cfg);
    void tick(double timeSec);
    ViewState view() const;

    // Perintah dari file kontrol / menu.
    void play();
    void pause();
    void toggle();
    void next();
    void prev();
    void stop();
    void rescan();

    // Putar lagu tertentu (indeks di daftar lagu yang diurutkan menurut nama, bukan urutan acak).
    void playIndex(int trackIndex);

    // Untuk menu: daftar lagu + keadaan sekarang.
    struct TrackSummary {
        std::string title;
        bool hasLrc = false;
    };
    std::vector<TrackSummary> trackSummaries() const;
    int currentIndex() const { return (mPos >= 0 && mPos < static_cast<int>(mOrder.size())) ? mOrder[mPos] : -1; }
    bool isPlaying() const { return mAudio.isPlaying(); }
    uint32_t libraryVersion() const { return mLibVersion; } // naik tiap daftar lagu dipindai ulang

    size_t trackCount() const { return mTracks.size(); }
    const std::filesystem::path& songsDir() const { return mSongsDir; }
    const std::filesystem::path& controlFile() const { return mControlFile; }

private:
    void scan();
    void startTrack(int index, bool autoplay);
    void advance(int dir);
    void pollControlFile();

    LyricsConfig mCfg;
    AudioPlayer mAudio;
    std::filesystem::path mDataDir, mSongsDir, mControlFile;

    std::vector<Track> mTracks;
    std::vector<int> mOrder; // urutan putar (indeks ke mTracks)
    int mPos = -1;           // posisi di mOrder
    uint32_t mLibVersion = 0;

    LrcData mLrc;
    bool mHasLrc = false;
    int64_t mTrackId = 0;
    bool mStarted = false;
    bool mUserStopped = false;

    // Penghalus posisi: MediaPlayer kadang melaporkan posisi sedikit lebih kecil dari
    // perkiraan kita; tanpa ini baris aktif bolak-balik di pergantian lirik.
    mutable int64_t mSmoothPos = 0;
    mutable int64_t mSmoothTrack = -1;

    uint32_t mSeenFinished = 0, mSeenErrors = 0;
    int mConsecutiveFailures = 0;
    double mNextPollSec = 0.0;
    std::mt19937 mRng{std::random_device{}()};
};

} // namespace lyrics
