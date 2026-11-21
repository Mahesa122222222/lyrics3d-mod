#include "LyricsController.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

#include "Log.h"

namespace fs = std::filesystem;

namespace lyrics {

namespace {

std::string lowerStr(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isAudioExt(const std::string& ext) {
    static const char* kExts[] = {".mp3", ".ogg", ".m4a", ".aac", ".wav", ".flac", ".opus"};
    for (auto* e : kExts)
        if (ext == e) return true;
    return false;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

LyricsController::LyricsController(const fs::path& dataDir, const LyricsConfig& cfg)
    : mCfg(cfg), mDataDir(dataDir) {
    mControlFile = mDataDir / "control.txt";
    applyConfig(cfg);
    scan();
    LYR_LOGI("Folder lagu: %s (%zu lagu)", mSongsDir.string().c_str(), mTracks.size());
    LYR_LOGI("File kontrol: %s (isi: play|pause|toggle|next|prev|stop|rescan)", mControlFile.string().c_str());
}

void LyricsController::applyConfig(const LyricsConfig& cfg) {
    mCfg = cfg;
    mSongsDir = cfg.songsDir.empty() ? (mDataDir / "songs") : fs::path(cfg.songsDir);
    mAudio.setVolume(cfg.volume);
}

void LyricsController::scan() {
    mTracks.clear();
    std::error_code ec;
    fs::create_directories(mSongsDir, ec);

    std::vector<fs::path> audios, lrcs;
    for (auto it = fs::directory_iterator(mSongsDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = lowerStr(it->path().extension().string());
        if (isAudioExt(ext)) audios.push_back(it->path());
        else if (ext == ".lrc") lrcs.push_back(it->path());
    }
    std::sort(audios.begin(), audios.end(), [](const fs::path& a, const fs::path& b) {
        return lowerStr(a.filename().string()) < lowerStr(b.filename().string());
    });

    for (auto& a : audios) {
        Track t;
        t.audio = a;
        t.title = a.stem().string();
        const std::string stem = lowerStr(a.stem().string());
        for (auto& l : lrcs)
            if (lowerStr(l.stem().string()) == stem) {
                t.lrc = l;
                break;
            }
        mTracks.push_back(std::move(t));
    }

    mOrder.resize(mTracks.size());
    for (size_t i = 0; i < mOrder.size(); ++i) mOrder[i] = static_cast<int>(i);
    if (mCfg.shuffle) std::shuffle(mOrder.begin(), mOrder.end(), mRng);
    mPos = -1;
}

void LyricsController::startTrack(int orderPos, bool autoplay) {
    if (mOrder.empty()) return;
    const int n = static_cast<int>(mOrder.size());
    mPos = ((orderPos % n) + n) % n;
    const Track& t = mTracks[mOrder[mPos]];

    mHasLrc = false;
    mLrc = LrcData{};
    if (!t.lrc.empty()) {
        mLrc = parseLrc(readFile(t.lrc));
        mHasLrc = mLrc.timed;
        if (!mHasLrc) LYR_LOGW("LRC tidak berisi timestamp yang valid: %s", t.lrc.string().c_str());
    }
    if (!mHasLrc) mLrc = makePlaceholder(t.title);

    ++mTrackId;
    mUserStopped = false;
    LYR_LOGI("Memutar [%d/%d] %s%s", mPos + 1, n, t.title.c_str(), mHasLrc ? "" : " (tanpa lirik)");
    mAudio.open(t.audio.string(), autoplay);
}

void LyricsController::advance(int dir) {
    if (mOrder.empty()) return;
    const int n = static_cast<int>(mOrder.size());
    int next = mPos + dir;
    if ((next >= n || next < 0) && !mCfg.loop && dir > 0) {
        mAudio.stop();
        mUserStopped = true;
        return;
    }
    if (next >= n && mCfg.shuffle) std::shuffle(mOrder.begin(), mOrder.end(), mRng);
    startTrack(next, true);
}

void LyricsController::play() {
    if (mTracks.empty()) return;
    if (!mStarted || mUserStopped || !mAudio.hasTrack()) {
        mStarted = true;
        startTrack(mPos < 0 ? 0 : mPos, true);
    } else {
        mAudio.resume();
    }
}
void LyricsController::pause() { mAudio.pause(); }
void LyricsController::toggle() {
    if (mAudio.isPlaying()) pause();
    else play();
}
void LyricsController::next() {
    mStarted = true;
    advance(+1);
}
void LyricsController::prev() {
    mStarted = true;
    advance(-1);
}
void LyricsController::stop() {
    mAudio.stop();
    mUserStopped = true;
}
void LyricsController::rescan() {
    mAudio.stop();
    scan();
    mStarted = false;
    mUserStopped = false;
    LYR_LOGI("Rescan: %zu lagu", mTracks.size());
}

void LyricsController::pollControlFile() {
    std::error_code ec;
    if (!fs::exists(mControlFile, ec)) return;
    std::string cmd = readFile(mControlFile);
    // Kosongkan file supaya perintah dijalankan sekali saja.
    { std::ofstream(mControlFile, std::ios::trunc); }

    std::string word;
    for (char c : cmd)
        if (!std::isspace(static_cast<unsigned char>(c))) word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (word.empty()) return;

    LYR_LOGI("Perintah: %s", word.c_str());
    if (word == "play") play();
    else if (word == "pause") pause();
    else if (word == "toggle") toggle();
    else if (word == "next") next();
    else if (word == "prev" || word == "previous") prev();
    else if (word == "stop") stop();
    else if (word == "rescan") rescan();
    else LYR_LOGW("Perintah tidak dikenal: %s", word.c_str());
}

void LyricsController::tick(double timeSec) {
    if (timeSec >= mNextPollSec) {
        mNextPollSec = timeSec + 0.25;
        pollControlFile();
    }

    if (!mStarted && mCfg.autoStart && !mTracks.empty()) {
        mStarted = true;
        startTrack(0, true);
        mSeenFinished = mAudio.finishedCount();
        mSeenErrors = mAudio.errorCount();
        return;
    }

    // Gagal membuka: lompat ke lagu berikutnya, tapi berhenti jika semuanya gagal.
    const uint32_t errs = mAudio.errorCount();
    if (errs != mSeenErrors) {
        mSeenErrors = errs;
        if (++mConsecutiveFailures >= static_cast<int>(mTracks.size())) {
            LYR_LOGE("Semua lagu gagal diputar; berhenti");
            mConsecutiveFailures = 0;
            mUserStopped = true;
            mHasLrc = false;
            mLrc = LrcData{};
            return;
        }
        advance(+1);
        return;
    }

    const uint32_t fin = mAudio.finishedCount();
    if (fin != mSeenFinished) {
        mSeenFinished = fin;
        mConsecutiveFailures = 0;
        advance(+1);
    }
}

ViewState LyricsController::view() const {
    ViewState v;
    if (mUserStopped || mLrc.lines.empty() || !mAudio.hasTrack()) return v;
    v.lrc = &mLrc;
    v.positionMs = mAudio.positionMs() - mCfg.lyricsOffsetMs;
    v.trackId = mTrackId;
    v.playing = mAudio.isPlaying();
    return v;
}

} // namespace lyrics
