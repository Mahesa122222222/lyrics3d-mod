#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace lyrics {

// Memutar file audio lewat android.media.MediaPlayer (JNI).
// Semua panggilan JNI terjadi di satu thread kerja milik kelas ini, jadi thread render
// tidak pernah terblokir oleh prepare()/seek, dan MediaPlayer tidak butuh Looper.
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Perintah asinkron (aman dipanggil dari thread mana pun).
    void open(const std::string& path, bool autoplay);
    void pause();
    void resume();
    void stop();
    void setVolume(float volume01);

    // Status (dibaca dari thread render).
    bool isPlaying() const { return mPlaying.load(); }
    bool hasTrack() const { return mHasTrack.load(); }
    int64_t durationMs() const { return mDurationMs.load(); }
    // Posisi dengan interpolasi antar polling (halus untuk animasi).
    int64_t positionMs() const;
    // Bertambah setiap lagu selesai sendiri / gagal dibuka.
    uint32_t finishedCount() const { return mFinished.load(); }
    uint32_t errorCount() const { return mErrors.load(); }

private:
    enum class CmdType { Open, Pause, Resume, Stop, Volume, Quit };
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
