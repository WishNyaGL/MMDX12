#pragma once
// Timecode formatting / parsing for the video render range (header-only).
// FormatTimecode: seconds -> "HH:MM:SS" (zero padded, rounds down to whole seconds, negative -> 0).
// ParseTimecode: accepts "HH:MM:SS", "MM:SS", "SS" with optional decimal seconds ("01:02:03.5"); a plain
// number is seconds; surrounding spaces are ignored. Rejects empty / garbage / negative input and a
// minutes or seconds field >= 60 when a higher field exists. Returns false on failure.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace mmdx {

inline std::string FormatTimecode(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    const long long total = (long long)std::floor(seconds);
    const int h = (int)(total / 3600);
    const int m = (int)((total / 60) % 60);
    const int s = (int)(total % 60);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    return buf;
}

inline bool ParseTimecode(const std::string& text, double& outSeconds) {
    // Trim surrounding whitespace.
    size_t begin = 0, end = text.size();
    while (begin < end && std::isspace((unsigned char)text[begin])) ++begin;
    while (end > begin && std::isspace((unsigned char)text[end - 1])) --end;
    if (begin >= end) return false;
    const std::string s = text.substr(begin, end - begin);

    // Split on ':'; fields are the numeric parts.
    double fields[3] = {0, 0, 0};
    int count = 0;
    size_t pos = 0;
    while (true) {
        if (count >= 3) return false;   // more than three fields
        const size_t colon = s.find(':', pos);
        const std::string part = colon == std::string::npos ? s.substr(pos) : s.substr(pos, colon - pos);
        if (part.empty()) return false;
        // A field is an optional sign-less decimal number only.
        size_t i = 0;
        bool dot = false, digits = false;
        for (; i < part.size(); ++i) {
            const char c = part[i];
            if (c == '.') {
                if (dot) return false;
                dot = true;
            } else if (std::isdigit((unsigned char)c)) {
                digits = true;
            } else {
                return false;
            }
        }
        if (!digits) return false;
        fields[count++] = std::atof(part.c_str());
        if (colon == std::string::npos) break;
        pos = colon + 1;
    }
    if (count == 0) return false;

    double seconds = 0.0;
    if (count == 3) {
        if (fields[0] < 0 || fields[1] < 0 || fields[2] < 0) return false;
        if (fields[1] >= 60.0 || fields[2] >= 60.0) return false;
        seconds = fields[0] * 3600.0 + fields[1] * 60.0 + fields[2];
    } else if (count == 2) {
        if (fields[0] < 0 || fields[1] < 0) return false;
        if (fields[1] >= 60.0) return false;
        seconds = fields[0] * 60.0 + fields[1];
    } else {
        if (fields[0] < 0) return false;
        seconds = fields[0];
    }
    if (seconds < 0.0) return false;
    outSeconds = seconds;
    return true;
}

}  // namespace mmdx
