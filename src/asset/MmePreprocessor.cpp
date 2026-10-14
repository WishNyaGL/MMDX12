// MmePreprocessor.cpp: the light preprocessor for MME .fx files (see MmePreprocessor.h). Hand-rolled tokeniser +
// macro expander + recursive-descent #if evaluator; no third-party preprocessor and no whole-file regex.
#include "asset/MmePreprocessor.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>

#include "asset/MmeText.h"
#include "core/TextUtil.h"

namespace mmdx {
namespace {

using mmetext::IsIdentChar;
using mmetext::IsIdentStart;
using mmetext::Lower;
using mmetext::ReadTextFileText;
using mmetext::ResolveRel;
using mmetext::Trim;

constexpr int kMaxMacroDepth = 200;

// ---- tokens ----------------------------------------------------------------------------------------------

enum class Tk { Ws, Id, Num, Str, Punct, Other };

struct Tok {
    Tk k = Tk::Other;
    std::string t;
};

bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::vector<Tok> Tokenize(const std::string& s) {
    std::vector<Tok> out;
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if (IsWs(c)) {
            const size_t b = i;
            while (i < n && IsWs(s[i])) ++i;
            out.push_back({Tk::Ws, s.substr(b, i - b)});
            continue;
        }
        if (c == '"' || c == '\'') {
            const char q = c;
            const size_t b = i++;
            while (i < n && s[i] != q) {
                if (s[i] == '\\' && i + 1 < n) i += 2;
                else ++i;
            }
            if (i < n) ++i;
            out.push_back({Tk::Str, s.substr(b, i - b)});
            continue;
        }
        if (IsIdentStart(c)) {
            const size_t b = i;
            while (i < n && IsIdentChar(s[i])) ++i;
            out.push_back({Tk::Id, s.substr(b, i - b)});
            continue;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))) {
            const size_t b = i;
            while (i < n && (std::isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '_')) ++i;
            out.push_back({Tk::Num, s.substr(b, i - b)});
            continue;
        }
        if (c == '#' && i + 1 < n && s[i + 1] == '#') {   // token paste is one token, not two '#'
            out.push_back({Tk::Punct, "##"});
            i += 2;
            continue;
        }
        out.push_back({Tk::Punct, std::string(1, c)});
        ++i;
    }
    return out;
}

// Classifies a raw spelling so a pasted token keeps a sensible kind for later re-scan / macro lookup.
Tk KindOfSpelling(const std::string& s) {
    if (s.empty()) return Tk::Other;
    if (s.front() == '"' || s.front() == '\'') return Tk::Str;
    if (IsIdentStart((unsigned char)s.front())) return Tk::Id;
    if (std::isdigit((unsigned char)s.front()) ||
        (s.front() == '.' && s.size() > 1 && std::isdigit((unsigned char)s[1])))
        return Tk::Num;
    return Tk::Punct;
}

// True when `s` re-tokenises to exactly one token spelled `s` (a valid `##` paste result, e.g. `vs_3_0`).
bool IsSingleToken(const std::string& s) {
    if (s.empty()) return false;
    std::vector<Tok> ts = Tokenize(s);
    size_t nz = 0;
    for (const Tok& t : ts) {
        if (t.k == Tk::Ws) continue;
        ++nz;
        if (nz > 1) return false;
        if (t.t != s) return false;
    }
    return nz == 1;
}

std::string JoinTokens(const std::vector<Tok>& ts, size_t from = 0, size_t to = std::string::npos) {
    std::string s;
    const size_t e = std::min(to, ts.size());
    for (size_t i = from; i < e; ++i) s += ts[i].t;
    return s;
}

// ---- macro table -----------------------------------------------------------------------------------------

struct Macro {
    bool func = false;
    std::vector<std::string> params;
    std::vector<Tok> body;   // without the parameter list for function-like macros
    std::string bodyText;    // body with whitespace collapsed, for the exposed defines table
};

// ---- the preprocessor ------------------------------------------------------------------------------------

struct Preproc {
    std::filesystem::path root;
    std::filesystem::path rootDir;
    int maxDepth = 16;
    std::map<std::string, Macro> macros;                 // insertion-ordered enough via std::map for the table
    std::vector<std::string> includeStack;               // lowercased UTF-8 keys
    std::vector<std::filesystem::path> includes;         // first-seen order
    std::vector<MmeDiag> diags;
    std::vector<std::string> outLines;
    std::vector<LineOrigin> origins;
    bool ok = false;

    void Diag(MmeDiag::Severity sev, const std::filesystem::path& f, int line, std::string msg) {
        diags.push_back({sev, f, line, std::move(msg)});
    }

    void Emit(const std::string& text, const std::filesystem::path& file, int line) {
        outLines.push_back(text);
        origins.push_back({file, line});
    }

    // ---- macro expansion ---------------------------------------------------------------------------------

    std::vector<Tok> Expand(const std::vector<Tok>& in, const std::set<std::string>& hidden, int depth);
    std::vector<Tok> Substitute(const Macro& m, const std::vector<std::vector<Tok>>& args,
                                const std::vector<std::string>& rawArgs, const std::set<std::string>& hidden,
                                int depth);
    std::vector<Tok> ExpandString(const std::string& s, const std::set<std::string>& hidden, int depth) {
        return Expand(Tokenize(s), hidden, depth);
    }

    bool IsHidden(const std::set<std::string>& h, const std::string& name) { return h.count(name) != 0; }

    // ---- directives --------------------------------------------------------------------------------------

    void ProcessFile(const std::filesystem::path& file);
    void ProcessLine(const std::vector<Tok>& tokens, const std::string& text, const std::filesystem::path& file,
                     int line);
    void DoDefine(const std::string& rest, const std::filesystem::path& file, int line);
    bool EvalCondition(const std::string& expr, const std::filesystem::path& file, int line);
};

// Token stream macro expansion. `hidden` disables the named macros (the "blue paint" rule) during this expansion
// only; it is copied into nested expansions, not mutated globally.
std::vector<Tok> Preproc::Expand(const std::vector<Tok>& in, const std::set<std::string>& hidden, int depth) {
    std::vector<Tok> out;
    if (depth > kMaxMacroDepth) return in;
    const size_t n = in.size();
    size_t i = 0;
    while (i < n) {
        const Tok& t = in[i];
        if (t.k == Tk::Id) {
            const auto it = macros.find(t.t);
            if (it != macros.end() && !IsHidden(hidden, t.t)) {
                const Macro& m = it->second;
                if (m.func) {
                    // function-like: only expands when immediately followed by '('
                    size_t j = i + 1;
                    while (j < n && in[j].k == Tk::Ws) ++j;
                    if (j < n && in[j].k == Tk::Punct && in[j].t == "(") {
                        // collect arguments
                        std::vector<std::vector<Tok>> args;
                        std::vector<std::string> rawArgs;
                        std::vector<Tok> cur;
                        size_t k = j + 1;
                        bool closed = false;
                        int brace = 1;
                        for (; k < n; ++k) {
                            const Tok& a = in[k];
                            if (a.k == Tk::Punct && (a.t == "(" || a.t == "[")) ++brace;
                            else if (a.k == Tk::Punct && (a.t == ")" || a.t == "]")) {
                                if (a.t == ")" && brace == 1) { closed = true; break; }
                                --brace;
                            }
                            if (a.k == Tk::Punct && a.t == "," && brace == 1) {
                                rawArgs.push_back(Trim(JoinTokens(cur)));
                                args.push_back(cur);
                                cur.clear();
                                continue;
                            }
                            cur.push_back(a);
                        }
                        if (!closed) { out.push_back(t); ++i; continue; }
                        // a trailing empty arg is not counted for a zero-parameter macro
                        if (!(cur.empty() && rawArgs.empty() && m.params.empty())) {
                            rawArgs.push_back(Trim(JoinTokens(cur)));
                            args.push_back(cur);
                        }
                        if (args.size() != m.params.size()) {
                            // tolerant: emit the identifier and keep scanning inside the parentheses
                            out.push_back(t);
                            ++i;
                            continue;
                        }
                        std::set<std::string> h2 = hidden;
                        h2.insert(t.t);
                        std::vector<Tok> sub = Substitute(m, args, rawArgs, hidden, depth);
                        std::vector<Tok> ex = Expand(sub, h2, depth + 1);
                        out.insert(out.end(), ex.begin(), ex.end());
                        i = k + 1;
                        continue;
                    }
                    out.push_back(t);
                    ++i;
                    continue;
                }
                // object-like
                std::set<std::string> h2 = hidden;
                h2.insert(t.t);
                std::vector<Tok> ex = Expand(m.body, h2, depth + 1);
                out.insert(out.end(), ex.begin(), ex.end());
                ++i;
                continue;
            }
        }
        out.push_back(t);
        ++i;
    }
    return out;
}

std::vector<Tok> Preproc::Substitute(const Macro& m, const std::vector<std::vector<Tok>>& args,
                                     const std::vector<std::string>& rawArgs, const std::set<std::string>& hidden,
                                     int depth) {
    // Pre-expand each argument (except where # or ## needs the raw form).
    std::vector<std::vector<Tok>> expanded(args.size());
    for (size_t a = 0; a < args.size(); ++a) expanded[a] = Expand(args[a], hidden, depth + 1);

    auto paramIndex = [&](const std::string& name) -> int {
        for (size_t p = 0; p < m.params.size(); ++p)
            if (m.params[p] == name) return (int)p;
        return -1;
    };
    // The raw spelling of a macro-body token, substituting a parameter with its unexpanded argument (C rules).
    auto rawSpelling = [&](const Tok& t) -> std::string {
        if (t.k == Tk::Id) {
            const int p = paramIndex(t.t);
            if (p >= 0) return Trim(rawArgs[(size_t)p]);
        }
        return t.t;
    };

    std::vector<Tok> out;
    const std::vector<Tok>& b = m.body;
    for (size_t i = 0; i < b.size(); ++i) {
        const Tok& t = b[i];
        // stringify: '#' followed by a parameter name -> a string literal of the unexpanded argument
        if (t.k == Tk::Punct && t.t == "#") {
            size_t j = i + 1;
            while (j < b.size() && b[j].k == Tk::Ws) ++j;
            if (j < b.size() && b[j].k == Tk::Id) {
                const int p = paramIndex(b[j].t);
                if (p >= 0) {
                    std::string s = "\"";
                    for (char c : rawArgs[(size_t)p]) {
                        if (c == '"' || c == '\\') s += '\\';
                        s += c;
                    }
                    s += '"';
                    out.push_back({Tk::Str, s});
                    i = j;
                    continue;
                }
            }
            out.push_back(t);
            continue;
        }
        if (t.k == Tk::Id) {
            const int p = paramIndex(t.t);
            if (p >= 0) {
                // a parameter adjacent to '##' contributes its unexpanded argument; otherwise its expansion
                const bool pasteAfter = (i + 1 < b.size() && b[i + 1].k == Tk::Punct && b[i + 1].t == "##");
                if (pasteAfter) {
                    const std::string raw = Trim(rawArgs[(size_t)p]);
                    if (raw.empty()) continue;   // placemarker
                    out.push_back({KindOfSpelling(raw), raw});
                    continue;
                }
                out.insert(out.end(), expanded[(size_t)p].begin(), expanded[(size_t)p].end());
                continue;
            }
            out.push_back(t);
            continue;
        }
        // paste: '##' concatenates the spellings of its left and right operands, then re-scans as one token
        if (t.k == Tk::Punct && t.t == "##") {
            size_t j = i + 1;
            while (j < b.size() && b[j].k == Tk::Ws) ++j;
            if (j >= b.size()) continue;   // a trailing '##' has nothing to paste
            const std::string rhs = rawSpelling(b[j]);
            if (rhs.empty()) {   // placemarker on the right: nothing to paste
                i = j;
                continue;
            }
            // drop trailing whitespace so the left operand is adjacent to '##'
            while (!out.empty() && out.back().k == Tk::Ws) out.pop_back();
            if (out.empty()) {   // placemarker on the left: the result is just the right spelling
                out.push_back({KindOfSpelling(rhs), rhs});
                i = j;
                continue;
            }
            const std::string combined = out.back().t + rhs;
            if (IsSingleToken(combined)) {
                out.back() = {KindOfSpelling(combined), combined};
            } else {
                // not a single token: keep both spellings separated by one space (MSVC / FXC behaviour)
                out.push_back({Tk::Ws, " "});
                out.push_back({KindOfSpelling(rhs), rhs});
            }
            i = j;
            continue;
        }
        out.push_back(t);
    }
    return out;
}

void Preproc::DoDefine(const std::string& restIn, const std::filesystem::path& file, int line) {
    const std::string rest = restIn;
    size_t i = 0;
    while (i < rest.size() && IsWs(rest[i])) ++i;
    if (i >= rest.size() || !IsIdentStart(rest[i])) {
        Diag(MmeDiag::Warning, file, line, "#define with no macro name");
        return;
    }
    const size_t ns = i;
    while (i < rest.size() && IsIdentChar(rest[i])) ++i;
    const std::string name = rest.substr(ns, i - ns);
    Macro m;
    if (i < rest.size() && rest[i] == '(') {
        // function-like (no whitespace allowed between name and '(')
        m.func = true;
        ++i;
        std::string cur;
        for (; i < rest.size(); ++i) {
            const char c = rest[i];
            if (c == ')') { break; }
            if (c == ',') {
                const std::string p = Trim(cur);
                if (!p.empty()) m.params.push_back(p);
                cur.clear();
                continue;
            }
            cur += c;
        }
        const std::string p = Trim(cur);
        if (!p.empty()) m.params.push_back(p);
        if (i < rest.size()) ++i;   // skip ')'
    }
    std::string body = Trim(rest.substr(i));
    m.body = Tokenize(body);
    m.bodyText = Trim(JoinTokens(m.body));
    macros[name] = std::move(m);
}

// ---- #if expression evaluator ---------------------------------------------------------------------------

// Recursive-descent over a token list with C operator precedence; unknown identifiers are 0.
struct CondEval {
    std::vector<Tok> ts;
    size_t i = 0;
    bool err = false;

    bool Eof() const { return i >= ts.size(); }
    const Tok& Peek() const {
        static const Tok none;
        return i < ts.size() ? ts[i] : none;
    }
    bool IsPunct(const char* p) const {
        return i < ts.size() && ts[i].k == Tk::Punct && ts[i].t == p;
    }
    void Skip() { ++i; }

    long long Parse() {
        long long v = Ternary();
        return v;
    }
    long long Ternary() {
        long long c = LogicalOr();
        if (IsPunct("?")) {
            Skip();
            long long a = Ternary();
            if (IsPunct(":")) Skip();
            else err = true;
            long long b = Ternary();
            return c ? a : b;
        }
        return c;
    }
    long long LogicalOr() {
        long long v = LogicalAnd();
        while (IsPunct("|") && i + 1 < ts.size() && ts[i + 1].t == "|") {
            i += 2;
            long long r = LogicalAnd();
            v = (v || r) ? 1 : 0;
        }
        return v;
    }
    long long LogicalAnd() {
        long long v = BitOr();
        while (IsPunct("&") && i + 1 < ts.size() && ts[i + 1].t == "&") {
            i += 2;
            long long r = BitOr();
            v = (v && r) ? 1 : 0;
        }
        return v;
    }
    long long BitOr() {
        long long v = BitXor();
        // a single '|' (not part of '||') is bitwise or
        while (IsPunct("|") && !(i + 1 < ts.size() && ts[i + 1].t == "|")) { Skip(); v |= BitXor(); }
        return v;
    }
    long long BitXor() {
        long long v = BitAnd();
        while (IsPunct("^")) { Skip(); v ^= BitAnd(); }
        return v;
    }
    long long BitAnd() {
        long long v = Equality();
        // a single '&' (not part of '&&') is bitwise and
        while (IsPunct("&") && !(i + 1 < ts.size() && ts[i + 1].t == "&")) { Skip(); v &= Equality(); }
        return v;
    }
    long long Equality() {
        long long v = Relational();
        for (;;) {
            if (IsPunct("=") && i + 1 < ts.size() && ts[i + 1].t == "=") { i += 2; v = (v == Relational()) ? 1 : 0; }
            else if (IsPunct("!") && i + 1 < ts.size() && ts[i + 1].t == "=") { i += 2; v = (v != Relational()) ? 1 : 0; }
            else break;
        }
        return v;
    }
    long long Relational() {
        long long v = Shift();
        for (;;) {
            if (IsPunct("<") && i + 1 < ts.size() && ts[i + 1].t == "=") { i += 2; v = (v <= Shift()) ? 1 : 0; }
            else if (IsPunct(">") && i + 1 < ts.size() && ts[i + 1].t == "=") { i += 2; v = (v >= Shift()) ? 1 : 0; }
            else if (IsPunct("<")) { Skip(); v = (v < Shift()) ? 1 : 0; }
            else if (IsPunct(">")) { Skip(); v = (v > Shift()) ? 1 : 0; }
            else break;
        }
        return v;
    }
    long long Shift() {
        long long v = Additive();
        for (;;) {
            if (IsPunct("<") && i + 1 < ts.size() && ts[i + 1].t == "<") { i += 2; v <<= Additive(); }
            else if (IsPunct(">") && i + 1 < ts.size() && ts[i + 1].t == ">") { i += 2; v >>= Additive(); }
            else break;
        }
        return v;
    }
    long long Additive() {
        long long v = Mul();
        for (;;) {
            if (IsPunct("+")) { Skip(); v += Mul(); }
            else if (IsPunct("-")) { Skip(); v -= Mul(); }
            else break;
        }
        return v;
    }
    long long Mul() {
        long long v = Unary();
        for (;;) {
            if (IsPunct("*")) { Skip(); v *= Unary(); }
            else if (IsPunct("/")) { Skip(); long long r = Unary(); v = r ? v / r : 0; }
            else if (IsPunct("%")) { Skip(); long long r = Unary(); v = r ? v % r : 0; }
            else break;
        }
        return v;
    }
    long long Unary() {
        if (IsPunct("!")) { Skip(); return Unary() ? 0 : 1; }
        if (IsPunct("~")) { Skip(); return ~Unary(); }
        if (IsPunct("-")) { Skip(); return -Unary(); }
        if (IsPunct("+")) { Skip(); return Unary(); }
        return Primary();
    }
    long long Primary() {
        if (IsPunct("(")) {
            Skip();
            long long v = Ternary();
            if (IsPunct(")")) Skip();
            else err = true;
            return v;
        }
        if (Eof()) { err = true; return 0; }
        const Tok& t = Peek();
        if (t.k == Tk::Num) {
            Skip();
            std::string s = t.t;
            // strip integer suffixes
            while (!s.empty() && !std::isdigit((unsigned char)s.back()) &&
                   !(s.size() > 1 && (s.back() >= 'a' && s.back() <= 'f')))
                s.pop_back();
            try {
                if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                    return (long long)std::stoull(s.substr(2), nullptr, 16);
                return std::stoll(s, nullptr, 10);
            } catch (...) {
                return 0;
            }
        }
        if (t.k == Tk::Id) {
            // character literal encoded as an Id? no; unknown identifier = 0 (C rule)
            Skip();
            return 0;
        }
        if (t.k == Tk::Str) { Skip(); return 0; }
        Skip();
        return 0;
    }
};

bool Preproc::EvalCondition(const std::string& exprIn, const std::filesystem::path& file, int line) {
    // Replace `defined(X)` / `defined X` with 1/0 before macro expansion (operands must not be expanded).
    std::vector<Tok> toks = Tokenize(exprIn);
    std::vector<Tok> rep;
    for (size_t i = 0; i < toks.size(); ++i) {
        if (toks[i].k == Tk::Id && toks[i].t == "defined") {
            size_t j = i + 1;
            while (j < toks.size() && toks[j].k == Tk::Ws) ++j;
            bool haveParen = j < toks.size() && toks[j].k == Tk::Punct && toks[j].t == "(";
            if (haveParen) ++j;
            while (j < toks.size() && toks[j].k == Tk::Ws) ++j;
            std::string name;
            if (j < toks.size() && toks[j].k == Tk::Id) {
                name = toks[j].t;
                ++j;
            }
            while (j < toks.size() && toks[j].k == Tk::Ws) ++j;
            if (haveParen && j < toks.size() && toks[j].k == Tk::Punct && toks[j].t == ")") ++j;
            rep.push_back({Tk::Num, macros.count(name) ? "1" : "0"});
            i = j - 1;
            continue;
        }
        rep.push_back(toks[i]);
    }
    std::vector<Tok> expanded = Expand(rep, {}, 0);
    // drop whitespace
    std::vector<Tok> clean;
    for (const Tok& t : expanded)
        if (t.k != Tk::Ws) clean.push_back(t);
    if (clean.empty()) {
        Diag(MmeDiag::Warning, file, line, "#if / #elif with an empty expression");
        return false;
    }
    CondEval ev;
    ev.ts = clean;
    const long long v = ev.Parse();
    if (ev.err) Diag(MmeDiag::Warning, file, line, "#if / #elif expression could not be fully evaluated");
    return v != 0;
}

// ---- line processing -------------------------------------------------------------------------------------

void Preproc::ProcessLine(const std::vector<Tok>& tokensIn, const std::string& textIn, const std::filesystem::path& file,
                          int line) {
    std::string text = textIn;
    size_t p = 0;
    while (p < text.size() && IsWs(text[p])) ++p;
    if (p >= text.size()) {
        Emit("", file, line);
        return;
    }
    if (text[p] != '#') {
        std::vector<Tok> ex = Expand(tokensIn, {}, 0);
        Emit(JoinTokens(ex), file, line);
        return;
    }
    // directive
    size_t d = p + 1;
    while (d < text.size() && IsWs(text[d])) ++d;
    const size_t ds = d;
    while (d < text.size() && IsIdentChar(text[d])) ++d;
    const std::string dir = text.substr(ds, d - ds);
    const std::string rest = Trim(text.substr(d));
    if (dir == "define") { DoDefine(rest, file, line); return; }
    if (dir == "undef") {
        size_t q = 0;
        while (q < rest.size() && IsIdentChar(rest[q])) ++q;
        macros.erase(rest.substr(0, q));
        return;
    }
    if (dir == "error") {
        Diag(MmeDiag::Error, file, line, rest.empty() ? "#error" : rest);
        return;
    }
    if (dir == "pragma" || dir == "line" || dir.empty()) return;   // ignored, kept out of the output
    if (dir == "include") {
        std::string rel = Trim(rest);
        if (!rel.empty() && (rel[0] == '"' || rel[0] == '<')) {
            const char close = rel[0] == '"' ? '"' : '>';
            const size_t e = rel.find(close, 1);
            if (e != std::string::npos) rel = rel.substr(1, e - 1);
        }
        std::filesystem::path inc;
        if (ResolveRel(file.parent_path(), rel, inc) ||
            (!rootDir.empty() && ResolveRel(rootDir, rel, inc))) {
            if (std::find(includes.begin(), includes.end(), inc) == includes.end()) includes.push_back(inc);
            ProcessFile(inc);
        } else {
            Diag(MmeDiag::Error, file, line, "include not found: " + rel);
        }
        return;
    }
    Diag(MmeDiag::Warning, file, line, "unknown directive #" + dir);
}

// ---- per-file pass ---------------------------------------------------------------------------------------

// Splits into logical lines (after joining backslash continuations), strips comments. Newlines inside block
// comments are preserved as line breaks so origins stay aligned.
std::string StripCommentsKeepNewlines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if (c == '"' || c == '\'') {
            const char q = c;
            out += c;
            ++i;
            while (i < n && s[i] != q) {
                if (s[i] == '\\' && i + 1 < n) { out += s[i++]; out += s[i++]; continue; }
                out += s[i++];
            }
            if (i < n) out += s[i++];
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') out += '\n';
                ++i;
            }
            i = std::min(n, i + 2);
            out += ' ';
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// Joins `\`-continued lines; returns pairs {joinedText, firstPhysicalLine}.
std::vector<std::pair<std::string, int>> JoinContinuations(const std::string& s) {
    std::vector<std::pair<std::string, int>> out;
    std::string cur;
    int startLine = 1;
    int line = 1;
    size_t i = 0;
    const size_t n = s.size();
    auto flush = [&]() {
        out.push_back({cur, startLine});
        cur.clear();
    };
    while (i <= n) {
        if (i == n || s[i] == '\n') {
            // a trailing backslash (ignoring trailing whitespace) continues onto the next line
            std::string body = cur;
            size_t e = body.size();
            while (e > 0 && (body[e - 1] == ' ' || body[e - 1] == '\t' || body[e - 1] == '\r')) --e;
            if (e > 0 && body[e - 1] == '\\') {
                cur = body.substr(0, e - 1);
                // keep accumulating; do not advance startLine
            } else {
                flush();
                startLine = line + 1;
            }
            if (i == n) break;
            ++line;
            ++i;
            continue;
        }
        cur += s[i];
        ++i;
    }
    if (!cur.empty()) out.push_back({cur, startLine});
    return out;
}

void Preproc::ProcessFile(const std::filesystem::path& file) {
    const std::string key = Lower(PathToUtf8(file));
    if ((int)includeStack.size() >= maxDepth) {
        Diag(MmeDiag::Warning, file, 0, "include depth limit reached");
        return;
    }
    if (std::find(includeStack.begin(), includeStack.end(), key) != includeStack.end()) {
        Diag(MmeDiag::Warning, file, 0, "include cycle: " + PathToUtf8(file.filename()));
        return;
    }
    std::string raw;
    if (!ReadTextFileText(file, raw)) {
        Diag(MmeDiag::Error, file, 0, "cannot read effect file");
        return;
    }
    if (file == root) ok = true;
    // normalise line endings
    std::string norm;
    norm.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            if (i + 1 < raw.size() && raw[i + 1] == '\n') continue;
            norm += '\n';
        } else {
            norm += raw[i];
        }
    }
    const std::string clean = StripCommentsKeepNewlines(norm);
    const auto lines = JoinContinuations(clean);

    includeStack.push_back(key);
    // conditional stack: each entry records the enclosing state and whether any branch has been taken
    struct Cond { bool parentActive; bool active; bool anyTaken; bool seenElse; };
    std::vector<Cond> conds;
    auto active = [&]() { return conds.empty() || conds.back().active; };

    for (const auto& [text, physLine] : lines) {
        std::string t = text;
        size_t p = 0;
        while (p < t.size() && IsWs(t[p])) ++p;
        std::vector<Tok> toks = Tokenize(t);
        if (p < t.size() && t[p] == '#') {
            size_t d = p + 1;
            while (d < t.size() && IsWs(t[d])) ++d;
            const size_t ds = d;
            while (d < t.size() && IsIdentChar(t[d])) ++d;
            const std::string dir = t.substr(ds, d - ds);
            const std::string rest = Trim(t.substr(d));
            if (dir == "if" || dir == "ifdef" || dir == "ifndef") {
                bool parent = active();
                bool val = false;
                if (parent) {
                    if (dir == "ifdef") val = macros.count(Trim(rest)) != 0;
                    else if (dir == "ifndef") val = macros.count(Trim(rest)) == 0;
                    else val = EvalCondition(rest, file, physLine);
                }
                conds.push_back({parent, parent && val, parent && val, false});
                continue;
            }
            if (dir == "elif") {
                if (conds.empty()) { Diag(MmeDiag::Warning, file, physLine, "#elif without #if"); continue; }
                Cond& c = conds.back();
                if (c.seenElse) { Diag(MmeDiag::Warning, file, physLine, "#elif after #else"); continue; }
                bool val = false;
                if (c.parentActive && !c.anyTaken) val = EvalCondition(rest, file, physLine);
                c.active = c.parentActive && !c.anyTaken && val;
                c.anyTaken = c.anyTaken || c.active;
                continue;
            }
            if (dir == "else") {
                if (conds.empty()) { Diag(MmeDiag::Warning, file, physLine, "#else without #if"); continue; }
                Cond& c = conds.back();
                c.active = c.parentActive && !c.anyTaken;
                c.anyTaken = true;
                c.seenElse = true;
                continue;
            }
            if (dir == "endif") {
                if (conds.empty()) { Diag(MmeDiag::Warning, file, physLine, "#endif without #if"); continue; }
                conds.pop_back();
                continue;
            }
            if (!active()) continue;   // other directives in a dead branch are skipped
            ProcessLine(toks, t, file, physLine);
            continue;
        }
        if (!active()) {
            Emit("", file, physLine);   // keep the line so origins stay aligned with the physical file
            continue;
        }
        ProcessLine(toks, t, file, physLine);
    }
    includeStack.pop_back();
}

} // namespace

MmePreprocessResult PreprocessMme(const std::filesystem::path& fx, int maxIncludeDepth,
                                  const std::map<std::string, std::string>& predefines) {
    Preproc pp;
    pp.root = fx;
    pp.rootDir = fx.parent_path();
    pp.maxDepth = maxIncludeDepth > 0 ? maxIncludeDepth : 16;
    for (const auto& [name, value] : predefines) {
        Macro m;
        m.body = Tokenize(value);
        m.bodyText = Trim(value);
        pp.macros[name] = std::move(m);
    }
    pp.ProcessFile(fx);

    MmePreprocessResult r;
    r.ok = pp.ok;
    r.includes = pp.includes;
    r.diags = pp.diags;
    for (const auto& [name, m] : pp.macros) r.defines[name] = m.bodyText;
    r.lines = pp.origins;
    std::string text;
    for (size_t i = 0; i < pp.outLines.size(); ++i) {
        text += pp.outLines[i];
        if (i + 1 < pp.outLines.size()) text += '\n';
    }
    r.text = std::move(text);
    return r;
}

} // namespace mmdx
