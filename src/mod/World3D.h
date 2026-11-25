#pragma once
// Real in-game 3D lyrics.
//
// The lyrics are drawn by the game's own renderer (inside its "render level" step) at fixed world coordinates, using the
// game's real camera. They are therefore genuinely part of the world: they stay still when you look around, get smaller
// when you walk away, and you can walk through them.
//
// This approach follows LeviMusic (TerserahGw/levimusic-android) and World Analysis (callmehuda/World-Analysis,
// Copyright 2026 Ekowat), which derive from BedrockTools by RadiantByte. GPL-3.0-only.
#include <cstdint>
#include <string>
#include <vector>

namespace lyrics::w3d {

enum class State { Idle, Preparing, Ready, Unsupported, BindingMismatch, HookFailed };

struct Cell {
    float x, y;      // in text cells, x centered, y up from the baseline
    uint8_t coverage; // 0..255
};

struct Part { // one group of words
    std::vector<Cell> fill;
    std::vector<Cell> outline;
    float widthCells = 0;
    float f0 = 0, f1 = 1; // part of the line time during which it is sung
};

struct Cue {
    uint64_t serial = 0;
    int64_t startMs = 0, endMs = 1;
    float cellsPerEm = 32;
    std::vector<Part> parts;
};

struct Settings {
    float textBlocks = 0.5f;   // height of one em in blocks
    float distance = 6.0f;     // blocks in front of the player
    float scatterDeg = 70.0f;  // width of the area in front of the player
    float heightBlocks = 0.0f; // shift up/down
    float jitterRad = 0.35f;   // how much panels are turned away from facing the camera
    int holdMs = 1500;         // how long words stay after their line ends
    float sung[3] = {1.0f, 0.85f, 0.24f};
    float unsung[3] = {1.0f, 1.0f, 1.0f};
    float grad[3] = {1.0f, 0.31f, 0.85f};
    int colorMode = 0; // 0 solid, 1 gradient, 2 rainbow
    float rainbowSpeed = 0.3f;
    float opacity = 1.0f;
    bool outline = true;
};

// Start preparing the renderer (once). Safe to call every frame.
// A file used to detect crashes while hooking a game build that has no verified profile.
void setGuardFile(const std::string& path);
void request(bool enable);
State state();
const char* statusText();
// True while the game is actually drawing the lyrics.
bool drawing();

void publishCue(Cue&& cue);
void setPlayback(int64_t positionMs, bool playing);
void setSettings(const Settings& s);
void unload();

} // namespace lyrics::w3d
