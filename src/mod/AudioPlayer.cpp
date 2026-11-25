#include "AudioPlayer.h"

#include <algorithm>
#include <chrono>

#include "Jni.h"
#include "Log.h"
#include "Utf8.h"

namespace lyrics {

namespace {

int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// All MediaPlayer method IDs that are used.
struct MpIds {
    jclass cls = nullptr; // global ref
    jmethodID ctor, setDataSource, prepare, start, pause, stop, release, seekTo;
    jmethodID getCurrentPosition, getDuration, isPlaying, setVolume;
};

bool loadIds(JNIEnv* e, MpIds& ids) {
    jclass local = e->FindClass("android/media/MediaPlayer");
    if (!local) {
        jni::clearException(e, "FindClass MediaPlayer");
        return false;
    }
    ids.cls = static_cast<jclass>(e->NewGlobalRef(local));
    e->DeleteLocalRef(local);
    ids.ctor = e->GetMethodID(ids.cls, "<init>", "()V");
    ids.setDataSource = e->GetMethodID(ids.cls, "setDataSource", "(Ljava/lang/String;)V");
    ids.prepare = e->GetMethodID(ids.cls, "prepare", "()V");
    ids.start = e->GetMethodID(ids.cls, "start", "()V");
    ids.pause = e->GetMethodID(ids.cls, "pause", "()V");
    ids.stop = e->GetMethodID(ids.cls, "stop", "()V");
    ids.release = e->GetMethodID(ids.cls, "release", "()V");
    ids.seekTo = e->GetMethodID(ids.cls, "seekTo", "(I)V");
    ids.getCurrentPosition = e->GetMethodID(ids.cls, "getCurrentPosition", "()I");
    ids.getDuration = e->GetMethodID(ids.cls, "getDuration", "()I");
    ids.isPlaying = e->GetMethodID(ids.cls, "isPlaying", "()Z");
    ids.setVolume = e->GetMethodID(ids.cls, "setVolume", "(FF)V");
    if (jni::clearException(e, "GetMethodID MediaPlayer")) return false;
    return true;
}

} // namespace

AudioPlayer::AudioPlayer() { mThread = std::thread([this] { threadMain(); }); }

AudioPlayer::~AudioPlayer() {
    push({CmdType::Quit, {}, false, 0});
    if (mThread.joinable()) mThread.join();
}

void AudioPlayer::push(Cmd c) {
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mQueue.push_back(std::move(c));
    }
    mCv.notify_one();
}

void AudioPlayer::open(const std::string& path, bool autoplay) { push({CmdType::Open, path, autoplay, 1.0f}); }
void AudioPlayer::pause() { push({CmdType::Pause, {}, false, 0}); }
void AudioPlayer::resume() { push({CmdType::Resume, {}, false, 0}); }
void AudioPlayer::stop() { push({CmdType::Stop, {}, false, 0}); }
void AudioPlayer::seek(int64_t ms) { push({CmdType::Seek, {}, false, static_cast<float>(ms < 0 ? 0 : ms)}); }
void AudioPlayer::setVolume(float v) { push({CmdType::Volume, {}, false, std::clamp(v, 0.0f, 1.0f)}); }

int64_t AudioPlayer::positionMs() const {
    int64_t pos = mPosMs.load();
    if (!mPlaying.load()) return pos;
    int64_t elapsed = (nowUs() - mPosStampUs.load()) / 1000;
    if (elapsed < 0) elapsed = 0;
    if (elapsed > 200) elapsed = 200; // polling stalled: don't drift too far
    int64_t dur = mDurationMs.load();
    int64_t p = pos + elapsed;
    return dur > 0 ? std::min(p, dur) : p;
}

void AudioPlayer::threadMain() {
    JNIEnv* e = jni::env();
    if (!e) {
        LYR_LOGE("AudioPlayer: cannot attach thread to the JVM; audio disabled");
        // Still drain the queue so the destructor does not hang.
        for (;;) {
            std::unique_lock<std::mutex> lk(mMutex);
            mCv.wait(lk, [this] { return !mQueue.empty(); });
            Cmd c = std::move(mQueue.front());
            mQueue.pop_front();
            if (c.type == CmdType::Quit) return;
            if (c.type == CmdType::Open) mErrors.fetch_add(1);
        }
    }

    MpIds ids;
    const bool idsOk = loadIds(e, ids);
    if (!idsOk) LYR_LOGE("AudioPlayer: MediaPlayer is not available");

    jobject player = nullptr; // global ref
    bool wantPlaying = false;  // the song is supposed to be playing
    float volume = 0.8f;

    auto release = [&] {
        if (player) {
            e->CallVoidMethod(player, ids.release);
            jni::clearException(e, "release");
            e->DeleteGlobalRef(player);
            player = nullptr;
        }
        wantPlaying = false;
        mPlaying = false;
        mHasTrack = false;
        mPosMs = 0;
        mDurationMs = 0;
    };

    auto applyVolume = [&] {
        if (player) {
            e->CallVoidMethod(player, ids.setVolume, volume, volume);
            jni::clearException(e, "setVolume");
        }
    };

    for (;;) {
        Cmd cmd{};
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(mMutex);
            mCv.wait_for(lk, std::chrono::milliseconds(30), [this] { return !mQueue.empty(); });
            if (!mQueue.empty()) {
                cmd = std::move(mQueue.front());
                mQueue.pop_front();
                have = true;
            }
        }

        if (have) {
            switch (cmd.type) {
            case CmdType::Quit:
                release();
                if (ids.cls) e->DeleteGlobalRef(ids.cls);
                jni::detachThread();
                return;

            case CmdType::Open: {
                release();
                if (!idsOk) {
                    mErrors.fetch_add(1);
                    break;
                }
                bool ok = false;
                e->PushLocalFrame(16);
                jobject local = e->NewObject(ids.cls, ids.ctor);
                if (local && !jni::clearException(e, "new MediaPlayer")) {
                    std::u16string u16 = utf8ToUtf16(cmd.path);
                    jstring jpath = e->NewString(reinterpret_cast<const jchar*>(u16.data()),
                                                 static_cast<jsize>(u16.size()));
                    e->CallVoidMethod(local, ids.setDataSource, jpath);
                    bool bad = jni::clearException(e, "setDataSource");
                    if (!bad) {
                        e->CallVoidMethod(local, ids.prepare);
                        bad = jni::clearException(e, "prepare");
                    }
                    if (!bad) {
                        player = e->NewGlobalRef(local);
                        ok = true;
                    } else {
                        e->CallVoidMethod(local, ids.release);
                        jni::clearException(e, "release(failed)");
                    }
                }
                e->PopLocalFrame(nullptr);

                if (!ok) {
                    LYR_LOGW("AudioPlayer: failed to open %s", cmd.path.c_str());
                    mErrors.fetch_add(1);
                    break;
                }
                applyVolume();
                jint dur = e->CallIntMethod(player, ids.getDuration);
                jni::clearException(e, "getDuration");
                mDurationMs = dur > 0 ? dur : 0;
                mHasTrack = true;
                mPosMs = 0;
                mPosStampUs = nowUs();
                if (cmd.autoplay) {
                    e->CallVoidMethod(player, ids.start);
                    if (!jni::clearException(e, "start")) {
                        wantPlaying = true;
                        mPlaying = true;
                    }
                }
                break;
            }

            case CmdType::Pause:
                if (player && wantPlaying) {
                    e->CallVoidMethod(player, ids.pause);
                    jni::clearException(e, "pause");
                    wantPlaying = false;
                    mPlaying = false;
                }
                break;

            case CmdType::Resume:
                if (player && !wantPlaying) {
                    e->CallVoidMethod(player, ids.start);
                    if (!jni::clearException(e, "resume")) {
                        wantPlaying = true;
                        mPlaying = true;
                        mPosStampUs = nowUs();
                    }
                }
                break;

            case CmdType::Stop:
                release();
                break;

            case CmdType::Volume:
                volume = cmd.value;
                applyVolume();
                break;

            case CmdType::Seek:
                if (player) {
                    jint to = static_cast<jint>(cmd.value);
                    const int64_t dur = mDurationMs.load();
                    if (dur > 0 && to > dur - 200) to = static_cast<jint>(std::max<int64_t>(0, dur - 200));
                    e->CallVoidMethod(player, ids.seekTo, to);
                    jni::clearException(e, "seekTo");
                    mPosMs = to;
                    mPosStampUs = nowUs();
                }
                break;
            }
            continue; // finish the queue before polling
        }

        // --- position polling ---
        if (player) {
            jint pos = e->CallIntMethod(player, ids.getCurrentPosition);
            jboolean playing = e->CallBooleanMethod(player, ids.isPlaying);
            if (jni::clearException(e, "poll")) continue;
            mPosMs = pos;
            mPosStampUs = nowUs();

            if (wantPlaying && !playing) {
                // MediaPlayer stopped by itself = song finished.
                LYR_LOGI("AudioPlayer: track finished");
                e->CallVoidMethod(player, ids.release);
                jni::clearException(e, "release(finished)");
                e->DeleteGlobalRef(player);
                player = nullptr;
                wantPlaying = false;
                mPlaying = false;
                mHasTrack = false;
                mFinished.fetch_add(1);
            }
        }
    }
}

} // namespace lyrics
