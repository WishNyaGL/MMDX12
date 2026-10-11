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
    // Group File slots by (path, channel). channel < 0 = full RGBA, 0..3 = one channel (R8 upload).
    // The channel of a single-channel read is its swizzle.
    struct Entry {
        std::filesystem::path path;
        int channel = -1;
        std::vector<std::pair<int, int>> slots;   // (material, slot) referencing this entry
    };
    std::map<std::string, int> byKey;
    std::vector<Entry> entries;
    for (int mi = 0; mi < (int)data.materials.size(); ++mi) {
        PbrMaterial& m = data.materials[mi];
        if (!m.valid) continue;
        ++data.materialsWithRecord;
        for (int si = 0; si < kPbrSlotCount; ++si) {
            PbrSlot& s = m.slots[si];
            if (s.tex.kind != PbrTexKind::File) continue;
            const bool single = SingleChannelSlot((PbrSlotId)si, s.type);
            const int channel = single ? s.swizzle : -1;
            const std::string key = PathToUtf8(s.tex.path) + "|" + std::to_string(channel);
            auto it = byKey.find(key);
            int index;
            if (it == byKey.end()) {
                Entry e;
                e.path = s.tex.path;
                e.channel = channel;
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
        if (!e.path.empty()) {
            ImageRGBA8 img;
            std::string err;
            if (LoadImageRGBA8(e.path, img, &err)) {
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
                data.textures[&e - entries.data()] = std::move(img);
            } else {
                LOG_WARN("PBR map load failed: %s (%s): %s", PathToUtf8(e.path).c_str(), label, err.c_str());
                ++missing;
            }
        }
    });
    data.filesMissing = missing.load();
    (void)total;

    // A missing / undecodable file falls the slot back to its constant; every other slot points the GPU record
    // at the (deduped) decoded texture.
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        const Entry& e = entries[idx];
        if (data.textures[idx].Empty()) {
            for (const auto& ms : e.slots) {
                PbrSlot& s = data.materials[(size_t)ms.first].slots[ms.second];
                s.tex.kind = PbrTexKind::None;
                s.tex.fileIndex = -1;
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
        }
    }
}

void BuildPbrModelData(const std::filesystem::path& modelPath, const std::vector<PmxMaterial>& pmxMaterials,
                       const std::string& modelName, PbrModelData& out) {
    out = PbrModelData{};
    out.modelName = modelName;
    if (pmxMaterials.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<RayMaterialDesc> descs;
    std::filesystem::path chosenEmd;
    RayParseStats stats;
    std::string error;
    if (!FindAndParseRayMaterials(modelPath, (int)pmxMaterials.size(), descs, &chosenEmd, &stats, &error)) {
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

} // namespace mmdx
