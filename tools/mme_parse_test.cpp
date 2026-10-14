// mme_parse_test: parse MME .fx files into the MmeEffect IR and report. Directory arguments recurse over *.fx
// (case-insensitive). Intended to run over the whole library and the captured effect pack without a window.
//
//   mme_parse_test <file-or-dir>... [--dump <fx>] [--json <out.json>] [--summary] [--quiet]
//
// --summary prints one JSON line per input file (kind, technique MMDPass list, offscreen use, diagnostic counts),
// for the M7a compatibility harness (tools/mme/mme_compat_run.py).
//
// Exit code 0 unless a crash-like condition occurs (an exception is reported as a FAIL line). --selftest runs a
// set of inline sources that exercise the preprocessor and parser and exits 1 if any case fails.
#include "asset/MmeAssign.h"
#include "asset/MmeEffect.h"
#include "asset/MmePreprocessor.h"
#include "core/TextUtil.h"

#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace mmdx;

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool EndsWithFx(const std::filesystem::path& p) {
    const std::string e = Lower(p.extension().string());
    return e == ".fx";
}

struct Summary {
    int techniques = 0;
    int passes = 0;
    int params = 0;
    int samplers = 0;
    int errors = 0;
    int warnings = 0;
    std::map<std::string, int> byMmdPass;
    std::map<std::string, int> byCommand;
    std::map<std::string, int> bySemantic;
};

Summary Summarise(const MmeEffectDesc& d) {
    Summary s;
    s.techniques = (int)d.techniques.size();
    s.params = (int)d.params.size();
    s.samplers = (int)d.samplers.size();
    for (const MmeTechnique& t : d.techniques) {
        s.passes += (int)t.passes.size();
        s.byMmdPass[t.mmdPass]++;
        const MmeAnnotation* script = FindAnnotation(t.annotations, "Script");
        for (const MmeScriptCommand& c : ParseMmeScript(script ? script->value.str : ""))
            s.byCommand[c.command]++;
        for (const MmePass& p : t.passes) {
            const MmeAnnotation* ps = FindAnnotation(p.annotations, "Script");
            for (const MmeScriptCommand& c : ParseMmeScript(ps ? ps->value.str : ""))
                s.byCommand[c.command]++;
        }
    }
    for (const MmeParam& p : d.params)
        if (!p.semantic.empty()) s.bySemantic[p.semantic]++;
    for (const MmeDiag& diag : d.diags) {
        if (diag.sev == MmeDiag::Error) ++s.errors;
        else if (diag.sev == MmeDiag::Warning) ++s.warnings;
    }
    return s;
}

// M7a: one JSON line per file, for tools/mme/mme_compat_run.py: kind, technique MMDPass list, whether an
// OFFSCREENRENDERTARGET is used, and the diagnostic counts.
void PrintSummaryLine(const std::filesystem::path& f, const MmeEffectDesc& d, const Summary& s) {
    nlohmann::json techniques = nlohmann::json::array();
    for (const MmeTechnique& t : d.techniques) {
        nlohmann::json tech;
        tech["name"] = t.name;
        tech["mmd_pass"] = t.mmdPass;
        tech["passes"] = (int)t.passes.size();
        techniques.push_back(std::move(tech));
    }
    nlohmann::json rec;
    rec["file"] = PathToUtf8(f);
    rec["ok"] = d.ok;
    rec["kind"] = d.kind();
    rec["script_class"] = d.scriptClass;
    rec["script_order"] = d.scriptOrder;
    rec["offscreen"] = d.UsesOffscreen();
    nlohmann::json offscreenTargets = nlohmann::json::array();
    for (const MmeParam& p : d.params)
        if (p.semantic == "OFFSCREENRENDERTARGET") offscreenTargets.push_back(p.name);
    rec["offscreen_targets"] = std::move(offscreenTargets);
    rec["techniques"] = std::move(techniques);
    rec["passes"] = s.passes;
    rec["errors"] = s.errors;
    rec["warnings"] = s.warnings;
    std::printf("%s\n", rec.dump().c_str());
}

void PrintValue(const MmeValue& v) {
    switch (v.kind) {
        case MmeValue::Bool: std::printf("%s", v.nums.empty() || v.nums[0] == 0 ? "false" : "true"); break;
        case MmeValue::Int: std::printf("%d", v.nums.empty() ? 0 : (int)v.nums[0]); break;
        case MmeValue::Float: {
            for (size_t i = 0; i < v.nums.size(); ++i) std::printf("%s%g", i ? "," : "", v.nums[i]);
            break;
        }
        case MmeValue::String: std::printf("\"%s\"", v.str.c_str()); break;
    }
}

void DumpEffect(const MmeEffectDesc& d) {
    std::printf("=== %s ===\n", d.file.string().c_str());
    std::printf("kind=%s class=%s order=%s output=%s standardsglobal=%d offscreen=%d ok=%d\n",
                d.kind().c_str(), d.scriptClass.c_str(), d.scriptOrder.c_str(), d.scriptOutput.c_str(),
                d.hasStandardsGlobal ? 1 : 0, d.UsesOffscreen() ? 1 : 0, d.ok ? 1 : 0);
    if (!d.globalScript.empty()) std::printf("global script: %s\n", d.globalScript.c_str());
    std::printf("includes (%zu):\n", d.includes.size());
    for (const auto& inc : d.includes) std::printf("  %s\n", inc.string().c_str());
    std::printf("params (%zu):\n", d.params.size());
    for (const MmeParam& p : d.params) {
        std::printf("  %s %s", p.type.c_str(), p.name.c_str());
        if (p.arraySize) std::printf("[%d]", p.arraySize);
        if (!p.semantic.empty()) std::printf(" : %s", p.semantic.c_str());
        for (const MmeAnnotation& a : p.annotations) {
            std::printf(" <%s %s = ", a.type.c_str(), a.name.c_str());
            PrintValue(a.value);
            std::printf(">");
        }
        if (p.hasInit) { std::printf(" = "); PrintValue(p.init); }
        std::printf(";\n");
    }
    std::printf("samplers (%zu):\n", d.samplers.size());
    for (const MmeSampler& s : d.samplers) {
        std::printf("  %s %s texture=%s", s.type.c_str(), s.name.c_str(), s.texture.c_str());
        if (!s.registerBinding.empty()) std::printf(" register(%s)", s.registerBinding.c_str());
        std::printf("\n");
        for (const auto& [k, v] : s.states) std::printf("      %s = %s\n", k.c_str(), v.c_str());
    }
    std::printf("techniques (%zu):\n", d.techniques.size());
    for (const MmeTechnique& t : d.techniques) {
        std::printf("  %s pass=%s subset=\"%s\" useTex=%d useSphere=%d useToon=%d\n", t.name.c_str(),
                    t.mmdPass.c_str(), t.subset.c_str(), t.useTexture, t.useSphereMap, t.useToon);
        for (const MmeAnnotation& a : t.annotations) {
            std::printf("      <%s %s = ", a.type.c_str(), a.name.c_str());
            PrintValue(a.value);
            std::printf(">\n");
        }
        for (const MmePass& p : t.passes) {
            std::printf("    pass %s", p.name.c_str());
            if (!p.vs.function.empty() || p.vs.isNull || p.vs.isAsm)
                std::printf("  VS=%s", p.vs.isAsm ? "asm" : (p.vs.isNull ? "NULL" : (p.vs.profile + " " + p.vs.function).c_str()));
            if (!p.ps.function.empty() || p.ps.isNull || p.ps.isAsm)
                std::printf("  PS=%s", p.ps.isAsm ? "asm" : (p.ps.isNull ? "NULL" : (p.ps.profile + " " + p.ps.function).c_str()));
            std::printf("\n");
            for (const auto& [k, v] : p.states) std::printf("        %s = %s\n", k.c_str(), v.c_str());
        }
    }
    std::printf("diags (%zu):\n", d.diags.size());
    for (const MmeDiag& g : d.diags) {
        const char sev = g.sev == MmeDiag::Error ? 'E' : (g.sev == MmeDiag::Warning ? 'W' : 'I');
        std::printf("  [%c] %s:%d %s\n", sev, g.file.filename().string().c_str(), g.line, g.message.c_str());
    }
}

// ---- self test -------------------------------------------------------------------------------------------

struct SelfCase {
    std::string name;
    std::string source;
    bool ok = false;
};

bool RunSelfTest() {
    struct Case { const char* name; const char* src; bool (*check)(const MmeEffectDesc&); };
    static const Case cases[] = {
        {"macro technique",
         "#define TEC(n, mmdp) technique n < string MMDPass = mmdp; > { pass P { VertexShader = compile vs_3_0 "
         "VS(); } }\nTEC(T0, \"object\")\n",
         [](const MmeEffectDesc& d) {
             return d.techniques.size() == 1 && d.techniques[0].name == "T0" && d.techniques[0].mmdPass == "object" &&
                    d.techniques[0].passes.size() == 1 && d.techniques[0].passes[0].vs.profile == "vs_3_0" &&
                    d.techniques[0].passes[0].vs.function == "VS";
         }},
        {"if expression",
         "#define X 1\n#define Y 3\n#if defined(X) && Y > 1\nfloat4 included : DIFFUSE < string Object = \"Geometry\"; >;\n"
         "#else\nfloat4 excluded;\n#endif\n",
         [](const MmeEffectDesc& d) {
             bool found = false;
             for (const MmeParam& p : d.params)
                 if (p.name == "included") found = true;
             for (const MmeParam& p : d.params)
                 if (p.name == "excluded") return false;
             return found;
         }},
        {"adjacent strings in script",
         "technique T < string Script = \"RenderColorTarget0=A;\" \"Pass=P0;\" ; > { pass P0 { PixelShader = compile "
         "ps_3_0 P(); } }\n",
         [](const MmeEffectDesc& d) {
             const MmeAnnotation* s = FindAnnotation(d.techniques[0].annotations, "Script");
             return s && s->value.str == "RenderColorTarget0=A;Pass=P0;";
         }},
        {"sampler_state forms",
         "texture T;\ntexture U;\nsampler S1 = sampler_state { Texture = <T>; ADDRESSU = WRAP; };\nsampler S2 = "
         "sampler_state { texture = (U); };\n",
         [](const MmeEffectDesc& d) {
             if (d.samplers.size() != 2) return false;
             return d.samplers[0].texture == "T" && d.samplers[1].texture == "U" &&
                    Lower(d.samplers[0].states.count("ADDRESSU") ? d.samplers[0].states.at("ADDRESSU") : "") == "wrap";
         }},
        {"compile args",
         "technique T { pass P { PixelShader = compile ps_2_0 PS(true, 2); } }\n",
         [](const MmeEffectDesc& d) {
             const MmeShaderRef& ps = d.techniques[0].passes[0].ps;
             return ps.function == "PS" && ps.args.size() == 2 && ps.args[0] == "true" && ps.args[1] == "2";
         }},
        {"asm pass",
         "technique T { pass P { VertexShader = asm { vs_2_0 dcl_position v0; }; } }\n",
         [](const MmeEffectDesc& d) { return d.techniques[0].passes[0].vs.isAsm; }},
        {"Subset",
         "technique T < string Subset = \"0-6,8\"; > { pass P { PixelShader = compile ps_3_0 P(); } }\n",
         [](const MmeEffectDesc& d) { return d.techniques[0].subset == "0-6,8"; }},
        {"UseToon false",
         "technique T < bool UseToon = false; > { pass P { PixelShader = compile ps_3_0 P(); } }\n",
         [](const MmeEffectDesc& d) { return d.techniques[0].useToon == 0; }},
        {"UseToon absent",
         "technique T { pass P { PixelShader = compile ps_3_0 P(); } }\n",
         [](const MmeEffectDesc& d) { return d.techniques[0].useToon == -1; }},
        {"STANDARDSGLOBAL post",
         "float Script : STANDARDSGLOBAL < string ScriptClass = \"scene\"; string ScriptOrder = \"postprocess\"; string "
         "ScriptOutput = \"color\"; > = 0.8;\n",
         [](const MmeEffectDesc& d) {
             return d.hasStandardsGlobal && d.scriptClass == "scene" && d.scriptOrder == "postprocess" &&
                    d.kind() == "post";
         }},
        {"shared texture",
         "shared texture DummyScreenTex : RENDERCOLORTARGET;\n",
         [](const MmeEffectDesc& d) {
             return d.params.size() == 1 && d.params[0].isShared && d.params[0].semantic == "RENDERCOLORTARGET" &&
                    d.params[0].type == "texture";
         }},
        {"row_major / column_major modifiers",
         "row_major float4x4 RM;\nfloat4x4 column_major CM;\n",
         [](const MmeEffectDesc& d) {
             const MmeParam* rm = nullptr;
             const MmeParam* cm = nullptr;
             for (const MmeParam& p : d.params) {
                 if (p.name == "RM") rm = &p;
                 if (p.name == "CM") cm = &p;
             }
             return rm && cm && rm->rowMajor && !rm->columnMajor && cm->columnMajor && !cm->rowMajor;
         }},
        {"token paste CAT",
         "#define CAT(a,b) a##b\ncompile CAT(vs,_3_0) VS()\n",
         [](const MmeEffectDesc& d) {
             return d.expandedSource.find("vs_3_0") != std::string::npos &&
                    d.expandedSource.find('#') == std::string::npos;
         }},
        {"stringify STR",
         "#define STR(x) #x\nconst char* s = STR(abc);\n",
         [](const MmeEffectDesc& d) { return d.expandedSource.find("\"abc\"") != std::string::npos; }},
        {"profile paste P",
         "#define P(x) x##_3_0\ntechnique T { pass P { PixelShader = compile P(ps) PS(); } }\n",
         [](const MmeEffectDesc& d) {
             const MmeShaderRef& ps = d.techniques[0].passes[0].ps;
             return ps.profile == "ps_3_0" && ps.function == "PS";
         }},
        {"outer macro rescans inner paste",
         "#define ShaderVer(x) x##_3_0\n"
         "#define TEC2(mmdp) technique T2 < string MMDPass = mmdp; > { pass P { VertexShader = compile "
         "ShaderVer(vs) VS(); } }\n"
         "TEC2(\"object\")\n",
         [](const MmeEffectDesc& d) {
             const MmeShaderRef& vs = d.techniques[0].passes[0].vs;
             return vs.profile == "vs_3_0" && vs.function == "VS";
         }},
        {"ternary initializer does not swallow following decls",
         "static float a = -1.0;\n"
         "static float b = (a < 0) ? 1.0 : 2.0;\n"
         "float4x4 RT : RENDERCOLORTARGET < int Width = 64; int Height = 64; >;\n"
         "float4x4 RT2 : RENDERCOLORTARGET < int Width = 8; >;\n",
         [](const MmeEffectDesc& d) {
             bool b = false, rt = false, rt2 = false;
             for (const MmeParam& p : d.params) {
                 if (p.name == "b") b = p.hasInit;
                 if (p.name == "RT") rt = p.semantic == "RENDERCOLORTARGET";
                 if (p.name == "RT2") rt2 = p.semantic == "RENDERCOLORTARGET";
             }
             return b && rt && rt2;
         }},
        {"greater-than in initializer", "static float a = 2.0;\nstatic float b = a > 1 ? 5.0 : 6.0;\nfloat c = 3.0;\n",
         [](const MmeEffectDesc& d) {
             bool b = false, c = false;
             for (const MmeParam& p : d.params) {
                 if (p.name == "b") b = p.hasInit;
                 if (p.name == "c") c = p.hasInit;
             }
             return b && c;
         }},
        {"anonymous cast initializer", "static float a = 1.0;\nstatic float b = (float)(a > 0.0f);\nfloat c = 2.0;\n",
         [](const MmeEffectDesc& d) {
             bool b = false, c = false;
             for (const MmeParam& p : d.params) {
                 if (p.name == "b") b = p.hasInit;
                 if (p.name == "c") c = p.hasInit;
             }
             return b && c;
         }},
        {"merge_strings no stray hash",
         "#define merge_strings(a,b) a##b\n"
         "#define facemap \"faceSdf.png\"\n"
         "texture faceShadowTexture < string ResourceName = merge_strings(\"textures/face/\", facemap); >;\n",
         [](const MmeEffectDesc& d) {
             if (d.expandedSource.find('#') != std::string::npos) return false;
             const MmeParam* p = nullptr;
             for (const MmeParam& q : d.params)
                 if (q.name == "faceShadowTexture") p = &q;
             if (!p) return false;
             const MmeAnnotation* a = FindAnnotation(p->annotations, "ResourceName");
             return a && a->value.str == "textures/face/faceSdf.png";
         }},
    };

    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "mme_parse_test_selftest.fx";
    int passed = 0, total = 0;
    for (const Case& c : cases) {
        ++total;
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << c.src;
        }
        bool ok = false;
        try {
            const MmeEffectDesc d = LoadMmeEffect(tmp);
            ok = d.ok && c.check(d);
        } catch (...) {
            ok = false;
        }
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", c.name);
        if (ok) ++passed;
    }
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    std::printf("selftest: %d/%d passed\n", passed, total);
    return passed == total;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path dump, jsonOut;
    bool quiet = false, selftest = false, summary = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--quiet") quiet = true;
        else if (a == L"--selftest") selftest = true;
        else if (a == L"--summary") summary = true;
        else if (a == L"--dump" && i + 1 < argc) dump = argv[++i];
        else if (a == L"--json" && i + 1 < argc) jsonOut = argv[++i];
        else inputs.push_back(a);
    }

    bool selfOk = true;
    if (!quiet) std::printf("---- selftest ----\n");
    selfOk = RunSelfTest();
    if (!selfOk) std::printf("selftest FAILED\n");
    if (selftest && inputs.empty() && dump.empty()) return selfOk ? 0 : 1;
    if (inputs.empty() && dump.empty()) {
        std::printf("usage: mme_parse_test <file-or-dir>... [--dump <fx>] [--json <out.json>] [--quiet]\n");
        return 2;
    }

    if (!dump.empty()) {
        DumpEffect(LoadMmeEffect(dump));
        if (inputs.empty()) return selfOk ? 0 : 1;
    }

    // gather .fx files
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

    const auto t0 = std::chrono::steady_clock::now();
    Summary totals;
    int okFiles = 0, failFiles = 0, errFiles = 0;
    nlohmann::json records = nlohmann::json::array();
    for (const std::filesystem::path& f : files) {
        try {
            const MmeEffectDesc d = LoadMmeEffect(f);
            const Summary s = Summarise(d);
            if (!d.ok) ++failFiles; else ++okFiles;
            if (s.errors > 0) ++errFiles;
            totals.techniques += s.techniques;
            totals.passes += s.passes;
            totals.params += s.params;
            totals.samplers += s.samplers;
            for (const auto& [k, v] : s.byMmdPass) totals.byMmdPass[k] += v;
            for (const auto& [k, v] : s.byCommand) totals.byCommand[k] += v;
            for (const auto& [k, v] : s.bySemantic) totals.bySemantic[k] += v;
            if (!quiet && !summary) {
                std::printf("%s %s tech=%d pass=%d params=%d samplers=%d kind=%s diags=%d/%d\n",
                            d.ok ? (s.errors ? "DIAG" : "OK") : "FAIL", PathToUtf8(f).c_str(), s.techniques,
                            s.passes, s.params, s.samplers, d.kind().c_str(), s.errors, s.warnings);
            }
            if (summary) PrintSummaryLine(f, d, s);
            nlohmann::json rec;
            rec["file"] = PathToUtf8(f);
            rec["ok"] = d.ok;
            rec["kind"] = d.kind();
            rec["techniques"] = s.techniques;
            rec["passes"] = s.passes;
            rec["params"] = s.params;
            rec["samplers"] = s.samplers;
            rec["errors"] = s.errors;
            rec["warnings"] = s.warnings;
            rec["includes"] = (int)d.includes.size();
            records.push_back(std::move(rec));
        } catch (const std::exception& e) {
            ++failFiles;
            std::printf("FAIL %s exception: %s\n", PathToUtf8(f).c_str(), e.what());
        } catch (...) {
            ++failFiles;
            std::printf("FAIL %s unknown exception\n", PathToUtf8(f).c_str());
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    std::printf("---- summary ----\n");
    std::printf("files=%zu ok=%d fail=%d files_with_errors=%d\n", files.size(), okFiles, failFiles, errFiles);
    std::printf("totals: techniques=%d passes=%d params=%d samplers=%d\n", totals.techniques, totals.passes,
                totals.params, totals.samplers);
    std::printf("MMDPass:\n");
    for (const auto& [k, v] : totals.byMmdPass) std::printf("  %s=%d\n", k.c_str(), v);
    std::printf("script commands:\n");
    for (const auto& [k, v] : totals.byCommand) std::printf("  %s=%d\n", k.c_str(), v);
    std::printf("semantics (top 40):\n");
    {
        std::vector<std::pair<std::string, int>> sem(totals.bySemantic.begin(), totals.bySemantic.end());
        std::sort(sem.begin(), sem.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < sem.size() && i < 40; ++i) std::printf("  %s=%d\n", sem[i].first.c_str(), sem[i].second);
    }
    std::printf("elapsed=%lld ms\n", (long long)ms);

    if (!jsonOut.empty()) {
        std::ofstream out(jsonOut, std::ios::binary | std::ios::trunc);
        out << records.dump(2);
    }
    return selfOk ? 0 : 1;
}
