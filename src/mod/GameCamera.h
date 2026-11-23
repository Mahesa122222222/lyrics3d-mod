#pragma once
// "Java mode" camera source: watches the matrices the game uploads to the GPU (glUniformMatrix4fv / glUniform4fv)
// and looks for the camera's view matrix and projection matrix. This is experimental: it depends on how the game
// renders, so it is verified at run time against the screen-motion tracker before it is trusted.
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include <pl/memory/Hook.hpp>

namespace lyrics {

struct GamePose {
    bool a = false, b = false;      // two ways of reading the same matrices (world->camera / camera->world)
    float ayaw = 0, apitch = 0;     // radians
    float byaw = 0, bpitch = 0;
    bool persp = false;             // a perspective matrix matching the screen was seen
    float fovV = 0;                 // its vertical field of view (radians)
    int calls = 0, rigid = 0;       // statistics of the last frame
};

class GameCamera {
public:
    static GameCamera& instance();

    bool install(); // true if at least one hook was installed
    void uninstall();
    bool installed() const { return mInstalled.load(); }
    void setScreen(int w, int h) { mAspect.store(h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 0.0f); }

    // Once per frame on the render thread: returns what was seen since the last call.
    GamePose endFrame();

    // Called from the GL hooks.
    void onMatrices(const float* v, int count, bool transposed);
    void onVec4(const float* v, int count);

private:
    void analyze(const float* m); // m = 16 floats, column-major as OpenGL sees them
    void record(bool viewLike, float yaw, float pitch);

    std::atomic<bool> mInstalled{false};
    std::atomic<float> mAspect{0.0f};
    pl::memory::HookHandle mHookM, mHookV;

    std::mutex mMutex;
    std::unordered_map<int, int> mCntA, mCntB;
    std::unordered_map<int, std::pair<float, float>> mValA, mValB;
    bool mPersp = false;
    float mFov = 0;
    int mCalls = 0, mRigid = 0;
};

} // namespace lyrics
