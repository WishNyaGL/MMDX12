#include "asset/MmeText.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "core/TextUtil.h"

namespace mmdx::mmetext {
namespace {

// Valid UTF-8 check (overlongs / surrogates / > U+10FFFF rejected): enough to decide whether the SJIS fallback
// is needed. MME text is mostly ASCII; only a few paths carry Japanese comments.
bool IsValidUtf8(const std::string& s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { ++i; continue; }
        int extra;
        unsigned int cp;
        if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else return false;
        if (i + extra >= n) return false;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += extra + 1;
    }
    return true;
}

} // namespace

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

bool ReadTextFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}

bool ReadTextFileText(const std::filesystem::path& p, std::string& out) {
    if (!ReadTextFile(p, out)) return false;
    if (IsValidUtf8(out)) return true;
    out = SjisToUtf8(out);
    return true;
}

bool ResolveRel(const std::filesystem::path& base, const std::string& relIn, std::filesystem::path& out) {
    std::string rel = relIn;
    for (char& c : rel)
        if (c == '\\') c = '/';
    if (rel.empty()) return false;
    if (rel.size() >= 2 && rel[1] == ':') {   // absolute (drive)
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(Utf8ToWide(rel)), ec)) {
            out = std::filesystem::path(Utf8ToWide(rel));
            return true;
        }
        return false;
    }
    std::filesystem::path cur = base;
    std::string comp;
    auto flush = [&](const std::string& c) -> bool {
        if (c.empty() || c == ".") return true;
        if (c == "..") { cur = cur.parent_path(); return true; }
        std::error_code ec;
        std::filesystem::path cand = cur / Utf8ToPath(c);
        if (std::filesystem::exists(cand, ec)) { cur = cand; return true; }
        const std::string want = Lower(c);
        if (std::filesystem::is_directory(cur, ec)) {
            for (auto it = std::filesystem::directory_iterator(
                     cur, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (Lower(PathToUtf8(it->path().filename())) == want) { cur = it->path(); return true; }
            }
        }
        return false;
    };
    for (size_t i = 0; i <= rel.size(); ++i) {
        if (i == rel.size() || rel[i] == '/' || rel[i] == '\\') {
            if (!flush(comp)) return false;
            comp.clear();
        } else {
            comp += rel[i];
        }
    }
    std::error_code ec;
    if (std::filesystem::exists(cur, ec) && !std::filesystem::is_directory(cur, ec)) { out = cur; return true; }
    return false;
}

std::string PathKey(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::weakly_canonical(p, ec);
    if (ec) abs = p;
    std::string s = PathToUtf8(abs);
    for (char& c : s)
        if (c == '\\') c = '/';
    return Lower(s);
}

} // namespace mmdx::mmetext
