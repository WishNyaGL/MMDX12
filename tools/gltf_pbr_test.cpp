// gltf_pbr_test : checks the P6 glTF/VRM -> PBR conversion (asset/PbrMaterial.h BuildImportedPbrModelData). Loads
// captures/assets_dl/BoomBox.glb (Khronos metallic-roughness / normal / occlusion / emissive sample) and
// captures/assets_dl/Seed-san.vrm (VRM: MToon = NPR, zero records). A missing file prints SKIP and is not a failure.
// Exit code 0 = pass, 1 = a check failed. Style: like tools/raymat_test.cpp.
#include "asset/ModelImport.h"
#include "asset/PbrMaterial.h"
#include "asset/PmxModel.h"

#include <algorithm>
#include <cmath>
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

// tools/testdata/<name>, found the same way (a committed generated file: no Blender in the test).
fs::path FindTestData(const char* name) {
    const fs::path rel = fs::path("tools") / "testdata" / name;
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

// The material of a loaded ext_test.glb by name (-1 when absent).
int MaterialIndex(const PmxModel& model, const char* name) {
    for (size_t i = 0; i < model.materials.size(); ++i)
        if (model.materials[i].name == name) return (int)i;
    return -1;
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

    // ---- ext_test.glb: the S2a glTF material / light extensions (tools/testdata/make_ext_test.py) ----
    const fs::path ext = FindTestData("ext_test.glb");
    if (ext.empty()) {
        printf("SKIP ext_test.glb (not found)\n");
    } else {
        PmxModel model;
        std::string err;
        if (!LoadModelFile(ext, ModelRole::Stage, model, &err)) {
            Expect(false, ("ext_test.glb loads (" + err + ")").c_str());
        } else {
            Expect(model.importedPbr.size() == model.materials.size(),
                   "ext_test: importedPbr is parallel to materials");
            PbrModelData pbr;
            BuildImportedPbrModelData(model, pbr);

            // Glass sphere: transmission 1, ior 1.45, volume thickness 0.6, attenuation colour / distance.
            const int glass = MaterialIndex(model, "Glass");
            Expect(glass >= 0, "ext_test: the Glass material is present");
            if (glass >= 0 && (size_t)glass < model.importedPbr.size()) {
                const ImportedPbr& ip = model.importedPbr[(size_t)glass];
                Expect(ip.valid, "ext_test: Glass has PBR data");
                Expect(std::fabs(ip.transmission - 1.0f) < 1e-4f, "ext_test: Glass transmissionFactor 1");
                Expect(ip.transmissionTex >= 0, "ext_test: Glass carries its transmission map");
                Expect(std::fabs(ip.ior - 1.45f) < 1e-3f, "ext_test: Glass ior 1.45");
                Expect(ip.volumeThickness > 0.5f && ip.thicknessTex >= 0,
                       "ext_test: Glass volume thickness factor + map");
                Expect(ip.attenuationDistance > 2.4f && ip.attenuationDistance < 2.6f,
                       "ext_test: Glass attenuationDistance 2.5");
                Expect(ip.attenuationColor[0] < ip.attenuationColor[1],
                       "ext_test: Glass attenuationColor is the authored greenish tint");
                if ((size_t)glass < pbr.materials.size() && pbr.materials[(size_t)glass].valid) {
                    const PbrMaterial& m = pbr.materials[(size_t)glass];
                    Expect(std::fabs(m.ext.transmission - 1.0f) < 1e-4f && std::fabs(m.ext.ior - 1.45f) < 1e-3f,
                           "ext_test: the record carries transmission / ior");
                    Expect(m.ext.volumeThickness > 0.5f && m.ext.volumeAttenuationDistance > 2.4f,
                           "ext_test: the record carries the volume values");
                    Expect(m.ext.transmissionTex.kind == PbrTexKind::File &&
                               m.ext.volumeThicknessTex.kind == PbrTexKind::File,
                           "ext_test: the transmission / thickness maps are decoded");
                }
            }

            // Panel: emissive strength 15 -> an HDR emissive factor in the record's emissive slot.
            const int panel = MaterialIndex(model, "Panel");
            Expect(panel >= 0, "ext_test: the Panel material is present");
            if (panel >= 0 && (size_t)panel < model.importedPbr.size()) {
                const ImportedPbr& ip = model.importedPbr[(size_t)panel];
                Expect(std::fabs(ip.emissiveStrength - 15.0f) < 1e-3f, "ext_test: Panel emissive strength 15");
                Expect(ip.emissive[2] > 0.99f && ip.emissive[1] > 0.8f && ip.emissive[0] < 0.3f,
                       "ext_test: Panel emissive factor is the authored cyan (not pre-multiplied)");
                if ((size_t)panel < pbr.materials.size() && pbr.materials[(size_t)panel].valid) {
                    const PbrMaterial& m = pbr.materials[(size_t)panel];
                    Expect(m.emissiveEnable && std::fabs(m.ext.emissiveStrength - 15.0f) < 1e-3f,
                           "ext_test: the record keeps emissive strength 15 (HDR)");
                }
            }

            // Coat: a clearcoat factor / roughness on a red base.
            const int coat = MaterialIndex(model, "Coat");
            Expect(coat >= 0, "ext_test: the Coat material is present");
            if (coat >= 0 && (size_t)coat < model.importedPbr.size()) {
                const ImportedPbr& ip = model.importedPbr[(size_t)coat];
                Expect(std::fabs(ip.clearcoat - 1.0f) < 1e-4f, "ext_test: Coat clearcoatFactor 1");
                Expect(std::fabs(ip.clearcoatRoughness - 0.05f) < 1e-3f, "ext_test: Coat roughness 0.05");
                if ((size_t)coat < pbr.materials.size() && pbr.materials[(size_t)coat].valid)
                    Expect(std::fabs(pbr.materials[(size_t)coat].ext.clearcoat - 1.0f) < 1e-4f,
                           "ext_test: the record carries the clearcoat");
            }

            // Checker: KHR_texture_transform on the base colour map (offset / scale / rotation).
            const int checker = MaterialIndex(model, "Checker");
            Expect(checker >= 0, "ext_test: the Checker material is present");
            if (checker >= 0 && (size_t)checker < model.importedPbr.size()) {
                const ImportedPbr& ip = model.importedPbr[(size_t)checker];
                Expect(std::fabs(ip.baseColorUv.scaleU - 2.0f) < 1e-3f && std::fabs(ip.baseColorUv.scaleV - 2.0f) < 1e-3f,
                       "ext_test: Checker base colour uv scale 2");
                Expect(std::fabs(ip.baseColorUv.rotation - 0.5235988f) < 1e-3f,
                       "ext_test: Checker base colour uv rotation 30 degrees");
                Expect(std::fabs(ip.baseColorUv.offsetU) > 0.1f || std::fabs(ip.baseColorUv.offsetV) > 0.1f,
                       "ext_test: Checker base colour uv offset");
                if ((size_t)checker < pbr.materials.size() && pbr.materials[(size_t)checker].valid) {
                    const PbrMaterial& m = pbr.materials[(size_t)checker];
                    const PbrSlot& alb = m.slots[(int)PbrSlotId::Albedo];
                    Expect(std::fabs(alb.uvScaleU - 2.0f) < 1e-3f && std::fabs(alb.uvOffsetU) > 0.1f,
                           "ext_test: the slot carries the uv scale / offset");
                    Expect(std::fabs(m.ext.uvRotation - 0.5235988f) < 1e-3f,
                           "ext_test: the record carries the uv rotation");
                }
            }

            // Lights: one point + one spot, converted to engine units at import.
            Expect(model.importedLights.size() == 2, "ext_test: two KHR_lights_punctual lights imported");
            for (const ImportedLight& l : model.importedLights) {
                if (l.name == "KeyPoint") {
                    Expect(l.kind == ImportedLight::Kind::Point, "ext_test: KeyPoint is a point light");
                    Expect(l.intensity > 0.45f && l.intensity < 0.55f,
                           "ext_test: a Blender 1000 W point light converts to ~0.5 engine intensity");
                    Expect(l.position.y > 50.0f, "ext_test: KeyPoint is 5 m above the origin in MMD units");
                } else if (l.name == "KeySpot") {
                    Expect(l.kind == ImportedLight::Kind::Spot, "ext_test: KeySpot is a spot light");
                    Expect(l.outerCone > l.innerCone && l.outerCone > 0.3f,
                           "ext_test: KeySpot keeps its cone angles");
                    Expect(l.direction.y < -0.3f, "ext_test: KeySpot points down at the scene");
                }
            }
        }
    }

    printf("selftest: %s\n", g_fails ? "FAILED" : "ok");
    return g_fails ? 1 : 0;
}
