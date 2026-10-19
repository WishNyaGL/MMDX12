#include "render/LookPreset.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace mmdx {
namespace {

constexpr float kMinExposure = 0.5f, kMaxExposure = 2.0f;
constexpr float kMinDofAperture = 0.2f, kMaxDofAperture = 3.0f;
constexpr float kMinVolumetricDensity = 0.25f, kMaxVolumetricDensity = 4.0f;
constexpr float kMinBloomIntensity = 0.0f, kMaxBloomIntensity = 1.5f;
constexpr float kMinBloomThreshold = 0.2f, kMaxBloomThreshold = 4.0f;
constexpr float kMinLutIntensity = 0.0f, kMaxLutIntensity = 1.0f;

float Clamp(float v, float lo, float hi) {
    if (!std::isfinite(v)) return lo;
    return std::clamp(v, lo, hi);
}

std::string Join(const std::string& prefix, const char* key) {
    return prefix.empty() ? std::string(key) : prefix + "." + key;
}

bool GetBool(const nlohmann::json& j, const std::string& prefix, const char* key, bool& out,
             std::vector<std::string>& problems) {
    if (!j.contains(key)) return true;
    if (!j[key].is_boolean()) {
        problems.push_back(Join(prefix, key) + ": expected a boolean");
        return false;
    }
    out = j[key].get<bool>();
    return true;
}

bool GetInt(const nlohmann::json& j, const std::string& prefix, const char* key, int& out,
            std::vector<std::string>& problems) {
    if (!j.contains(key)) return true;
    if (!j[key].is_number_integer()) {
        problems.push_back(Join(prefix, key) + ": expected an integer");
        return false;
    }
    out = j[key].get<int>();
    return true;
}

bool GetFloat(const nlohmann::json& j, const std::string& prefix, const char* key, float& out,
              std::vector<std::string>& problems) {
    if (!j.contains(key)) return true;
    if (!j[key].is_number()) {
        problems.push_back(Join(prefix, key) + ": expected a number");
        return false;
    }
    out = j[key].get<float>();
    return true;
}

bool GetString(const nlohmann::json& j, const std::string& prefix, const char* key, std::string& out,
               std::vector<std::string>& problems) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) {
        problems.push_back(Join(prefix, key) + ": expected a string");
        return false;
    }
    out = j[key].get<std::string>();
    return true;
}

bool GetObject(const nlohmann::json& j, const std::string& prefix, const char* key, const nlohmann::json*& out,
               std::vector<std::string>& problems) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) {
        problems.push_back(Join(prefix, key) + ": expected an object");
        return false;
    }
    out = &j[key];
    return true;
}

void ParseParams(const nlohmann::json& j, const std::string& prefix, std::map<std::string, float>& out,
                 std::vector<std::string>& problems) {
    if (!j.is_object()) {
        problems.push_back(prefix + ": expected an object");
        return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_number()) {
            problems.push_back(prefix + "." + it.key() + ": expected a number");
            continue;
        }
        out[it.key()] = it.value().get<float>();
    }
}

void ParseMaterials(const nlohmann::json& j, const std::string& prefix, MaterialClassOverrides& out,
                    std::vector<std::string>& problems) {
    if (!j.is_object()) {
        problems.push_back(prefix + ": expected an object");
        return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!it.value().is_number_integer()) {
            problems.push_back(prefix + "." + it.key() + ": expected an integer");
            continue;
        }
        out[it.key()] = it.value().get<int>();
    }
}

void ParseBloom(const nlohmann::json& j, LookData& out, std::vector<std::string>& problems) {
    if (!GetBool(j, "bloom", "enabled", out.bloom, problems)) return;
    GetFloat(j, "bloom", "intensity", out.bloomIntensity, problems);
    GetFloat(j, "bloom", "threshold", out.bloomThreshold, problems);
    GetBool(j, "bloom", "convolution", out.bloomConvolution, problems);
    GetFloat(j, "bloom", "radius", out.bloomLook.radius, problems);
    GetInt(j, "bloom", "blend", out.bloomLook.blend, problems);
    GetFloat(j, "bloom", "depthMask", out.bloomLook.depthMask, problems);
    GetInt(j, "bloom", "kernelPreset", out.bloomKernelPreset, problems);
    const nlohmann::json* kernel = nullptr;
    if (!GetObject(j, "bloom", "kernel", kernel, problems) || !kernel) return;
    BloomKernel& k = out.bloomLook.kernel;
    GetInt(*kernel, "bloom.kernel", "rays", k.rays, problems);
    GetFloat(*kernel, "bloom.kernel", "rotation", k.rotation, problems);
    GetFloat(*kernel, "bloom.kernel", "spikeLength", k.spikeLength, problems);
    GetFloat(*kernel, "bloom.kernel", "spikeIntensity", k.spikeIntensity, problems);
    GetFloat(*kernel, "bloom.kernel", "glowRadius", k.glowRadius, problems);
    GetFloat(*kernel, "bloom.kernel", "glowIntensity", k.glowIntensity, problems);
    GetFloat(*kernel, "bloom.kernel", "dispersion", k.dispersion, problems);
    GetFloat(*kernel, "bloom.kernel", "anamorphic", k.anamorphic, problems);
}

void ParseEffects(const nlohmann::json& j, LookData& out, std::vector<std::string>& problems) {
    if (!j.is_array()) {
        problems.push_back("effects: expected an array");
        return;
    }
    for (size_t i = 0; i < j.size(); ++i) {
        const std::string prefix = "effects[" + std::to_string(i) + "]";
        if (!j[i].is_object()) {
            problems.push_back(prefix + ": expected an object");
            continue;
        }
        EffectStackEntry e;
        GetString(j[i], prefix, "pack", e.pack, problems);
        GetBool(j[i], prefix, "enabled", e.enabled, problems);
        GetString(j[i], prefix, "textureFolder", e.textureFolder, problems);
        if (j[i].contains("params")) ParseParams(j[i]["params"], prefix + ".params", e.params, problems);
        out.effects.push_back(std::move(e));
    }
}

void ParseSurface(const nlohmann::json& j, LookData& out, std::vector<std::string>& problems) {
    out.hasSurface = true;
    if (!j.is_object()) {
        problems.push_back("surface: expected an object");
        return;
    }
    GetString(j, "surface", "pack", out.surface.pack, problems);
    GetString(j, "surface", "textureFolder", out.surface.textureFolder, problems);
    if (j.contains("params")) ParseParams(j["params"], "surface.params", out.surface.params, problems);
    if (j.contains("materials")) ParseMaterials(j["materials"], "surface.materials", out.surface.materials, problems);
}

void ParsePacks(const nlohmann::json& j, LookData& out, std::vector<std::string>& problems) {
    if (!j.is_array()) {
        problems.push_back("packs: expected an array");
        return;
    }
    for (size_t i = 0; i < j.size(); ++i) {
        const std::string prefix = "packs[" + std::to_string(i) + "]";
        if (!j[i].is_object()) {
            problems.push_back(prefix + ": expected an object");
            continue;
        }
        LookPackRef p;
        GetString(j[i], prefix, "id", p.id, problems);
        GetString(j[i], prefix, "minVersion", p.minVersion, problems);
        GetString(j[i], prefix, "source", p.source, problems);
        out.packs.push_back(std::move(p));
    }
}

void ParseSets(const nlohmann::json& j, LookData& out, std::vector<std::string>& problems) {
    if (!j.is_array()) {
        problems.push_back("sets: expected an array");
        return;
    }
    out.setsLighting = out.setsEffects = out.setsColor = out.setsSurface = false;
    for (size_t i = 0; i < j.size(); ++i) {
        if (!j[i].is_string()) {
            problems.push_back("sets[" + std::to_string(i) + "]: expected a string");
            continue;
        }
        const std::string name = j[i].get<std::string>();
        if (name == "lighting") out.setsLighting = true;
        else if (name == "effects") out.setsEffects = true;
        else if (name == "color") out.setsColor = true;
        else if (name == "surface") out.setsSurface = true;
        else problems.push_back("sets[" + std::to_string(i) + "]: unknown group \"" + name + "\"");
    }
}

std::vector<LookPackRef> RebuildPacks(const LookData& look) {
    std::vector<LookPackRef> packs;
    const auto add = [&packs](const std::string& id) {
        if (id.empty()) return;
        for (const LookPackRef& p : packs)
            if (p.id == id) return;
        packs.push_back(LookPackRef{id, "", ""});
    };
    for (const EffectStackEntry& e : look.effects) add(e.pack);
    if (look.hasSurface) add(look.surface.pack);
    return packs;
}

nlohmann::json ParamsToJson(const std::map<std::string, float>& params) {
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [key, value] : params) j[key] = value;
    return j;
}

nlohmann::json BloomKernelToJson(const BloomKernel& k) {
    return {
        {"rays", k.rays},
        {"rotation", k.rotation},
        {"spikeLength", k.spikeLength},
        {"spikeIntensity", k.spikeIntensity},
        {"glowRadius", k.glowRadius},
        {"glowIntensity", k.glowIntensity},
        {"dispersion", k.dispersion},
        {"anamorphic", k.anamorphic},
    };
}

} // namespace

bool ParseLookJson(const nlohmann::json& j, LookData& out, std::vector<std::string>& problems) {
    out = LookData{};
    if (!j.is_object()) {
        problems.push_back("look: expected an object");
        return false;
    }

    GetInt(j, "", "lighting", out.lighting, problems);
    GetFloat(j, "", "exposure", out.exposure, problems);
    GetBool(j, "", "shadows", out.shadows, problems);
    GetBool(j, "", "ssao", out.ssao, problems);
    GetBool(j, "", "ssr", out.ssr, problems);
    GetBool(j, "", "autoLuminous", out.autoLuminous, problems);

    const nlohmann::json* bloom = nullptr;
    if (GetObject(j, "", "bloom", bloom, problems) && bloom) ParseBloom(*bloom, out, problems);

    const nlohmann::json* dof = nullptr;
    if (GetObject(j, "", "dof", dof, problems) && dof) {
        GetBool(*dof, "dof", "enabled", out.dof, problems);
        GetFloat(*dof, "dof", "aperture", out.dofAperture, problems);
    }

    const nlohmann::json* volumetric = nullptr;
    if (GetObject(j, "", "volumetric", volumetric, problems) && volumetric) {
        GetBool(*volumetric, "volumetric", "enabled", out.volumetric, problems);
        GetFloat(*volumetric, "volumetric", "density", out.volumetricDensity, problems);
    }

    const nlohmann::json* color = nullptr;
    if (GetObject(j, "", "color", color, problems) && color) {
        GetString(*color, "color", "lut", out.colorLut, problems);
        GetString(*color, "color", "lutFile", out.lutFile, problems);
        GetFloat(*color, "color", "intensity", out.lutIntensity, problems);
    }

    if (j.contains("effects")) ParseEffects(j["effects"], out, problems);

    if (j.contains("surface")) ParseSurface(j["surface"], out, problems);

    if (j.contains("packs")) ParsePacks(j["packs"], out, problems);
    else out.packs = RebuildPacks(out);

    if (j.contains("sets")) ParseSets(j["sets"], out, problems);
    else out.setsSurface = out.hasSurface;

    out.exposure = Clamp(out.exposure, kMinExposure, kMaxExposure);
    out.dofAperture = Clamp(out.dofAperture, kMinDofAperture, kMaxDofAperture);
    out.volumetricDensity = Clamp(out.volumetricDensity, kMinVolumetricDensity, kMaxVolumetricDensity);
    out.bloomIntensity = Clamp(out.bloomIntensity, kMinBloomIntensity, kMaxBloomIntensity);
    out.bloomThreshold = Clamp(out.bloomThreshold, kMinBloomThreshold, kMaxBloomThreshold);
    out.lutIntensity = Clamp(out.lutIntensity, kMinLutIntensity, kMaxLutIntensity);
    return problems.empty();
}

nlohmann::json LookToJson(const LookData& look) {
    nlohmann::json effects = nlohmann::json::array();
    for (const EffectStackEntry& e : look.effects) {
        effects.push_back({
            {"pack", e.pack},
            {"enabled", e.enabled},
            {"params", ParamsToJson(e.params)},
            {"textureFolder", e.textureFolder},
        });
    }

    nlohmann::json packs = nlohmann::json::array();
    for (const LookPackRef& p : look.packs) {
        packs.push_back({{"id", p.id}, {"minVersion", p.minVersion}, {"source", p.source}});
    }

    nlohmann::json sets = nlohmann::json::array();
    if (look.setsLighting) sets.push_back("lighting");
    if (look.setsEffects) sets.push_back("effects");
    if (look.setsColor) sets.push_back("color");
    if (look.setsSurface) sets.push_back("surface");

    nlohmann::json j = {
        {"lighting", look.lighting},
        {"exposure", Clamp(look.exposure, kMinExposure, kMaxExposure)},
        {"shadows", look.shadows},
        {"ssao", look.ssao},
        {"ssr", look.ssr},
        {"bloom",
         {
             {"enabled", look.bloom},
             {"intensity", Clamp(look.bloomIntensity, kMinBloomIntensity, kMaxBloomIntensity)},
             {"threshold", Clamp(look.bloomThreshold, kMinBloomThreshold, kMaxBloomThreshold)},
             {"convolution", look.bloomConvolution},
             {"radius", look.bloomLook.radius},
             {"blend", look.bloomLook.blend},
             {"depthMask", look.bloomLook.depthMask},
             {"kernelPreset", look.bloomKernelPreset},
             {"kernel", BloomKernelToJson(look.bloomLook.kernel)},
         }},
        {"dof", {{"enabled", look.dof}, {"aperture", Clamp(look.dofAperture, kMinDofAperture, kMaxDofAperture)}}},
        {"volumetric",
         {{"enabled", look.volumetric},
          {"density", Clamp(look.volumetricDensity, kMinVolumetricDensity, kMaxVolumetricDensity)}}},
        {"color",
         {{"lut", look.colorLut}, {"lutFile", look.lutFile}, {"intensity", Clamp(look.lutIntensity, kMinLutIntensity, kMaxLutIntensity)}}},
        {"autoLuminous", look.autoLuminous},
        {"effects", effects},
        {"packs", packs},
        {"sets", sets},
    };

    if (look.hasSurface) {
        j["surface"] = {
            {"pack", look.surface.pack},
            {"params", ParamsToJson(look.surface.params)},
            {"textureFolder", look.surface.textureFolder},
            {"materials", look.surface.materials},
        };
    }
    return j;
}

} // namespace mmdx
