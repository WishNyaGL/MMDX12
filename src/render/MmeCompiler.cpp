// MmeCompiler.cpp: M2's MME pass compiler (see MmeCompiler.h and docs/mme/mme_design.md §2.2). One pass becomes a
// translation unit = M1's expandedSource with every technique / asm block blanked (spaces, newlines kept) plus a
// generated wrapper entry that declares the varying parameters with their semantics, binds the compile arguments to
// the user function's uniform parameters and applies the DX9 -> SM5 fix-ups (_INDEX -> SV_VertexID, VPOS -> SV_Position
// - 0.5, VFACE -> SV_IsFrontFace). FXC compiles it to DXBC (vs_5_0 / ps_5_0) and ID3D11ShaderReflection reports the
// $Globals layout, the bound resources and both signatures. Bytecode is cached on disk; failures are never cached.
// The compiler is thread-safe: state is local, cache writes go through a per-thread tmp file + rename.
#include "render/MmeCompiler.h"

#include <Windows.h>

#include <d3dcommon.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "asset/MmePreprocessor.h"
#include "asset/MmeText.h"
#include "core/TextUtil.h"

namespace mmdx {
namespace {

// Counters for tools/mme_compile_test's summary (see the public accessors at the bottom of this file).
std::atomic<uint32_t> g_mmeCacheHits{0};
std::atomic<uint32_t> g_mmeStageCompiles{0};
std::atomic<uint32_t> g_mmeStaticFixups{0};

using mmetext::Lower;
using mmetext::Trim;

// ---- tiny tokeniser (same idea as M1; no comments - the expanded source already has none) -------------------------

enum class TK { Id, Num, Str, Punct };

struct Tok {
    TK k = TK::Punct;
    std::string t;
    size_t pos = 0;   // byte offset in the tokenised text
};

bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::vector<Tok> Tokenize(const std::string& s) {
    std::vector<Tok> out;
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if (IsWs(c)) { ++i; continue; }
        if (c == '"') {
            const size_t b = i++;
            while (i < n && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < n) i += 2;
                else ++i;
            }
            if (i < n) ++i;
            out.push_back({TK::Str, s.substr(b, i - b), b});
            continue;
        }
        if (mmetext::IsIdentStart(c)) {
            const size_t b = i;
            while (i < n && mmetext::IsIdentChar(s[i])) ++i;
            out.push_back({TK::Id, s.substr(b, i - b), b});
            continue;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))) {
            const size_t b = i;
            while (i < n && (std::isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '_')) ++i;
            out.push_back({TK::Num, s.substr(b, i - b), b});
            continue;
        }
        out.push_back({TK::Punct, std::string(1, c), i});
        ++i;
    }
    return out;
}

std::string UpperAscii(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

bool IsId(const Tok& t, const char* s) { return t.k == TK::Id && t.t == s; }

// Index of the token matching the bracket opened at `open`; size() when unbalanced.
size_t MatchPair(const std::vector<Tok>& tk, size_t open, const char* o, const char* c) {
    int d = 0;
    for (size_t i = open; i < tk.size(); ++i) {
        if (tk[i].t == o) ++d;
        else if (tk[i].t == c) { --d; if (d == 0) return i; }
    }
    return tk.size();
}
size_t MatchBrace(const std::vector<Tok>& tk, size_t open) { return MatchPair(tk, open, "{", "}"); }
size_t MatchParen(const std::vector<Tok>& tk, size_t open) { return MatchPair(tk, open, "(", ")"); }

// ---- blanking technique / asm blocks ------------------------------------------------------------------------------

// Removes `#line` directives (to end of line) from the expanded source, replacing them with spaces. FXC would
// otherwise attribute every diagnostic to the file a stale `#line` names; the compiler maps lines back itself.
// Any other `#` outside a string literal is replaced by a space: after preprocessing it can only be an artifact
// (M1's `##` string pasting leaves a stray `#`, which FXC rejects).
void StripLineDirectives(std::string& src) {
    size_t i = 0;
    bool atLineStart = true, inString = false;
    while (i < src.size()) {
        const char c = src[i];
        if (c == '\n') { atLineStart = true; ++i; continue; }
        if (inString) {
            if (c == '\\' && i + 1 < src.size()) { i += 2; continue; }
            if (c == '"') inString = false;
            ++i;
            continue;
        }
        if (c == '"') { inString = true; atLineStart = false; ++i; continue; }
        if (c != ' ' && c != '\t') {
            if (c == '#' && atLineStart) {
                const size_t e = src.find('\n', i);
                const size_t end = e == std::string::npos ? src.size() : e;
                for (size_t q = i; q < end; ++q) src[q] = ' ';
                i = end;
                continue;
            }
            if (c == '#') src[i] = ' ';   // stray paste artifact
            atLineStart = false;
        }
        ++i;
    }
}

std::string BlankRanges(const std::string& s, const std::vector<std::pair<size_t, size_t>>& ranges) {
    std::string out = s;
    for (const auto& [a, b] : ranges)
        for (size_t i = a; i < b && i < out.size(); ++i)
            if (out[i] != '\n') out[i] = ' ';
    return out;
}

std::vector<std::pair<size_t, size_t>> FindBlankRanges(const std::vector<Tok>& tk) {
    std::vector<std::pair<size_t, size_t>> r;
    for (size_t i = 0; i < tk.size(); ++i) {
        if (tk[i].k != TK::Id) continue;
        const std::string lo = Lower(tk[i].t);
        if (lo == "technique" || lo == "technique10" || lo == "technique11") {
            size_t j = i + 1;
            while (j < tk.size() && tk[j].t != "{") { if (tk[j].t == ";") break; ++j; }
            if (j < tk.size() && tk[j].t == "{") {
                const size_t close = MatchBrace(tk, j);
                if (close < tk.size()) r.push_back({tk[i].pos, tk[close].pos + 1});
            }
        } else if (lo == "asm") {
            size_t j = i + 1;
            if (j < tk.size() && tk[j].t == "{") {
                const size_t close = MatchBrace(tk, j);
                if (close < tk.size()) r.push_back({tk[i].pos, tk[close].pos + 1});
            }
        }
    }
    return r;
}

// ---- signature / struct parsing -----------------------------------------------------------------------------------

struct ParamInfo {
    bool uniform = false, isIn = false, isOut = false;
    std::string type;         // "float4" / "VS_OUTPUT" / "struct VS_OUTPUT"
    std::string name;
    std::string semantic;     // as written (e.g. TEXCOORD1, _INDEX, VPOS, VFACE, POSITION, COLOR)
    std::string defaultVal;
    bool hasDefault = false;
};

struct FuncInfo {
    int status = 0;           // 0 not found, 1 ok, 2 ambiguous
    std::string retType = "void";
    std::string retSemantic;
    std::vector<ParamInfo> params;
};

struct Member {
    std::string type;
    std::string name;
    std::string semantic;
};

void SplitTopLevel(const std::vector<Tok>& tk, size_t from, size_t to, const char* sep,
                   std::vector<std::pair<size_t, size_t>>& out) {
    size_t start = from;
    int d = 0;
    for (size_t i = from; i < to; ++i) {
        const std::string& t = tk[i].t;
        if (t == "(" || t == "[" || t == "{") ++d;
        else if (t == ")" || t == "]" || t == "}") { if (d > 0) --d; }
        else if (t == sep && d == 0) { out.push_back({start, i}); start = i + 1; }
    }
    out.push_back({start, to});
}

ParamInfo ParseParam(const std::vector<Tok>& tk, size_t from, size_t to) {
    ParamInfo p;
    size_t i = from;
    while (i < to && tk[i].k == TK::Id) {
        const std::string lo = Lower(tk[i].t);
        if (lo == "uniform") { p.uniform = true; ++i; }
        else if (lo == "in") { p.isIn = true; ++i; }
        else if (lo == "out") { p.isOut = true; ++i; }
        else if (lo == "inout") { p.isIn = true; p.isOut = true; ++i; }
        else if (lo == "const") { ++i; }
        else break;
    }
    if (i < to && Lower(tk[i].t) == "struct" && i + 1 < to) { p.type = "struct " + tk[i + 1].t; i += 2; }
    else if (i < to && tk[i].k == TK::Id) { p.type = tk[i].t; ++i; }
    if (i < to && tk[i].k == TK::Id) { p.name = tk[i].t; ++i; }
    if (i < to && tk[i].t == ":") {
        ++i;
        if (i < to && tk[i].k == TK::Id) { p.semantic = tk[i].t; ++i; }
    }
    if (i < to && tk[i].t == "=") {
        ++i;
        p.hasDefault = true;
        std::string d;
        for (; i < to; ++i) {
            if (!d.empty() && tk[i].k != TK::Punct) d += ' ';
            d += tk[i].t;
        }
        p.defaultVal = Trim(d);
    }
    // HLSL / D3DX: a parameter with neither `in` nor `out` is an input.
    if (!p.isIn && !p.isOut) p.isIn = true;
    return p;
}

Member ParseMember(const std::vector<Tok>& tk, size_t from, size_t to) {
    Member m;
    size_t i = from;
    if (i < to && Lower(tk[i].t) == "struct" && i + 1 < to) { m.type = "struct " + tk[i + 1].t; i += 2; }
    else if (i < to && tk[i].k == TK::Id) { m.type = tk[i].t; ++i; }
    if (i < to && tk[i].k == TK::Id) { m.name = tk[i].t; ++i; }
    while (i < to && tk[i].t == "[") { i = MatchPair(tk, i, "[", "]"); if (i < to) ++i; }
    if (i < to && tk[i].t == ":") {
        ++i;
        if (i < to && tk[i].k == TK::Id) { m.semantic = tk[i].t; ++i; }
    }
    return m;
}

// Collects `struct NAME { members }` definitions.
std::map<std::string, std::vector<Member>> ParseStructs(const std::vector<Tok>& tk) {
    std::map<std::string, std::vector<Member>> out;
    for (size_t i = 0; i + 1 < tk.size(); ++i) {
        if (!(tk[i].k == TK::Id && Lower(tk[i].t) == "struct" && tk[i + 1].k == TK::Id)) continue;
        const std::string sname = tk[i + 1].t;
        size_t j = i + 2;
        while (j < tk.size() && tk[j].t != "{") { if (tk[j].t == ";") break; ++j; }
        if (!(j < tk.size() && tk[j].t == "{")) continue;
        const size_t close = MatchBrace(tk, j);
        std::vector<Member> members;
        size_t start = j + 1;
        int d = 0;
        for (size_t q = j + 1; q < close && q < tk.size(); ++q) {
            const std::string& t = tk[q].t;
            if (t == "{" || t == "(" || t == "[") ++d;
            else if (t == "}" || t == ")" || t == "]") { if (d > 0) --d; }
            else if (t == ";" && d == 0) {
                const Member m = ParseMember(tk, start, q);
                if (!m.name.empty()) members.push_back(m);
                start = q + 1;
            }
        }
        out[sname] = members;
    }
    return out;
}

FuncInfo ParseFunction(const std::vector<Tok>& tk, const std::string& name, int expectedUniforms) {
    FuncInfo best;
    int matches = 0;
    for (size_t i = 1; i + 1 < tk.size(); ++i) {
        if (!(tk[i].k == TK::Id && tk[i].t == name)) continue;
        if (tk[i + 1].t != "(") continue;
        if (tk[i - 1].k != TK::Id) continue;   // preceded by a return type
        const size_t close = MatchParen(tk, i + 1);
        if (close >= tk.size()) continue;
        size_t k = close + 1;
        std::string retSem;
        if (k < tk.size() && tk[k].t == ":") {
            ++k;
            if (k < tk.size() && tk[k].k == TK::Id) { retSem = tk[k].t; ++k; }
        }
        if (!(k < tk.size() && tk[k].t == "{")) continue;   // must be a definition

        // return type: walk back over identifiers (handles `struct VS_OUTPUT`)
        size_t rs = i - 1;
        while (rs > 0) {
            const Tok& prev = tk[rs - 1];
            if (prev.k == TK::Id) {
                const std::string lo = Lower(prev.t);
                if (lo == "static" || lo == "const" || lo == "inline" || lo == "uniform" || lo == "return") break;
                --rs;
            } else break;
        }
        std::string ret;
        for (size_t q = rs; q < i; ++q) { if (!ret.empty()) ret += ' '; ret += tk[q].t; }

        FuncInfo fi;
        fi.status = 1;
        fi.retType = ret.empty() ? "void" : ret;
        fi.retSemantic = retSem;
        std::vector<std::pair<size_t, size_t>> parts;
        SplitTopLevel(tk, i + 2, close, ",", parts);
        for (const auto& [a, b] : parts) {
            if (a >= b) continue;
            fi.params.push_back(ParseParam(tk, a, b));
        }
        if (fi.params.size() == 1 && fi.params[0].type == "void" && fi.params[0].name.empty()) fi.params.clear();

        int unif = 0;
        for (const ParamInfo& p : fi.params)
            if (p.uniform) ++unif;
        if (unif == expectedUniforms) { best = fi; ++matches; }
    }
    if (matches == 1) return best;
    FuncInfo r;
    r.status = matches == 0 ? 0 : 2;
    return r;
}

// ---- semantic helpers ----------------------------------------------------------------------------------------------

void SplitSemantic(const std::string& raw, std::string& base, uint32_t& index) {
    size_t e = raw.size();
    while (e > 0 && std::isdigit((unsigned char)raw[e - 1])) --e;
    index = e < raw.size() ? (uint32_t)std::atoi(raw.c_str() + e) : 0;
    base = UpperAscii(raw.substr(0, e));
}

int MaskCount(uint8_t m) {
    int c = 0;
    for (int i = 0; i < 4; ++i)
        if (m & (1u << i)) ++c;
    return c;
}

std::string FloatType(int components) {
    switch (components) {
        case 1: return "float";
        case 2: return "float2";
        case 3: return "float3";
        case 4: return "float4";
        default: return "float4";
    }
}

std::string StripStructPrefix(const std::string& t) { return Lower(t).rfind("struct ", 0) == 0 ? t.substr(7) : t; }

// ---- wrapper generation --------------------------------------------------------------------------------------------

struct WrapperOut {
    std::string text;
    bool usesVertexId = false;
    bool usesVpos = false;
    bool usesVface = false;
    bool alphaTest = false;   // the PS wrapper injected an alpha-test clip()
    bool badMrt = false;   // object mode: the user PS writes MRT / DEPTH itself (no host outputs added)
    std::string error;   // non-empty => generation failed
};

std::string SemiIndex(const std::string& semantic, uint32_t index) {
    return index ? semantic + std::to_string(index) : semantic;
}

// The host constants block (object mode). register(b13) is the processor register (space 0: SM 5.0 under FXC has
// no register spaces -- X3721; b1 is left free because user DX9 bool registers also use `register(b1)`).
// See docs/mme/mme_design.md §2.6.
std::string HostCbufferText() {
    return
        "// MMDX12 MME host constants (object mode; b13 space0, see MmeCompiler.cpp)\n"
        "cbuffer __MmdxHost : register(b13) {\n"
        "    row_major float4x4 gMmdxView;\n"
        "    row_major float4x4 gMmdxViewProjNoJitter;\n"
        "    row_major float4x4 gMmdxPrevViewProjNoJitter;\n"
        "    float4 gMmdxParams;   // x = sun intensity, y = 1 when the output is linearised\n"
        "};\n";
}

// OctEncode + SrgbToLinear, copied from shaders/common.hlsli under __mmdx_ names (the wrapper does not include
// common.hlsli).
std::string OctEncodeText() {
    return
        "float2 __mmdx_oct_wrap(float2 v) { return (1.0 - abs(v.yx)) * (v.xy >= 0.0 ? 1.0 : -1.0); }\n"
        "float2 __mmdx_oct_encode(float3 n) {\n"
        "    n /= (abs(n.x) + abs(n.y) + abs(n.z));\n"
        "    n.xy = n.z >= 0.0 ? n.xy : __mmdx_oct_wrap(n.xy);\n"
        "    return n.xy;\n"
        "}\n"
        "float3 __mmdx_srgb_to_linear(float3 c) {\n"
        "    return c <= 0.04045 ? c / 12.92 : pow(max(c, 0.0) * (1.0 / 1.055) + 0.055 / 1.055, 2.4);\n"
        "}\n";
}

// `TEXCOORD12` is the previous world position (host buffer slot 0, host use); user VS inputs are ordinary varyings.
bool IsHostObjectInput(const std::string& semantic) { return UpperAscii(semantic) == "TEXCOORD12"; }

// ---- alpha test (D3D12 has no alpha test; the PS wrapper injects a clip()) ----------------------------------------
// MME's AlphaTestEnable + AlphaRef (0..255 in D3D9) + AlphaFunc -> a clip() in the generated PS wrapper. Returns ""
// when the function needs no clip (unknown / ALWAYS), else the D3D-style comparison operator.
const std::string* PassState(const MmePass& p, const char* name) {
    for (const auto& [k, v] : p.states)
        if (k == name) return &v;
    return nullptr;
}

bool PassBool(const MmePass& p, const char* name, bool def) {
    const std::string* v = PassState(p, name);
    if (!v) return def;
    const std::string s = Lower(Trim(*v));
    if (s == "true" || s == "1") return true;
    if (s == "false" || s == "0") return false;
    return def;
}

std::string AlphaTestOp(const std::string& raw) {
    const std::string s = UpperAscii(Trim(raw));
    if (s == "NEVER") return "never";
    if (s == "LESS") return "<";
    if (s == "EQUAL") return "==";
    if (s == "LESSEQUAL") return "<=";
    if (s == "GREATER") return ">";
    if (s == "NOTEQUAL") return "!=";
    if (s == "GREATEREQUAL") return ">=";
    return {};   // ALWAYS / unknown: no clip
}

std::string FloatLit(float f) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", f);
    return std::string(buf);
}

// The alpha-test part of the program (de)duplication key: passes with identical shader functions but different
// alpha-test states must not share a compiled program.
std::string AlphaTestKey(const MmePass& p) {
    std::string s;
    for (const char* n : {"ALPHATESTENABLE", "ALPHAREF", "ALPHAFUNC"}) {
        if (const std::string* v = PassState(p, n)) { s += '\x01'; s += n; s += '='; s += *v; }
    }
    return s;
}

WrapperOut BuildVsWrapper(const std::vector<Tok>& tk, const std::string& fn, const std::vector<std::string>& args,
                          bool objectMode, const std::map<std::string, std::vector<Member>>& structs) {
    WrapperOut w;
    const FuncInfo f = ParseFunction(tk, fn, (int)args.size());
    if (f.status == 0) { w.error = "vertex function not found: " + fn; return w; }
    if (f.status == 2) { w.error = "ambiguous vertex function: " + fn; return w; }

    std::vector<std::string> entryParams;
    std::vector<std::string> pre, post, callArgs;
    bool needVid = false;
    size_t argIdx = 0;
    for (const ParamInfo& p : f.params) {
        if (p.uniform) {
            if (argIdx < args.size()) callArgs.push_back(args[argIdx++]);
            else if (p.hasDefault) callArgs.push_back(p.defaultVal);
            else { w.error = "missing compile argument for uniform parameter '" + p.name + "'"; return w; }
            continue;
        }
        if (p.type.empty() || p.name.empty()) { w.error = "unparsable parameter in " + fn; return w; }
        const std::string base = UpperAscii(p.semantic);
        // A struct-typed input (`VS_OUTPUT VS(VS_INPUT IN)`): the POSITION / NORMAL / other semantics live on the
        // struct's fields, so expose them as flat entry inputs and rebuild the user's struct before the call. This
        // keeps the host extras (TEXCOORD12/13/14/15) working exactly like the flat-input case.
        const std::string structName = StripStructPrefix(p.type);
        auto structIt = (!p.isOut && p.semantic.empty()) ? structs.find(structName) : structs.end();
        if (p.isIn && p.isOut) {
            // inout: expose as a separate in + out so the caller binds the varying correctly
            const std::string loc = p.name + "__loc";
            const std::string outName = p.name + "__o";
            entryParams.push_back(p.type + " " + p.name + (p.semantic.empty() ? "" : " : " + p.semantic));
            entryParams.push_back("out " + p.type + " " + outName + (p.semantic.empty() ? "" : " : " + p.semantic));
            pre.push_back(p.type + " " + loc + " = " + p.name + ";");
            post.push_back(outName + " = " + loc + ";");
            callArgs.push_back(loc);
        } else if (objectMode && p.isIn && structIt != structs.end()) {
            pre.push_back(structName + " " + p.name + " = (" + structName + ")0;");
            for (const Member& m : structIt->second) {
                if (m.name.empty() || m.semantic.empty()) continue;   // unsemantic / nested members stay zero
                const std::string mb = UpperAscii(m.semantic);
                std::string expr;
                if (mb == "_INDEX") {
                    needVid = true;
                    w.usesVertexId = true;
                    expr = "(" + m.type + ")__vid";
                } else if (objectMode && IsHostObjectInput(mb)) {
                    // the host owns TEXCOORD12 (previous world position): do not declare it, bind it to zero
                    expr = "(" + m.type + ")0";
                } else {
                    const std::string en = p.name + "__" + m.name;
                    entryParams.push_back(m.type + " " + en + " : " + m.semantic);
                    expr = en;
                }
                pre.push_back(p.name + "." + m.name + " = " + expr + ";");
            }
            callArgs.push_back(p.name);
        } else if (p.isIn) {
            if (base == "_INDEX") {
                needVid = true;
                w.usesVertexId = true;
                callArgs.push_back("(" + p.type + ")__vid");
            } else if (objectMode && IsHostObjectInput(base)) {
                // the host owns TEXCOORD12 (previous world position): do not declare it, bind it to zero
                callArgs.push_back("(" + p.type + ")0");
            } else {
                entryParams.push_back(p.type + " " + p.name + (p.semantic.empty() ? "" : " : " + p.semantic));
                callArgs.push_back(p.name);
            }
        } else if (p.isOut) {
            entryParams.push_back("out " + p.type + " " + p.name + (p.semantic.empty() ? "" : " : " + p.semantic));
            callArgs.push_back(p.name);
        }
    }
    if (needVid) entryParams.push_back("uint __vid : SV_VertexID");

    std::string call = fn + "(";
    for (size_t i = 0; i < callArgs.size(); ++i) { if (i) call += ", "; call += callArgs[i]; }
    call += ")";

    std::string s;
    s += "// MMDX12 MME wrapper (vertex)\n";
    if (objectMode) { s += HostCbufferText(); s += OctEncodeText(); }

    // Source of the input world position (POSITION) and normal (NORMAL) for the host extras. A struct input's
    // fields are searched too (the member is reached through the parameter, e.g. `i.pos`).
    std::string posSrc, nrmSrc;
    if (objectMode) {
        for (const ParamInfo& p : f.params) {
            if (p.uniform || !p.isIn) continue;
            const std::string base = UpperAscii(p.semantic);
            if (base == "POSITION" && posSrc.empty()) { posSrc = p.name; continue; }
            if (base == "NORMAL" && nrmSrc.empty()) { nrmSrc = p.name; continue; }
            if (!base.empty()) continue;
            auto sit = structs.find(StripStructPrefix(p.type));
            if (sit == structs.end()) continue;
            for (const Member& m : sit->second) {
                const std::string mb = UpperAscii(m.semantic);
                if (mb == "POSITION" && posSrc.empty()) posSrc = p.name + "." + m.name;
                else if (mb == "NORMAL" && nrmSrc.empty()) nrmSrc = p.name + "." + m.name;
            }
        }
    }

    if (objectMode && !posSrc.empty()) {
        // inputs: the previous world position (host) plus the user's declared inputs
        std::string inDecl = "float3 __mmdx_prev : TEXCOORD12";
        for (const std::string& e : entryParams) inDecl += ", " + e;
        std::string pos3 = "float4(" + posSrc + ".xyz, 1)";
        std::string nrm3 = nrmSrc.empty() ? std::string("float3(0, 0, 0)") : nrmSrc;
        std::string extra =
            "    __o.cur = mul(" + pos3 + ", gMmdxViewProjNoJitter);\n"
            "    __o.prv = mul(float4(__mmdx_prev, 1), gMmdxPrevViewProjNoJitter);\n"
            "    __o.n = mul(" + nrm3 + ", (float3x3)gMmdxView);\n";

        if (f.retType == "void") {
            // no return value: the user writes its varyings through `out` parameters; append the extras to those.
            s += "void __mmdx_vs(" + inDecl +
                 ", out float4 __mmdx_cur : TEXCOORD13, out float4 __mmdx_prv : TEXCOORD14, "
                 "out float3 __mmdx_n : TEXCOORD15) {\n";
            for (const std::string& l : pre) s += "    " + l + "\n";
            s += "    " + call + ";\n";
            for (const std::string& l : post) s += "    " + l + "\n";
            s += "    __mmdx_cur = mul(" + pos3 + ", gMmdxViewProjNoJitter);\n";
            s += "    __mmdx_prv = mul(float4(__mmdx_prev, 1), gMmdxPrevViewProjNoJitter);\n";
            s += "    __mmdx_n = mul(" + nrm3 + ", (float3x3)gMmdxView);\n";
        } else {
            // the user VS returns its clip position directly (POSITION) or inside a struct: wrap the return in a
            // struct so its fields keep their semantics and the extras are appended.
            s += "struct __mmdx_vs_out {\n";
            s += "    " + StripStructPrefix(f.retType) + " u" + (f.retSemantic.empty() ? "" : " : " + f.retSemantic) + ";\n";
            s += "    float4 cur : TEXCOORD13;\n";
            s += "    float4 prv : TEXCOORD14;\n";
            s += "    float3 n : TEXCOORD15;\n";
            s += "};\n";
            s += "__mmdx_vs_out __mmdx_vs(" + inDecl + ") {\n";
            for (const std::string& l : pre) s += "    " + l + "\n";
            s += "    __mmdx_vs_out __o;\n";
            s += "    __o.u = " + call + ";\n";
            for (const std::string& l : post) s += "    " + l + "\n";
            s += extra;
            s += "    return __o;\n";
        }
    } else if (objectMode) {
        // no POSITION input: the host cannot place the surface, compile without the extras (the PS wrapper still
        // keyed on object mode but the host output is degenerate). Rare; logged by the caller.
        s += f.retType + " __mmdx_vs(";
        for (size_t i = 0; i < entryParams.size(); ++i) { if (i) s += ", "; s += entryParams[i]; }
        s += ")";
        if (f.retType != "void" && !f.retSemantic.empty()) s += " : " + f.retSemantic;
        s += " {\n";
        for (const std::string& l : pre) s += "    " + l + "\n";
        if (f.retType == "void") {
            s += "    " + call + ";\n";
            for (const std::string& l : post) s += "    " + l + "\n";
        } else {
            s += "    " + f.retType + " __ret = " + call + ";\n";
            for (const std::string& l : post) s += "    " + l + "\n";
            s += "    return __ret;\n";
        }
    } else {
        s += f.retType + " __mmdx_vs(";
        for (size_t i = 0; i < entryParams.size(); ++i) { if (i) s += ", "; s += entryParams[i]; }
        s += ")";
        if (f.retType != "void" && !f.retSemantic.empty()) s += " : " + f.retSemantic;
        s += " {\n";
        for (const std::string& l : pre) s += "    " + l + "\n";
        if (f.retType == "void") {
            s += "    " + call + ";\n";
            for (const std::string& l : post) s += "    " + l + "\n";
        } else {
            s += "    " + f.retType + " __ret = " + call + ";\n";
            for (const std::string& l : post) s += "    " + l + "\n";
            s += "    return __ret;\n";
        }
    }
    s += "}\n";
    w.text = std::move(s);
    return w;
}

struct PsElement {
    std::string field;    // m0, m1, ...
    std::string base;     // upper semantic base, e.g. TEXCOORD
    uint32_t index = 0;
    D3D_NAME sv = D3D_NAME_UNDEFINED;
    std::string fieldType;
    int comps = 1;        // component count of the VS output element
};

class PsWrapperBuilder {
public:
    PsWrapperBuilder(const std::vector<Tok>& tk, const std::map<std::string, std::vector<Member>>& structs)
        : tk_(tk), structs_(structs) {}

    WrapperOut Build(const MmePass& pass, const std::string& fn, const std::vector<std::string>& args,
                     const std::vector<MmeSigElement>& vsOut, bool objectMode) {
        WrapperOut w;
        const FuncInfo f = ParseFunction(tk_, fn, (int)args.size());
        if (f.status == 0) { w.error = "pixel function not found: " + fn; return w; }
        if (f.status == 2) { w.error = "ambiguous pixel function: " + fn; return w; }

        // The user PS's colour output (the SV_Target / COLOR0 expression) is used both for object mode's host MRT
        // and for the alpha test's clip(). Compute it unconditionally.
        int userColors = 0;
        bool userDepth = false;
        std::string colorExpr;
        if (f.retType != "void") {
            const std::string rt = StripStructPrefix(f.retType);
            if (IsStruct(rt)) {
                for (const Member& m : structs_.at(rt)) {
                    std::string b; uint32_t ix; SplitSemantic(m.semantic, b, ix);
                    if (b == "COLOR" || b == "SV_TARGET") { ++userColors; colorExpr = "__ret." + m.name; }
                    else if (b == "DEPTH" || b == "SV_DEPTH") userDepth = true;
                }
            } else {
                std::string b; uint32_t ix; SplitSemantic(f.retSemantic, b, ix);
                if (b == "DEPTH" || b == "SV_DEPTH") userDepth = true;
                else { ++userColors; colorExpr = "__ret"; }
            }
        } else {
            for (const ParamInfo& p : f.params) {
                if (p.uniform || !p.isOut) continue;
                std::string b; uint32_t ix; SplitSemantic(p.semantic, b, ix);
                if (b == "COLOR" || b == "SV_TARGET") { ++userColors; colorExpr = p.name; }
                else if (b == "DEPTH" || b == "SV_DEPTH") userDepth = true;
            }
        }
        const bool hostMrt = objectMode && userColors == 1 && !userDepth;
        w.badMrt = objectMode && !hostMrt;

        // Alpha test (MME design 2.3): AlphaTestEnable + AlphaRef (0..255) + AlphaFunc -> clip() on the user PS's
        // colour alpha. Clip discards where the test fails.
        std::string alphaClip;
        if (PassBool(pass, "ALPHATESTENABLE", false) && !colorExpr.empty()) {
            const std::string op = AlphaTestOp(PassState(pass, "ALPHAFUNC") ? *PassState(pass, "ALPHAFUNC") : std::string());
            if (!op.empty()) {
                const std::string* refRaw = PassState(pass, "ALPHAREF");
                float ref01 = 0.0f;
                if (refRaw) {
                    try {
                        ref01 = std::stof(Trim(*refRaw)) / 255.0f;
                    } catch (...) {
                        ref01 = 0.0f;
                    }
                }
                alphaClip = "    clip((" + FloatLit(ref01) + " " + op + " (" + colorExpr + ").a) ? 1.0 : -1.0);\n";
                w.alphaTest = true;
            }
        }

        // input struct from the VS output signature (register order)
        std::vector<MmeSigElement> sorted = vsOut;
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const MmeSigElement& a, const MmeSigElement& b) { return a.reg < b.reg; });
        std::string structDecl = "struct __mmdx_ps_in {\n";
        std::string posField;
        for (size_t i = 0; i < sorted.size(); ++i) {
            PsElement e;
            e.field = "m" + std::to_string(i);
            e.sv = sorted[i].sv;
            e.fieldType = FloatType(std::max(1, MaskCount(sorted[i].mask)));
            e.comps = std::max(1, MaskCount(sorted[i].mask));
            SplitSemantic(sorted[i].semantic, e.base, e.index);
            if (e.index == 0 && sorted[i].index != 0) e.index = sorted[i].index;
            elems_.push_back(e);
            std::string sem;
            if (sorted[i].sv == D3D_NAME_POSITION) { sem = "SV_Position"; posField = e.field; }
            else sem = SemiIndex(sorted[i].semantic.empty() ? "TEXCOORD" : sorted[i].semantic, sorted[i].index);
            structDecl += "    " + e.fieldType + " " + e.field + " : " + sem + ";\n";
        }
        structDecl += "};\n";
        if (!sorted.empty()) structsOut_ = structDecl;
        posField_ = posField;

        // entry
        std::vector<std::string> entryParams;
        std::vector<std::string> pre, post, callArgs;
        for (const ParamInfo& p : f.params) {
            if (p.uniform) {
                if (argIdx_ < args.size()) callArgs.push_back(args[argIdx_++]);
                else if (p.hasDefault) callArgs.push_back(p.defaultVal);
                else { w.error = "missing compile argument for uniform parameter '" + p.name + "'"; return w; }
                continue;
            }
            const std::string typeName = StripStructPrefix(p.type);
            if (!p.isOut && p.semantic.empty() && IsStruct(typeName)) {
                const std::string local = p.name + "__v";
                if (!BuildStructInit(typeName, local, pre)) { w.error = "cannot build struct " + typeName; return w; }
                callArgs.push_back(local);
                continue;
            }
            if (p.type.empty() || p.name.empty()) { w.error = "unparsable parameter in " + fn; return w; }
            const std::string sem = p.semantic.empty() ? "" : (" : " + p.semantic);
            if (p.isOut && !p.isIn) {
                // an out parameter (COLORn / DEPTH / TEXCOORDn) is a wrapper output
                entryParams.push_back("out " + p.type + " " + p.name + sem);
                callArgs.push_back(p.name);
            } else if (p.isOut && p.isIn) {
                const std::string loc = p.name + "__loc", outName = p.name + "__o";
                pre.push_back(p.type + " " + loc + " = " + ExprForSemantic(p.semantic, p.type) + ";");
                post.push_back(outName + " = " + loc + ";");
                entryParams.push_back("out " + p.type + " " + outName + sem);
                callArgs.push_back(loc);
            } else {
                // varying input with a semantic
                callArgs.push_back(ExprForSemantic(p.semantic, p.type));
            }
        }
        if (needFace_) entryParams.push_back("bool __ff : SV_IsFrontFace");

        std::string call = fn + "(";
        for (size_t i = 0; i < callArgs.size(); ++i) { if (i) call += ", "; call += callArgs[i]; }
        call += ")";

        std::string s;
        s += "// MMDX12 MME wrapper (pixel)\n";
        if (objectMode) { s += HostCbufferText(); s += OctEncodeText(); }
        s += structsOut_;

        // entry signature
        std::string entry = "__mmdx_ps(";
        if (!sorted.empty()) entry += "__mmdx_ps_in IN";
        for (size_t i = 0; i < entryParams.size(); ++i) {
            if (i || !sorted.empty()) entry += ", ";
            entry += entryParams[i];
        }
        entry += ")";

        const std::string curField = FieldFor("TEXCOORD", 13);
        const std::string prvField = FieldFor("TEXCOORD", 14);
        const std::string nrmField = FieldFor("TEXCOORD", 15);
        const bool canHost = objectMode && hostMrt && !curField.empty() && !prvField.empty() && !nrmField.empty();
        if (objectMode && hostMrt && !canHost) w.badMrt = true;

        if (canHost) {
            s += "struct __mmdx_ps_out {\n";
            s += "    float4 c0 : SV_Target0;\n";
            s += "    float4 c1 : SV_Target1;\n";
            s += "    float4 c2 : SV_Target2;\n";
            s += "};\n";
            s += "__mmdx_ps_out " + entry + " {\n";
            for (const std::string& l : pre) s += "    " + l + "\n";
            if (f.retType == "void") {
                s += "    " + call + ";\n";
                for (const std::string& l : post) s += "    " + l + "\n";
            } else {
                s += "    " + StripStructPrefix(f.retType) + " __ret = " + call + ";\n";
                for (const std::string& l : post) s += "    " + l + "\n";
            }
            s += "    float4 __c = " + colorExpr + ";\n";
            s += alphaClip;
            s += "    __mmdx_ps_out __o;\n";
            // The effect's own alpha: it is the blend factor of the pass's alpha / additive blending (particles,
            // floors, sprites). Forcing 1 here made every translucent object draw opaque.
            s += "    __o.c0 = (gMmdxParams.y != 0) ? float4(__mmdx_srgb_to_linear(saturate(__c.rgb)) * gMmdxParams.x, __c.a) : __c;\n";
            s += "    float3 __n = normalize(IN." + nrmField + ".xyz);\n";
            s += "    __o.c1 = float4(__mmdx_oct_encode(__n), -1.0, 1.0);\n";
            s += "    float2 __uvc = IN." + curField + ".xy / IN." + curField + ".w * float2(0.5, -0.5) + 0.5;\n";
            s += "    float2 __uvp = IN." + prvField + ".xy / IN." + prvField + ".w * float2(0.5, -0.5) + 0.5;\n";
            s += "    __o.c2 = float4(__uvc - __uvp, 0, 0);\n";
            s += "    return __o;\n";
            s += "}\n";
        } else {
            s += f.retType + " " + entry;
            if (f.retType != "void" && !f.retSemantic.empty()) s += " : " + f.retSemantic;
            s += " {\n";
            for (const std::string& l : pre) s += "    " + l + "\n";
            if (f.retType == "void") {
                s += "    " + call + ";\n";
                for (const std::string& l : post) s += "    " + l + "\n";
                s += alphaClip;
            } else {
                s += "    " + f.retType + " __ret = " + call + ";\n";
                for (const std::string& l : post) s += "    " + l + "\n";
                s += alphaClip;
                s += "    return __ret;\n";
            }
            s += "}\n";
        }
        w.text = std::move(s);
        w.usesVpos = usesVpos_;
        w.usesVface = needFace_;
        w.badMrt = w.badMrt || (objectMode && !canHost);
        return w;
    }

private:
    bool IsStruct(const std::string& name) const { return structs_.count(name) != 0; }

    // Field name of the PS input element with this semantic base + index (empty when absent).
    std::string FieldFor(const std::string& base, uint32_t index) const {
        for (const PsElement& e : elems_)
            if (e.base == base && e.index == index) return e.field;
        return {};
    }

    const PsElement* Find(const std::string& raw) const {
        std::string base;
        uint32_t index = 0;
        SplitSemantic(raw, base, index);
        if (base == "POSITION" || base == "SV_POSITION") {
            for (const PsElement& e : elems_)
                if (e.sv == D3D_NAME_POSITION) return &e;
            return nullptr;
        }
        for (const PsElement& e : elems_)
            if (e.base == base && e.index == index) return &e;
        return nullptr;
    }

    static int TypeComps(const std::string& type) {
        const std::string t = Lower(type);
        if (t == "float2" || t == "int2" || t == "uint2" || t == "half2" || t == "bool2") return 2;
        if (t == "float3" || t == "int3" || t == "uint3" || t == "half3" || t == "bool3") return 3;
        if (t == "float4" || t == "int4" || t == "uint4" || t == "half4" || t == "bool4") return 4;
        return 1;
    }

    // Width-coerces an element field to the user parameter's type (D3D9 filled missing components with 0).
    std::string Coerce(const PsElement& e, const std::string& targetType) {
        const int tc = TypeComps(targetType);
        const int ec = e.comps;
        const std::string field = "IN." + e.field;
        if (tc == ec) return field;
        if (tc < ec) return field + "." + std::string("xyzw").substr(0, (size_t)tc);
        std::string s = targetType + "(" + field;
        if (ec > 1) s += "." + std::string("xyzw").substr(0, (size_t)ec);
        for (int i = ec; i < tc; ++i) s += ", 0";
        s += ")";
        return s;
    }

    std::string ExprForSemantic(const std::string& semantic, const std::string& type) {
        std::string base;
        uint32_t index = 0;
        SplitSemantic(semantic, base, index);
        if (base == "VPOS") {
            usesVpos_ = true;
            if (posField_.empty()) return "(" + type + ")0";
            if (type == "float2") return "(" + posField_ + ".xy - 0.5)";
            if (type == "float3") return "float3(" + posField_ + ".xy - 0.5, 0)";
            if (type == "float4") return "float4(" + posField_ + ".xy - 0.5, 0, 1)";
            return "(" + type + ")(" + posField_ + ".xy - 0.5)";
        }
        if (base == "VFACE") {
            needFace_ = true;
            return "(" + type + ")(__ff ? 1.0 : -1.0)";
        }
        const PsElement* e = Find(semantic);
        if (e) return Coerce(*e, type);
        return "(" + type + ")0";
    }

    bool BuildStructInit(const std::string& structName, const std::string& outName, std::vector<std::string>& lines) {
        auto it = structs_.find(structName);
        if (it == structs_.end()) return false;
        lines.push_back(structName + " " + outName + " = (" + structName + ")0;");
        for (const Member& m : it->second) {
            const std::string mt = StripStructPrefix(m.type);
            if (m.semantic.empty() && IsStruct(mt)) {
                const std::string sub = outName + "_" + m.name;
                if (BuildStructInit(mt, sub, lines)) lines.push_back(outName + "." + m.name + " = " + sub + ";");
            } else if (!m.semantic.empty()) {
                lines.push_back(outName + "." + m.name + " = " + ExprForSemantic(m.semantic, m.type) + ";");
            }
        }
        return true;
    }

    const std::vector<Tok>& tk_;
    const std::map<std::string, std::vector<Member>>& structs_;
    std::vector<PsElement> elems_;
    std::string structsOut_, posField_;
    bool needFace_ = false, usesVpos_ = false;
    size_t argIdx_ = 0;
};

// ---- non-literal global initializer rewrite (-> static) ------------------------------------------------------------

int RewriteNonLiteralGlobals(std::string& src, const MmeEffectDesc& fx, std::vector<Tok>& tk) {
    std::set<std::string> targets;
    for (const MmeParam& p : fx.params)
        if (p.hasInit && !p.isStatic && p.init.kind == MmeValue::String) targets.insert(p.name);
    if (targets.empty()) return 0;

    std::vector<size_t> inserts;
    int depth = 0;
    for (size_t i = 0; i + 1 < tk.size(); ++i) {
        if (tk[i].t == "{") { ++depth; continue; }
        if (tk[i].t == "}") { if (depth > 0) --depth; continue; }
        if (depth != 0) continue;
        if (tk[i].k != TK::Id || !targets.count(tk[i].t)) continue;
        if (tk[i + 1].t != "=") continue;
        if (i == 0 || tk[i - 1].k != TK::Id) continue;
        inserts.push_back(tk[i - 1].pos);
    }
    std::sort(inserts.begin(), inserts.end());
    inserts.erase(std::unique(inserts.begin(), inserts.end()), inserts.end());
    for (auto it = inserts.rbegin(); it != inserts.rend(); ++it) src.insert(*it, "static ");
    return (int)inserts.size();
}

// ---- hashing / cache ----------------------------------------------------------------------------------------------

uint64_t Fnv1a(uint64_t h, const void* data, size_t n) {
    const auto* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::string KeyHex(const std::string& material) {
    uint64_t h1 = 1469598103934665603ull;
    uint64_t h2 = 1099511628211ull;
    h1 = Fnv1a(h1, material.data(), material.size());
    h2 = Fnv1a(h2, material.data(), material.size());
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", (unsigned long long)h1, (unsigned long long)h2);
    return std::string(buf);
}

std::string CompilerVersion() {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void*)&D3DCompile, &m))
        return "d3dcompiler:?";
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    const auto t = std::filesystem::last_write_time(path, ec);
    return std::string("d3dcompiler:") + std::to_string((unsigned long long)(ec ? 0 : sz)) + ":" +
           std::to_string((long long)t.time_since_epoch().count());
}

ComPtr<ID3DBlob> CacheLoadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamsize n = in.tellg();
    if (n <= 0) return {};
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3DCreateBlob((SIZE_T)n, &blob))) return {};
    in.seekg(0);
    if (!in.read((char*)blob->GetBufferPointer(), n)) return {};
    return blob;
}

void CacheStoreFile(const std::filesystem::path& path, ID3DBlob* blob) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return;
        out.write((const char*)blob->GetBufferPointer(), (std::streamsize)blob->GetBufferSize());
        if (!out) return;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

// ---- include handler (fallback path only) -------------------------------------------------------------------------

class MmeInclude : public ID3DInclude {
public:
    explicit MmeInclude(std::filesystem::path root) : root_(std::move(root)) {}

    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData,
                                   UINT* pBytes) override {
        std::filesystem::path base = root_;
        if (pParentData) {
            auto it = dirOf_.find(pParentData);
            if (it != dirOf_.end()) base = it->second;
        }
        std::filesystem::path resolved;
        if (!mmetext::ResolveRel(base, pFileName ? pFileName : "", resolved)) return E_FAIL;
        auto buf = std::make_unique<std::string>();
        if (!mmetext::ReadTextFileText(resolved, *buf)) return E_FAIL;
        const void* p = buf->data();
        dirOf_[p] = resolved.parent_path();
        buffers_.push_back(std::move(buf));
        *ppData = p;
        *pBytes = (UINT)buffers_.back()->size();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }

private:
    std::filesystem::path root_;
    std::map<const void*, std::filesystem::path> dirOf_;
    std::vector<std::unique_ptr<std::string>> buffers_;
};

// ---- reflection ---------------------------------------------------------------------------------------------------

MmeSigElement MakeSig(const D3D11_SIGNATURE_PARAMETER_DESC& p) {
    MmeSigElement e;
    e.semantic = p.SemanticName ? p.SemanticName : "";
    e.index = p.SemanticIndex;
    e.reg = p.Register;
    e.mask = p.Mask;
    e.type = p.ComponentType;
    e.sv = p.SystemValueType;
    return e;
}

bool ReflectStage(ID3DBlob* blob, MmeStage& st) {
    if (!blob) return false;
    ComPtr<ID3D11ShaderReflection> refl;
    if (FAILED(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&refl))) || !refl) return false;
    D3D11_SHADER_DESC sd{};
    if (FAILED(refl->GetDesc(&sd))) return false;

    for (UINT i = 0; i < sd.ConstantBuffers; ++i) {
        ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
        if (!cb) continue;
        D3D11_SHADER_BUFFER_DESC bd{};
        if (FAILED(cb->GetDesc(&bd))) continue;
        if (!bd.Name || std::string(bd.Name) != "$Globals") continue;
        st.cbSize = bd.Size;
        for (UINT v = 0; v < bd.Variables; ++v) {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            if (!var) continue;
            D3D11_SHADER_VARIABLE_DESC vd{};
            if (FAILED(var->GetDesc(&vd))) continue;
            MmeCbVar cv;
            cv.name = vd.Name ? vd.Name : "";
            cv.offset = vd.StartOffset;
            cv.size = vd.Size;
            if (vd.DefaultValue && vd.Size)
                cv.defaultValue.assign((const uint8_t*)vd.DefaultValue, (const uint8_t*)vd.DefaultValue + vd.Size);
            if (ID3D11ShaderReflectionType* ty = var->GetType()) {
                D3D11_SHADER_TYPE_DESC td{};
                if (SUCCEEDED(ty->GetDesc(&td))) {
                    cv.rows = td.Rows;
                    cv.columns = td.Columns;
                    cv.elements = td.Elements;
                    cv.columnMajor = (td.Class == D3D_SVC_MATRIX_COLUMNS);
                    switch (td.Type) {
                        case D3D_SVT_FLOAT: cv.kind = MmeCbVar::Float; break;
                        case D3D_SVT_INT: cv.kind = MmeCbVar::Int; break;
                        case D3D_SVT_UINT: cv.kind = MmeCbVar::UInt; break;
                        case D3D_SVT_BOOL: cv.kind = MmeCbVar::Bool; break;
                        default: cv.kind = MmeCbVar::Other; break;
                    }
                }
            }
            st.cb.push_back(std::move(cv));
        }
    }

    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        if (FAILED(refl->GetResourceBindingDesc(i, &bd))) continue;
        MmeResBinding rb;
        rb.name = bd.Name ? bd.Name : "";
        rb.slot = bd.BindPoint;
        rb.dim = bd.Dimension;
        if (bd.Type == D3D_SIT_TEXTURE) rb.kind = MmeResBinding::Texture;
        else if (bd.Type == D3D_SIT_SAMPLER) rb.kind = MmeResBinding::Sampler;
        else continue;
        st.res.push_back(std::move(rb));
    }

    for (UINT i = 0; i < sd.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{};
        if (SUCCEEDED(refl->GetInputParameterDesc(i, &p))) st.inputs.push_back(MakeSig(p));
    }
    for (UINT i = 0; i < sd.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{};
        if (SUCCEEDED(refl->GetOutputParameterDesc(i, &p))) st.outputs.push_back(MakeSig(p));
    }
    return true;
}

// ---- FXC error parsing --------------------------------------------------------------------------------------------

void AppendErrorDiags(std::vector<MmeDiag>& out, const std::string& text, const std::vector<LineOrigin>& origins,
                      const std::filesystem::path& root) {
    int kept = 0;
    size_t start = 0;
    while (start <= text.size() && kept < 5) {
        const size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t err = line.find("error");
        if (err != std::string::npos) {
            MmeDiag d;
            d.sev = MmeDiag::Error;
            const size_t open = line.rfind('(', err);
            int lineNum = 0;
            if (open != std::string::npos) {
                const size_t close = line.find(')', open);
                std::string inside = close == std::string::npos ? line.substr(open + 1) : line.substr(open + 1, close - open - 1);
                const size_t comma = inside.find(',');
                if (comma != std::string::npos) inside = inside.substr(0, comma);
                lineNum = std::atoi(Trim(inside).c_str());
            }
            if (!origins.empty() && lineNum > 0 && lineNum <= (int)origins.size()) {
                d.file = origins[(size_t)lineNum - 1].file;
                d.line = origins[(size_t)lineNum - 1].line;
            } else {
                d.file = root;
                d.line = lineNum;
            }
            d.message = Trim(line.substr(err));
            out.push_back(std::move(d));
            ++kept;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

// ---- one stage compile --------------------------------------------------------------------------------------------

HRESULT DoCompile(const std::string& src, const char* srcName, ID3DInclude* inc, const char* entry, const char* target,
                  const std::vector<D3D_SHADER_MACRO>& macros, ComPtr<ID3DBlob>& out, ComPtr<ID3DBlob>& errs) {
    const UINT flags = D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    return D3DCompile(src.data(), src.size(), srcName, macros.data(), inc, entry, target, flags, 0, &out, &errs);
}

} // namespace

// -------------------------------------------------------------------------------------------------------------------

namespace {

std::string SerializeDefines(const std::vector<std::pair<std::string, std::string>>& defs) {
    std::string s;
    for (const auto& [k, v] : defs) { s += k; s += '='; s += v; s += ';'; }
    return s;
}

MmeProgram CompileProgramImpl(const MmeEffectDesc& fx, const MmeTechnique& t, const MmePass& p,
                              const MmeCompileOptions& optIn, std::string* dump) {
    MmeProgram prog;
    prog.technique = t.name;
    prog.pass = p.name;

    if (p.vs.isAsm || p.ps.isAsm) {
        prog.ok = false;
        prog.diags.push_back({MmeDiag::Error, fx.file, p.line, "asm shaders unsupported"});
        return prog;
    }

    const bool hasVs = !p.vs.isNull && !p.vs.function.empty();
    const bool hasPs = !p.ps.isNull && !p.ps.function.empty();

    // Object mode applies only to the object / object_ss / edge MMDPass families (docs/mme/mme_design.md §2.6).
    const std::string mp = Lower(t.mmdPass);
    const bool objectMode = optIn.objectMode && (mp == "object" || mp == "object_ss" || mp == "edge");
    prog.objectMode = objectMode;

    MmeCompileOptions opt = optIn;
    if (opt.cacheDir.empty()) opt.cacheDir = ExecutableDir() / L"shader_cache" / L"mme";
    std::vector<std::pair<std::string, std::string>> defList;
    for (const auto& [k, v] : opt.defines) defList.push_back({k, v});
    auto hasDef = [&](const char* n) {
        for (const auto& [k, v] : defList) if (k == n) return true;
        return false;
    };
    if (!hasDef("MMDX12")) defList.push_back({"MMDX12", "1"});
    if (!hasDef("MME_VERSION")) defList.push_back({"MME_VERSION", "37"});
    std::vector<D3D_SHADER_MACRO> macros;
    for (const auto& [k, v] : defList) macros.push_back({k.c_str(), v.c_str()});
    macros.push_back({nullptr, nullptr});
    const std::string definesSer = SerializeDefines(defList);
    const std::string dllVer = CompilerVersion();
    const std::filesystem::path cacheDir = opt.cacheDir;
    const std::string srcName = PathToUtf8(fx.file);

    // source: blank technique / asm blocks, optionally rewrite non-literal globals
    std::string blanked = BlankRanges(fx.expandedSource, FindBlankRanges(Tokenize(fx.expandedSource)));
    StripLineDirectives(blanked);
    {
        std::vector<Tok> tk0 = Tokenize(blanked);
        const int n = RewriteNonLiteralGlobals(blanked, fx, tk0);
        if (n > 0) g_mmeStaticFixups.fetch_add((uint32_t)n, std::memory_order_relaxed);
    }
    std::vector<Tok> tk = Tokenize(blanked);
    const std::map<std::string, std::vector<Member>> structs = ParseStructs(tk);

    // optional fallback source (the original root file text, includes resolved by the handler)
    std::string rootText;
    bool haveFallback = false;
    if (fx.ok && !fx.file.empty()) haveFallback = mmetext::ReadTextFileText(fx.file, rootText);

    // origins are needed only for diagnostics; recompute lazily from M1's preprocessor
    std::vector<LineOrigin> origins;
    bool originsTried = false;
    auto getOrigins = [&]() -> const std::vector<LineOrigin>& {
        if (!originsTried) {
            originsTried = true;
            if (fx.ok && !fx.file.empty()) {
                MmePreprocessResult r = PreprocessMme(fx.file);
                if (r.ok) origins = r.lines;
            }
        }
        return origins;
    };

    auto compileStage = [&](const std::string& target, const char* entry, const std::string& wrapper,
                            MmeStage& stage, bool& ok) {
        ok = false;
        const std::string primary = blanked + "\n" + wrapper;
        const std::string material =
            wrapper + "\x01" + target + "\x01" + definesSer + "\x01" + dllVer + (objectMode ? "\x01OBJ2" : "") +
            (prog.alphaTest ? "\x01AT" : "");
        const std::string primaryKey = KeyHex(material + "\x01" + primary);
        const std::string fallbackKey = KeyHex(material + "\x01" "FB\x01" + rootText + "\x01" + wrapper);
        const std::string fileSuffix = target[0] == 'v' ? ".vs.dxbc" : ".ps.dxbc";

        ComPtr<ID3DBlob> blob;
        bool usedFallback = false;
        ComPtr<ID3DBlob> errBlob;

        if (opt.useCache) {
            blob = CacheLoadFile(cacheDir / (primaryKey + fileSuffix));
            if (blob) g_mmeCacheHits.fetch_add(1, std::memory_order_relaxed);
        }

        if (!blob) {
            g_mmeStageCompiles.fetch_add(1, std::memory_order_relaxed);
            ComPtr<ID3DBlob> errs;
            if (SUCCEEDED(DoCompile(primary, srcName.c_str(), nullptr, entry, target.c_str(), macros, blob,
                                    errs))) {
                if (opt.useCache && blob) CacheStoreFile(cacheDir / (primaryKey + fileSuffix), blob.Get());
            } else {
                if (haveFallback) {
                    // retry once from the original file text with an ID3DInclude
                    ComPtr<ID3DBlob> fbBlob;
                    if (opt.useCache) {
                        fbBlob = CacheLoadFile(cacheDir / (fallbackKey + fileSuffix));
                        if (fbBlob) g_mmeCacheHits.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (!fbBlob) {
                        MmeInclude inc(fx.file.parent_path());
                        ComPtr<ID3DBlob> fbErrs;
                        const std::string fbSrc = rootText + "\n" + wrapper;
                        g_mmeStageCompiles.fetch_add(1, std::memory_order_relaxed);
                        if (SUCCEEDED(DoCompile(fbSrc, srcName.c_str(), &inc, entry, target.c_str(), macros,
                                                fbBlob, fbErrs))) {
                            if (opt.useCache && fbBlob) CacheStoreFile(cacheDir / (fallbackKey + fileSuffix),
                                                                        fbBlob.Get());
                        }
                    }
                    if (fbBlob) { blob = fbBlob; usedFallback = true; }
                }
                // diagnostics come from the primary (expanded-source) attempt: it points at user code, while the
                // fallback's errors are about our own preprocessor divergence.
                errBlob = errs;
            }
        }

        if (!blob) {
            const char* msg = errBlob ? (const char*)errBlob->GetBufferPointer() : "";
            const SIZE_T len = errBlob ? errBlob->GetBufferSize() : 0;
            AppendErrorDiags(prog.diags, std::string(msg, len), getOrigins(), fx.file);
            return;
        }
        stage.code = blob;
        if (!ReflectStage(blob.Get(), stage)) {
            prog.diags.push_back({MmeDiag::Error, fx.file, p.line, std::string("reflection failed (") + target + ")"});
            return;
        }
        if (usedFallback) {
            prog.usedFallbackSource = true;
            prog.diags.push_back({MmeDiag::Warning, fx.file, p.line, "preprocessor divergence"});
        }
        ok = true;
    };

    if (dump) {
        *dump = "// primary source: <expandedSource blanked, " + std::to_string(blanked.size()) + " bytes>\n";
    }

    bool vsOk = true, psOk = true;
    if (hasVs) {
        WrapperOut w = BuildVsWrapper(tk, p.vs.function, p.vs.args, objectMode, structs);
        if (!w.error.empty()) {
            prog.diags.push_back({MmeDiag::Error, fx.file, p.line, w.error});
            vsOk = false;
        } else {
            prog.usesVertexId = w.usesVertexId;
            if (dump) *dump += "\n// ---- VS wrapper ----\n" + w.text;
            compileStage("vs_5_0", "__mmdx_vs", w.text, prog.vs, vsOk);
        }
    }

    if (hasPs) {
        PsWrapperBuilder pb(tk, structs);
        WrapperOut w = pb.Build(p, p.ps.function, p.ps.args, prog.vs.outputs, objectMode);
        if (objectMode) prog.hostMrt = !w.badMrt;
        prog.alphaTest = w.alphaTest;
        if (!w.error.empty()) {
            prog.diags.push_back({MmeDiag::Error, fx.file, p.line, w.error});
            psOk = false;
        } else {
            prog.usesVpos = w.usesVpos;
            prog.usesVface = w.usesVface;
            if (dump) *dump += "\n// ---- PS wrapper ----\n" + w.text;
            compileStage("ps_5_0", "__mmdx_ps", w.text, prog.ps, psOk);
        }
    }

    if (hasPs) {
        for (const MmeSigElement& e : prog.ps.outputs) {
            if (e.sv == D3D_NAME_TARGET) ++prog.psOutputs;
            if (e.sv == D3D_NAME_DEPTH) prog.psWritesDepth = true;
        }
    }

    prog.ok = vsOk && psOk;
    return prog;
}

} // namespace

MmeProgram CompileMmePass(const MmeEffectDesc& fx, const MmeTechnique& t, const MmePass& p,
                          const MmeCompileOptions& opt) {
    return CompileProgramImpl(fx, t, p, opt, nullptr);
}

std::vector<MmeProgram> CompileMmeEffect(const MmeEffectDesc& fx, const MmeCompileOptions& opt) {
    std::vector<MmeProgram> out;
    std::map<std::string, MmeProgram> cache;
    for (const MmeTechnique& t : fx.techniques) {
        const std::string mp = Lower(t.mmdPass);
        const bool objTechnique = opt.objectMode && (mp == "object" || mp == "object_ss" || mp == "edge");
        for (const MmePass& p : t.passes) {
            auto keyOf = [](const MmeShaderRef& s) {
                std::string k = s.function;
                for (const std::string& a : s.args) { k += ','; k += a; }
                return k;
            };
            const std::string key = keyOf(p.vs) + "|" + keyOf(p.ps) + (objTechnique ? "\x01OBJ" : "") + AlphaTestKey(p);
            MmeProgram prog;
            auto it = cache.find(key);
            if (it != cache.end()) {
                prog = it->second;
            } else {
                prog = CompileMmePass(fx, t, p, opt);
                cache.emplace(key, prog);
            }
            prog.technique = t.name;
            prog.pass = p.name;
            out.push_back(std::move(prog));
        }
    }
    return out;
}

std::string DumpMmeWrapper(const MmeEffectDesc& fx, const MmeTechnique& t, const MmePass& p,
                           const MmeCompileOptions& opt, std::string* reason) {
    std::string dump;
    (void)CompileProgramImpl(fx, t, p, opt, &dump);
    if (reason) reason->clear();
    return dump;
}

uint32_t GetMmeCacheHitCount() { return g_mmeCacheHits.load(std::memory_order_relaxed); }
void ResetMmeCacheHitCount() { g_mmeCacheHits.store(0, std::memory_order_relaxed); }
uint32_t GetMmeStageCompileCount() { return g_mmeStageCompiles.load(std::memory_order_relaxed); }
void ResetMmeStageCompileCount() { g_mmeStageCompiles.store(0, std::memory_order_relaxed); }
uint32_t GetMmeStaticFixupCount() { return g_mmeStaticFixups.load(std::memory_order_relaxed); }
void ResetMmeStaticFixupCount() { g_mmeStaticFixups.store(0, std::memory_order_relaxed); }

} // namespace mmdx
