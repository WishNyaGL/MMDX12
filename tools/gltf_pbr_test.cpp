// gltf_pbr_test : checks the P6 glTF/VRM -> PBR conversion (asset/PbrMaterial.h BuildImportedPbrModelData). Loads
// captures/assets_dl/BoomBox.glb (Khronos metallic-roughness / normal / occlusion / emissive sample) and
// captures/assets_dl/Seed-san.vrm (VRM: MToon = NPR, zero records). A missing file prints SKIP and is not a failure.
// Exit code 0 = pass, 1 = a check failed. Style: like tools/raymat_test.cpp.
#include "asset/ModelImport.h"
#include "asset/PbrMaterial.h"
#include "asset/PmxModel.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace mmdx;
namespace fs = std::filesystem;

namespace {

int g_fails = 0;
void Expect(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

// Finds captures/assets_dl/<name> by walking up from the working directory (works from the repo root or build/bin)
// and by the test source's own directory. Returns empty when absent.
fs::path FindAsset(const char* name) {
    const fs::path rel = fs::path("captures") / "assets_dl" / name;
    std::error_code ec;
    for (fs::path dir = fs::current_path(ec); !dir.empty(); dir = dir.parent_path()) {
        const fs::path p = dir / rel;
        if (fs::is_regular_file(p, ec)) return p;
        if (dir == dir.parent_path()) break;
    }
    const fs::path near = fs::path(__FILE__).parent_path().parent_path() / rel;
    if (fs::is_regular_file(near, ec)) return near;
    return {};
}

} // namespace

int wmain() {
    // ---- BoomBox.glb: metallic-roughness, normal, occlusion and emissive maps + factors ----
    const fs::path box = FindAsset("BoomBox.glb");
    if (box.empty()) {
        printf("SKIP BoomBox.glb (not found)\n");
    } else {
        PmxModel model;
        std::string err;
        if (!LoadModelFile(box, ModelRole::Prop, model, &err)) {
            Expect(false, ("BoomBox.glb loads (" + err + ")").c_str());
        } else {
            Expect(model.importedPbr.size() == model.materials.size(),
                   "BoomBox: importedPbr is parallel to materials");
            if (!model.importedPbr.empty()) {
                const ImportedPbr& ip = model.importedPbr[0];
                Expect(ip.valid, "BoomBox: material 0 has PBR data");
                Expect(ip.metalRoughTex >= 0 && ip.normalTex >= 0 && ip.occlusionTex >= 0 && ip.emissiveTex >= 0,
                       "BoomBox: all four PBR maps are carried");
                Expect(ip.emissive[0] > 0.99f && ip.emissive[1] > 0.99f && ip.emissive[2] > 0.99f,
                       "BoomBox: emissive factor (1,1,1)");
            }

            PbrModelData pbr;
            BuildImportedPbrModelData(model, pbr);
            Expect(pbr.materialsWithRecord == 1, "BoomBox: one material with a record");
            // One packed metal-roughness image read as two R8 entries (G roughness, B metalness) plus normal (RGBA),
            // occlusion (R) and emissive (RGBA): 5 decoded textures.
            Expect(pbr.texturePaths.size() == 5, "BoomBox: 5 decoded PBR textures");
            auto hasChannel = [&](int c) {
                return std::find(pbr.textureChannels.begin(), pbr.textureChannels.end(), c) != pbr.textureChannels.end();
            };
            Expect(hasChannel(1) && hasChannel(2), "BoomBox: roughness (G=1) and metalness (B=2) channels present");
            Expect(hasChannel(0), "BoomBox: occlusion R (0) channel present");
            Expect(hasChannel(-1), "BoomBox: an RGBA (full) map present (normal / emissive)");

            if (pbr.materials.size() == 1 && pbr.materials[0].valid) {
                const PbrMaterial& m = pbr.materials[0];
                const PbrSlot& rough = m.slots[(int)PbrSlotId::Smoothness];
                const PbrSlot& metal = m.slots[(int)PbrSlotId::Metalness];
                const PbrSlot& nrm = m.slots[(int)PbrSlotId::Normal];
                const PbrSlot& occ = m.slots[(int)PbrSlotId::Occlusion];
                const PbrSlot& emi = m.slots[(int)PbrSlotId::Emissive];
                const PbrSlot& alb = m.slots[(int)PbrSlotId::Albedo];
                Expect(rough.type == 2 && rough.swizzle == 1 && rough.tex.kind == PbrTexKind::File,
                       "BoomBox: SMOOTHNESS = G channel, linear roughness, file");
                Expect(metal.swizzle == 2 && metal.tex.kind == PbrTexKind::File,
                       "BoomBox: METALNESS = B channel, file");
                Expect(nrm.type == 0 && nrm.tex.kind == PbrTexKind::File && m.slots[(int)PbrSlotId::Normal].hasScale,
                       "BoomBox: NORMAL = RGB tangent space with scale");
                Expect(occ.swizzle == 0 && occ.tex.kind == PbrTexKind::File && occ.aoStrength > 0.99f,
                       "BoomBox: OCCLUSION = R channel, strength 1");
                Expect(emi.tex.kind == PbrTexKind::File && m.emissiveEnable,
                       "BoomBox: EMISSIVE map + enable");
                Expect(alb.from == 3 && m.albedoApplyDiffuse == 1,
                       "BoomBox: ALBEDO = PMX diffuse texture with the factor applied once");
            }
        }
    }

    // ---- Seed-san.vrm: VRM / MToon is NPR, so no records at all ----
    const fs::path vrm = FindAsset("Seed-san.vrm");
    if (vrm.empty()) {
        printf("SKIP Seed-san.vrm (not found)\n");
    } else {
        Expect(!ModelHasImportedPbr(vrm), "Seed-san.vrm: no imported PBR data (VRM / MToon is NPR)");
        PmxModel model;
        std::string err;
        if (!LoadModelFile(vrm, ModelRole::Prop, model, &err)) {
            printf("SKIP Seed-san.vrm (load: %s)\n", err.c_str());
        } else {
            PbrModelData pbr;
            BuildImportedPbrModelData(model, pbr);
            Expect(pbr.materialsWithRecord == 0, "Seed-san.vrm: zero PBR records");
        }
    }

    printf("selftest: %s\n", g_fails ? "FAILED" : "ok");
    return g_fails ? 1 : 0;
}
