#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace lyrics {

// Plays audio files through android.media.MediaPlayer (JNI).
// All JNI calls happen on a single worker thread owned by this class, so the render thread
// is never blocked by prepare()/seek, and MediaPlayer does not need a Looper.
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Asynchronous commands (safe to call from any thread).
    void open(const std::string& path, bool autoplay);
    void pause();
    void resume();
    void stop();
    void setVolume(float volume01);
    void seek(int64_t ms);

    // State (read from the render thread).
    bool isPlaying() const { return mPlaying.load(); }
    bool hasTrack() const { return mHasTrack.load(); }
    int64_t durationMs() const { return mDurationMs.load(); }
    // Position interpolated between polls (smooth for animation).
    int64_t positionMs() const;
    // Incremented whenever a song finishes on its own / fails to open.
    uint32_t finishedCount() const { return mFinished.load(); }
    uint32_t errorCount() const { return mErrors.load(); }

private:
    enum class CmdType { Open, Pause, Resume, Stop, Volume, Seek, Quit };
    struct Cmd {
        CmdType type;
        std::string path;
        bool autoplay = true;
        float value = 1.0f;
    };

    void push(Cmd c);
    void threadMain();

    std::thread mThread;
    std::mutex mMutex;
    std::condition_variable mCv;
    std::deque<Cmd> mQueue;

    std::atomic<bool> mPlaying{false};
    std::atomic<bool> mHasTrack{false};
    std::atomic<int64_t> mDurationMs{0};
    std::atomic<int64_t> mPosMs{0};
    std::atomic<int64_t> mPosStampUs{0};
    std::atomic<uint32_t> mFinished{0};
    std::atomic<uint32_t> mErrors{0};
};

} // namespace lyrics
