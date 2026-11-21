#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lyrics {

struct LrcLine {
    int64_t timeMs = 0;
    std::string text; // UTF-8, kosong = jeda/instrumental
};

struct LrcData {
    std::vector<LrcLine> lines; // terurut naik menurut timeMs
    std::string title;
    std::string artist;
    bool timed = false; // true jika ada minimal satu baris bertimestamp
};

// Mendukung: [mm:ss.xx], [mm:ss.xxx], [mm:ss:xx], banyak timestamp per baris,
// tag [ti:] [ar:] [offset:], timestamp kata <mm:ss.xx> (dibuang), BOM, CRLF.
LrcData parseLrc(const std::string& content);

// Lirik pengganti untuk lagu tanpa .lrc: satu baris judul di detik 0.
LrcData makePlaceholder(const std::string& title);

// Indeks baris aktif pada posMs, atau -1 jika posisi sebelum baris pertama.
int findLineIndex(const std::vector<LrcLine>& lines, int64_t posMs);

} // namespace lyrics
