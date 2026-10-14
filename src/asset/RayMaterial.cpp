// RayMaterial.cpp: reads Ray-MMD material data from a model's .emd and the per-material .fx (see RayMaterial.h).
// A tolerant scatter parser: .emd -> .fx -> #include chain, a shallow preprocessor (#define / #include / simple
// #if over integer expressions) and a reader of the const float / float3 slot values. No rendering, no semantics.
#include "asset/RayMaterial.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "asset/MmeText.h"
#include "core/TextUtil.h"

#include <fstream>

namespace mmdx {
namespace {

using mmetext::IsIdentChar;
using mmetext::IsIdentStart;
using mmetext::Lower;
using mmetext::PathKey;
using mmetext::ReadTextFile;
using mmetext::ReadTextFileText;
using mmetext::ResolveRel;
using mmetext::SplitLines;
using mmetext::Trim;

constexpr int kMaxIncludeDepth = 8;
constexpr int kMaxExpandDepth = 64;

std::string StripComment(const std::string& s) {
    size_t p = s.find("//");
    return p == std::string::npos ? s : s.substr(0, p);
}

// ---- integer expression evaluator (for #if / #elif) --------------------------------------------
struct Tok {
    enum Kind { Ident, Number, Op } kind = Op;
    std::string text;
    long long value = 0;
};

void Tokenize(const std::string& s, std::vector<Tok>& out) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const char c = s[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (IsIdentStart(c)) {
            const size_t b = i;
            while (i < n && IsIdentChar(s[i])) ++i;
            out.push_back({Tok::Ident, s.substr(b, i - b), 0});
            continue;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))) {
            const size_t b = i;
            while (i < n && (std::isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == 'x' || s[i] == 'X')) ++i;
            const std::string num = s.substr(b, i - b);
            const bool isFloat = num.find('.') != std::string::npos;
            out.push_back({Tok::Number, num, isFloat ? (long long)strtod(num.c_str(), nullptr)
                                                       : strtoll(num.c_str(), nullptr, 0)});
            continue;
        }
        // multi-char operators
        const char d = (i + 1 < n) ? s[i + 1] : '\0';
        auto two = [&](char a, char b) { return c == a && d == b; };
        if (two('|', '|') || two('&', '&') || two('=', '=') || two('!', '=') || two('<', '=') || two('>', '=') ||
            two('<', '<') || two('>', '>')) {
            out.push_back({Tok::Op, std::string{c, d}, 0});
            i += 2;
            continue;
        }
        out.push_back({Tok::Op, std::string(1, c), 0});
        ++i;
    }
}

struct ExprEval {
    const std::vector<Tok>& t;
    size_t i = 0;
    bool ok = true;
    bool At(const char* op) const { return i < t.size() && t[i].kind == Tok::Op && t[i].text == op; }
    bool Ident(const char* name) const { return i < t.size() && t[i].kind == Tok::Ident && t[i].text == name; }
    long long ParseTernary();
    long long ParseBinary(int level);
    long long ParseUnary();
    long long ParsePrimary();
    long long Run() { return ParseTernary(); }
};

long long ExprEval::ParsePrimary() {
    if (i >= t.size()) { ok = false; return 0; }
    if (At("(")) {
        ++i;
        const long long v = ParseTernary();
        if (At(")")) ++i; else ok = false;
        return v;
    }
    if (t[i].kind == Tok::Number) return t[i++].value;
    if (t[i].kind == Tok::Ident) { ++i; return t[i - 1].text == "true" ? 1 : 0; }   // unknown identifiers -> 0
    ok = false;
    ++i;
    return 0;
}

long long ExprEval::ParseUnary() {
    if (At("!") || At("-") || At("~") || At("+")) {
        const std::string op = t[i++].text;
        const long long v = ParseUnary();
        if (op == "!") return !v;
        if (op == "-") return -v;
        if (op == "~") return ~v;
        return v;
    }
    return ParsePrimary();
}

// Binary levels by precedence (low -> high): ||, &&, |, ^, &, ==/!=, </<=/>/>=, <</>>, +/-, */ /%/.
long long ExprEval::ParseBinary(int level) {
    static const std::vector<std::vector<std::string>> kLevels = {
        {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<", "<=", ">", ">="}, {"<<", ">>"}, {"+", "-"},
        {"*", "/", "%"}};
    if (level >= (int)kLevels.size()) return ParseUnary();
    long long v = ParseBinary(level + 1);
    for (;;) {
        bool matched = false;
        for (const std::string& op : kLevels[level]) {
            if (At(op.c_str())) {
                ++i;
                const long long r = ParseBinary(level + 1);
                if (op == "||") v = (v || r);
                else if (op == "&&") v = (v && r);
                else if (op == "|") v = v | r;
                else if (op == "^") v = v ^ r;
                else if (op == "&") v = v & r;
                else if (op == "==") v = (v == r);
                else if (op == "!=") v = (v != r);
                else if (op == "<") v = (v < r);
                else if (op == "<=") v = (v <= r);
                else if (op == ">") v = (v > r);
                else if (op == ">=") v = (v >= r);
                else if (op == "<<") v = v << r;
                else if (op == ">>") v = v >> r;
                else if (op == "+") v = v + r;
                else if (op == "-") v = v - r;
                else if (op == "*") v = v * r;
                else if (op == "/") v = r ? v / r : 0;
                else if (op == "%") v = r ? v % r : 0;
                matched = true;
                break;
            }
        }
        if (!matched) break;
    }
    return v;
}

long long ExprEval::ParseTernary() {
    long long v = ParseBinary(0);
    if (At("?")) {
        ++i;
        const long long a = ParseTernary();
        if (At(":")) ++i; else ok = false;
        const long long b = ParseTernary();
        v = v ? a : b;
    }
    return v;
}

struct Macro {
    std::string value;
    std::filesystem::path declFile;
};

void ExpandTokens(const std::vector<Tok>& in, const std::map<std::string, Macro>& macros, std::vector<Tok>& out,
                  int depth, std::set<std::string>& stack) {
    for (const Tok& tk : in) {
        if (tk.kind == Tok::Ident && depth < kMaxExpandDepth && !stack.count(tk.text)) {
            const auto it = macros.find(tk.text);
            if (it != macros.end()) {
                std::vector<Tok> sub;
                Tokenize(it->second.value, sub);
                stack.insert(tk.text);
                ExpandTokens(sub, macros, out, depth + 1, stack);
                stack.erase(tk.text);
                continue;
            }
        }
        out.push_back(tk);
    }
}

// Evaluates a #if expression. `defined(X)` / `defined X` is resolved before macro expansion (C semantics).
bool EvalExpr(const std::string& expr, const std::map<std::string, Macro>& macros, long long& result) {
    std::vector<Tok> raw;
    Tokenize(expr, raw);
    std::vector<Tok> repl;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i].kind == Tok::Ident && raw[i].text == "defined") {
            bool paren = (i + 1 < raw.size() && raw[i + 1].text == "(");
            size_t j = paren ? i + 2 : i + 1;
            const bool has = (j < raw.size() && raw[j].kind == Tok::Ident) && macros.count(raw[j].text);
            repl.push_back({Tok::Number, "0", has ? 1 : 0});
            i = paren ? j + 1 : j;
            if (paren && i < raw.size() && raw[i].text == ")") {}
            continue;
        }
        repl.push_back(raw[i]);
    }
    std::vector<Tok> expanded;
    std::set<std::string> stack;
    ExpandTokens(repl, macros, expanded, 0, stack);
    ExprEval eval{expanded};
    result = eval.Run();
    return eval.ok;
}

// ---- shallow preprocessor over an #include chain -------------------------------------------------
struct ConstVal {
    int comps = 0;             // 0 unparsed, 1 scalar, 2 float2, 3 float3, 4 float4
    float v[4] = {0, 0, 0, 0};
};

struct ParsedFx {
    std::map<std::string, Macro> defines;
    std::map<std::string, ConstVal> consts;
    bool isRay = false;
    std::map<std::string, int> unknown;   // unknown / unparsed preprocessor directive -> count
};

// Extracts the RHS value of a const declaration: a number, floatN(a,b,c) or a define reference.
ConstVal EvalValueExpr(const std::string& expr, const std::map<std::string, Macro>& macros) {
    ConstVal cv;
    std::string e = Trim(StripComment(expr));
    if (e.empty()) return cv;
    // A single number / define reference.
    auto number = [](const std::string& s, float& out) -> bool {
        std::string t = Trim(s);
        while (!t.empty() && (t.back() == 'f' || t.back() == 'F' || t.back() == 'u' || t.back() == 'U' ||
                              t.back() == 'l' || t.back() == 'L'))
            t.pop_back();
        if (t.empty()) return false;
        char* end = nullptr;
        const double d = strtod(t.c_str(), &end);
        if (end == t.c_str() || (end && *end != '\0')) return false;
        out = (float)d;
        return true;
    };
    // float3(a,b,c) / float2(...) / ...
    const size_t paren = e.find('(');
    if (paren != std::string::npos && e.back() == ')') {
        const std::string fn = Lower(Trim(e.substr(0, paren)));
        if (fn == "float2" || fn == "float3" || fn == "float4" ||
            fn == "double2" || fn == "double3" || fn == "double4") {
            const int want = fn.back() - '0';
            std::vector<float> comps;
            std::string body = e.substr(paren + 1, e.size() - paren - 2);
            size_t start = 0;
            bool bad = false;
            while (start <= body.size()) {
                const size_t comma = body.find(',', start);
                const std::string part = Trim(body.substr(start, comma == std::string::npos ? std::string::npos
                                                                                          : comma - start));
                // A component may itself be a define reference.
                float f = 0;
                std::string p = part;
                if (!number(p, f)) {
                    auto it = macros.find(Trim(p));
                    if (it != macros.end() && number(it->second.value, f)) {
                    } else {
                        bad = true;
                    }
                }
                comps.push_back(f);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            if (!bad && (int)comps.size() == want) {
                cv.comps = want;
                for (int k = 0; k < want; ++k) cv.v[k] = comps[k];
                return cv;
            }
            cv.comps = 0;   // unknown function (SSS_SKIN_TRANSMITTANCE(...)): leave unparsed
            return cv;
        }
        return cv;   // unknown function call -> unparsed
    }
    float f = 0;
    if (number(e, f)) { cv.comps = 1; cv.v[0] = f; return cv; }
    auto it = macros.find(Trim(e));
    if (it != macros.end()) {
        if (number(it->second.value, f)) { cv.comps = 1; cv.v[0] = f; }
        else {
            ConstVal inner = EvalValueExpr(it->second.value, macros);
            if (inner.comps) return inner;
        }
    }
    return cv;
}

void ParseFile(const std::filesystem::path& file, int depth, const std::filesystem::path& rootDir,
               ParsedFx& out, std::set<std::string>& visited) {
    if (depth > kMaxIncludeDepth) return;
    const std::string key = PathKey(file);
    if (!visited.insert(key).second) return;   // cycle-safe
    std::string content;
    if (!ReadTextFile(file, content)) return;
    if (content.find("MaterialParam") != std::string::npos || content.find("EncodeGbuffer") != std::string::npos)
        out.isRay = true;

    // Conditional stack: active = the enclosing region is active; taken[level] = some branch already taken;
    // thisActive[level] = this branch is currently active.
    struct Frame { bool parentActive; bool taken; bool thisActive; };
    std::vector<Frame> cond;
    auto active = [&]() { return cond.empty() || (cond.back().parentActive && cond.back().thisActive); };

    // Join backslash-continued logical lines first (multi-line #if expressions are common in these files).
    std::vector<std::string> logical;
    {
        std::string cur;
        for (const std::string& raw : SplitLines(content)) {
            if (!raw.empty() && raw.back() == '\\') { cur += raw.substr(0, raw.size() - 1); continue; }
            cur += raw;
            logical.push_back(cur);
            cur.clear();
        }
        if (!cur.empty()) logical.push_back(cur);
    }
    for (const std::string& rawLine : logical) {
        const std::string line = Trim(rawLine);
        if (line.empty()) continue;
        if (line[0] == '#') {
            std::string rest = Trim(line.substr(1));
            // The directive name is the leading identifier; the rest (with or without a space) is the argument.
            size_t de = 0;
            while (de < rest.size() && IsIdentChar(rest[de])) ++de;
            const std::string dir = Lower(rest.substr(0, de));
            const std::string arg = Trim(rest.substr(de));
            if (dir == "if" || dir == "ifdef" || dir == "ifndef") {
                const bool parentActive = active();
                bool val = false;
                if (parentActive) {
                    if (dir == "if") {
                        long long r = 0;
                        if (EvalExpr(arg, out.defines, r)) val = r != 0;
                        else out.unknown["#if(complex)"]++;
                    } else {
                        const bool defined = out.defines.count(arg) != 0;
                        val = (dir == "ifdef") ? defined : !defined;
                    }
                }
                cond.push_back({parentActive, val, val});
                continue;
            }
            if (dir == "elif") {
                if (cond.empty()) { out.unknown["#elif(stray)"]++; continue; }
                Frame& f = cond.back();
                if (f.taken) { f.thisActive = false; }
                else {
                    bool val = false;
                    if (f.parentActive) {
                        long long r = 0;
                        if (EvalExpr(arg, out.defines, r)) val = r != 0;
                        else out.unknown["#elif(complex)"]++;
                    }
                    f.thisActive = val;
                    f.taken = f.taken || val;
                }
                continue;
            }
            if (dir == "else") {
                if (cond.empty()) { out.unknown["#else(stray)"]++; continue; }
                Frame& f = cond.back();
                f.thisActive = !f.taken;
                f.taken = true;
                continue;
            }
            if (dir == "endif") {
                if (cond.empty()) { out.unknown["#endif(stray)"]++; continue; }
                cond.pop_back();
                continue;
            }
            if (!active()) continue;
            if (dir == "define") {
                if (arg.empty() || !IsIdentStart(arg[0])) { out.unknown["#define"]++; continue; }
                size_t e = 0;
                while (e < arg.size() && IsIdentChar(arg[e])) ++e;
                const std::string name = arg.substr(0, e);
                if (e < arg.size() && arg[e] == '(') {   // function-like: kept opaque, not expanded
                    out.defines[name] = {Trim(arg.substr(e)), file};
                    continue;
                }
                out.defines[name] = {Trim(StripComment(arg.substr(e))), file};
                continue;
            }
            if (dir == "undef") {
                out.defines.erase(arg);
                continue;
            }
            if (dir == "include") {
                if (arg.empty()) { out.unknown["#include"]++; continue; }
                char open = arg[0], close = '>';
                if (open == '"') close = '"';
                else if (open != '<') { out.unknown["#include"]++; continue; }
                const size_t end = arg.find(close, 1);
                if (end == std::string::npos) { out.unknown["#include"]++; continue; }
                const std::string rel = arg.substr(1, end - 1);
                std::filesystem::path inc;
                if (ResolveRel(file.parent_path(), rel, inc) ||
                    (!rootDir.empty() && ResolveRel(rootDir, rel, inc)))
                    ParseFile(inc, depth + 1, rootDir, out, visited);
                else
                    out.unknown["#include(missing)"]++;
                continue;
            }
            if (dir == "pragma" || dir == "error" || dir == "line" || dir == "warning")
                continue;
            out.unknown["#" + dir]++;
            continue;
        }
        if (!active()) continue;

        // const <type> <name> = <expr>;  (the slot value declarations)
        std::vector<Tok> toks;
        Tokenize(line, toks);
        if (toks.size() >= 4 && toks[0].kind == Tok::Ident && toks[0].text == "const") {
            const std::string type = Lower(toks[1].text);
            if (toks[1].kind == Tok::Ident && type.rfind("float", 0) == 0 && toks[2].kind == Tok::Ident) {
                const std::string name = toks[2].text;
                const size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    size_t semi = line.find(';', eq);
                    const std::string rhs = line.substr(eq + 1, semi == std::string::npos ? std::string::npos
                                                                                          : semi - eq - 1);
                    ConstVal cv = EvalValueExpr(rhs, out.defines);
                    if (cv.comps) out.consts[name] = cv;
                }
            }
        }
    }
}

// Parsed-fx cache: parsing an .fx chain once per material is wasteful; validate with (size, mtime).
struct CacheEntry {
    ParsedFx parsed;
    uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    bool valid = false;
};
std::unordered_map<std::string, CacheEntry>& FxCache() {
    static std::unordered_map<std::string, CacheEntry> cache;
    return cache;
}

const ParsedFx* ParseFxCached(const std::filesystem::path& fx) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(fx, ec)) return nullptr;
    const std::string key = PathKey(fx);
    CacheEntry& e = FxCache()[key];
    const uintmax_t size = std::filesystem::file_size(fx, ec);
    const auto mtime = std::filesystem::last_write_time(fx, ec);
    if (!e.valid || e.size != size || e.mtime != mtime) {
        e.parsed = ParsedFx{};
        std::set<std::string> visited;
        ParseFile(fx, 1, fx.parent_path(), e.parsed, visited);
        e.size = size;
        e.mtime = mtime;
        e.valid = true;
    }
    return &e.parsed;
}

// ---- slot extraction ----------------------------------------------------------------------------
struct SlotInfo {
    RaySlot slot;
    const char* prefix;
    const char* valueName;   // const base for the scalar/vector value
};

const SlotInfo kSlots[] = {
    {RaySlot::Albedo, "ALBEDO", "albedo"},
    {RaySlot::AlbedoSub, "ALBEDO_SUB", "albedoSub"},
    {RaySlot::Alpha, "ALPHA", "alpha"},
    {RaySlot::Normal, "NORMAL", "normal"},
    {RaySlot::NormalSub, "NORMAL_SUB", "normalSub"},
    {RaySlot::Smoothness, "SMOOTHNESS", "smoothness"},
    {RaySlot::Metalness, "METALNESS", "metalness"},
    {RaySlot::Specular, "SPECULAR", "specular"},
    {RaySlot::Occlusion, "OCCLUSION", "occlusion"},
    {RaySlot::Parallax, "PARALLAX", "parallax"},
    {RaySlot::Emissive, "EMISSIVE", "emissive"},
    {RaySlot::CustomA, "CUSTOM_A", "customA"},
    {RaySlot::CustomB, "CUSTOM_B", "customB"},
};

// Resolves a #define to an integer. A value that names another define is followed (bounded), so
// `#define NORMAL_MAP_FROM ALBEDO_MAP_FROM` yields ALBEDO_MAP_FROM's number.
int IntDef(const ParsedFx& fx, const std::string& name, int def, bool* present = nullptr) {
    if (present) *present = false;
    const auto it = fx.defines.find(name);
    if (it == fx.defines.end()) return def;
    if (present) *present = true;
    const std::string value = Trim(it->second.value);
    const std::string low = Lower(value);
    if (low == "true") return 1;
    if (low == "false") return 0;
    char* end = nullptr;
    const long long n = strtoll(value.c_str(), &end, 0);
    if (end != value.c_str()) return (int)n;   // starts with a number
    if (IsIdentStart(value.empty() ? '\0' : value[0]) && fx.defines.count(value) && value != name)
        return IntDef(fx, value, def);         // a reference to another define
    return def;
}

// A #define string literal, unquoted (ALBEDO_MAP_FILE "albedo.png").
std::string StringDef(const ParsedFx& fx, const std::string& name) {
    const auto it = fx.defines.find(name);
    if (it == fx.defines.end()) return {};
    std::string v = Trim(it->second.value);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}

void ExtractSlots(const ParsedFx& fx, RayMaterialDesc& desc) {
    for (const SlotInfo& si : kSlots) {
        RayMapSlot& s = desc.slots[(int)si.slot];
        const std::string p = si.prefix;
        s.from = IntDef(fx, p + "_MAP_FROM", 0);
        s.type = IntDef(fx, p + "_MAP_TYPE", 0);
        s.swizzle = IntDef(fx, p + "_MAP_SWIZZLE", 0);
        s.uvFlip = IntDef(fx, p + "_MAP_UV_FLIP", 0);
        s.applyScale = IntDef(fx, p + "_MAP_APPLY_SCALE", 0);
        s.colorFlip = IntDef(fx, p + "_MAP_COLOR_FLIP", 0);
        s.applyMorphColor = IntDef(fx, p + "_MAP_APPLY_MORPH_COLOR", 0);
        s.applyDiffuse = IntDef(fx, p + "_MAP_APPLY_DIFFUSE", 0);
        s.applyMorphIntensity = IntDef(fx, p + "_MAP_APPLY_MORPH_INTENSITY", 0);
        s.applyBlink = IntDef(fx, p + "_MAP_APPLY_BLINK", 0);
        if (IntDef(fx, p + "_ENABLE", 0, &s.hasEnable)) s.enabled = IntDef(fx, p + "_ENABLE", 0) != 0;

        const std::string fileDef = StringDef(fx, p + "_MAP_FILE");
        s.fileDeclared = !fileDef.empty();
        if (s.fileDeclared && (s.from == 1 || s.from == 2)) {
            // Relative to the file that declared the macro, else the root .fx folder.
            std::filesystem::path base;
            const auto it = fx.defines.find(p + "_MAP_FILE");
            if (it != fx.defines.end() && !it->second.declFile.empty()) base = it->second.declFile.parent_path();
            std::filesystem::path resolved;
            if (!base.empty() && ResolveRel(base, fileDef, resolved)) s.file = resolved;
            else if (ResolveRel(desc.fx.empty() ? std::filesystem::path() : desc.fx.parent_path(), fileDef, resolved))
                s.file = resolved;
            else s.fileMissing = true;
        }

        // MAP_UV_SCALE define (may be scalar or float2)
        if (fx.defines.count(p + "_MAP_UV_SCALE")) {
            ConstVal uv = EvalValueExpr(fx.defines.at(p + "_MAP_UV_SCALE").value, fx.defines);
            if (uv.comps) {
                s.hasUvScale = true;
                s.uvScaleU = uv.v[0];
                s.uvScaleV = uv.comps >= 2 ? uv.v[1] : uv.v[0];
            }
        }
        // const <base>MapLoopNum (texture repeat)
        const auto loopIt = fx.consts.find(std::string(si.valueName) + "MapLoopNum");
        if (loopIt != fx.consts.end() && loopIt->second.comps) {
            s.loopU = loopIt->second.v[0];
            s.loopV = loopIt->second.comps >= 2 ? loopIt->second.v[1] : loopIt->second.v[0];
        }
        // a per-slot scale constant (normalMapScale / parallaxMapScale ...)
        const auto scaleIt = fx.consts.find(std::string(si.valueName) + "MapScale");
        if (scaleIt != fx.consts.end() && scaleIt->second.comps == 1) {
            s.hasScale = true;
            s.scale = scaleIt->second.v[0];
        }
        // the slot's own const value
        const auto valIt = fx.consts.find(si.valueName);
        if (valIt != fx.consts.end() && valIt->second.comps) {
            s.hasValue = true;
            s.valueComponents = valIt->second.comps;
            for (int k = 0; k < valIt->second.comps && k < 3; ++k) s.value[k] = valIt->second.v[k];
            // HLSL broadcasts a scalar initialiser (`const float3 albedo = 1.0;` is 1,1,1)
            if (valIt->second.comps == 1) s.value[1] = s.value[2] = s.value[0];
        }
    }
    desc.shadingModel = IntDef(fx, "CUSTOM_ENABLE", 0);
}

bool HasRayMacro(const ParsedFx& fx) {
    for (const auto& [name, macro] : fx.defines) {
        if (name == "CUSTOM_ENABLE") return true;
        if (name.size() > 9 && name.compare(name.size() - 9, 9, "_MAP_FROM") == 0) return true;
    }
    return false;
}

// ---- .emd ---------------------------------------------------------------------------------------
struct EmdEntry {
    int index = -1;   // -1 = the default Obj
    std::string value;
};

bool ReadEmdObj(const std::filesystem::path& emd, std::vector<EmdEntry>& out) {
    std::string content;
    if (!ReadTextFileText(emd, content)) return false;
    bool inEffect = false;
    for (const std::string& rawLine : SplitLines(content)) {
        const std::string t = Trim(rawLine);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t[0] == '[') { inEffect = Lower(t).rfind("[effect]", 0) == 0; continue; }
        if (!inEffect) continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(t.substr(0, eq));
        const std::string value = Trim(StripComment(t.substr(eq + 1)));
        if (Lower(key).rfind("obj", 0) != 0) continue;
        int index = -1;
        const size_t br = key.find('[');
        if (br != std::string::npos) {
            const size_t end = key.find(']', br);
            if (end != std::string::npos) {
                const std::string num = Trim(key.substr(br + 1, end - br - 1));
                if (!num.empty() && num.find_first_not_of("0123456789") == std::string::npos)
                    index = atoi(num.c_str());
            }
        } else if (key.size() > 3) {
            continue;   // "Obj.show" and friends: not an assignment
        }
        out.push_back({index, value});   // explicit "none" is kept: it overrides the default Obj
    }
    return true;
}

// Cache of ParseRayEmd by (emd, materialCount) is unnecessary: the probe parses each chosen .emd once.

} // namespace

const char* RaySlotName(RaySlot slot) {
    switch (slot) {
    case RaySlot::Albedo: return "ALBEDO";
    case RaySlot::AlbedoSub: return "ALBEDO_SUB";
    case RaySlot::Alpha: return "ALPHA";
    case RaySlot::Normal: return "NORMAL";
    case RaySlot::NormalSub: return "NORMAL_SUB";
    case RaySlot::Smoothness: return "SMOOTHNESS";
    case RaySlot::Metalness: return "METALNESS";
    case RaySlot::Specular: return "SPECULAR";
    case RaySlot::Occlusion: return "OCCLUSION";
    case RaySlot::Parallax: return "PARALLAX";
    case RaySlot::Emissive: return "EMISSIVE";
    case RaySlot::CustomA: return "CUSTOM_A";
    case RaySlot::CustomB: return "CUSTOM_B";
    default: return "?";
    }
}

bool IsRayMaterialFx(const std::filesystem::path& fx) {
    const ParsedFx* p = ParseFxCached(fx);
    if (!p) return false;
    return p->isRay || HasRayMacro(*p);
}

bool IsRayMmdMainEffect(const std::filesystem::path& p) {
    // The markers are ASCII, so the raw bytes answer both encodings; no preprocessing, no include chase.
    const auto lowerWide = [](std::wstring s) {
        for (wchar_t& c : s) c = (wchar_t)towlower(c);
        return s;
    };
    std::filesystem::path fx = p;
    if (lowerWide(p.extension().wstring()) == L".x") {
        // A .x is ranked by its same-stem .fx in the same folder (case-insensitive stem match).
        const std::wstring stem = lowerWide(p.stem().wstring());
        std::error_code ec;
        if (!std::filesystem::is_directory(p.parent_path(), ec)) return false;
        for (const std::filesystem::directory_entry& e :
             std::filesystem::directory_iterator(p.parent_path(), std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec) || lowerWide(e.path().extension().wstring()) != L".fx") continue;
            if (lowerWide(e.path().stem().wstring()) == stem) {
                fx = e.path();
                break;
            }
        }
        if (lowerWide(fx.extension().wstring()) != L".fx") return false;
    }
    std::ifstream in(fx, std::ios::binary);
    if (!in) return false;
    std::string text(64 * 1024, '\0');
    in.read(text.data(), (std::streamsize)text.size());
    text.resize((size_t)in.gcount());
    // "#include ray.conf" (any quoting, case-insensitive file name) or an MME object bound to ray_controller.pmx.
    const std::string lower = Lower(text);
    size_t at = lower.find("ray.conf");
    while (at != std::string::npos) {
        const size_t lineStart = lower.rfind('\n', at) == std::string::npos ? 0 : lower.rfind('\n', at) + 1;
        const std::string line = lower.substr(lineStart, lower.find('\n', lineStart) - lineStart);
        if (line.find("#include") != std::string::npos) return true;
        at = lower.find("ray.conf", at + 1);
    }
    return lower.find("ray_controller.pmx") != std::string::npos;
}

std::vector<RayEmdCandidate> FindRayEmdCandidates(const std::filesystem::path& modelFile, int pmxMaterialCount,
                                                  int maxDepth) {
    std::vector<RayEmdCandidate> result;
    std::error_code ec;
    const std::filesystem::path dir = modelFile.parent_path();
    if (!std::filesystem::is_directory(dir, ec)) return result;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::wstring ext = it->path().extension().wstring();
        for (wchar_t& c : ext) c = (wchar_t)towlower(c);
        if (ext != L".emd") continue;
        // depth == levels below `dir` (0 = dir itself); the walk is bounded.
        if (maxDepth >= 0 && it.depth() > maxDepth) {
            it.disable_recursion_pending();
            continue;
        }
        RayEmdCandidate cand;
        cand.path = it->path();
        std::vector<EmdEntry> entries;
        if (!ReadEmdObj(cand.path, entries)) {
            cand.readFailed = true;
            result.push_back(cand);
            continue;
        }
        std::set<std::wstring> rayFx;
        for (const EmdEntry& e : entries) {
            ++cand.objCount;
            if (e.index >= 0 && pmxMaterialCount > 0 && e.index < pmxMaterialCount) ++cand.inRangeEntries;
            std::filesystem::path fx;
            if (ResolveRel(cand.path.parent_path(), e.value, fx) && IsRayMaterialFx(fx)) {
                ++cand.rayEntries;
                rayFx.insert(fx.lexically_normal().wstring());
            }
        }
        cand.rayFxDistinct = (int)rayFx.size();
        cand.raySharePct = cand.objCount ? cand.rayEntries * 100 / cand.objCount : 0;
        result.push_back(cand);
    }
    std::sort(result.begin(), result.end(), [](const RayEmdCandidate& a, const RayEmdCandidate& b) {
        // A mostly-Ray .emd first; among those, the one with more distinct material .fx carries more authored
        // per-material data (a model often ships a plain one-.fx-for-all Ray .emd next to its mapped one).
        const bool aRay = a.raySharePct >= 50, bRay = b.raySharePct >= 50;
        if (aRay != bRay) return aRay;
        if (a.rayFxDistinct != b.rayFxDistinct) return a.rayFxDistinct > b.rayFxDistinct;
        if (a.raySharePct != b.raySharePct) return a.raySharePct > b.raySharePct;
        if (a.rayEntries != b.rayEntries) return a.rayEntries > b.rayEntries;
        if (a.inRangeEntries != b.inRangeEntries) return a.inRangeEntries > b.inRangeEntries;
        return PathToUtf8(a.path) < PathToUtf8(b.path);
    });
    return result;
}

bool ParseRayEmd(const std::filesystem::path& emd, int pmxMaterialCount, std::vector<RayMaterialDesc>& out,
                 RayParseStats* stats, std::string* error) {
    out.assign(pmxMaterialCount > 0 ? (size_t)pmxMaterialCount : 0, RayMaterialDesc{});
    std::vector<EmdEntry> entries;
    if (!ReadEmdObj(emd, entries)) {
        if (error) *error = "cannot read " + PathToUtf8(emd);
        return false;
    }
    if (stats) ++stats->emdRead;
    std::map<int, std::string> byIndex;   // index -> value
    std::string def;
    for (const EmdEntry& e : entries) {
        if (e.index >= 0) byIndex[e.index] = e.value;
        else if (def.empty()) def = e.value;
    }
    std::set<std::string> seenFx;   // for distinct fx statistics
    for (int i = 0; i < pmxMaterialCount; ++i) {
        auto it = byIndex.find(i);
        const std::string value = it != byIndex.end() ? it->second : def;
        if (value.empty()) continue;
        std::filesystem::path fx;
        if (!ResolveRel(emd.parent_path(), value, fx)) continue;
        const ParsedFx* parsed = ParseFxCached(fx);
        const std::string fxKey = PathKey(fx);
        const bool distinct = seenFx.insert(fxKey).second;
        if (!parsed) {
            if (stats && distinct) {
                ++stats->fxFailed;
                stats->failedFx.push_back(fx);
            }
            continue;
        }
        const bool ray = parsed->isRay || HasRayMacro(*parsed);
        if (stats && distinct) {
            ++stats->fxRead;
            stats->parsedFx.push_back(fx);
            if (ray) {
                ++stats->fxRay;
                stats->rayFx.push_back(fx);
            }
            for (const auto& [name, count] : parsed->unknown) stats->unknownDirectives[name] += count;
        }
        RayMaterialDesc& desc = out[i];
        desc.valid = true;
        desc.ray = ray;
        desc.fx = fx;
        if (!ray) continue;   // non-Ray fx: keep valid (found) but no slot data
        ExtractSlots(*parsed, desc);
        for (const auto& [name, macro] : parsed->defines) desc.defines[name] = macro.value;
        if (stats && distinct) {
            for (int k = 0; k < kRaySlotCount; ++k)
                if (desc.slots[k].fileMissing) ++stats->missingFiles;
        }
    }
    return true;
}

bool FindAndParseRayMaterials(const std::filesystem::path& modelFile, int pmxMaterialCount,
                              std::vector<RayMaterialDesc>& out, std::filesystem::path* chosenEmd,
                              RayParseStats* stats, std::string* error) {
    out.clear();
    const std::vector<RayEmdCandidate> cands = FindRayEmdCandidates(modelFile, pmxMaterialCount);
    const RayEmdCandidate* best = nullptr;
    for (const RayEmdCandidate& c : cands) {
        if (c.readFailed || c.rayEntries == 0) continue;
        best = &c;
        break;   // candidates are sorted best-first
    }
    if (!best) {
        out.assign(pmxMaterialCount > 0 ? (size_t)pmxMaterialCount : 0, RayMaterialDesc{});
        if (chosenEmd) chosenEmd->clear();
        return false;
    }
    if (chosenEmd) *chosenEmd = best->path;
    return ParseRayEmd(best->path, pmxMaterialCount, out, stats, error);
}

} // namespace mmdx
