// MmeEffect.cpp: parses an MME .fx into the IR of MmeEffect.h. The parser runs on the preprocessed text (so
// macro-generated techniques and #if branches are already resolved) with per-line origins, so every diagnostic and
// every IR line points at the original file. It is tolerant: an unexpected token sequence logs a warning and skips
// to the next ';' at depth 0, never aborting the whole file.
#include "asset/MmeEffect.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

#include "asset/MmePreprocessor.h"
#include "asset/MmeText.h"
#include "core/TextUtil.h"

namespace mmdx {
namespace {

using mmetext::IsIdentChar;
using mmetext::Lower;
using mmetext::Trim;

// ---- tokens ----------------------------------------------------------------------------------------------

enum class Tk { Id, Num, Str, Punct };

struct Tok {
    Tk k = Tk::Punct;
    std::string t;
    int line = 0;   // index into the preprocessor's origin array
};

bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::vector<Tok> Tokenize(const std::string& s) {
    std::vector<Tok> out;
    const size_t n = s.size();
    size_t i = 0;
    int line = 0;
    while (i < n) {
        const char c = s[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (IsWs(c)) { ++i; continue; }
        if (c == '"') {
            const size_t b = i++;
            while (i < n && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < n) i += 2;
                else ++i;
            }
            if (i < n) ++i;
            out.push_back({Tk::Str, s.substr(b, i - b), line});
            continue;
        }
        if (mmetext::IsIdentStart(c)) {
            const size_t b = i;
            while (i < n && IsIdentChar(s[i])) ++i;
            out.push_back({Tk::Id, s.substr(b, i - b), line});
            continue;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))) {
            const size_t b = i;
            while (i < n && (std::isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '_')) ++i;
            out.push_back({Tk::Num, s.substr(b, i - b), line});
            continue;
        }
        out.push_back({Tk::Punct, std::string(1, c), line});
        ++i;
    }
    return out;
}

bool IsPunct(const Tok& t, const char* p) { return t.k == Tk::Punct && t.t == p; }
bool IsId(const Tok& t, const char* s) { return t.k == Tk::Id && t.t == s; }

std::string Unquote(const std::string& s) {
    if (s.size() < 2 || s.front() != '"') return s;
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() - 1) { out += s[++i]; continue; }
        out += s[i];
    }
    return out;
}

// ---- value / annotation parsing --------------------------------------------------------------------------

bool LooksNumeric(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') ++i;
    bool digit = false, dot = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (std::isdigit((unsigned char)c)) { digit = true; continue; }
        if (c == '.' && !dot) { dot = true; continue; }
        if ((c == 'x' || c == 'X') && digit) continue;
        if (std::isxdigit((unsigned char)c)) continue;   // hex digits / exponent letters
        return false;
    }
    return digit;
}

std::string TrimTokens(const std::vector<Tok>& ts, size_t from, size_t to) {
    std::string s;
    for (size_t i = from; i < to && i < ts.size(); ++i) {
        // insert a space only where the source had one between identifiers/numbers
        if (!s.empty()) {
            const Tok& p = ts[i - 1];
            const Tok& c = ts[i];
            const bool wordish = (p.k == Tk::Id || p.k == Tk::Num) && (c.k == Tk::Id || c.k == Tk::Num);
            if (wordish) s += ' ';
        }
        s += ts[i].t;
    }
    return Trim(s);
}

// ---- statement scanner over the token stream -------------------------------------------------------------

struct Parser {
    const std::vector<Tok>& toks;
    const std::vector<LineOrigin>& origins;
    std::filesystem::path root;
    std::vector<MmeDiag> diags;
    MmeEffectDesc desc;

    explicit Parser(const std::vector<Tok>& t, const std::vector<LineOrigin>& o, std::filesystem::path r)
        : toks(t), origins(o), root(std::move(r)) {}

    void Diag(MmeDiag::Severity sev, int lineIdx, std::string msg) {
        std::filesystem::path f = root;
        int line = 0;
        if (lineIdx >= 0 && lineIdx < (int)origins.size()) { f = origins[(size_t)lineIdx].file; line = origins[(size_t)lineIdx].line; }
        diags.push_back({sev, f, line, std::move(msg)});
    }

    // Skips a balanced brace block starting at the '{' at index `i` (which must be a '{'); returns index past the
    // matching '}'.
    size_t SkipBraces(size_t i) {
        int depth = 0;
        for (; i < toks.size(); ++i) {
            if (IsPunct(toks[i], "{")) ++depth;
            else if (IsPunct(toks[i], "}")) { --depth; if (depth <= 0) return i + 1; }
        }
        return toks.size();
    }

    // Skips a struct/class/cbuffer declaration: its brace block then the trailing ';' (if any).
    size_t SkipStructDecl(size_t i) {
        for (; i < toks.size(); ++i) {
            if (IsPunct(toks[i], "{")) { i = SkipBraces(i); break; }
            if (IsPunct(toks[i], ";")) return i + 1;
        }
        while (i < toks.size() && !IsPunct(toks[i], ";")) ++i;
        return i < toks.size() ? i + 1 : i;
    }

    void Run() {
        size_t i = 0;
        const size_t n = toks.size();
        while (i < n) {
            const Tok& t = toks[i];
            if (IsPunct(t, ";") || IsPunct(t, "}") || IsPunct(t, "{")) { ++i; continue; }
            if (t.k == Tk::Id) {
                const std::string lo = Lower(t.t);
                if (lo == "struct" || lo == "class" || lo == "cbuffer" || lo == "namespace" || lo == "interface" ||
                    lo == "typedef") {
                    i = SkipStructDecl(i);
                    continue;
                }
                if (lo == "technique" || lo == "technique10" || lo == "technique11") {
                    i = ParseTechnique(i);
                    continue;
                }
                if (lo.rfind("sampler", 0) == 0) {
                    i = ParseSamplerDecl(i);
                    continue;
                }
            }
            i = ParseGlobalDecl(i);
        }
    }

    // True when `decl` (up to index `at`) reads as a function signature: a matched '()' set whose '(' immediately
    // follows the declarator name, with no top-level '=' before it.
    bool LooksLikeFunctionSig(size_t begin, size_t at) {
        int paren = 0;
        bool sawParen = false;
        for (size_t i = begin; i < at; ++i) {
            if (IsPunct(toks[i], "(")) { if (paren == 0) sawParen = true; ++paren; }
            else if (IsPunct(toks[i], ")")) --paren;
            else if (IsPunct(toks[i], "=") && paren == 0) return false;
        }
        return sawParen && paren == 0;
    }

    // Parses one global declaration (possibly several comma-separated declarators). Returns the index past the ';'.
    //
    // Two passes over the token range bound the statement. The first treats `<...>` as an annotation block, but
    // ONLY counts `<`/`>` at the declarator's top level (paren/bracket/brace == 0): a `<`/`>` used as a comparison
    // operator inside a cast, a parenthesised expression or a ternary (`=(a < 0) ? 1.0 : 2.0;`) must not be read as
    // an annotation bracket, or the whole rest of the file is swallowed (the AutoLuminous4 RENDERCOLORTARGET bug).
    // A raw comparison initialiser that is not parenthesised (`static float b = a < 1;`) still leaves angle depth
    // dangling after pass 1, so pass 2 re-scans ignoring `<`/`>` entirely and finds the terminating ';'.
    size_t ParseGlobalDecl(size_t i) {
        const size_t begin = i;
        size_t end = toks.size();   // index of the terminating ';'
        bool found = false;
        for (int pass = 0; pass < 2 && !found; ++pass) {
            const bool trackAngle = pass == 0;
            int paren = 0, bracket = 0, brace = 0, angle = 0;
            for (i = begin; i < toks.size(); ++i) {
                const Tok& t = toks[i];
                if (t.k != Tk::Punct) continue;
                const bool topLevel = paren == 0 && bracket == 0 && brace == 0;
                if (t.t == "(") ++paren;
                else if (t.t == ")") --paren;
                else if (t.t == "[") ++bracket;
                else if (t.t == "]") --bracket;
                else if (t.t == "<") { if (trackAngle && topLevel) ++angle; }
                else if (t.t == ">") { if (angle > 0 && topLevel) --angle; }
                else if (t.t == "{") {
                    if (topLevel && angle == 0 && LooksLikeFunctionSig(begin, i)) {
                        // a function definition: skip its body (and any trailing ';') and finish here
                        size_t j = SkipBraces(i);
                        if (j < toks.size() && IsPunct(toks[j], ";")) ++j;
                        return j;
                    }
                    ++brace;
                } else if (t.t == "}") { if (brace > 0) --brace; }
                else if (t.t == ";" && topLevel && angle == 0) { end = i; found = true; break; }
            }
        }
        const size_t stop = found ? i : toks.size();
        if (stop <= begin) return stop + 1;
        ParseDeclarators(begin, found ? end : stop);
        return found ? i + 1 : i;
    }

    // Splits `[begin, end)` into [storage] type name [declarator]* and emits an MmeParam per declarator.
    void ParseDeclarators(size_t begin, size_t end) {
        size_t p = begin;
        bool isStatic = false, isShared = false, isUniform = false, isConst = false;
        bool rowMajor = false, columnMajor = false;
        for (; p < end; ++p) {
            if (toks[p].k != Tk::Id) break;
            const std::string lo = Lower(toks[p].t);
            if (lo == "static") isStatic = true;
            else if (lo == "shared") isShared = true;
            else if (lo == "uniform") isUniform = true;
            else if (lo == "const") isConst = true;
            // type modifiers (HLSL `row_major` / `column_major`); they may precede or follow the base type
            else if (lo == "row_major") rowMajor = true;
            else if (lo == "column_major") columnMajor = true;
            else break;
        }
        if (p >= end || toks[p].k != Tk::Id) {
            Diag(MmeDiag::Warning, begin < toks.size() ? toks[begin].line : -1, "unrecognised global declaration");
            return;
        }
        const std::string type = Lower(toks[p].t);
        const int declLine = toks[p].line;
        ++p;
        // modifiers that follow the base type (`float4x4 row_major M;`)
        while (p < end && toks[p].k == Tk::Id) {
            const std::string lo = Lower(toks[p].t);
            if (lo == "row_major") { rowMajor = true; ++p; }
            else if (lo == "column_major") { columnMajor = true; ++p; }
            else break;
        }
        // declarators
        while (p < end) {
            while (p < end && IsPunct(toks[p], ",")) ++p;
            if (p >= end) break;
            if (toks[p].k != Tk::Id) break;
            MmeParam param;
            param.type = type;
            param.name = toks[p].t;
            param.isStatic = isStatic;
            param.isShared = isShared;
            param.isUniform = isUniform;
            param.isConst = isConst;
            param.rowMajor = rowMajor;
            param.columnMajor = columnMajor;
            param.line = 0;
            param.file = root;
            if (declLine >= 0 && declLine < (int)origins.size()) {
                param.line = origins[(size_t)declLine].line;
                param.file = origins[(size_t)declLine].file;
            }
            ++p;
            // array size
            if (p < end && IsPunct(toks[p], "[")) {
                ++p;
                std::string num;
                while (p < end && !IsPunct(toks[p], "]")) { num += toks[p].t; ++p; }
                if (p < end) ++p;
                try { param.arraySize = std::stoi(Trim(num)); } catch (...) { param.arraySize = 0; }
            }
            // ': SEMANTIC' / ': register(x)'
            while (p < end && IsPunct(toks[p], ":")) {
                ++p;
                if (p < end && toks[p].k == Tk::Id) {
                    const std::string id = toks[p].t;
                    const std::string idLo = Lower(id);
                    ++p;
                    if (idLo == "register" && p < end && IsPunct(toks[p], "(")) {
                        ++p;
                        std::string reg;
                        while (p < end && !IsPunct(toks[p], ")")) { reg += toks[p].t; ++p; }
                        if (p < end) ++p;
                        param.semantic = param.semantic;   // register is not a semantic
                        (void)reg;
                    } else {
                        param.semantic = UpperAscii(id);
                    }
                }
            }
            // annotations (`< ... >`), used by texture/target declarations too
            if (p < end && IsPunct(toks[p], "<")) p = ParseAnnotationBlock(p, param.annotations);
            // initializer
            if (p < end && IsPunct(toks[p], "=")) {
                ++p;
                param.hasInit = true;
                p = ParseInitializer(p, end, param.init);
            }
            desc.params.push_back(std::move(param));
            if (p < end && IsPunct(toks[p], ",")) continue;
            break;
        }
    }

    static std::string UpperAscii(std::string s) {
        for (char& c : s)
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        return s;
    }

    // Parses an annotation list starting at '<'; returns the index past '>'.
    size_t ParseAnnotationBlock(size_t i, std::vector<MmeAnnotation>& out) {
        if (!IsPunct(toks[i], "<")) return i;
        ++i;
        while (i < toks.size() && !IsPunct(toks[i], ">")) {
            if (IsPunct(toks[i], ";")) { ++i; continue; }
            if (toks[i].k != Tk::Id) { ++i; continue; }
            MmeAnnotation a;
            a.type = Lower(toks[i].t);
            ++i;
            if (i >= toks.size() || toks[i].k != Tk::Id) {
                Diag(MmeDiag::Warning, i < toks.size() ? toks[i].line : -1, "annotation without a name");
                while (i < toks.size() && !IsPunct(toks[i], ";") && !IsPunct(toks[i], ">")) ++i;
                continue;
            }
            a.name = toks[i].t;
            ++i;
            if (i < toks.size() && IsPunct(toks[i], "=")) {
                ++i;
                i = ParseValue(i, a.value);
            }
            out.push_back(std::move(a));
            while (i < toks.size() && !IsPunct(toks[i], ";") && !IsPunct(toks[i], ">")) ++i;
        }
        if (i < toks.size() && IsPunct(toks[i], ">")) ++i;
        return i;
    }

    // Parses a value at `i`: string (with adjacent concatenation), number, true/false, {..}, floatN(..), identifier
    // or a raw expression fallback. Returns the index past the value.
    size_t ParseValue(size_t i, MmeValue& v) {
        if (i >= toks.size()) { v.kind = MmeValue::String; return i; }
        const Tok& t = toks[i];
        if (t.k == Tk::Str) {
            std::string s;
            while (i < toks.size() && toks[i].k == Tk::Str) { s += Unquote(toks[i].t); ++i; }
            v.kind = MmeValue::String;
            v.str = std::move(s);
            return i;
        }
        if (t.k == Tk::Num) {
            v.kind = (t.t.find('.') != std::string::npos) ? MmeValue::Float : MmeValue::Int;
            try { v.nums.push_back(std::stof(t.t)); } catch (...) { v.nums.push_back(0.0f); }
            ++i;
            return i;
        }
        if (t.k == Tk::Id) {
            const std::string lo = Lower(t.t);
            if (lo == "true" || lo == "false") {
                v.kind = MmeValue::Bool;
                v.nums.push_back(lo == "true" ? 1.0f : 0.0f);
                ++i;
                return i;
            }
            if (i + 1 < toks.size() && IsPunct(toks[i + 1], "(")) {
                // float3(...) / int2(...) constructor
                size_t j = i + 2;
                std::vector<Tok> inner;
                int paren = 1;
                for (; j < toks.size(); ++j) {
                    if (IsPunct(toks[j], "(")) ++paren;
                    else if (IsPunct(toks[j], ")")) { if (--paren == 0) { ++j; break; } }
                    inner.push_back(toks[j]);
                }
                v = NumericList(inner, lo.find("int") == 0 ? MmeValue::Int : MmeValue::Float);
                return j;
            }
            if (i + 1 < toks.size() && IsPunct(toks[i + 1], "{")) {
                size_t j = i + 2;
                std::vector<Tok> inner;
                int brace = 1;
                for (; j < toks.size(); ++j) {
                    if (IsPunct(toks[j], "{")) ++brace;
                    else if (IsPunct(toks[j], "}")) { if (--brace == 0) { ++j; break; } }
                    inner.push_back(toks[j]);
                }
                v = NumericList(inner, MmeValue::Float);
                return j;
            }
            // a bare identifier value: treat as string
            v.kind = MmeValue::String;
            v.str = t.t;
            ++i;
            return i;
        }
        if (IsPunct(t, "{")) {
            size_t j = i + 1;
            std::vector<Tok> inner;
            int brace = 1;
            for (; j < toks.size(); ++j) {
                if (IsPunct(toks[j], "{")) ++brace;
                else if (IsPunct(toks[j], "}")) { if (--brace == 0) { ++j; break; } }
                inner.push_back(toks[j]);
            }
            v = NumericList(inner, MmeValue::Float);
            return j;
        }
        if (IsPunct(t, "-") || IsPunct(t, "+")) {
            v.kind = MmeValue::Float;
            bool neg = IsPunct(t, "-");
            if (i + 1 < toks.size() && toks[i + 1].k == Tk::Num) {
                try { v.nums.push_back((neg ? -1.0f : 1.0f) * std::stof(toks[i + 1].t)); } catch (...) { v.nums.push_back(0.0f); }
                return i + 2;
            }
        }
        // raw expression fallback: read until ';' or '>' at depth 0
        v.kind = MmeValue::String;
        size_t j = i;
        int d = 0;
        std::string raw;
        for (; j < toks.size(); ++j) {
            if (IsPunct(toks[j], "(") || IsPunct(toks[j], "[")) ++d;
            else if (IsPunct(toks[j], ")") || IsPunct(toks[j], "]")) --d;
            else if (d == 0 && (IsPunct(toks[j], ";") || IsPunct(toks[j], ">"))) break;
            if (!raw.empty()) raw += ' ';
            raw += toks[j].t;
        }
        v.str = Trim(raw);
        return j;
    }

    // A braced or constructor numeric list: comma-separated literals, optionally floatN(...) nested.
    static MmeValue NumericList(const std::vector<Tok>& inner, MmeValue::Kind kind) {
        MmeValue v;
        v.kind = kind;
        for (size_t i = 0; i < inner.size(); ++i) {
            if (inner[i].k == Tk::Num) {
                try { v.nums.push_back(std::stof(inner[i].t)); } catch (...) { v.nums.push_back(0.0f); }
            } else if (IsPunct(inner[i], "(") && i + 1 < inner.size() && inner[i + 1].k == Tk::Id) {
                // nested like float2(...): handled by the outer loop's contents anyway
            }
        }
        return v;
    }

    // Initializer after '=' up to `end` (the terminating ';' index) or the next declarator comma at depth 0.
    size_t ParseInitializer(size_t i, size_t end, MmeValue& v) {
        if (i >= end) { v.kind = MmeValue::String; return i; }
        if (toks[i].k == Tk::Str) {
            std::string s;
            while (i < end && toks[i].k == Tk::Str) { s += Unquote(toks[i].t); ++i; }
            v.kind = MmeValue::String;
            v.str = std::move(s);
            return i;
        }
        if (toks[i].k == Tk::Num) {
            v.kind = (toks[i].t.find('.') != std::string::npos) ? MmeValue::Float : MmeValue::Int;
            try { v.nums.push_back(std::stof(toks[i].t)); } catch (...) { v.nums.push_back(0.0f); }
            return i + 1;
        }
        // a literal vector / matrix initializer: `{a,b,c}` or `floatN(...) / intN(...)`
        if (IsPunct(toks[i], "{") ||
            (toks[i].k == Tk::Id && i + 1 < end && IsPunct(toks[i + 1], "("))) {
            return ParseValue(i, v);
        }
        // an expression initializer: keep the raw text (hasInit is already true)
        size_t j = i;
        int d = 0;
        std::string raw;
        for (; j < end; ++j) {
            if (IsPunct(toks[j], "(") || IsPunct(toks[j], "{") || IsPunct(toks[j], "[")) ++d;
            else if (IsPunct(toks[j], ")") || IsPunct(toks[j], "}") || IsPunct(toks[j], "]")) --d;
            else if (d == 0 && IsPunct(toks[j], ",")) break;
            if (j == i) raw = toks[j].t;
            else raw += ' ' + toks[j].t;
        }
        v.kind = MmeValue::String;
        v.str = Trim(raw);
        return j;
    }

    // 'sampler' / 'sampler2D' ... Name [: register] [= sampler_state { ... }] ;
    size_t ParseSamplerDecl(size_t i) {
        const int declLine = toks[i].line;
        MmeSampler s;
        s.type = Lower(toks[i].t);
        s.line = 0;
        s.file = root;
        if (declLine >= 0 && declLine < (int)origins.size()) {
            s.line = origins[(size_t)declLine].line;
            s.file = origins[(size_t)declLine].file;
        }
        ++i;
        if (i < toks.size() && toks[i].k == Tk::Id) { s.name = toks[i].t; ++i; }
        // optional ': register(sN)'
        if (i < toks.size() && IsPunct(toks[i], ":")) {
            ++i;
            if (i < toks.size() && IsId(toks[i], "register")) {
                ++i;
                if (i < toks.size() && IsPunct(toks[i], "(")) {
                    ++i;
                    std::string reg;
                    while (i < toks.size() && !IsPunct(toks[i], ")")) { reg += toks[i].t; ++i; }
                    if (i < toks.size()) ++i;
                    s.registerBinding = reg;
                }
            }
        }
        // optional '= sampler_state { ... }'
        if (i < toks.size() && IsPunct(toks[i], "=")) {
            ++i;
            if (i < toks.size() && (IsId(toks[i], "sampler_state") || IsId(toks[i], "SamplerState") ||
                                    IsId(toks[i], "samplerstate"))) {
                ++i;
                if (i < toks.size() && IsPunct(toks[i], "{")) {
                    ++i;
                    while (i < toks.size() && !IsPunct(toks[i], "}")) {
                        if (IsPunct(toks[i], ";")) { ++i; continue; }
                        if (toks[i].k != Tk::Id) { ++i; continue; }
                        const std::string state = UpperAscii(toks[i].t);
                        ++i;
                        if (i < toks.size() && IsPunct(toks[i], "=")) ++i;
                        // value: <T> / (T) / text until ';'
                        std::string val;
                        if (i < toks.size() && (IsPunct(toks[i], "<") || IsPunct(toks[i], "("))) {
                            const char open = toks[i].t[0];
                            const char close = open == '<' ? '>' : ')';
                            ++i;
                            std::string inner;
                            while (i < toks.size() && !IsPunct(toks[i], std::string(1, close).c_str())) {
                                inner += toks[i].t;
                                ++i;
                            }
                            if (i < toks.size()) ++i;
                            val = Trim(inner);
                            if (state == "TEXTURE") s.texture = val;
                            s.states[state] = val;
                            continue;
                        }
                        while (i < toks.size() && !IsPunct(toks[i], ";") && !IsPunct(toks[i], "}")) {
                            if (!val.empty()) val += ' ';
                            val += toks[i].t;
                            ++i;
                        }
                        val = Trim(val);
                        if (state == "TEXTURE") s.texture = val;
                        s.states[state] = val;
                    }
                    if (i < toks.size() && IsPunct(toks[i], "}")) ++i;
                }
            }
        }
        // consume the terminating ';'
        while (i < toks.size() && !IsPunct(toks[i], ";") && !IsPunct(toks[i], "}")) ++i;
        if (i < toks.size() && IsPunct(toks[i], ";")) ++i;
        desc.samplers.push_back(std::move(s));
        return i;
    }

    // 'technique Name < ann > { pass ... }'
    size_t ParseTechnique(size_t i) {
        const std::string kw = Lower(toks[i].t);
        const int declLine = toks[i].line;
        if (kw == "technique10" || kw == "technique11") {
            Diag(MmeDiag::Warning, declLine, kw + " is not supported; technique skipped");
            // skip to the body and beyond
            for (; i < toks.size(); ++i) {
                if (IsPunct(toks[i], "{")) { i = SkipBraces(i); break; }
                if (IsPunct(toks[i], ";")) break;
            }
            return i < toks.size() ? i : toks.size();
        }
        MmeTechnique tech;
        tech.line = 0;
        if (declLine >= 0 && declLine < (int)origins.size()) tech.line = origins[(size_t)declLine].line;
        ++i;
        if (i < toks.size() && toks[i].k == Tk::Id) { tech.name = toks[i].t; ++i; }
        if (i < toks.size() && IsPunct(toks[i], "<")) i = ParseAnnotationBlock(i, tech.annotations);
        if (i < toks.size() && IsPunct(toks[i], "{")) {
            ++i;
            while (i < toks.size() && !IsPunct(toks[i], "}")) {
                if (IsPunct(toks[i], ";")) { ++i; continue; }
                if (toks[i].k == Tk::Id && Lower(toks[i].t) == "pass") {
                    i = ParsePass(i, tech.passes);
                    continue;
                }
                ++i;   // tolerate stray tokens
            }
            if (i < toks.size() && IsPunct(toks[i], "}")) ++i;
        }
        DeriveTechnique(tech);
        desc.techniques.push_back(std::move(tech));
        return i;
    }

    // 'pass Name < ann > { states }'
    size_t ParsePass(size_t i, std::vector<MmePass>& out) {
        MmePass pass;
        pass.line = 0;
        if (i < toks.size() && toks[i].line >= 0 && toks[i].line < (int)origins.size())
            pass.line = origins[(size_t)toks[i].line].line;
        ++i;   // 'pass'
        if (i < toks.size() && toks[i].k == Tk::Id) { pass.name = toks[i].t; ++i; }
        if (i < toks.size() && IsPunct(toks[i], "<")) i = ParseAnnotationBlock(i, pass.annotations);
        if (!(i < toks.size() && IsPunct(toks[i], "{"))) {
            if (i < toks.size() && IsPunct(toks[i], ";")) ++i;
            out.push_back(std::move(pass));
            return i;
        }
        ++i;   // '{'
        while (i < toks.size() && !IsPunct(toks[i], "}")) {
            if (IsPunct(toks[i], ";")) { ++i; continue; }
            if (toks[i].k != Tk::Id) { ++i; continue; }
            const std::string state = UpperAscii(toks[i].t);
            const int stateLine = toks[i].line;
            ++i;
            if (i < toks.size() && IsPunct(toks[i], "=")) ++i;
            // value up to ';' at depth 0
            const size_t valStart = i;
            int d = 0;
            for (; i < toks.size(); ++i) {
                if (IsPunct(toks[i], "(") || IsPunct(toks[i], "{") || IsPunct(toks[i], "[")) ++d;
                else if (IsPunct(toks[i], ")") || IsPunct(toks[i], "]")) --d;
                else if (IsPunct(toks[i], "}")) { if (d == 0) break; --d; }
                else if (IsPunct(toks[i], ";") && d == 0) break;
            }
            const size_t valEnd = i;
            if (i < toks.size() && IsPunct(toks[i], ";")) ++i;
            std::string raw = TrimTokens(toks, valStart, valEnd);
            pass.states.push_back({state, raw});
            if (state == "VERTEXSHADER" || state == "PIXELSHADER") {
                MmeShaderRef& ref = (state == "VERTEXSHADER") ? pass.vs : pass.ps;
                ParseShaderRef(toks, valStart, valEnd, ref);
            }
            (void)stateLine;
        }
        if (i < toks.size() && IsPunct(toks[i], "}")) ++i;
        out.push_back(std::move(pass));
        return i;
    }

    // `compile vs_3_0 Fn(args)` / `NULL` / `asm { ... }`
    static void ParseShaderRef(const std::vector<Tok>& ts, size_t from, size_t to, MmeShaderRef& ref) {
        size_t p = from;
        while (p < to && ts[p].k == Tk::Id && Lower(ts[p].t) == "compile") ++p;
        if (p < to && ts[p].k == Tk::Id) {
            const std::string lo = Lower(ts[p].t);
            if (lo == "null") { ref.isNull = true; return; }
            if (lo == "asm") { ref.isAsm = true; return; }
            // a profile token is `vs_N_M` / `ps_N_M`; otherwise the next identifier is the function itself
            if (lo.rfind("vs_", 0) == 0 || lo.rfind("ps_", 0) == 0) { ref.profile = lo; ++p; }
        }
        if (p < to && ts[p].k == Tk::Id) { ref.function = ts[p].t; ++p; }
        if (p < to && IsPunct(ts[p], "(")) {
            ++p;
            std::string cur;
            int d = 0;
            for (; p < to; ++p) {
                if (IsPunct(ts[p], "(")) { ++d; cur += '('; continue; }
                if (IsPunct(ts[p], ")")) {
                    if (d == 0) break;
                    --d;
                    cur += ')';
                    continue;
                }
                if (IsPunct(ts[p], ",") && d == 0) {
                    ref.args.push_back(Trim(cur));
                    cur.clear();
                    continue;
                }
                if (!cur.empty() && ts[p].k != Tk::Punct) cur += ' ';
                cur += ts[p].t;
            }
            if (!Trim(cur).empty()) ref.args.push_back(Trim(cur));
        }
    }

    // Derives mmdPass / subset / use* from the technique annotations.
    void DeriveTechnique(MmeTechnique& tech) {
        tech.mmdPass = "object";
        tech.subset.clear();
        tech.useTexture = tech.useSphereMap = tech.useToon = -1;
        for (const MmeAnnotation& a : tech.annotations) {
            const std::string name = Lower(a.name);
            if (name == "mmdpass") {
                std::string v = a.value.str;
                if (v.empty() && !a.value.nums.empty()) v = std::to_string((int)a.value.nums[0]);
                if (!v.empty()) tech.mmdPass = Lower(v);
            } else if (name == "subset") {
                tech.subset = a.value.str;
            } else if (name == "usetexture") {
                tech.useTexture = a.value.nums.empty() ? (a.value.str == "true" ? 1 : 0) : (a.value.nums[0] != 0 ? 1 : 0);
            } else if (name == "usespheremap") {
                tech.useSphereMap = a.value.nums.empty() ? (a.value.str == "true" ? 1 : 0) : (a.value.nums[0] != 0 ? 1 : 0);
            } else if (name == "usetoon") {
                tech.useToon = a.value.nums.empty() ? (a.value.str == "true" ? 1 : 0) : (a.value.nums[0] != 0 ? 1 : 0);
            }
        }
    }
};

// ---- script parsing --------------------------------------------------------------------------------------

const char* CanonicalCommand(const std::string& low) {
    if (low == "rendercolortarget" || low == "rendercolortarget0") return "RenderColorTarget0";
    if (low == "rendercolortarget1") return "RenderColorTarget1";
    if (low == "rendercolortarget2") return "RenderColorTarget2";
    if (low == "rendercolortarget3") return "RenderColorTarget3";
    if (low == "renderdepthstenciltarget") return "RenderDepthStencilTarget";
    if (low == "clearsetcolor") return "ClearSetColor";
    if (low == "clearsetdepth") return "ClearSetDepth";
    if (low == "clear") return "Clear";
    if (low == "scriptexternal") return "ScriptExternal";
    if (low == "pass") return "Pass";
    if (low == "loopbycount") return "LoopByCount";
    if (low == "loopend") return "LoopEnd";
    if (low == "loopgetindex") return "LoopGetIndex";
    if (low == "draw") return "Draw";
    return nullptr;
}

// ---- STANDARDSGLOBAL extraction --------------------------------------------------------------------------

void ApplyStandardsGlobal(MmeEffectDesc& d) {
    for (const MmeParam& p : d.params) {
        if (p.semantic != "STANDARDSGLOBAL") continue;
        d.hasStandardsGlobal = true;
        for (const MmeAnnotation& a : p.annotations) {
            const std::string name = Lower(a.name);
            if (name == "scriptclass") d.scriptClass = Lower(a.value.str);
            else if (name == "scriptorder") d.scriptOrder = Lower(a.value.str);
            else if (name == "scriptoutput") d.scriptOutput = a.value.str;
            else if (name == "script") d.globalScript = a.value.str;
        }
        break;
    }
    if (d.scriptClass.empty()) d.scriptClass = "object";
    if (d.scriptOrder.empty()) d.scriptOrder = "standard";
}

} // namespace

const MmeAnnotation* FindAnnotation(const std::vector<MmeAnnotation>& a, std::string_view name) {
    for (const MmeAnnotation& x : a)
        if (Lower(x.name) == Lower(std::string(name))) return &x;
    return nullptr;
}

std::vector<MmeScriptCommand> ParseMmeScript(std::string_view script) {
    std::vector<MmeScriptCommand> out;
    std::string s(script);
    size_t start = 0;
    while (start <= s.size()) {
        size_t semi = s.find(';', start);
        std::string entry = Trim(s.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
        if (!entry.empty()) {
            MmeScriptCommand cmd;
            const size_t eq = entry.find('=');
            if (eq == std::string::npos) {
                cmd.command = entry;   // unknown form (no '=')
            } else {
                const std::string name = Trim(entry.substr(0, eq));
                cmd.arg = Trim(entry.substr(eq + 1));
                const char* canon = CanonicalCommand(Lower(name));
                if (canon) { cmd.command = canon; cmd.known = true; }
                else cmd.command = name;
            }
            out.push_back(std::move(cmd));
        }
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    return out;
}

std::string MmeEffectDesc::kind() const {
    if (scriptOrder == "preprocess") return "pre";
    if (scriptClass == "scene" || (scriptClass == "sceneorobject" && scriptOrder == "postprocess")) return "post";
    return "object";
}

bool MmeEffectDesc::UsesOffscreen() const {
    for (const MmeParam& p : params)
        if (p.semantic == "OFFSCREENRENDERTARGET") return true;
    return false;
}

MmeEffectDesc LoadMmeEffect(const std::filesystem::path& fx, const MmeLoadOptions& opt) {
    MmeEffectDesc desc;
    desc.file = fx;
    try {
        MmePreprocessResult pp = PreprocessMme(fx, opt.maxIncludeDepth, opt.predefines);
        desc.includes = pp.includes;
        desc.defines = pp.defines;
        desc.expandedSource = pp.text;
        for (const MmeDiag& d : pp.diags) desc.diags.push_back(d);
        if (!pp.ok) return desc;   // ok stays false only when the file could not be read
        const std::vector<Tok> toks = Tokenize(pp.text);
        Parser parser(toks, pp.lines, fx);
        parser.desc.file = fx;
        parser.Run();
        desc.params = std::move(parser.desc.params);
        desc.samplers = std::move(parser.desc.samplers);
        desc.techniques = std::move(parser.desc.techniques);
        for (const MmeDiag& d : parser.diags) desc.diags.push_back(d);
        ApplyStandardsGlobal(desc);
        desc.ok = true;
    } catch (const std::filesystem::filesystem_error& e) {
        desc.diags.push_back({MmeDiag::Error, fx, 0, std::string("filesystem error: ") + e.what()});
        desc.ok = false;
    } catch (const std::exception& e) {
        desc.diags.push_back({MmeDiag::Error, fx, 0, std::string("internal parser failure: ") + e.what()});
        desc.ok = false;
    } catch (...) {
        desc.diags.push_back({MmeDiag::Error, fx, 0, "internal parser failure"});
        desc.ok = false;
    }
    return desc;
}

} // namespace mmdx
