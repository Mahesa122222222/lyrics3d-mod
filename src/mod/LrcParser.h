#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lyrics {

struct LrcLine {
    int64_t timeMs = 0;
    std::string text; // UTF-8, empty = pause/instrumental
};

struct LrcData {
    std::vector<LrcLine> lines; // sorted ascending by timeMs
    std::string title;
    std::string artist;
    bool timed = false; // true if at least one line has a timestamp
};

// Supports: [mm:ss.xx], [mm:ss.xxx], [mm:ss:xx], multiple timestamps per line,
// tag [ti:] [ar:] [offset:], word timestamps <mm:ss.xx> (dropped), BOM, CRLF.
LrcData parseLrc(const std::string& content);

// Placeholder lyrics for songs without .lrc: a single title line at second 0.
LrcData makePlaceholder(const std::string& title);

// Index of the active line at posMs, or -1 if the position is before the first line.
int findLineIndex(const std::vector<LrcLine>& lines, int64_t posMs);

} // namespace lyrics
