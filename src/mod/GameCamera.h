#pragma once
// "Real camera" camera source: watches the matrices the game uploads to the GPU (glUniformMatrix4fv / glUniform4fv)
// and looks for the camera's view matrix and projection matrix. This is experimental: it depends on how the game
// renders, so it is verified at run time against the screen-motion tracker before it is trusted.
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include <pl/memory/Hook.hpp>

namespace lyrics {

struct GamePose {
    bool a = false, b = false, c = false; // three ways of reading the matrices (view / inverse view / view*projection)
    float cyaw = 0, cpitch = 0;
    float ayaw = 0, apitch = 0;     // radians
    float byaw = 0, bpitch = 0;
    bool persp = false;             // a perspective matrix matching the screen was seen
    float fovV = 0;                 // its vertical field of view (radians)
    int calls = 0, rigid = 0;       // statistics of the last frame
};

// Writes the names of the game's exported functions that look camera-related into a text file (for diagnosis).
// Returns the number of lines written, or -1 when the game library or its symbol table cannot be found.
int dumpGameSymbols(const std::string& outPath);

class GameCamera {
public:
    static GameCamera& instance();

    bool install(); // true if at least one hook was installed
    void uninstall();
    bool installed() const { return mInstalled.load(); }
    void setScreen(int w, int h) { mAspect.store(h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 0.0f); }

    // Once per frame on the render thread: returns what was seen since the last call.
    GamePose endFrame(double timeNow);

    // Called from the GL hooks.
    void onMatrices(const float* v, int count, bool transposed);
    void onVec4(const float* v, int count);
    // Buffer uploads (uniform buffers): the world camera is usually sent this way.
    void onBlob(const void* data, size_t bytes, unsigned target, int kind);
    struct MapInfo { void* ptr = nullptr; intptr_t len = 0; unsigned access = 0; };
    void setMap(unsigned target, const MapInfo& m);
    bool getMap(unsigned target, MapInfo& m);

private:
    void analyze(const float* m); // m = 16 floats, column-major as OpenGL sees them
    void record(int kind, float yaw, float pitch); // kind 0 = A, 1 = B, 2 = C

    std::atomic<bool> mInstalled{false};
    std::atomic<float> mAspect{0.0f};
    pl::memory::HookHandle mHookM, mHookV, mHookSub, mHookData, mHookMap, mHookUnmap, mHookFlush;

    std::mutex mMutex;
    std::unordered_map<int, int> mCntA, mCntB, mCntC;
    std::unordered_map<int, std::pair<float, float>> mValA, mValB, mValC;
    bool mPersp = false;
    float mFov = 0;
    int mCalls = 0, mRigid = 0;
    // buffer statistics (for the log)
    std::atomic<int> mNSub{0}, mNData{0}, mNMap{0}, mNUnmap{0}, mNFlush{0}, mNPersist{0};
    std::atomic<long> mBytes{0}, mBudget{0};
    std::atomic<int> mBigSkipped{0};
    std::unordered_map<unsigned, MapInfo> mMaps;
    bool mHave[3] = {false, false, false};
    float mLastYaw[3] = {0, 0, 0}, mLastPitch[3] = {0, 0, 0};
    double mLogT = 0.0;
    int mLogN = 0;
};

} // namespace lyrics
