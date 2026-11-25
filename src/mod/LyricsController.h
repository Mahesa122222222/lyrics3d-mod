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
    std::filesystem::path lrc; // empty if there is none
    std::string title;
};

// Manages the playlist, loads .lrc files, and prepares the ViewState for the renderer.
// All methods are called from a single thread (the render thread), except the constructor.
class LyricsController {
public:
    LyricsController(const std::filesystem::path& dataDir, const LyricsConfig& cfg);

    void applyConfig(const LyricsConfig& cfg);
    void tick(double timeSec);
    ViewState view() const;

    // Commands from the control file / menu.
    void play();
    void pause();
    void toggle();
    void next();
    void prev();
    void stop();
    void rescan();
    void seekMs(int64_t ms);                                         // jump inside the current song (audio position)
    int64_t audioPositionMs() const { return mAudio.positionMs(); }  // without the lyrics offset
    int64_t audioDurationMs() const { return mAudio.durationMs(); }
    bool hasAudioTrack() const { return mAudio.hasTrack(); }

    // Play a specific song (index into the name-sorted song list, not the shuffled order).
    void playIndex(int trackIndex);

    // For the menu: song list + current state.
    struct TrackSummary {
        std::string title;
        bool hasLrc = false;
    };
    std::vector<TrackSummary> trackSummaries() const;
    int currentIndex() const { return (mPos >= 0 && mPos < static_cast<int>(mOrder.size())) ? mOrder[mPos] : -1; }
    bool isPlaying() const { return mAudio.isPlaying(); }
    uint32_t libraryVersion() const { return mLibVersion; } // incremented every time the song list is rescanned

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
    std::vector<int> mOrder; // play order (indices into mTracks)
    int mPos = -1;           // position in mOrder
    uint32_t mLibVersion = 0;

    LrcData mLrc;
    bool mHasLrc = false;
    int64_t mTrackId = 0;
    bool mStarted = false;
    bool mUserStopped = false;

    // Position smoothing: MediaPlayer sometimes reports a position slightly smaller than
    // our estimate; without this the active line flips back and forth at lyric changes.
    mutable int64_t mSmoothPos = 0;
    mutable int64_t mSmoothTrack = -1;

    uint32_t mSeenFinished = 0, mSeenErrors = 0;
    int mConsecutiveFailures = 0;
    double mNextPollSec = 0.0;
    std::mt19937 mRng{std::random_device{}()};
};

} // namespace lyrics
