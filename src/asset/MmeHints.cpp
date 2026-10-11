// MmeHints.cpp: follows a model's own MikuMikuEffect references (see MmeHints.h). A tolerant, line / token scatter
// parser: no full HLSL, only the .emd effect list, #include chains, #define literals and ResourceName textures.
#include "asset/MmeHints.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

#include "core/Log.h"
#include "core/TextUtil.h"

namespace mmdx {
namespace {

constexpr int kMaxIncludeDepth = 8;

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool IsIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool IsIdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

bool ReadTextFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    // UTF-8 BOM: paths in .fx are ASCII in practice; the BOM only confuses the first token.
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); lines.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

// Case-insensitive path resolution (`rel` relative to `base`): exact first, then directory scanning per component.
bool ResolveRel(const std::filesystem::path& base, const std::string& relIn, std::filesystem::path& out) {
    std::string rel = relIn;
    for (char& c : rel) if (c == '\\') c = '/';
    if (rel.empty()) return false;
    if (rel.size() >= 2 && rel[1] == ':') {   // absolute (drive)
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(rel), ec)) { out = std::filesystem::path(rel); return true; }
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
            for (auto it = std::filesystem::directory_iterator(cur, std::filesystem::directory_options::skip_permission_denied, ec);
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

// Resolve an MME expression to a plain string: string literals, identifier defines and merge_strings(...) (arg
// concatenation) collapse to their text; numbers and unknown tokens contribute nothing.
std::string ResolveExpr(const std::string& expr, const std::map<std::string, std::string>& defs,
                        std::set<std::string>& stack, int depth);

std::string ResolveDefine(const std::string& name, const std::map<std::string, std::string>& defs,
                          std::set<std::string>& stack, int depth) {
    if (depth > kMaxIncludeDepth * 2 || stack.count(name)) return {};
    const auto it = defs.find(name);
    if (it == defs.end()) return {};
    stack.insert(name);
    std::string v = ResolveExpr(it->second, defs, stack, depth + 1);
    stack.erase(name);
    return v;
}

std::string ResolveExpr(const std::string& expr, const std::map<std::string, std::string>& defs,
                        std::set<std::string>& stack, int depth) {
    std::string out;
    size_t i = 0;
    const size_t n = expr.size();
    while (i < n) {
        const char c = expr[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == '"') {   // string literal
            ++i;
            while (i < n && expr[i] != '"') {
                if (expr[i] == '\\' && i + 1 < n) { out += expr[i + 1]; i += 2; continue; }
                out += expr[i++];
            }
            ++i;
            continue;
        }
        if (IsIdentStart(c)) {
            const size_t s = i;
            while (i < n && IsIdentChar(expr[i])) ++i;
            const std::string id = expr.substr(s, i - s);
            const std::string low = Lower(id);
            // a function-like token (merge_strings, ...): its arguments are handled by this same scan
            if (low == "merge_strings" || low == "concat" || low == "string") continue;
            out += ResolveDefine(id, defs, stack, depth);
            continue;
        }
        ++i;   // numbers, punctuation: nothing
    }
    return out;
}

struct TexDecl {
    std::string variable;
    std::string expr;
    std::filesystem::path dir;   // the file that declared it
    std::filesystem::path root;  // the first .fx's folder (MME resolves ResourceName against the effect)
};

// Scatter-parse one file: #define table, `texture NAME < ... ResourceName = EXPR ... >` declarations and #include.
void ParseFile(const std::filesystem::path& file, int depth, const std::filesystem::path& rootFxDir,
               std::map<std::string, std::string>& defs, std::vector<TexDecl>& texDecls,
               std::set<std::string>& visited, bool& followed) {
    if (depth > kMaxIncludeDepth) return;
    const std::string key = Lower(PathToUtf8(file));
    if (!visited.insert(key).second) return;   // cycle-safe
    std::string content;
    if (!ReadTextFile(file, content)) return;
    followed = true;

    const std::vector<std::string> lines = SplitLines(content);
    for (const std::string& line : lines) {
        const std::string t = Trim(line);
        if (t.size() < 8 || t[0] != '#') continue;
        std::string rest = Trim(t.substr(1));
        if (rest.size() < 6 || Lower(rest.substr(0, 6)) != "define") continue;
        rest = Trim(rest.substr(6));
        if (rest.empty() || !IsIdentStart(rest[0])) continue;
        size_t e = 0;
        while (e < rest.size() && IsIdentChar(rest[e])) ++e;
        const std::string name = rest.substr(0, e);
        const std::string value = Trim(rest.substr(e));
        if (!value.empty()) defs[name] = value;   // ignore #ifdef / #ifndef guards: collect everything
    }

    // texture declarations: `texture NAME < ... string ResourceName = EXPR ; ... >` (multi-line tolerant)
    size_t i = 0;
    const size_t n = content.size();
    std::string curVar;
    while (i < n) {
        const char c = content[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == '/' && i + 1 < n && content[i + 1] == '/') { while (i < n && content[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < n && content[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(content[i] == '*' && content[i + 1] == '/')) ++i;
            i = std::min(n, i + 2);
            continue;
        }
        if (c == '"') { ++i; while (i < n && content[i] != '"') { if (content[i] == '\\') ++i; ++i; } ++i; continue; }
        if (IsIdentStart(c)) {
            const size_t s = i;
            while (i < n && IsIdentChar(content[i])) ++i;
            const std::string id = content.substr(s, i - s);
            const std::string low = Lower(id);
            if (low.rfind("texture", 0) == 0) {   // texture / texture2D / ...
                size_t j = i;
                while (j < n && std::isspace((unsigned char)content[j])) ++j;
                if (j < n && IsIdentStart(content[j])) {
                    const size_t vs = j;
                    while (j < n && IsIdentChar(content[j])) ++j;
                    size_t k = j;
                    while (k < n && std::isspace((unsigned char)content[k])) ++k;
                    if (k < n && content[k] == '<') curVar = content.substr(vs, j - vs);
                }
            } else if (low == "resourcename") {
                size_t j = i;
                while (j < n && std::isspace((unsigned char)content[j])) ++j;
                if (j < n && content[j] == '=') {
                    ++j;
                    size_t end = content.find(';', j);
                    if (end == std::string::npos) end = n;
                    const std::string expr = Trim(content.substr(j, end - j));
                    if (!expr.empty()) texDecls.push_back({curVar, expr, file, rootFxDir});
                    i = end < n ? end + 1 : n;
                }
            }
            continue;
        }
        ++i;
    }

    // #include <x> / #include "x": relative to the including file, then to the first .fx's folder
    for (const std::string& line : lines) {
        const std::string t = Trim(line);
        if (t.size() < 8 || t[0] != '#') continue;
        std::string rest = Trim(t.substr(1));
        if (rest.size() < 7 || Lower(rest.substr(0, 7)) != "include") continue;
        rest = Trim(rest.substr(7));
        if (rest.empty()) continue;
        char open = rest[0], close = '>';
        if (open == '"') close = '"';
        else if (open != '<') continue;
        const size_t end = rest.find(close, 1);
        if (end == std::string::npos) continue;
        const std::string rel = rest.substr(1, end - 1);
        std::filesystem::path inc;
        if (ResolveRel(file.parent_path(), rel, inc) ||
            (!rootFxDir.empty() && ResolveRel(rootFxDir, rel, inc)))
            ParseFile(inc, depth + 1, rootFxDir, defs, texDecls, visited, followed);
    }
}

} // namespace

MmeHints ReadMmeHints(const std::filesystem::path& modelFile) {
    MmeHints hints;
    try {
        std::filesystem::path emd = modelFile;
        emd.replace_extension(L".emd");
        std::error_code ec;
        if (!std::filesystem::exists(emd, ec)) {
            // No same-stem .emd: a single .emd beside the model is its only effect sidecar (several = ambiguous).
            std::filesystem::path only;
            int count = 0;
            for (const auto& e : std::filesystem::directory_iterator(modelFile.parent_path(), ec)) {
                if (ec) break;
                std::wstring ext = e.path().extension().wstring();
                for (wchar_t& c : ext) c = (wchar_t)towlower(c);
                if (ext == L".emd" && e.is_regular_file(ec)) {
                    only = e.path();
                    ++count;
                }
            }
            if (count != 1) return hints;
            emd = only;
        }
        std::string content;
        if (!ReadTextFile(emd, content)) return hints;
        hints.found = true;

        // [Effect] Obj / Obj[n] entries -> the .fx files the model loads (paths relative to the .emd)
        bool inEffect = false;
        std::vector<std::filesystem::path> fxFiles;
        for (const std::string& line : SplitLines(content)) {
            const std::string t = Trim(line);
            if (t.empty()) continue;
            if (t[0] == '[') { inEffect = Lower(t).rfind("[effect]", 0) == 0; continue; }
            if (!inEffect) continue;
            if (Lower(t).rfind("obj", 0) != 0) continue;
            const size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            const std::string value = Trim(t.substr(eq + 1));
            if (value.empty() || Lower(value) == "none") continue;
            std::filesystem::path fx;
            if (ResolveRel(emd.parent_path(), value, fx)) fxFiles.push_back(fx);
        }
        if (fxFiles.empty()) return hints;

        const std::filesystem::path rootFxDir = fxFiles.front().parent_path();
        std::map<std::string, std::string> defs;
        std::vector<TexDecl> texDecls;
        std::set<std::string> visited;
        bool followed = false;
        for (const std::filesystem::path& fx : fxFiles)
            ParseFile(fx, 1, rootFxDir, defs, texDecls, visited, followed);

        for (const TexDecl& td : texDecls) {
            std::set<std::string> stack;
            const std::string path = ResolveExpr(td.expr, defs, stack, 0);
            if (path.empty()) continue;
            std::filesystem::path file;
            // ResourceName is an MME effect reference: relative to the declaring file, then to the effect folder
            if (!ResolveRel(td.dir.parent_path(), path, file) &&
                !(!td.root.empty() && ResolveRel(td.root, path, file)))
                continue;   // keep only files that exist
            hints.textures.push_back({td.variable, file});
        }
        LOG_INFO("mme: %s: %d fx, %d textures", PathToUtf8(emd.filename()).c_str(), (int)fxFiles.size(),
                 (int)hints.textures.size());
        hints.found = followed;
    } catch (...) {
        // never throw; degrade to found = false
        hints = MmeHints{};
    }
    return hints;
}

} // namespace mmdx
