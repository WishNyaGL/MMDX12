#pragma once
#include <json.hpp>
#include <string>
#include <vector>

#include "render/RenderTypes.h"
#include "render/ShaderPack.h"

namespace mmdx {

// A pack a look uses: the manifest id, the minimum pack version it was made for and the index URL it comes from
// (empty = built-in / local).
struct LookPackRef {
    std::string id;
    std::string minVersion;
    std::string source;
    bool operator==(const LookPackRef&) const = default;
};

// The data of a Look preset: a shareable bundle of the visual settings (lighting, exposure, screen effects, colour
// LUT, bloom look, the effect-pack stack and optionally one surface-pack choice).
struct LookData {
    int lighting = 0;
    float exposure = 1.0f;
    bool shadows = true, ssao = true, ssr = true;
    bool bloom = true;
    float bloomIntensity = 0.35f, bloomThreshold = 1.1f;
    bool bloomConvolution = false;
    BloomLook bloomLook;
    int bloomKernelPreset = 0;                               // render/RenderTypes.h
    bool dof = false;
    float dofAperture = 1.0f;
    bool volumetric = false;
    float volumetricDensity = 1.0f;
    bool autoLuminous = true;
    std::string colorLut;                                    // ColorLutEntry id, "" = none
    float lutIntensity = 1.0f;
    std::string lutFile;                                     // optional bundled LUT strip, relative to the look folder
    std::vector<EffectStackEntry> effects;                   // render/ShaderPack.h
    bool hasSurface = false;                                 // applied to one character
    ShaderChoice surface;                                    // only pack / params / textureFolder / materials
    std::vector<LookPackRef> packs;                          // every pack the look uses (effects + surface)
    // which groups the look sets (absent groups are left alone when applied)
    bool setsLighting = true, setsEffects = true, setsColor = true, setsSurface = false;
    bool operator==(const LookData&) const = default;
};

// Parses the "look" block of a pack.json. Missing keys keep the defaults; wrong types are reported in `problems` and
// never throw.
bool ParseLookJson(const nlohmann::json& j, LookData& out, std::vector<std::string>& problems);
nlohmann::json LookToJson(const LookData& look);

} // namespace mmdx
