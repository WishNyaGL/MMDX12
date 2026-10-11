#pragma once
// Low-level text / path helpers shared by the MikuMikuEffect followers (MmeHints, RayMaterial): trimming,
// tolerant file reads (UTF-8 BOM stripped, Shift-JIS converted), line splitting and case-insensitive relative
// path resolution. These functions have no policy of their own; MmeHints and RayMaterial decide what to do.
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx::mmetext {

inline std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// ASCII-only lowercase (multibyte bytes untouched).
inline std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

inline bool IsIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
inline bool IsIdentChar(char c) { return IsIdentStart(c) || (c >= '0' && c <= '9'); }

std::vector<std::string> SplitLines(const std::string& s);

// Reads a file as raw bytes; a leading UTF-8 BOM is removed. No encoding conversion (MmeHints' behaviour).
bool ReadTextFile(const std::filesystem::path& p, std::string& out);

// Reads a file and returns UTF-8: a leading BOM is removed, otherwise valid UTF-8 is kept as is and anything
// else is converted from Shift-JIS (code page 932). MME .fx / .emd files are Shift-JIS in practice.
bool ReadTextFileText(const std::filesystem::path& p, std::string& out);

// Case-insensitive path resolution (`rel` relative to `base`): exact first, then directory scanning per
// component. `rel` accepts both separators; "..", "." and absolute (drive) paths are handled.
bool ResolveRel(const std::filesystem::path& base, const std::string& relIn, std::filesystem::path& out);

// Canonical key for path caches: the absolute path lowercased (ASCII), separators normalised to '/'.
std::string PathKey(const std::filesystem::path& p);

} // namespace mmdx::mmetext
