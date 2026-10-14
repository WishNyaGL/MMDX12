// mme_script_test: M3b exercises the MME semantic binder (render/MmeSemantics) and the script VM (render/MmeScriptVM)
// on the CPU only (no window, no GPU). It runs an inline self test first, then optional effect files.
//
//   mme_script_test <fx>... [--trace] [--all]
//
// For each effect it loads it (M1), compiles it (M2, CompileMmeEffect) and runs the first technique (or every
// technique with --all) against a recording host that prints one line per call, with a MmeConstantWriter per compiled
// program writing a fixed MmeFrameValues. --trace additionally prints, per stage, every bound variable with its
// semantic and written value. Exit code 0 unless a self-test case fails (then 1).
#include "asset/MmeEffect.h"
#include "render/MmeCompiler.h"
#include "render/MmeSemantics.h"
#include "render/MmeScriptVM.h"

#include <DirectXMath.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace mmdx;
using namespace DirectX;

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string FormatFloats(const float* v, int n) {
    std::string s;
    char buf[64];
    for (int i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), "%g", v[i]);
        if (i) s += ",";
        s += buf;
    }
    return s;
}

// ---- recording host ------------------------------------------------------------------------------------------------

struct RecordingHost : IMmeHost {
    bool print = true;
    std::vector<std::string> log;
    std::vector<MmeConstantWriter*> writers;

    void Emit(const std::string& line) {
        log.push_back(line);
        if (print) std::printf("%s\n", line.c_str());
    }

    void SetColorTarget(int index, const std::string& name) override {
        Emit("RT" + std::to_string(index) + "=" + name);
    }
    void SetDepthTarget(const std::string& name) override { Emit("RTD=" + name); }
    void Clear(bool color, bool depth, const float color4[4], float depthValue) override {
        if (color && depth)
            Emit("CLEAR color(" + FormatFloats(color4, 4) + ") depth " + std::to_string(depthValue));
        else if (color)
            Emit("CLEAR color(" + FormatFloats(color4, 4) + ")");
        else
            Emit("CLEAR depth " + std::to_string(depthValue));
    }
    void ScriptExternal() override { Emit("EXTERNAL"); }
    void DrawBuffer(const MmeTechnique& t, const MmePass& p) override {
        (void)t;
        Emit("DRAWBUFFER " + p.name);
    }
    void DrawGeometry(const MmeTechnique& t, const MmePass& p) override {
        (void)t;
        Emit("DRAWGEOMETRY " + p.name);
    }
    bool ReadParam(const std::string& name, float out[4]) override {
        return MmeConstantWriter::ReadParam(fx_, writers, name, out);
    }
    void SetLoopIndex(const std::string& name, int index) override {
        Emit("LOOPINDEX " + name + "=" + std::to_string(index));
        for (MmeConstantWriter* w : writers) w->SetScalar(name, (float)index);
    }
    void Diag(const std::string& message) override { Emit("DIAG " + message); }

    const MmeEffectDesc& fx_;
    explicit RecordingHost(const MmeEffectDesc& fx) : fx_(fx) {}
};

// ---- loading / compiling inline effects ----------------------------------------------------------------------------

std::filesystem::path WriteTemp(const std::string& src, const std::string& tag) {
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / ("mme_script_test_" + tag + ".fx");
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << src;
    return tmp;
}

MmeEffectDesc LoadInline(const std::string& src, const std::string& tag) {
    const std::filesystem::path tmp = WriteTemp(src, tag);
    MmeEffectDesc fx = LoadMmeEffect(tmp);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return fx;
}

std::vector<MmeProgram> CompileInline(const MmeEffectDesc& fx, const std::string& tag) {
    MmeCompileOptions opt;
    opt.useCache = false;   // inline test sources: never hit a stale cache
    (void)tag;
    return CompileMmeEffect(fx, opt);
}

// Binds one writer per stage that has a $Globals cbuffer; returns them (owned by `store`).
std::vector<MmeConstantWriter*> BindWriters(const MmeEffectDesc& fx, const std::vector<MmeProgram>& progs,
                                            std::vector<MmeConstantWriter*>& store,
                                            std::vector<MmeDiag>& diags) {
    std::vector<MmeConstantWriter*> out;
    for (const MmeProgram& pr : progs) {
        if (!pr.vs.cb.empty()) {
            auto* w = new MmeConstantWriter();
            w->Bind(fx, pr.vs, diags);
            store.push_back(w);
            out.push_back(w);
        }
        if (!pr.ps.cb.empty()) {
            auto* w = new MmeConstantWriter();
            w->Bind(fx, pr.ps, diags);
            store.push_back(w);
            out.push_back(w);
        }
    }
    return out;
}

void FreeWriters(std::vector<MmeConstantWriter*>& store) {
    for (MmeConstantWriter* w : store) delete w;
    store.clear();
}

// A fixed frame for tests and file runs.
MmeFrameValues MakeFrame() {
    MmeFrameValues v;
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 20, -40, 1), XMVectorSet(0, 10, 0, 1),
                                           XMVectorSet(0, 1, 0, 0));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(30.0f), 16.0f / 9.0f, 0.5f, 3000.0f);
    XMStoreFloat4x4(&v.view, view);
    XMStoreFloat4x4(&v.proj, proj);
    v.cameraPos = {0, 20, -40};
    v.cameraDir = {0, -0.35f, 0.94f};
    v.lightDir = {-0.408f, -0.816f, 0.408f};
    v.lightColor = {0.6f, 0.6f, 0.6f};
    v.viewportW = 1280;
    v.viewportH = 720;
    v.time = 1.5f;
    v.elapsed = 1.0f / 60.0f;
    v.sceneTime = 2.5f;
    v.sceneElapsed = 1.0f / 30.0f;
    v.self = MmeControllerPanel{};
    return v;
}

// ---- self test -----------------------------------------------------------------------------------------------------

struct SelfResult {
    std::string name;
    bool ok = false;
    std::string detail;
};

// The technique named `name` (case-insensitive) or the first when empty.
const MmeTechnique* FindTechnique(const MmeEffectDesc& fx, const std::string& name) {
    if (fx.techniques.empty()) return nullptr;
    if (name.empty()) return &fx.techniques[0];
    for (const MmeTechnique& t : fx.techniques)
        if (Lower(t.name) == Lower(name)) return &t;
    return &fx.techniques[0];
}

bool LogHas(const RecordingHost& h, const std::string& needle) {
    for (const std::string& s : h.log)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

int CountPrefix(const RecordingHost& h, const std::string& prefix) {
    int n = 0;
    for (const std::string& s : h.log)
        if (s.rfind(prefix, 0) == 0) ++n;
    return n;
}

bool RunSelfTest() {
    std::vector<SelfResult> results;
    auto add = [&](const std::string& name, bool ok, std::string detail = {}) {
        results.push_back({name, ok, std::move(detail)});
    };

    // 1. nested LoopByCount + LoopGetIndex
    {
        const char* src =
            "float Loop1 = 2;\n"
            "float Loop2 = 3;\n"
            "technique T < string Script = \"LoopByCount=Loop1; LoopGetIndex=Loop1;"
            "LoopByCount=Loop2; LoopGetIndex=Loop2; Pass=P; LoopEnd=; LoopEnd=;\" >\n"
            "{ pass P < string Script=\"Draw=Buffer;\"; > { } }\n";
        MmeEffectDesc fx = LoadInline(src, "loop");
        RecordingHost host(fx);
        host.print = false;
        const MmeTechnique* t = FindTechnique(fx, "");
        int draws = t ? RunMmeTechnique(fx, *t, "scene", host) : 0;
        const bool ok = t && draws == 6 && CountPrefix(host, "DRAWBUFFER") == 6 &&
                        CountPrefix(host, "LOOPINDEX") == 8;   // 2 outer x (3 inner + 1 outer)
        add("nested LoopByCount + LoopGetIndex", ok,
            "draws=" + std::to_string(draws) + " buf=" + std::to_string(CountPrefix(host, "DRAWBUFFER")) +
                " idx=" + std::to_string(CountPrefix(host, "LOOPINDEX")) + " log=" + std::to_string(host.log.size()));
    }

    // 2. technique without script: passes run in order
    {
        const char* src =
            "float4 PS() : COLOR { return 1; }\n"
            "technique T { pass P0 { PixelShader = compile ps_2_0 PS(); } pass P1 { PixelShader = compile ps_2_0 PS(); } }\n";
        MmeEffectDesc fx = LoadInline(src, "order");
        RecordingHost host(fx);
        host.print = false;
        const MmeTechnique* t = FindTechnique(fx, "");
        int draws = t ? RunMmeTechnique(fx, *t, "object", host) : 0;
        bool order = false;
        if (host.log.size() == 2 && host.log[0] == "DRAWGEOMETRY P0" && host.log[1] == "DRAWGEOMETRY P1")
            order = true;
        add("technique without script (passes in order)", t && draws == 2 && order);
    }

    // 3. pass without script (Draw=Geometry); in a scene class -> host Diag
    {
        const char* src =
            "float4 PS() : COLOR { return 1; }\n"
            "technique T { pass P0 { PixelShader = compile ps_2_0 PS(); } }\n";
        MmeEffectDesc fx = LoadInline(src, "scenegeom");
        RecordingHost host(fx);
        host.print = false;
        const MmeTechnique* t = FindTechnique(fx, "");
        int draws = t ? RunMmeTechnique(fx, *t, "scene", host) : 0;
        const bool ok = t && draws == 0 && CountPrefix(host, "DRAWGEOMETRY") == 0 && CountPrefix(host, "DIAG") == 1;
        add("pass without script in scene class -> Diag", ok, "draws=" + std::to_string(draws));
    }

    // 4. RenderColorTarget persistence across passes
    {
        const char* src =
            "texture2D RT : RENDERCOLORTARGET < int Width = 4; int Height = 4; >;\n"
            "float4 PS() : COLOR { return 1; }\n"
            "technique T < string Script = \"RenderColorTarget0=RT; Pass=P0; Pass=P1;\" > {\n"
            "  pass P0 < string Script=\"Draw=Buffer;\"; > { PixelShader = compile ps_2_0 PS(); }\n"
            "  pass P1 < string Script=\"Draw=Buffer;\"; > { PixelShader = compile ps_2_0 PS(); }\n"
            "}\n";
        MmeEffectDesc fx = LoadInline(src, "persist");
        RecordingHost host(fx);
        host.print = false;
        const MmeTechnique* t = FindTechnique(fx, "");
        int draws = t ? RunMmeTechnique(fx, *t, "scene", host) : 0;
        const bool ok = t && CountPrefix(host, "RT0=RT") == 1 && CountPrefix(host, "DRAWBUFFER") == 2 && draws == 2;
        add("RenderColorTarget persistence across passes", ok);
    }

    // 5. ClearSetColor reads a float4 param initializer (via the binder)
    {
        const char* src =
            "float4 ClearColor = {0.2,0.4,0.6,0.8};\n"
            "float4 PS() : COLOR { return ClearColor; }\n"
            "technique T < string Script = \"ClearSetColor=ClearColor; Clear=Color;\" > {\n"
            "  pass P0 { PixelShader = compile ps_2_0 PS(); }\n"
            "}\n";
        MmeEffectDesc fx = LoadInline(src, "clearset");
        std::vector<MmeProgram> progs = CompileInline(fx, "clearset");
        std::vector<MmeConstantWriter*> store;
        std::vector<MmeDiag> diags;
        std::vector<MmeConstantWriter*> writers = BindWriters(fx, progs, store, diags);
        RecordingHost host(fx);
        host.print = false;
        host.writers = writers;
        const MmeFrameValues frame = MakeFrame();
        for (MmeConstantWriter* w : writers) w->Write(frame);
        const MmeTechnique* t = FindTechnique(fx, "");
        if (t) RunMmeTechnique(fx, *t, "scene", host);
        const bool ok = LogHas(host, "CLEAR color(0.2,0.4,0.6,0.8)");
        FreeWriters(store);
        add("ClearSetColor reads float4 initializer", ok);
    }

    // 6. RenderColorTarget alias of RenderColorTarget0
    {
        const auto cmds = ParseMmeScript("RenderColorTarget=ScnMap;");
        const bool ok = cmds.size() == 1 && cmds[0].command == "RenderColorTarget0" && cmds[0].arg == "ScnMap" && cmds[0].known;
        add("RenderColorTarget alias of RenderColorTarget0", ok);
    }

    // 7. matrix semantic column-major transpose check
    {
        const char* src =
            "float4x4 V2 : VIEW < string Object=\"Camera\"; >;\n"
            "float4 PS() : COLOR { return mul(float4(1,0,0,0), V2); }\n"
            "technique T { pass P0 { PixelShader = compile ps_2_0 PS(); } }\n";
        MmeEffectDesc fx = LoadInline(src, "matrix");
        std::vector<MmeProgram> progs = CompileInline(fx, "matrix");
        std::vector<MmeConstantWriter*> store;
        std::vector<MmeDiag> diags;
        std::vector<MmeConstantWriter*> writers = BindWriters(fx, progs, store, diags);
        const MmeFrameValues frame = MakeFrame();
        for (MmeConstantWriter* w : writers) w->Write(frame);
        float v2[4] = {};
        bool found2 = false, columnMajor = false;
        for (MmeConstantWriter* w : writers) {
            if (w->ReadValue("V2", v2)) found2 = true;
            for (const MmeBoundVar& bv : w->Vars())
                if (bv.name == "V2") columnMajor = bv.columnMajor;
        }
        const float* vm = reinterpret_cast<const float*>(&frame.view);
        // The variable is column-major (HLSL default): our row-vector matrix is written transposed, so its first
        // row equals the camera view matrix's first column.
        bool ok = found2 && columnMajor;
        const float expect2[4] = {vm[0], vm[4], vm[8], vm[12]};
        if (ok)
            for (int i = 0; i < 4; ++i)
                if (v2[i] != expect2[i]) ok = false;
        FreeWriters(store);
        add("matrix semantic column-major transpose", ok,
            "V2=" + FormatFloats(v2, 4) + " columnMajor=" + (columnMajor ? "1" : "0") + " expect=" + FormatFloats(expect2, 4));
    }

    // 8. CONTROLOBJECT (self) items Si (=si*10) / Tr / XYZ / bool
    {
        const char* src =
            "float Si : CONTROLOBJECT < string name=\"(self)\"; string item=\"Si\"; >;\n"
            "float Tr : CONTROLOBJECT < string name=\"(self)\"; string item=\"Tr\"; >;\n"
            "float3 XYZ : CONTROLOBJECT < string name=\"(self)\"; string item=\"XYZ\"; >;\n"
            "bool Vis : CONTROLOBJECT < string name=\"(self)\"; >;\n"
            "float4 PS() : COLOR { return float4(Si, Tr, XYZ.x, Vis ? 1 : 0); }\n"
            "technique T { pass P0 { PixelShader = compile ps_2_0 PS(); } }\n";
        MmeEffectDesc fx = LoadInline(src, "control");
        std::vector<MmeProgram> progs = CompileInline(fx, "control");
        std::vector<MmeConstantWriter*> store;
        std::vector<MmeDiag> diags;
        std::vector<MmeConstantWriter*> writers = BindWriters(fx, progs, store, diags);
        MmeFrameValues frame = MakeFrame();
        frame.self.x = 1; frame.self.y = 2; frame.self.z = 3;
        frame.self.si = 0.5f; frame.self.tr = 0.25f; frame.self.visible = true;
        for (MmeConstantWriter* w : writers) w->Write(frame);
        float si[4] = {}, tr[4] = {}, xyz[4] = {}, vis[4] = {};
        for (MmeConstantWriter* w : writers) {
            w->ReadValue("Si", si);
            w->ReadValue("Tr", tr);
            w->ReadValue("XYZ", xyz);
            w->ReadValue("Vis", vis);
        }
        const bool ok = si[0] == 5.0f && tr[0] == 0.25f && xyz[0] == 1.0f && xyz[1] == 2.0f && xyz[2] == 3.0f &&
                        vis[0] == 1.0f;
        FreeWriters(store);
        add("CONTROLOBJECT (self) Si/Tr/XYZ/bool", ok,
            "Si=" + std::to_string(si[0]) + " Tr=" + std::to_string(tr[0]) + " Vis=" + std::to_string(vis[0]));
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

// ---- file runs -----------------------------------------------------------------------------------------------------

void TraceWriter(const char* stage, const MmeConstantWriter& w) {
    for (const MmeBoundVar& v : w.Vars()) {
        float f[4] = {};
        w.ReadValue(v.name, f);
        int n = (int)(v.size / 4);
        if (n > 4) n = 4;
        std::printf("    [%s] %-24s %-28s = %s\n", stage, v.name.c_str(),
                    v.semantic.empty() ? "(no semantic)" : v.semantic.c_str(), FormatFloats(f, n).c_str());
    }
}

void RunFile(const std::filesystem::path& file, bool trace, bool all) {
    MmeEffectDesc fx = LoadMmeEffect(file);
    if (!fx.ok) {
        std::printf("cannot load %s\n", file.string().c_str());
        return;
    }
    std::vector<MmeProgram> progs = CompileMmeEffect(fx);
    std::vector<MmeConstantWriter*> store;
    std::vector<MmeDiag> diags;
    std::vector<MmeConstantWriter*> writers = BindWriters(fx, progs, store, diags);

    const MmeFrameValues frame = MakeFrame();
    for (MmeConstantWriter* w : writers) w->Write(frame);

    std::printf("===== %s (class=%s order=%s programs=%zu) =====\n", file.string().c_str(),
                fx.scriptClass.c_str(), fx.scriptOrder.c_str(), progs.size());

    RecordingHost host(fx);
    host.writers = writers;

    std::string cls = fx.scriptClass.empty() ? "object" : fx.scriptClass;
    const size_t count = all ? fx.techniques.size() : (fx.techniques.empty() ? 0 : 1);
    for (size_t i = 0; i < count; ++i) {
        const MmeTechnique& t = fx.techniques[i];
        std::printf("--- technique %s ---\n", t.name.c_str());
        int draws = RunMmeTechnique(fx, t, cls, host);
        std::printf("draws=%d\n", draws);
    }

    if (trace) {
        std::printf("-- constants --\n");
        size_t idx = 0;
        for (const MmeProgram& pr : progs) {
            if (!pr.vs.cb.empty() && idx < writers.size()) TraceWriter("vs", *writers[idx++]);
            if (!pr.ps.cb.empty() && idx < writers.size()) TraceWriter("ps", *writers[idx++]);
        }
    }

    for (const MmeDiag& d : diags)
        std::printf("  diag: %s\n", d.message.c_str());

    FreeWriters(store);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::filesystem::path> inputs;
    bool trace = false, all = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--trace") trace = true;
        else if (a == L"--all") all = true;
        else inputs.push_back(a);
    }

    std::printf("---- selftest ----\n");
    const bool selfOk = RunSelfTest();

    for (const std::filesystem::path& f : inputs) RunFile(f, trace, all);

    std::printf("selftest: %s\n", selfOk ? "PASS" : "FAIL");
    return selfOk ? 0 : 1;
}
