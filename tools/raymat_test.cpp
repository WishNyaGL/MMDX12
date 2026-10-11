// raymat_test : a synthetic self test of the Ray-MMD material parser (asset/RayMaterial.h). Writes an .emd + .fx
// tree (Obj / Obj[n] / none, a #if branch, float3 values, define-to-define references, a nested include, an
// include cycle, a missing map file, a non-Ray fx) into a temp dir, then checks candidate ranking, the .emd parse,
// slots and the content rule. Exit code 0 = pass. No model file is needed (the candidate scan only looks at the
// folder). Style: like tools/pack_suggest_test.cpp / rest_pose_test.cpp.
#include "asset/RayMaterial.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace mmdx;
namespace fs = std::filesystem;

namespace {

int g_fails = 0;
void Expect(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

void Write(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

// The Ray .fx: slot macros, a #if branch, float3 values, a define-to-define reference, a nested include and a
// cycle back to itself.
const char* kRayFx = R"(// a Ray material
#define ALBEDO_MAP_FROM 1
#define ALBEDO_MAP_FILE "tex.png"
#define ALBEDO_MAP_UV_FLIP 1
#define NORMAL_MAP_FROM ALBEDO_MAP_FROM
#define NORMAL_SUB_MAP_FROM 0
#define CUSTOM_ENABLE 3
#define SPECULAR_MAP_FROM 1
#define SPECULAR_MAP_FILE "missing_spec.png"

const float3 albedo = float3(0.25, 0.5, 0.75);
const float3 albedoSub = 0.8;
const float smoothness = 0.4;
const float2 albedoMapLoopNum = float2(2.0, 3.0);

#if CUSTOM_ENABLE == 3
#define ANISO_ON 1
#elif CUSTOM_ENABLE == 1
#define ANISO_ON 0
#else
#define ANISO_ON 9
#endif
#ifdef CUSTOM_ENABLE
#define SEEN 1
#endif

#include "nested.h"
)";

// The nested include also declares a slot macro and includes back the root (a cycle).
const char* kNested = R"(
#define NESTED 42
#define NORMAL_SUB_MAP_FROM 5
#include "ray.fx"
)";

// A plain MMD effect (SeriousShader-like): no slot macros, no MaterialParam / EncodeGbuffer.
const char* kNonRayFx = R"(// a SeriousShader-like effect
#define ShadowDarkness 1.6
#define ToonPower 0.9
float4 MaterialDiffuse : DIFFUSE<string Object = "Geometry";>;
)";

// 4 non-none entries, 2 Ray (the default and Obj[0]); Obj[1] is a deliberate "none".
const char* kEmdA = R"([Info]
Version = 3

[Effect]
Obj = ray.fx
Obj.show = true
Obj[0] = ray.fx
Obj[1] = none
Obj[2] = plain.fx
)";

// 2 non-none entries, both Ray: share 100% beats a.emd's 50%.
const char* kEmdB = R"([Effect]
Obj[0] = ray.fx
Obj[2] = ray.fx
)";

} // namespace

int wmain() {
    const fs::path dir = fs::temp_directory_path() / L"mmdx_raymat_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    Write(dir / L"ray.fx", kRayFx);
    Write(dir / L"nested.h", kNested);
    Write(dir / L"plain.fx", kNonRayFx);
    Write(dir / L"tex.png", "not an image");
    Write(dir / L"a.emd", kEmdA);
    Write(dir / L"b.emd", kEmdB);

    // Content rule: the Ray .fx (through its include chain) is Ray; the plain effect is not.
    Expect(IsRayMaterialFx(dir / L"ray.fx"), "content rule: slot-macro fx is Ray");
    Expect(!IsRayMaterialFx(dir / L"plain.fx"), "content rule: SeriousShader-like fx is not Ray");
    Expect(!IsRayMaterialFx(dir / L"missing.fx"), "content rule: missing file is not Ray");

    // Candidate ranking: b.emd is 100% Ray, a.emd 50%.
    const std::vector<RayEmdCandidate> cands = FindRayEmdCandidates(dir / L"model.pmx", 3);
    Expect(cands.size() == 2, "two .emd candidates found");
    if (cands.size() == 2) {
        Expect(cands[0].path.filename() == "b.emd", "b.emd ranks first (higher Ray share)");
        Expect(cands[0].rayEntries == 2 && cands[0].raySharePct == 100, "b.emd: 2/2 Ray entries, share 100%");
        Expect(cands[1].path.filename() == "a.emd", "a.emd second");
        Expect(cands[1].objCount == 4 && cands[1].rayEntries == 2 && cands[1].raySharePct == 50,
               "a.emd: 4 Obj entries, 2 Ray, share 50%");
        Expect(cands[1].inRangeEntries == 3, "a.emd: 3 indexed entries in PMX material range");
    }

    // FindAndParse picks the best candidate. b.emd has no default, so material 1 is unassigned.
    {
        std::vector<RayMaterialDesc> descs;
        fs::path chosen;
        RayParseStats stats;
        std::string err;
        Expect(FindAndParseRayMaterials(dir / L"model.pmx", 3, descs, &chosen, &stats, &err), "find + parse succeeds");
        Expect(chosen.filename() == "b.emd", "the best .emd (b) is chosen");
        Expect(descs.size() == 3, "one desc per material");
        if (descs.size() == 3) {
            Expect(descs[0].valid && descs[0].ray, "b: material 0 is Ray");
            Expect(!descs[1].valid, "b: material 1 unassigned -> no desc");
            Expect(descs[2].valid && descs[2].ray, "b: material 2 is Ray");
        }
    }

    // Parse a.emd directly: the default + Obj[0] are ray.fx, Obj[1] is none, Obj[2] is a non-Ray fx.
    {
        std::vector<RayMaterialDesc> descs;
        RayParseStats stats;
        std::string err;
        Expect(ParseRayEmd(dir / L"a.emd", 3, descs, &stats, &err), "a.emd parses");
        Expect(descs.size() == 3, "a: one desc per material");
        if (descs.size() == 3) {
            const RayMaterialDesc& m0 = descs[0];
            Expect(m0.valid && m0.ray, "a material 0: valid Ray desc (from the default)");
            Expect(m0.shadingModel == 3, "a material 0: CUSTOM_ENABLE 3");
            Expect(m0.slots[(int)RaySlot::Albedo].from == 1, "a material 0: ALBEDO FROM 1");
            Expect(m0.slots[(int)RaySlot::Albedo].uvFlip == 1, "a material 0: ALBEDO UV_FLIP 1");
            Expect(m0.slots[(int)RaySlot::Albedo].hasValue && m0.slots[(int)RaySlot::Albedo].valueComponents == 3 &&
                       m0.slots[(int)RaySlot::Albedo].value[0] > 0.24f &&
                       m0.slots[(int)RaySlot::Albedo].value[0] < 0.26f &&
                       m0.slots[(int)RaySlot::Albedo].value[2] > 0.74f,
                   "a material 0: ALBEDO float3 value (0.25,0.5,0.75)");
            Expect(!m0.slots[(int)RaySlot::Albedo].file.empty() &&
                       m0.slots[(int)RaySlot::Albedo].file.filename() == "tex.png" &&
                       !m0.slots[(int)RaySlot::Albedo].fileMissing,
                   "a material 0: ALBEDO map file resolves");
            Expect(m0.slots[(int)RaySlot::Albedo].loopU > 1.99f && m0.slots[(int)RaySlot::Albedo].loopU < 2.01f &&
                       m0.slots[(int)RaySlot::Albedo].loopV > 2.99f,
                   "a material 0: ALBEDO loop (2,3)");
            Expect(m0.slots[(int)RaySlot::Normal].from == 1, "a material 0: NORMAL FROM via define reference");
            Expect(m0.slots[(int)RaySlot::NormalSub].from == 5, "a material 0: NORMAL_SUB FROM from nested include");
            Expect(m0.slots[(int)RaySlot::Smoothness].hasValue &&
                       m0.slots[(int)RaySlot::Smoothness].value[0] > 0.39f &&
                       m0.slots[(int)RaySlot::Smoothness].value[0] < 0.41f,
                   "a material 0: SMOOTHNESS value 0.4");
            Expect(m0.slots[(int)RaySlot::Specular].fileMissing, "a material 0: missing specular map flagged");
            Expect(m0.defines.count("ANISO_ON") && m0.defines.count("SEEN") && m0.defines.count("NESTED"),
                   "a material 0: #if branch / #ifdef / nested include defines survive");
            Expect(m0.defines.count("CUSTOM_ENABLE"), "a material 0: raw define table kept");
            Expect(!descs[1].valid, "a material 1: none -> no desc");
            Expect(descs[2].valid && !descs[2].ray, "a material 2: valid but not Ray");
        }
    }

    // A model whose only .emd points at a non-Ray fx must not resolve.
    {
        const fs::path d2 = dir / L"plainonly";
        fs::create_directories(d2, ec);
        Write(d2 / L"plain.fx", kNonRayFx);
        Write(d2 / L"p.emd", "[Effect]\nObj[0] = plain.fx\n");
        std::vector<RayMaterialDesc> d;
        fs::path c;
        const bool ok = FindAndParseRayMaterials(d2 / L"p.pmx", 1, d, &c);
        Expect(!ok && c.empty(), "a non-Ray .emd is not chosen");
    }

    fs::remove_all(dir, ec);
    printf("selftest: %s\n", g_fails ? "FAILED" : "ok");
    return g_fails ? 1 : 0;
}
