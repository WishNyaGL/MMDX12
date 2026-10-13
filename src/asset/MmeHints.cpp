// MmeHints.cpp: follows a model's own MikuMikuEffect references (see MmeHints.h). A tolerant, line / token scatter
// parser: no full HLSL, only the .emd effect list, #include chains, #define literals and ResourceName textures.
#include "asset/MmeHints.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <set>

#include "asset/MmeText.h"
#include "core/Log.h"
#include "core/TextUtil.h"

namespace mmdx {
namespace {

using mmetext::IsIdentChar;
using mmetext::IsIdentStart;
using mmetext::Lower;
using mmetext::ReadTextFile;
using mmetext::ResolveRel;
using mmetext::SplitLines;
using mmetext::Trim;

constexpr int kMaxIncludeDepth = 8;

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

// ---- blend-hint classification (model's own .emd Obj[n] effects) ------------------------

// Strips // and /* */ comments (string literals kept) so tokens inside comments do not classify.
std::string StripComments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const char c = s[i];
        if (c == '"') {
            out += c; ++i;
            while (i < n && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < n) { out += s[i++]; out += s[i++]; continue; }
                out += s[i++];
            }
            if (i < n) out += s[i++];
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { while (i < n && s[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            i = std::min(n, i + 2);
            continue;
        }
        out += c; ++i;
    }
    return out;
}

// True when `keyword` is assigned the bare identifier `value` (case-insensitive, optional whitespace
// around '='). Used for `DestBlend = one` and `ZWriteEnable = false`.
bool AssignedTo(const std::string& code, const char* keyword, const char* value) {
    const std::string low = Lower(code);
    const std::string kw = Lower(keyword);
    const std::string val = Lower(value);
    size_t pos = 0;
    while ((pos = low.find(kw, pos)) != std::string::npos) {
        size_t j = pos + kw.size();
        if (j < low.size() && IsIdentChar(low[j])) { pos = j; continue; }   // not a full keyword match
        while (j < low.size() && std::isspace((unsigned char)low[j])) ++j;
        if (j < low.size() && low[j] == '=') {
            ++j;
            while (j < low.size() && std::isspace((unsigned char)low[j])) ++j;
            if (low.compare(j, val.size(), val) == 0) {
                const size_t k = j + val.size();
                if (k >= low.size() || !IsIdentChar(low[k])) return true;
            }
        }
        pos = j;
    }
    return false;
}

// #include targets of a file (comment-stripped), using the same resolution rules as ParseFile.
std::vector<std::string> ExtractIncludes(const std::string& code) {
    std::vector<std::string> out;
    for (const std::string& line : SplitLines(code)) {
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
        out.push_back(rest.substr(1, end - 1));
    }
    return out;
}

struct FxClass {
    bool additive = false;       // DestBlend = one, or the full_ex additive family (AddtiveAlphaColor / ReviseDestRate)
    bool noDepthWrite = false;   // ZWriteEnable = false in an object pass
};

// Classifies one .fx and its #include chain: additive if an object pass / #define state sets DestBlend = one
// (any spacing/case) or the file is the full_ex additive family; NoDepthWrite if ZWriteEnable = false appears.
void ClassifyFile(const std::filesystem::path& file, int depth, const std::filesystem::path& rootFxDir,
                  std::set<std::string>& visited, FxClass& out) {
    if (depth > kMaxIncludeDepth) return;
    const std::string key = Lower(PathToUtf8(file));
    if (!visited.insert(key).second) return;   // cycle-safe
    std::string content;
    if (!ReadTextFile(file, content)) return;
    const std::string code = StripComments(content);
    const std::string low = Lower(code);
    if (low.find("addtivealphacolor") != std::string::npos || low.find("revisedestrate") != std::string::npos)
        out.additive = true;
    if (AssignedTo(code, "destblend", "one")) out.additive = true;
    if (AssignedTo(code, "zwriteenable", "false")) out.noDepthWrite = true;
    for (const std::string& rel : ExtractIncludes(code)) {
        std::filesystem::path inc;
        if (ResolveRel(file.parent_path(), rel, inc) ||
            (!rootFxDir.empty() && ResolveRel(rootFxDir, rel, inc)))
            ClassifyFile(inc, depth + 1, rootFxDir, visited, out);
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
        // Obj[n] = <fx> (n = PMX material index): collected so each material's effect can be classified. An
        // explicit `Obj[n] = none` is recorded as an empty path (no hint) distinct from a gap (uses the default).
        std::map<int, std::filesystem::path> materialFx;
        std::filesystem::path defaultFx;
        for (const std::string& line : SplitLines(content)) {
            const std::string t = Trim(line);
            if (t.empty()) continue;
            if (t[0] == '[') { inEffect = Lower(t).rfind("[effect]", 0) == 0; continue; }
            if (!inEffect) continue;
            if (Lower(t).rfind("obj", 0) != 0) continue;
            const size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            // `Obj[n]` selects one PMX material; a dot after `Obj` (`Obj.show`) is not an entry.
            int objIndex = -1;
            size_t p = 3;
            while (p < t.size() && std::isspace((unsigned char)t[p])) ++p;
            if (p < t.size() && t[p] == '[') {
                size_t e = t.find(']', p + 1);
                if (e == std::string::npos) continue;
                try {
                    objIndex = std::stoi(Trim(t.substr(p + 1, e - p - 1)));
                } catch (...) {
                    continue;
                }
            } else if (p < t.size() && t[p] != '=') {
                continue;   // "Obj.show" and the like
            }
            const std::string value = Trim(t.substr(eq + 1));
            if (value.empty() || Lower(value) == "none") {
                if (objIndex >= 0) materialFx[objIndex] = std::filesystem::path();   // explicit none: no hint
                continue;
            }
            std::filesystem::path fx;
            if (!ResolveRel(emd.parent_path(), value, fx)) continue;
            if (objIndex >= 0) materialFx[objIndex] = fx;
            else defaultFx = fx;
            fxFiles.push_back(fx);
        }
        if (fxFiles.empty()) return hints;

        // Per-material blend hints: classify each distinct .fx (with its #include chain) once.
        std::map<std::string, FxClass> fxClasses;
        const auto classify = [&](const std::filesystem::path& fx) -> FxClass {
            const std::string key = Lower(PathToUtf8(fx));
            const auto cached = fxClasses.find(key);
            if (cached != fxClasses.end()) return cached->second;
            FxClass cls;
            std::set<std::string> cv;
            ClassifyFile(fx, 1, fx.parent_path(), cv, cls);
            fxClasses[key] = cls;
            return cls;
        };
        const auto hintOf = [](const FxClass& c) -> uint8_t {
            if (c.additive) return (uint8_t)2;
            if (c.noDepthWrite) return (uint8_t)1;
            return (uint8_t)0;
        };
        uint8_t defaultHint = 0;
        if (!defaultFx.empty()) defaultHint = hintOf(classify(defaultFx));
        hints.materialBlendDefault = defaultHint;
        if (!materialFx.empty()) {
            int maxIdx = 0;
            for (const auto& [idx, fx] : materialFx) maxIdx = std::max(maxIdx, idx);
            hints.materialBlend.assign((size_t)maxIdx + 1, defaultHint);   // gaps use the bare Obj default
            for (const auto& [idx, fx] : materialFx)
                if (idx >= 0) hints.materialBlend[(size_t)idx] = fx.empty() ? 0 : hintOf(classify(fx));
        }

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
