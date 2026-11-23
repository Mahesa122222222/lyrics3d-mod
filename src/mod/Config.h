#pragma once

#include <string>
#include <string_view>

#include <pl/Config.hpp>

// Follows the template pattern: struct ModConfig + two JSON builder functions (used by config_generator)
// + a pl::config::Schema specialization. The template Config.cpp is NOT modified.
namespace clange_me {

struct ModConfig {
    int version = 1;

    // --- general ---
    bool enabled = true;       // show the lyrics overlay
    bool autoStart = true;     // start the playlist as soon as the game starts
    bool loop = true;          // restart the playlist after the last song
    bool shuffle = false;
    std::string songsDir = ""; // empty = <mod data folder>/songs
    float volume = 0.8f;       // 0..1
    int lyricsOffsetMs = 0;    // positive = lyrics appear later

    // --- display ---
    float fontSize = 0.055f;   // text height, fraction of screen height
    float positionY = 0.55f;   // 0 = screen center, 1 = top edge, -1 = bottom edge
    float opacity = 1.0f;      // 0..1
    bool showContext = true;   // show the dimmed previous/next lines
    float lineSpacing = 1.25f;
    std::string fontFamily = "sans-serif";
    bool bold = true;
    std::string sungColor = "#FFD93D";    // already-sung part
    std::string unsungColor = "#FFFFFF";  // not-yet-sung part
    std::string extrudeColor = "#2A1F4D"; // 3D thick side
    float glow = 1.0f;                    // 0 = off

    // --- colors & party mode ---
    std::string colorMode = "solid";        // solid | gradient | rainbow
    std::string gradientColor = "#FF4FD8";  // gradient end color (from sungColor to this)
    float rainbowSpeed = 0.3f;              // rainbow cycles per second
    bool sparkle = false;                   // sparkle stars around the text
    float sparkleAmount = 0.5f;             // 0..1
    bool partyNotes = false;                // floating music notes
    float notesAmount = 0.5f;               // 0..1

    // --- world mode (words float in the world and stay put when the camera turns) ---
    std::string layoutMode = "world";  // world | screen
    float worldFontSize = 0.07f;       // word text height, fraction of screen height
    bool worldCaps = true;             // HURUF BESAR
    int worldHoldMs = 1500;            // how long a word stays after its line finishes
    float worldSpread = 1.0f;          // how widely words are spread
    float worldDistance = 7.0f;        // how many blocks in front of you the words appear
    bool worldWalk = true;             // words stay behind when you walk (uses the on-screen joystick)
    float walkSpeed = 4.3f;            // blocks per second at full joystick
    float gameFovDeg = 70.0f;          // field of view of the game camera (vertical), for anchoring
    std::string camTrack = "motion";   // motion (watch the picture) | swipe (finger swipes) | java (read the real game camera, experimental)
    float camSens = 0.20f;             // degrees of camera turn per pixel of swipe
    bool camInvertX = false;
    bool camInvertY = false;
    float camZoneLeft = 0.30f;         // swipes on the left side of the screen (joystick) are ignored

    // --- 3D effects ---
    float fovDeg = 50.0f;
    float depth = 2.2f;          // text distance from the camera (virtual world units)
    float maxYawDeg = 16.0f;     // left-right sway
    float pitchDeg = 5.0f;       // up-down sway
    float floatAmount = 0.02f;   // floating up and down
    int extrudeLayers = 6;       // 0 = flat text
    float extrudeDepth = 0.07f;  // total thickness
    int transitionMs = 450;      // line change animation duration
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace clange_me

// Name used throughout the mod code.
using LyricsConfig = clange_me::ModConfig;

#define LYR_FIELD(key, ttl, desc)                      \
    if (name == key) return {.title = ttl, .description = desc};

template <> struct pl::config::Schema<clange_me::ModConfig> {
    static constexpr std::string_view title = "Lyrics 3D Config";
    static constexpr std::string_view description = {};

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version") return {.title = "Version", .readOnly = true};

        LYR_FIELD("enabled", "Show lyrics", "Turn off to hide the lyrics overlay.")
        LYR_FIELD("autoStart", "Autoplay", "Start playing the playlist as soon as the game starts.")
        LYR_FIELD("loop", "Repeat playlist", "Go back to the first song after the last one ends.")
        LYR_FIELD("shuffle", "Shuffle", "Play songs in random order.")
        LYR_FIELD("songsDir", "Songs folder", "Empty = the songs folder inside the mod data folder.")
        LYR_FIELD("volume", "Volume", "0.0 to 1.0.")
        LYR_FIELD("lyricsOffsetMs", "Lyrics offset (ms)", "Positive = lyrics appear later, negative = earlier.")

        LYR_FIELD("fontSize", "Font size", "Fraction of screen height, e.g. 0.055.")
        LYR_FIELD("positionY", "Vertical position", "0 = center, 1 = top edge, -1 = bottom edge.")
        LYR_FIELD("opacity", "Opacity", "0.0 to 1.0.")
        LYR_FIELD("showContext", "Previous/next lines", "Show the dimmed neighbouring lines.")
        LYR_FIELD("lineSpacing", "Line spacing", "Multiple of the font height.")
        LYR_FIELD("fontFamily", "Font", "Android font family name, e.g. sans-serif, serif, monospace.")
        LYR_FIELD("bold", "Bold", "Use bold text.")
        LYR_FIELD("sungColor", "Sung color", "Format #RRGGBB.")
        LYR_FIELD("unsungColor", "Unsung color", "Format #RRGGBB.")
        LYR_FIELD("extrudeColor", "3D side color", "Format #RRGGBB.")
        LYR_FIELD("glow", "Glow", "0 = off, 1 = normal.")

        LYR_FIELD("colorMode", "Color mode", "solid = one color, gradient = gradient, rainbow = rainbow.")
        LYR_FIELD("gradientColor", "Gradient color", "Gradient end color. Format #RRGGBB.")
        LYR_FIELD("rainbowSpeed", "Rainbow speed", "Color cycles per second.")
        LYR_FIELD("sparkle", "Sparkles", "Small twinkling stars around the text.")
        LYR_FIELD("sparkleAmount", "Sparkle amount", "0.0 to 1.0.")
        LYR_FIELD("partyNotes", "Music notes (party mode)", "Music notes floating up the screen.")
        LYR_FIELD("notesAmount", "Music notes amount", "0.0 to 1.0.")

        LYR_FIELD("layoutMode", "Display mode", "world = words floating in the world, screen = lines on the screen.")
        LYR_FIELD("worldFontSize", "Word size (world)", "Fraction of screen height.")
        LYR_FIELD("worldCaps", "Uppercase", "Convert words to UPPERCASE in world mode.")
        LYR_FIELD("worldHoldMs", "Word hold time (ms)", "After its line has finished.")
        LYR_FIELD("worldSpread", "Word spread", "1 = normal, smaller = tighter.")
        LYR_FIELD("worldDistance", "Word distance (blocks)", "How far in front of you the words appear.")
        LYR_FIELD("worldWalk", "Leave words behind when walking", "Uses the on-screen joystick to estimate your movement.")
        LYR_FIELD("walkSpeed", "Walking speed (blocks/s)", "Tune so that words pass you at the right speed.")
        LYR_FIELD("gameFovDeg", "Game FOV", "Set this to the field of view in the game's video settings (default 70).")
        LYR_FIELD("camTrack", "Camera tracking", "motion = follow the picture, swipe = follow finger swipes, java = read the real game camera (experimental).")
        LYR_FIELD("camSens", "Camera turn sensitivity", "Degrees of camera turn per pixel of finger swipe.")
        LYR_FIELD("camInvertX", "Invert left-right", "Enable if the text moves the wrong way.")
        LYR_FIELD("camInvertY", "Invert up-down", "Enable if the text moves the wrong way.")
        LYR_FIELD("camZoneLeft", "Joystick zone (ignored)", "Fraction of screen width from the left.")

        LYR_FIELD("fovDeg", "Camera field of view", "Degrees.")
        LYR_FIELD("depth", "Text distance", "Virtual world units.")
        LYR_FIELD("maxYawDeg", "Left-right sway", "Degrees. 0 = still.")
        LYR_FIELD("pitchDeg", "Up-down sway", "Degrees. 0 = still.")
        LYR_FIELD("floatAmount", "Floating effect", "How much the text bobs up and down.")
        LYR_FIELD("extrudeLayers", "Thickness layers", "0 = flat text, more layers = thicker.")
        LYR_FIELD("extrudeDepth", "3D thickness", "Total thickness of the text sides.")
        LYR_FIELD("transitionMs", "Animation duration (ms)", "Speed of the transition between lines.")
        return {};
    }
};

#undef LYR_FIELD
