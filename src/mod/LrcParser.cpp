#include "LrcParser.h"

#include <algorithm>
#include <cctype>

namespace lyrics {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// "mm:ss", "mm:ss.xx", "mm:ss:xx" -> ms. false jika bukan timestamp.
bool parseTimestamp(const std::string& tag, int64_t& outMs) {
    size_t c1 = tag.find(':');
    if (c1 == std::string::npos) return false;
    std::string mm = tag.substr(0, c1);
    std::string rest = tag.substr(c1 + 1);
    if (!allDigits(mm) || mm.size() > 5) return false;

    std::string ss, frac;
    size_t sep = rest.find_first_of(".:");
    if (sep == std::string::npos) {
        ss = rest;
    } else {
        ss = rest.substr(0, sep);
        frac = rest.substr(sep + 1);
    }
    if (!allDigits(ss) || ss.size() > 2) return false;
    if (!frac.empty() && !allDigits(frac)) return false;

    int64_t ms = 0;
    if (!frac.empty()) {
        std::string f = frac.substr(0, 3);
        while (f.size() < 3) f.push_back('0');
        ms = std::stoi(f);
    }
    outMs = std::stoll(mm) * 60000 + std::stoll(ss) * 1000 + ms;
    return true;
}

// Hapus timestamp kata <mm:ss.xx> di dalam teks.
std::string stripWordTags(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<') {
            size_t close = s.find('>', i);
            if (close != std::string::npos) {
                int64_t dummy;
                if (parseTimestamp(s.substr(i + 1, close - i - 1), dummy)) {
                    i = close + 1;
                    continue;
                }
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

} // namespace

LrcData parseLrc(const std::string& contentIn) {
    LrcData data;
    std::string content = contentIn;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    int64_t offsetMs = 0;
    size_t pos = 0;
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = content.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? content.size() + 1 : nl + 1;

        line = trim(line);
        if (line.empty() || line[0] != '[') continue;

        std::vector<int64_t> times;
        size_t i = 0;
        while (i < line.size() && line[i] == '[') {
            size_t close = line.find(']', i);
            if (close == std::string::npos) break;
            std::string tag = line.substr(i + 1, close - i - 1);
            int64_t t;
            if (parseTimestamp(tag, t)) {
                times.push_back(t);
            } else {
                size_t colon = tag.find(':');
                if (colon != std::string::npos) {
                    std::string key = lower(trim(tag.substr(0, colon)));
                    std::string val = trim(tag.substr(colon + 1));
                    if (key == "ti") data.title = val;
                    else if (key == "ar") data.artist = val;
                    else if (key == "offset") {
                        try { offsetMs = std::stoll(val); } catch (...) {}
                    }
                }
            }
            i = close + 1;
        }
        if (times.empty()) continue;

        std::string text = trim(stripWordTags(line.substr(i)));
        for (int64_t t : times) data.lines.push_back({t, text});
    }

    // Spesifikasi LRC: offset positif membuat lirik muncul lebih cepat.
    for (auto& l : data.lines) l.timeMs = std::max<int64_t>(0, l.timeMs - offsetMs);
    std::stable_sort(data.lines.begin(), data.lines.end(),
                     [](const LrcLine& a, const LrcLine& b) { return a.timeMs < b.timeMs; });

    data.timed = !data.lines.empty();
    return data;
}

LrcData makePlaceholder(const std::string& title) {
    LrcData d;
    d.title = title;
    d.timed = false;
    d.lines.push_back({0, title});
    return d;
}

int findLineIndex(const std::vector<LrcLine>& lines, int64_t posMs) {
    auto it = std::upper_bound(lines.begin(), lines.end(), posMs,
                               [](int64_t p, const LrcLine& l) { return p < l.timeMs; });
    return static_cast<int>(it - lines.begin()) - 1;
}

} // namespace lyrics
