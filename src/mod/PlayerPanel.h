#pragma once
// In-game music player panel (Spotify style): a small "now playing" pill that opens a full panel with the
// current song, synced lyrics, the song list and quick settings.
//
// It is drawn with the launcher's HUD overlay (pl::modmenu::submitDrawCommands) and reacts to touches, so it
// does not depend on the launcher's settings menu. The overall idea (HUD draw commands + touch callback) follows
// LeviMusic (TerserahGw/levimusic-android). GPL-3.0-only.
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "LrcParser.h"

namespace lyrics {

struct PanelConfig {
    bool show = true;   // show the small pill
    float scale = 1.0f; // pill size
    float x = 0.03f;    // pill position, 0..1 of the free space
    float y = 0.93f;
};

// Everything the panel needs to know, refreshed every frame by the mod (render thread).
struct PanelModel {
    bool hasTrack = false;
    bool playing = false;
    std::string title, artist;
    int64_t posMs = 0, durMs = 0; // audio position (progress bar)
    int64_t lyricPosMs = 0;       // position used for the lyrics (audio - lyrics offset)
    int lyricOffsetMs = 0;
    const LrcData* lrc = nullptr; // may be null
    int currentSong = -1;         // index in the song list
    const std::vector<std::string>* songTitles = nullptr;
    const std::vector<char>* songHasLrc = nullptr;
    std::string songsDir;
    float volume = 0.8f;
    bool shuffle = false, loop = true;
    std::string layoutMode = "dynamic";
    bool cardShow = true, r3Outline = true;
    float r3Size = 0.5f, r3Scatter = 70.f, r3Height = 0.f;
    std::string statusText; // state of the real 3D renderer
    float glass = 0.55f;    // glass strength 0..1 (0 clear, 1 dark frosted)
};

// Things the panel asks the mod to do (called on the render thread).
struct PanelHost {
    std::function<void()> toggle, next, prev, rescan, openMenu;
    std::function<void(int)> playIndex;
    std::function<void(int64_t)> seekMs;
    std::function<void(const std::string&, const std::string&)> setSetting;
};

class PlayerPanel {
public:
    // Input thread. Returns true if the touch was used by the panel (the game must not see it).
    bool onTouch(int action, int pointerId, float x, float y);

    // Render thread. `ready` = the launcher overlay is available.
    void frame(double timeSec, bool ready, const PanelConfig& cfg, const PanelModel& model, const std::string_view moduleId,
               const PanelHost& host);

    // Remove everything that was drawn.
    void shutdown(const std::string_view moduleId);

    bool isOpen() const;

private:
    enum class Tab { Lyrics = 0, Library = 1, Settings = 2 };
    enum class Cap { None, Swallow, Seek, Volume, List };
    struct Ev {
        enum K { Toggle, Next, Prev, Rescan, OpenMenu, Play, Seek, SeekLine, Set } k = Toggle;
        int i = 0;
        int64_t ms = 0;
        std::string a, b;
    };

    mutable std::mutex mMu;
    // --- UI state (shared with the input thread, guarded by mMu) ---
    bool mOpen = false;
    double mToggleT = -10.0;
    Tab mTab = Tab::Lyrics;
    float mScroll = 0.f; // library scroll in pixels
    PanelConfig mCfg;
    float mSurfW = 0, mSurfH = 0;
    int mLibCount = 0;
    int64_t mDurMs = 0;
    bool mHasTrack = false;
    const LrcData* mLrcSeen = nullptr; // only compared, never dereferenced from the input thread
    std::vector<int64_t> mLineTimes;   // times of the lyric lines (copied for the input thread)
    float mLyrScroll = 0.f;            // smoothed active line index (render thread)
    bool mSetCard = true, mSetOutline = true; // mirrors of the model for the input thread
    float mSetSize = 0.5f, mSetScatter = 70.f, mSetHeight = 0.f;
    // touch capture
    Cap mCap = Cap::None;
    int mCapId = -1;
    float mCapStartX = 0, mCapStartY = 0, mCapStartScroll = 0;
    bool mCapMoved = false;
    bool mSeeking = false;
    float mSeekFrac = 0.f;
    float mVolDrag = -1.f;
    std::vector<Ev> mEvents;

    // --- render thread only ---
    float mTabPos = 0.f; // animated position of the tab highlight
    bool mIconsDone = false;
    double mLastSubmit = -10.0, mLastFrame = 0.0;
    bool mDrewSomething = false;
    std::unordered_map<std::string, std::string> mCoverIds;
};

} // namespace lyrics
