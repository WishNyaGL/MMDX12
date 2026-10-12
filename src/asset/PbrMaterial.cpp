// PbrMaterial.cpp: Ray-MMD -> engine-neutral PBR conversion (see PbrMaterial.h). Resolves constants on the CPU,
// keeps the raw FROM/TYPE/UV hints the shader needs, and decodes the referenced file textures once each.
#include "asset/PbrMaterial.h"
#include "asset/PmxModel.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <execution>
#include <map>
#include <set>

namespace mmdx {
namespace {

// Ray-MMD constants (docs/raymmd_semantics.md §1.9, §4).
constexpr float kDefaultAlphaThreshold = 0.999f;

float DefineFloat(const std::map<std::string, std::string>& defines, const char* name, float fallback) {
    const auto it = defines.find(name);
    if (it == defines.end()) return fallback;
    const char* s = it->second.c_str();
    char* end = nullptr;
    const double d = strtod(s, &end);
    return end != s ? (float)d : fallback;
}

int DefineInt(const std::map<std::string, std::string>& defines, const char* name, int fallback) {
    const auto it = defines.find(name);
    if (it == defines.end()) return fallback;
    return atoi(it->second.c_str());
}

// ShininessToSmoothness (material_common_2.0.fxsub): 1 - (2 / (spec + 2))^0.125. `spec == 0` -> 0.
float ShininessToSmoothness(float spec) {
    if (spec <= 0.0f) return 0.0f;
    return 1.0f - std::pow(std::max(0.0f, 2.0f / (spec + 2.0f)), 0.125f);
}

// True for a slot that reads a single channel of its map (the rest is RGB / a constant).
bool SingleChannelSlot(PbrSlotId id, int type) {
    switch (id) {
    case PbrSlotId::Alpha:
    case PbrSlotId::Smoothness:
    case PbrSlotId::Metalness:
    case PbrSlotId::Occlusion:
    case PbrSlotId::CustomA:
    case PbrSlotId::CustomB:
        return true;
    case PbrSlotId::Specular:
        return type >= 2;   // SPECULAR_MAP_TYPE 2/3 are grayscale
    default:
        return false;
    }
}

} // namespace

const char* PbrSlotName(PbrSlotId slot) {
    switch (slot) {
    case PbrSlotId::Albedo: return "ALBEDO";
    case PbrSlotId::AlbedoSub: return "ALBEDO_SUB";
    case PbrSlotId::Alpha: return "ALPHA";
    case PbrSlotId::Normal: return "NORMAL";
    case PbrSlotId::NormalSub: return "NORMAL_SUB";
    case PbrSlotId::Smoothness: return "SMOOTHNESS";
    case PbrSlotId::Metalness: return "METALNESS";
    case PbrSlotId::Specular: return "SPECULAR";
    case PbrSlotId::Occlusion: return "OCCLUSION";
    case PbrSlotId::Emissive: return "EMISSIVE";
    case PbrSlotId::CustomA: return "CUSTOM_A";
    case PbrSlotId::CustomB: return "CUSTOM_B";
    default: return "?";
    }
}

PbrMaterial RayToPbr(const RayMaterialDesc& desc, const PmxMaterial& pmxMat) {
    PbrMaterial out;
    if (!desc.valid || !desc.ray) return out;   // no Ray content rule match -> no record
    out.valid = true;
    out.shadingModel = desc.shadingModel;
    out.alphaThreshold = DefineFloat(desc.defines, "ALPHA_THRESHOLD", kDefaultAlphaThreshold);
    out.albedoSubEnable = DefineInt(desc.defines, "ALBEDO_SUB_ENABLE", 0);
    out.albedoApplyDiffuse = DefineInt(desc.defines, "ALBEDO_MAP_APPLY_DIFFUSE", 0);
    out.emissiveEnable = DefineInt(desc.defines, "EMISSIVE_ENABLE", 0) != 0;

    // Ray and PBR slot orders differ (Ray has Parallax between Occlusion and Emissive, P2 drops it).
    static const RaySlot kRayOfPbr[kPbrSlotCount] = {
        RaySlot::Albedo,  RaySlot::AlbedoSub, RaySlot::Alpha,   RaySlot::Normal,    RaySlot::NormalSub,
        RaySlot::Smoothness, RaySlot::Metalness, RaySlot::Specular, RaySlot::Occlusion, RaySlot::Emissive,
        RaySlot::CustomA, RaySlot::CustomB,
    };
    for (int i = 0; i < kPbrSlotCount; ++i) {
        const RayMapSlot& src = desc.slots[(int)kRayOfPbr[i]];
        PbrSlot& dst = out.slots[i];
        dst.from = src.from;
        dst.type = src.type;
        dst.swizzle = src.swizzle;
        dst.uvFlip = src.uvFlip;
        dst.applyScale = src.applyScale;
        dst.colorFlip = src.colorFlip;
        dst.hasValue = src.hasValue;
        for (int k = 0; k < 3; ++k) dst.value[k] = src.value[k];
        dst.hasScale = src.hasScale;
        dst.scale = src.scale;
        dst.loopU = src.loopU;
        dst.loopV = src.loopV;
        dst.uvScaleU = src.uvScaleU;
        dst.uvScaleV = src.uvScaleV;

        switch (src.from) {
        case 0:
            break;   // the constant value (as declared)
        case 1:
        case 2:
            if (!src.file.empty() && !src.fileMissing) {
                dst.tex.kind = PbrTexKind::File;
                dst.tex.path = src.file;
            }
            break;
        case 3:
            if (pmxMat.textureIndex >= 0) {
                dst.tex.kind = PbrTexKind::PmxDiffuse;
                dst.tex.pmxIndex = pmxMat.textureIndex;
            }
            break;
        case 4:
            if (pmxMat.sphereTextureIndex >= 0) {
                dst.tex.kind = PbrTexKind::PmxSphere;
                dst.tex.pmxIndex = pmxMat.sphereTextureIndex;
            }
            break;
        case 5:
            // A shared toon is a built-in ramp (no model texture): fall back to the constant.
            if (!pmxMat.sharedToon && pmxMat.toonIndex >= 0) {
                dst.tex.kind = PbrTexKind::PmxToon;
                dst.tex.pmxIndex = pmxMat.toonIndex;
            }
            break;
        case 6:
            break;   // screen dummy: unsupported, the constant stands
        case 7:       // PMX ambient colour
            dst.hasValue = true;
            dst.value[0] = pmxMat.ambient.x;
            dst.value[1] = pmxMat.ambient.y;
            dst.value[2] = pmxMat.ambient.z;
            break;
        case 8:       // PMX specular colour
            dst.hasValue = true;
            dst.value[0] = pmxMat.specular.x;
            dst.value[1] = pmxMat.specular.y;
            dst.value[2] = pmxMat.specular.z;
            break;
        case 9:       // PMX specular power -> smoothness (SMOOTHNESS / METALNESS only)
            dst.hasValue = true;
            dst.value[0] = ShininessToSmoothness(pmxMat.specularPower);
            break;
        default:
            break;
        }
    }
    return out;
}

void DecodePbrTextures(PbrModelData& data, const char* label) {
    // Group texture slots by (source, channel). channel < 0 = full RGBA, 0..3 = one channel (R8 upload).
    // The channel of a single-channel read is its swizzle. `File` sources dedupe by resolved path, `Embedded`
    // sources by the PmxModel texture index; both are keyed with the channel (the same packed metal-roughness image
    // is read as two R8 entries: G = roughness, B = metalness).
    struct Entry {
        std::filesystem::path path;                 // File source
        const std::vector<uint8_t>* bytes = nullptr; // Embedded source (valid during the decode only)
        int channel = -1;
        float mapScale = 1.0f;                       // multiply the decoded channel (glTF roughness/metalness factor)
        float aoStrength = -1.0f;                    // >= 0: bake glTF occlusion strength (lerp 1 .. map) into `channel`
        std::vector<std::pair<int, int>> slots;      // (material, slot) referencing this entry
    };
    std::map<std::string, int> byKey;
    std::vector<Entry> entries;
    for (int mi = 0; mi < (int)data.materials.size(); ++mi) {
        PbrMaterial& m = data.materials[mi];
        if (!m.valid) continue;
        ++data.materialsWithRecord;
        for (int si = 0; si < kPbrSlotCount; ++si) {
            PbrSlot& s = m.slots[si];
            if (s.tex.kind != PbrTexKind::File && s.tex.kind != PbrTexKind::Embedded) continue;
            const bool single = SingleChannelSlot((PbrSlotId)si, s.type);
            const int channel = single ? s.swizzle : -1;
            // glTF occlusion strength (lerp 1 .. map) and the roughness / metalness factors have no Ray slot flag to
            // ride on; they are baked into the decoded R8 channel here. RayToPbr never sets these, so Ray output is
            // byte-identical (its `mapScale` stays 1 and `aoStrength` -1).
            const float aoStrength = ((PbrSlotId)si == PbrSlotId::Occlusion && single) ? s.aoStrength : -1.0f;
            std::string key = (s.tex.kind == PbrTexKind::File ? PathToUtf8(s.tex.path)
                                                              : "emb:" + std::to_string(s.tex.pmxIndex)) +
                              "|" + std::to_string(channel) + "|" + std::to_string(s.mapScale) + "|" +
                              std::to_string(aoStrength);
            auto it = byKey.find(key);
            int index;
            if (it == byKey.end()) {
                Entry e;
                e.path = s.tex.path;
                e.bytes = s.tex.bytes;
                e.channel = channel;
                e.mapScale = s.mapScale;
                e.aoStrength = aoStrength;
                index = (int)entries.size();
                byKey.emplace(key, index);
                entries.push_back(std::move(e));
            } else {
                index = it->second;
            }
            entries[index].slots.emplace_back(mi, si);
        }
    }

    data.texturePaths.resize(entries.size());
    data.textures.resize(entries.size());
    data.textureChannels.resize(entries.size(), -1);

    for (size_t e = 0; e < entries.size(); ++e) {
        data.texturePaths[e] = entries[e].path;
        data.textureChannels[e] = entries[e].channel;
    }

    const int total = (int)entries.size();
    std::atomic<int> missing{0};
    // Map-size cap: a decoded texture whose longest side exceeds this drops its leading mip levels until level 0
    // fits, then keeps the remaining (already box-filtered) chain. Keeps VRAM bounded for 4K source maps.
    constexpr uint32_t kMaxPbrMapSize = 2048;
    std::for_each(std::execution::par, entries.begin(), entries.end(), [&](Entry& e) {
        const size_t idx = (size_t)(&e - entries.data());
        ImageRGBA8 img;
        std::string err;
        bool loaded = false;
        if (e.bytes && !e.bytes->empty()) {
            loaded = LoadImageRGBA8FromMemory(e.bytes->data(), e.bytes->size(), img, &err);
        } else if (!e.path.empty()) {
            loaded = LoadImageRGBA8(e.path, img, &err);
        }
        if (loaded) {
            size_t drop = 0;
            while (drop + 1 < img.mips.size() &&
                   std::max(img.mips[drop].width, img.mips[drop].height) > kMaxPbrMapSize)
                ++drop;
            if (drop > 0) {
                img.mips.erase(img.mips.begin(), img.mips.begin() + (std::ptrdiff_t)drop);
                // hasAlpha reflected level 0 before the cap; recompute over the capped level 0.
                img.hasAlpha = false;
                for (size_t i = 3; i < img.mips[0].pixels.size(); i += 4)
                    if (img.mips[0].pixels[i] < 255) { img.hasAlpha = true; break; }
            }
            // glTF occlusion: ao = 1 + strength * (map - 1), baked so the shader's PbrSlotAo stays unchanged.
            if (e.aoStrength >= 0.0f && e.channel >= 0 && e.aoStrength < 0.999f) {
                const size_t c = (size_t)e.channel;
                for (ImageRGBA8::Level& lv : img.mips)
                    for (size_t p = c; p < lv.pixels.size(); p += 4)
                        lv.pixels[p] = (uint8_t)std::lround(
                            std::clamp(255.0f * (1.0f + e.aoStrength * ((float)lv.pixels[p] / 255.0f - 1.0f)), 0.0f, 255.0f));
            }
            // glTF roughness / metallic factors: multiply the decoded R8 channel by the factor.
            if (e.mapScale != 1.0f && e.channel >= 0)
                for (ImageRGBA8::Level& lv : img.mips)
                    for (size_t p = (size_t)e.channel; p < lv.pixels.size(); p += 4)
                        lv.pixels[p] = (uint8_t)std::lround(std::clamp((float)lv.pixels[p] * e.mapScale, 0.0f, 255.0f));
            data.textures[idx] = std::move(img);
        } else if (e.bytes || !e.path.empty()) {
            LOG_WARN("PBR map load failed: %s (%s): %s",
                     e.path.empty() ? "(embedded)" : PathToUtf8(e.path).c_str(), label, err.c_str());
            ++missing;
        }
    });
    data.filesMissing = missing.load();
    (void)total;

    // A missing / undecodable source falls the slot back to its constant; every other slot points the GPU record
    // at the (deduped) decoded texture.
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        const Entry& e = entries[idx];
        if (data.textures[idx].Empty()) {
            for (const auto& ms : e.slots) {
                PbrSlot& s = data.materials[(size_t)ms.first].slots[ms.second];
                s.tex.kind = PbrTexKind::None;
                s.tex.fileIndex = -1;
                s.tex.bytes = nullptr;
            }
            continue;
        }
        uint64_t bytes = 0;
        for (const ImageRGBA8::Level& lv : data.textures[idx].mips) {
            const uint64_t px = (uint64_t)lv.width * lv.height;
            bytes += e.channel >= 0 ? px : px * 4;   // R8 vs RGBA8 VRAM
        }
        data.textureBytes += bytes;
        for (const auto& ms : e.slots) {
            PbrSlot& s = data.materials[(size_t)ms.first].slots[ms.second];
            s.tex.kind = PbrTexKind::File;
            s.tex.fileIndex = (int)idx;
            s.tex.bytes = nullptr;
        }
    }
}

void BuildPbrModelData(const std::filesystem::path& modelPath, const std::vector<PmxMaterial>& pmxMaterials,
                       const std::string& modelName, PbrModelData& out, const std::filesystem::path& forcedEmd) {
    out = PbrModelData{};
    out.modelName = modelName;
    if (pmxMaterials.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<RayMaterialDesc> descs;
    std::filesystem::path chosenEmd;
    RayParseStats stats;
    std::string error;
    bool have = false;
    if (!forcedEmd.empty()) {
        // An explicit user choice (P5): parse it and require at least one reading Ray material. A malformed or
        // non-Ray .emd warns and falls back to the best auto candidate (or to no records at all).
        if (ParseRayEmd(forcedEmd, (int)pmxMaterials.size(), descs, &stats, &error) &&
            std::any_of(descs.begin(), descs.end(), [](const RayMaterialDesc& d) { return d.valid && d.ray; })) {
            chosenEmd = forcedEmd;
            have = true;
        } else {
            if (error.empty()) error = "no Ray materials in the file";
            LOG_WARN("PBR: the chosen .emd for '%s' is not usable (%s); falling back: %s", modelName.c_str(),
                     PathToUtf8(forcedEmd).c_str(), error.c_str());
            error.clear();
        }
    }
    if (!have && !FindAndParseRayMaterials(modelPath, (int)pmxMaterials.size(), descs, &chosenEmd, &stats, &error)) {
        if (!error.empty())
            LOG_WARN("PBR: no Ray materials for '%s': %s", modelName.c_str(), error.c_str());
        return;
    }

    out.materials.resize(pmxMaterials.size());
    for (size_t i = 0; i < pmxMaterials.size(); ++i)
        out.materials[i] = RayToPbr(descs[i], pmxMaterials[i]);

    const auto tParse = std::chrono::steady_clock::now();
    DecodePbrTextures(out, modelName.c_str());
    const auto tDecode = std::chrono::steady_clock::now();

    if (out.materialsWithRecord == 0) {
        out.materials.clear();
        return;
    }
    out.loadMs = std::chrono::duration<double, std::milli>(tDecode - t0).count();
    const double parseMs = std::chrono::duration<double, std::milli>(tParse - t0).count();
    const double decodeMs = std::chrono::duration<double, std::milli>(tDecode - tParse).count();
    LOG_INFO("PBR model=%s materials=%d/%d textures=%d filesMissing=%d vramMB=%.1f loadMs=%.1f (parse %.1f decode %.1f)",
             modelName.c_str(), out.materialsWithRecord, (int)out.materials.size(), (int)out.texturePaths.size(),
             out.filesMissing, (double)out.textureBytes / (1024.0 * 1024.0), out.loadMs, parseMs, decodeMs);
}

void BuildImportedPbrModelData(const PmxModel& pmx, PbrModelData& out) {
    out = PbrModelData{};
    out.modelName = pmx.name;
    if (pmx.importedPbr.empty() || pmx.importedPbr.size() != pmx.materials.size()) return;
    const auto t0 = std::chrono::steady_clock::now();

    // A PmxModel texture index -> a PbrTextureRef (embedded bytes or a resolved file path).
    const auto refFor = [&](int index) {
        PbrTextureRef ref;
        if (index < 0 || (size_t)index >= pmx.textures.size()) return ref;
        if ((size_t)index < pmx.embeddedTextures.size() && !pmx.embeddedTextures[(size_t)index].empty()) {
            ref.kind = PbrTexKind::Embedded;
            ref.pmxIndex = index;
            ref.bytes = &pmx.embeddedTextures[(size_t)index];
        } else {
            const std::filesystem::path p = pmx.ResolveTexturePath(index);
            if (!p.empty()) {
                ref.kind = PbrTexKind::File;
                ref.path = p;
            }
        }
        return ref;
    };

    out.materials.resize(pmx.materials.size());
    for (size_t i = 0; i < pmx.materials.size(); ++i) {
        const ImportedPbr& ip = pmx.importedPbr[i];
        if (!ip.valid) continue;
        const PmxMaterial& pm = pmx.materials[i];
        PbrMaterial& m = out.materials[i];
        m.valid = true;
        m.shadingModel = 0;   // DEFAULT (per the plan)

        // Albedo: the PMX material's own diffuse texture is the glTF base colour; apply the base colour factor once
        // (FROM 3 + APPLY_DIFFUSE, exactly like a Ray ALBEDO_MAP_FROM 3).
        PbrSlot& albedo = m.slots[(int)PbrSlotId::Albedo];
        if (pm.textureIndex >= 0) {
            albedo.from = 3;
            albedo.tex.kind = PbrTexKind::PmxDiffuse;
            albedo.tex.pmxIndex = pm.textureIndex;
        } else {
            albedo.from = 0;
            albedo.hasValue = true;
            albedo.value[0] = pm.diffuse.x;
            albedo.value[1] = pm.diffuse.y;
            albedo.value[2] = pm.diffuse.z;
        }
        m.albedoApplyDiffuse = 1;

        // Smoothness: the glTF roughnessFactor; a metal-roughness map carries roughness in G, linear (TYPE 2 -> the
        // shader decodes GGX alpha = r^2, matching glTF). applyScale 1 x the slot value; the factor is baked into
        // the R8 channel when a map exists (the map decoder ignores applyScale).
        const PbrTextureRef mrRef = ip.metalRoughTex >= 0 ? refFor(ip.metalRoughTex) : PbrTextureRef{};
        PbrSlot& rough = m.slots[(int)PbrSlotId::Smoothness];
        rough.type = 2;
        rough.hasValue = true;
        rough.value[0] = ip.roughness;
        if (mrRef.kind != PbrTexKind::None) {
            rough.from = 1;
            rough.swizzle = 1;   // G
            rough.applyScale = 1;
            rough.tex = mrRef;
            rough.mapScale = ip.roughness;
        } else {
            rough.from = 0;
        }

        // Metalness: B of the same map.
        PbrSlot& metal = m.slots[(int)PbrSlotId::Metalness];
        metal.hasValue = true;
        metal.value[0] = ip.metallic;
        if (mrRef.kind != PbrTexKind::None) {
            metal.from = 1;
            metal.swizzle = 2;   // B
            metal.applyScale = 1;
            metal.tex = mrRef;
            metal.mapScale = ip.metallic;
        } else {
            metal.from = 0;
        }

        // Normal: RGB tangent-space (TYPE 0). glTF's +Y-up tangent space matches the Ray derivative frame (its
        // bitangent is negated, see pbr_common.hlsli PbrTangentToWorld), so no uvFlip. Scale rides value.w.
        if (ip.normalTex >= 0) {
            PbrSlot& nrm = m.slots[(int)PbrSlotId::Normal];
            nrm.from = 1;
            nrm.type = 0;
            nrm.tex = refFor(ip.normalTex);
            nrm.hasScale = true;
            nrm.scale = ip.normalScale;
        }

        // Occlusion: R channel, strength lerp(1, map) baked at decode (PbrSlot::aoStrength).
        if (ip.occlusionTex >= 0 && ip.occlusionStrength > 0.0f) {
            PbrSlot& occ = m.slots[(int)PbrSlotId::Occlusion];
            occ.from = 1;
            occ.type = 0;        // linear, UV0
            occ.swizzle = 0;     // R
            occ.tex = refFor(ip.occlusionTex);
            occ.aoStrength = ip.occlusionStrength;
        }

        // Emissive: sRGB colour map x emissiveFactor. The shader sRGB-decodes PbrEmissive and multiplies by value
        // (applyScale 1); the factor is baked into the RGBA map (the RGBA path is not channel-baked).
        const bool emissive = ip.emissive[0] > 0 || ip.emissive[1] > 0 || ip.emissive[2] > 0;
        m.emissiveEnable = emissive;
        PbrSlot& em = m.slots[(int)PbrSlotId::Emissive];
        if (ip.emissiveTex >= 0) {
            em.from = 1;
            em.applyScale = 1;
            em.hasValue = true;
            em.value[0] = ip.emissive[0];
            em.value[1] = ip.emissive[1];
            em.value[2] = ip.emissive[2];
            em.tex = refFor(ip.emissiveTex);
        } else if (emissive) {
            em.from = 0;
            em.hasValue = true;
            em.value[0] = ip.emissive[0];
            em.value[1] = ip.emissive[1];
            em.value[2] = ip.emissive[2];
        }
    }

    const auto tDecodeStart = std::chrono::steady_clock::now();
    DecodePbrTextures(out, out.modelName.c_str());
    const auto tDecode = std::chrono::steady_clock::now();

    if (out.materialsWithRecord == 0) {
        out.materials.clear();
        return;
    }
    out.loadMs = std::chrono::duration<double, std::milli>(tDecode - t0).count();
    const double decodeMs = std::chrono::duration<double, std::milli>(tDecode - tDecodeStart).count();
    LOG_INFO("PBR model=%s materials=%d/%d textures=%d filesMissing=%d vramMB=%.1f loadMs=%.1f (parse %.1f decode %.1f)",
             out.modelName.c_str(), out.materialsWithRecord, (int)out.materials.size(), (int)out.texturePaths.size(),
             out.filesMissing, (double)out.textureBytes / (1024.0 * 1024.0), out.loadMs, 0.0, decodeMs);
}

} // namespace mmdx
