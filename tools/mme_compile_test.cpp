// mme_compile_test: compile MME .fx effect files with M2's pass compiler (render/MmeCompiler) and report the pass /
// stage success rate, the fix-ups applied and the errors grouped by message. Directory arguments recurse over *.fx
// (case-insensitive). No window, no D3D device: FXC + reflection only.
//
//   mme_compile_test <file-or-dir>... [--no-cache] [--jobs N] [--json out.json] [--verbose]
//                     [--dump-wrapper <fx>] [--no-selftest] [--object-mode]
//
// Exit code 0 unless a self-test case fails (then 1).
#include "asset/MmeEffect.h"
#include "render/MmeCompiler.h"
#include "core/TextUtil.h"

#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace mmdx;

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool EndsWithFx(const std::filesystem::path& p) { return Lower(p.extension().string()) == ".fx"; }

// M5a: locate shaders/mme/standard.fx relative to the running executable (the build copies shaders next to it) or
// relative to this source's repository root.
std::filesystem::path FindStandardFx() {
    std::error_code ec;
    const std::filesystem::path exeDir = std::filesystem::current_path(ec);
    const std::filesystem::path candidates[] = {
        exeDir / "shaders" / "mme" / "standard.fx",
        exeDir / ".." / ".." / "shaders" / "mme" / "standard.fx",
        exeDir / ".." / ".." / ".." / "shaders" / "mme" / "standard.fx",
        exeDir / "shaders" / "mme" / "standard.fx",
    };
    for (const auto& c : candidates)
        if (std::filesystem::exists(c, ec)) return c;
    return {};
}

std::string UpperEnglish(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'A' + 'a');
    return s;
}

// Strips the leading path and "(line,col)" prefix from an FXC error line, leaving the message text.
std::string NormaliseError(const std::string& line) {
    std::string s = line;
    const size_t open = s.find("error ");
    if (open != std::string::npos) s = s.substr(open);
    return s;
}

struct Totals {
    std::atomic<int> effectsOk{0}, effectsPart{0}, effectsFail{0};
    std::atomic<int> programsOk{0}, programsFail{0};
    std::atomic<int> indexFixups{0}, vposFixups{0}, vfaceFixups{0}, staticFixups{0}, fallbackCount{0};
};

// ---- self test ---------------------------------------------------------------------------------------------------

struct SelfResult {
    std::string name;
    bool ok = false;
    std::string detail;
};

// Compiles a single inline effect and returns its programs (errors land in the effect's diags + program diags).
std::vector<MmeProgram> CompileInline(const std::string& src, const std::string& tag, MmeCompileOptions& opt,
                                      std::string& err) {
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("mme_compile_test_" + tag + ".fx");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << src;
    }
    MmeEffectDesc fx = LoadMmeEffect(tmp);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    if (!fx.ok) {
        err = "load failed";
        return {};
    }
    return CompileMmeEffect(fx, opt);
}

bool RunSelfTest(MmeCompileOptions& opt) {
    std::vector<SelfResult> results;
    auto add = [&](const std::string& name, bool ok, std::string detail = {}) {
        results.push_back({name, ok, std::move(detail)});
    };

    // 0. the built-in standard effect (M5a, shaders/mme/standard.fx): every program compiles, in object mode too.
    {
        std::filesystem::path std_ = FindStandardFx();
        if (!std_.empty()) {
            MmeEffectDesc fx = LoadMmeEffect(std_);
            if (!fx.ok) {
                add("standard.fx loads", false, "load failed");
            } else {
                auto progs = CompileMmeEffect(fx, opt);
                bool ok = !progs.empty();
                for (const MmeProgram& p : progs) ok = ok && p.ok;
                add("standard.fx all programs OK", ok,
                    ok ? "" : ("programs=" + std::to_string(progs.size())));
                MmeCompileOptions om = opt;
                om.objectMode = true;
                auto progsObj = CompileMmeEffect(fx, om);
                bool okObj = !progsObj.empty();
                for (const MmeProgram& p : progsObj) okObj = okObj && p.ok;
                add("standard.fx object mode OK", okObj,
                    okObj ? "" : ("programs=" + std::to_string(progsObj.size())));
            }
        } else {
            add("standard.fx found", false, "shaders/mme/standard.fx not found");
        }
    }

    // 1. compile-argument binding
    {
        const char* src =
            "float4 VS(float4 P : POSITION, uniform bool useT) : POSITION { return useT ? P : P.zxyw; }\n"
            "float4 PS(uniform bool useT) : COLOR { return useT ? float4(1,0,0,1) : float4(0,1,0,1); }\n"
            "technique T { pass P { VertexShader = compile vs_3_0 VS(true); PixelShader = compile ps_3_0 PS(true); } }\n";
        std::string e;
        auto progs = CompileInline(src, "argbind", opt, e);
        bool ok = !progs.empty() && progs[0].ok && progs[0].vs.code && progs[0].ps.code;
        add("compile args binding", ok, e);
    }
    // 2. _INDEX fix-up
    {
        const char* src =
            "float4 VS(float4 P : POSITION, int idx : _INDEX) : POSITION { return P + idx; }\n"
            "technique T { pass P { VertexShader = compile vs_3_0 VS(); } }\n";
        std::string e;
        auto progs = CompileInline(src, "index", opt, e);
        bool ok = !progs.empty() && progs[0].ok && progs[0].usesVertexId;
        add("_INDEX -> SV_VertexID", ok, e);
    }
    // 3. VPOS fix-up
    {
        const char* src =
            "float4 PS(float2 p : VPOS) : COLOR { return float4(p, 0, 1); }\n"
            "technique T { pass P { PixelShader = compile ps_3_0 PS(); } }\n";
        std::string e;
        auto progs = CompileInline(src, "vpos", opt, e);
        bool ok = !progs.empty() && progs[0].ok && progs[0].usesVpos;
        add("VPOS -> SV_Position-0.5", ok, e);
    }
    // 4. VFACE fix-up
    {
        const char* src =
            "float4 PS(float f : VFACE) : COLOR { return f > 0 ? float4(1,1,1,1) : float4(0,0,0,1); }\n"
            "technique T { pass P { PixelShader = compile ps_3_0 PS(); } }\n";
        std::string e;
        auto progs = CompileInline(src, "vface", opt, e);
        bool ok = !progs.empty() && progs[0].ok && progs[0].usesVface;
        add("VFACE -> SV_IsFrontFace", ok, e);
    }
    // 5. PS reads a subset of VS outputs, in a different order (must link: PS input sig == VS output regs)
    {
        const char* src =
            "struct VS_OUT { float4 Pos : POSITION; float2 T1 : TEXCOORD1; float2 T0 : TEXCOORD0; };\n"
            "VS_OUT VS(float4 P : POSITION, float2 uv : TEXCOORD0) { VS_OUT o; o.Pos = P; o.T0 = uv; o.T1 = 1 - uv; return o; }\n"
            "float4 PS(float2 a : TEXCOORD1) : COLOR { return float4(a, 0, 1); }\n"
            "technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }\n";
        std::string e;
        auto progs = CompileInline(src, "subset", opt, e);
        bool ok = false;
        if (!progs.empty() && progs[0].ok) {
            // PS input registers must match the VS output registers by semantic
            std::map<std::pair<std::string, uint32_t>, uint32_t> vsReg;
            for (const MmeSigElement& el : progs[0].vs.outputs) vsReg[{UpperEnglish(el.semantic), el.index}] = el.reg;
            ok = !progs[0].ps.inputs.empty();
            for (const MmeSigElement& el : progs[0].ps.inputs) {
                auto it = vsReg.find({UpperEnglish(el.semantic), el.index});
                if (it == vsReg.end() || it->second != el.reg) ok = false;
            }
        }
        add("PS reads subset of VS outputs", ok, e);
    }
    // 6. struct input + struct output
    {
        const char* src =
            "struct VS_IN { float4 P : POSITION; float3 N : NORMAL; };\n"
            "struct VS_OUT { float4 Pos : POSITION; float3 N : TEXCOORD0; };\n"
            "VS_OUT VS(VS_IN i) { VS_OUT o; o.Pos = i.P; o.N = i.N; return o; }\n"
            "float4 PS(float3 n : TEXCOORD0) : COLOR { return float4(n, 1); }\n"
            "technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }\n";
        std::string e;
        auto progs = CompileInline(src, "struct", opt, e);
        bool ok = !progs.empty() && progs[0].ok;
        add("struct input/output", ok, e);
    }
    // 7. MRT COLOR0/COLOR1
    {
        const char* src =
            "struct OUT { float4 c0 : COLOR0; float4 c1 : COLOR1; };\n"
            "OUT PS() { OUT o; o.c0 = 1; o.c1 = 0.5; return o; }\n"
            "technique T { pass P { PixelShader = compile ps_3_0 PS(); } }\n";
        std::string e;
        auto progs = CompileInline(src, "mrt", opt, e);
        bool ok = !progs.empty() && progs[0].ok && progs[0].psOutputs >= 2;
        add("MRT COLOR0/COLOR1", ok, e);
    }
    // 8. DEPTH output
    {
        const char* src =
            "struct OUT { float4 c : COLOR; float d : DEPTH; };\n"
            "OUT PS() { OUT o; o.c = 1; o.d = 0.5; return o; }\n"
            "technique T { pass P { PixelShader = compile ps_3_0 PS(); } }\n";
        std::string e;
        auto progs = CompileInline(src, "depth", opt, e);
        bool ok = !progs.empty() && progs[0].ok && progs[0].psWritesDepth;
        add("DEPTH output", ok, e);
    }
    // 9. NULL pixel shader
    {
        const char* src =
            "float4 VS(float4 P : POSITION) : POSITION { return P; }\n"
            "technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = NULL; } }\n";
        std::string e;
        auto progs = CompileInline(src, "nullps", opt, e);
        bool ok = !progs.empty() && progs[0].ok && progs[0].vs.code && !progs[0].ps.code;
        add("NULL pixel shader", ok, e);
    }
    // 10. explicit sampler register(s0) -> reflection slot 0
    {
        const char* src =
            "sampler S : register(s0);\n"
            "float4 PS(float2 uv : TEXCOORD0) : COLOR { return tex2D(S, uv); }\n"
            "technique T { pass P { PixelShader = compile ps_3_0 PS(); } }\n";
        std::string e;
        auto progs = CompileInline(src, "s0", opt, e);
        bool ok = false;
        if (!progs.empty() && progs[0].ok) {
            for (const MmeResBinding& r : progs[0].ps.res)
                if (r.name == "S" && r.slot == 0) ok = true;
        }
        add("sampler register(s0) slot", ok, e);
    }
    // 11. object mode, struct-returning VS (MMDPass defaults to "object"): the PS gets the host 3-target MRT.
    {
        const char* src =
            "struct VS_OUT { float4 Pos : POSITION; float3 N : TEXCOORD0; };\n"
            "VS_OUT VS(float4 P : POSITION, float3 N : NORMAL) { VS_OUT o; o.Pos = P; o.N = N; return o; }\n"
            "float4 PS(VS_OUT IN) : COLOR { return float4(IN.N, 1); }\n"
            "technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }\n";
        std::string e;
        MmeCompileOptions om = opt;
        om.objectMode = true;
        auto progs = CompileInline(src, "objstruct", om, e);
        bool ok = !progs.empty() && progs[0].ok && progs[0].objectMode && progs[0].hostMrt &&
                  progs[0].ps.outputs.size() >= 3 && progs[0].psOutputs >= 3;
        add("object mode: struct VS, 3 SV_Target PS", ok, e);
    }
    // 12. object mode, float4 : POSITION + out params
    {
        const char* src =
            "float4 VS(float4 P : POSITION, float3 N : NORMAL, out float2 uv : TEXCOORD0, out float3 n2 : TEXCOORD1)"
            " : POSITION { uv = P.xy; n2 = N; return P; }\n"
            "float4 PS(float2 uv : TEXCOORD0, float3 n : TEXCOORD1) : COLOR { return float4(n, uv.x); }\n"
            "technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }\n";
        std::string e;
        MmeCompileOptions om = opt;
        om.objectMode = true;
        auto progs = CompileInline(src, "objoutps", om, e);
        bool ok = !progs.empty() && progs[0].ok && progs[0].objectMode && progs[0].hostMrt &&
                  progs[0].ps.outputs.size() >= 3 && progs[0].psOutputs >= 3;
        add("object mode: POSITION+out VS, 3 SV_Target PS", ok, e);
    }
    // 13. object mode, struct-typed VS *input* (the POSITION / NORMAL fields live on the struct)
    {
        const char* src =
            "struct VS_IN { float4 Pos : POSITION; float3 Normal : NORMAL; float2 uv : TEXCOORD0; };\n"
            "struct VS_OUT { float4 Pos : POSITION; float2 uv : TEXCOORD0; float3 N : TEXCOORD1; };\n"
            "VS_OUT VS(VS_IN i) { VS_OUT o; o.Pos = i.Pos; o.uv = i.uv; o.N = i.Normal; return o; }\n"
            "float4 PS(VS_OUT IN) : COLOR { return float4(IN.N, 1); }\n"
            "technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }\n";
        std::string e;
        MmeCompileOptions om = opt;
        om.objectMode = true;
        auto progs = CompileInline(src, "objstructin", om, e);
        bool ok = !progs.empty() && progs[0].ok && progs[0].objectMode && progs[0].hostMrt &&
                  progs[0].ps.outputs.size() >= 3 && progs[0].psOutputs >= 3;
        add("object mode: struct VS input, 3 SV_Target PS", ok, e);
    }

    int passed = 0;
    for (const SelfResult& r : results) {
        std::printf("%s %s%s\n", r.ok ? "PASS" : "FAIL", r.name.c_str(),
                    r.ok || r.detail.empty() ? "" : (" (" + r.detail + ")").c_str());
        if (r.ok) ++passed;
    }
    std::printf("selftest: %d/%zu passed\n", passed, results.size());
    return passed == (int)results.size();
}

// ---- per-effect compilation --------------------------------------------------------------------------------------

struct EffectResult {
    std::filesystem::path file;
    bool ok = false;
    int passesTotal = 0, passesOk = 0;
    bool usedFallback = false;
    std::vector<std::string> failPassNames;
    std::string firstError;
};

EffectResult CompileOne(const std::filesystem::path& f, const MmeCompileOptions& opt, Totals& tot) {
    EffectResult r;
    r.file = f;
    MmeEffectDesc fx = LoadMmeEffect(f);
    if (!fx.ok) {
        tot.effectsFail.fetch_add(1);
        return r;
    }
    std::vector<MmeProgram> progs = CompileMmeEffect(fx, opt);
    r.passesTotal = (int)progs.size();
    for (const MmeProgram& pr : progs) {
        if (pr.usedFallbackSource) { r.usedFallback = true; tot.fallbackCount.fetch_add(1); }
        if (pr.usesVertexId) tot.indexFixups.fetch_add(1);
        if (pr.usesVpos) tot.vposFixups.fetch_add(1);
        if (pr.usesVface) tot.vfaceFixups.fetch_add(1);
        if (pr.ok) {
            ++r.passesOk;
            tot.programsOk.fetch_add(1);
        } else {
            tot.programsFail.fetch_add(1);
            r.failPassNames.push_back((pr.technique.empty() ? "?" : pr.technique) + "." + (pr.pass.empty() ? "?" : pr.pass));
            for (const MmeDiag& d : pr.diags) {
                if (d.sev != MmeDiag::Error) continue;
                if (r.firstError.empty()) r.firstError = NormaliseError(d.message);
                break;
            }
        }
    }
    if (r.passesTotal == 0) {
        r.ok = true;
        tot.effectsOk.fetch_add(1);
    } else if (r.passesOk == r.passesTotal) {
        r.ok = true;
        tot.effectsOk.fetch_add(1);
    } else if (r.passesOk == 0) {
        tot.effectsFail.fetch_add(1);
    } else {
        tot.effectsPart.fetch_add(1);
    }
    return r;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path jsonOut, dumpWrapper;
    bool verbose = false, noCache = false, noSelftest = false, objectMode = false;
    int jobs = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--verbose") verbose = true;
        else if (a == L"--no-cache") noCache = true;
        else if (a == L"--no-selftest") noSelftest = true;
        else if (a == L"--object-mode") objectMode = true;
        else if (a == L"--jobs" && i + 1 < argc) jobs = std::atoi(WideToUtf8(argv[++i]).c_str());
        else if (a == L"--json" && i + 1 < argc) jsonOut = argv[++i];
        else if (a == L"--dump-wrapper" && i + 1 < argc) dumpWrapper = argv[++i];
        else inputs.push_back(a);
    }

    MmeCompileOptions opt;
    opt.useCache = !noCache;
    opt.objectMode = objectMode;

    bool selfOk = true;
    if (!noSelftest) {
        std::printf("---- selftest ----\n");
        selfOk = RunSelfTest(opt);
        if (!selfOk) std::printf("selftest FAILED\n");
    }

    if (!dumpWrapper.empty()) {
        MmeEffectDesc fx = LoadMmeEffect(dumpWrapper);
        if (!fx.ok) {
            std::printf("cannot load %s\n", dumpWrapper.string().c_str());
            return 2;
        }
        for (const MmeTechnique& t : fx.techniques) {
            for (const MmePass& p : t.passes) {
                std::string dump = DumpMmeWrapper(fx, t, p, opt);
                std::printf("===== %s :: %s.%s =====\n%s\n", dumpWrapper.string().c_str(), t.name.c_str(),
                            p.name.c_str(), dump.c_str());
            }
        }
        if (inputs.empty()) return selfOk ? 0 : 1;
    }

    if (inputs.empty()) {
        if (!noSelftest && dumpWrapper.empty()) return selfOk ? 0 : 1;
        std::printf("usage: mme_compile_test <file-or-dir>... [--no-cache] [--jobs N] [--json out.json] "
                    "[--verbose] [--dump-wrapper <fx>] [--no-selftest]\n");
        return selfOk ? 0 : 2;
    }

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const std::filesystem::path& in : inputs) {
        if (std::filesystem::is_directory(in, ec)) {
            for (auto it = std::filesystem::recursive_directory_iterator(
                     in, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec) && EndsWithFx(it->path())) files.push_back(it->path());
            }
        } else {
            files.push_back(in);
        }
    }
    std::sort(files.begin(), files.end());

    ResetMmeCacheHitCount();
    ResetMmeStageCompileCount();
    ResetMmeStaticFixupCount();
    const auto t0 = std::chrono::steady_clock::now();

    if (jobs <= 0) jobs = (int)std::thread::hardware_concurrency();
    if (jobs <= 0) jobs = 1;

    Totals tot;
    std::vector<EffectResult> results(files.size());
    std::atomic<size_t> next{0};
    std::mutex logMutex;
    auto worker = [&]() {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= files.size()) break;
            EffectResult r = CompileOne(files[i], opt, tot);
            results[i] = std::move(r);
            if (verbose) {
                std::lock_guard<std::mutex> lock(logMutex);
                const EffectResult& e = results[i];
                const char* cls = e.passesTotal == 0 ? "OK" : (e.passesOk == e.passesTotal ? "OK" : (e.passesOk == 0 ? "FAIL" : "PART"));
                std::printf("%s %s programs=%d/%d\n", cls, PathToUtf8(e.file).c_str(), e.passesOk, e.passesTotal);
                for (const std::string& fp : e.failPassNames) std::printf("      fail %s\n", fp.c_str());
                if (!e.firstError.empty()) std::printf("      error %s\n", e.firstError.c_str());
            }
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < jobs; ++i) threads.emplace_back(worker);
    for (std::thread& th : threads) th.join();

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    tot.staticFixups.store((int)GetMmeStaticFixupCount());

    // non-verbose per effect line + error grouping
    std::map<std::string, int> errorCounts;
    nlohmann::json records = nlohmann::json::array();
    for (const EffectResult& e : results) {
        const char* cls = e.passesTotal == 0 ? "OK" : (e.passesOk == e.passesTotal ? "OK" : (e.passesOk == 0 ? "FAIL" : "PART"));
        if (!verbose)
            std::printf("%s %s programs=%d/%d\n", cls, PathToUtf8(e.file).c_str(), e.passesOk, e.passesTotal);
        else
            std::printf("%s %s programs=%d/%d\n", cls, PathToUtf8(e.file).c_str(), e.passesOk, e.passesTotal);
        if (!e.firstError.empty()) errorCounts[e.firstError]++;
        nlohmann::json rec;
        rec["file"] = PathToUtf8(e.file);
        rec["ok"] = e.ok;
        rec["passes_ok"] = e.passesOk;
        rec["passes_total"] = e.passesTotal;
        rec["fallback"] = e.usedFallback;
        rec["first_error"] = e.firstError;
        records.push_back(std::move(rec));
    }

    std::printf("---- summary ----\n");
    std::printf("effects: ok=%d part=%d fail=%d\n", tot.effectsOk.load(), tot.effectsPart.load(), tot.effectsFail.load());
    const int pOk = tot.programsOk.load(), pFail = tot.programsFail.load();
    std::printf("programs: ok=%d failed=%d (%.1f%% ok)\n", pOk, pFail,
                (pOk + pFail) ? 100.0 * pOk / (pOk + pFail) : 0.0);
    std::printf("fix-ups: _INDEX=%d VPOS=%d VFACE=%d nonliteral-static=%d\n", tot.indexFixups.load(),
                tot.vposFixups.load(), tot.vfaceFixups.load(), tot.staticFixups.load());
    std::printf("fallback source: %d\n", tot.fallbackCount.load());
    std::printf("cache hits: %u (stage compiles: %u)\n", GetMmeCacheHitCount(), GetMmeStageCompileCount());
    std::printf("elapsed=%lld ms\n", (long long)ms);
    std::printf("top errors:\n");
    {
        std::vector<std::pair<std::string, int>> ev(errorCounts.begin(), errorCounts.end());
        std::sort(ev.begin(), ev.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < ev.size() && i < 15; ++i) std::printf("  %d  %s\n", ev[i].second, ev[i].first.c_str());
    }

    if (!jsonOut.empty()) {
        std::ofstream out(jsonOut, std::ios::binary | std::ios::trunc);
        out << records.dump(2);
    }
    return selfOk ? 0 : 1;
}
