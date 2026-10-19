#include "app/LookApply.h"

namespace mmdx {
namespace {

std::vector<LookPackRef> BuildPacks(const LookData& look) {
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

} // namespace

LookData CaptureLook(const AppSettings& s, const ShaderChoice* surface) {
    LookData look;
    look.lighting = s.lighting;
    look.exposure = s.exposure;
    look.shadows = s.shadows;
    look.ssao = s.ssao;
    look.ssr = s.ssr;
    look.bloom = s.bloom;
    look.bloomIntensity = s.bloomIntensity;
    look.bloomThreshold = s.bloomThreshold;
    look.bloomConvolution = s.bloomConvolution;
    look.bloomLook = s.bloomLook;
    look.bloomKernelPreset = s.bloomKernelPreset;
    look.dof = s.dof;
    look.dofAperture = s.dofAperture;
    look.volumetric = s.volumetric;
    look.volumetricDensity = s.volumetricDensity;
    look.autoLuminous = s.autoLuminous;
    look.colorLut = s.colorLut;
    look.lutIntensity = s.lutIntensity;
    look.effects = s.effectStack;
    look.setsLighting = true;
    look.setsEffects = true;
    look.setsColor = true;
    if (surface) {
        look.hasSurface = true;
        look.setsSurface = true;
        look.surface.pack = surface->pack;
        look.surface.params = surface->params;
        look.surface.textureFolder = surface->textureFolder;
        look.surface.materials = surface->materials;
    } else {
        look.hasSurface = false;
        look.setsSurface = false;
    }
    look.packs = BuildPacks(look);
    return look;
}

void ApplyLook(const LookData& look, AppSettings& s, ShaderChoice* surface) {
    if (look.setsLighting) {
        s.lighting = look.lighting;
        s.exposure = look.exposure;
        s.shadows = look.shadows;
    }
    if (look.setsEffects) {
        s.ssao = look.ssao;
        s.ssr = look.ssr;
        s.bloom = look.bloom;
        s.bloomIntensity = look.bloomIntensity;
        s.bloomThreshold = look.bloomThreshold;
        s.bloomConvolution = look.bloomConvolution;
        s.bloomLook = look.bloomLook;
        s.bloomKernelPreset = look.bloomKernelPreset;
        s.dof = look.dof;
        s.dofAperture = look.dofAperture;
        s.volumetric = look.volumetric;
        s.volumetricDensity = look.volumetricDensity;
        s.autoLuminous = look.autoLuminous;
        s.effectStack = look.effects;
    }
    if (look.setsColor) {
        s.colorLut = look.colorLut;
        s.lutIntensity = look.lutIntensity;
    }
    if (look.setsSurface && surface && look.hasSurface) {
        surface->SwitchPack(look.surface.pack);
        surface->params = look.surface.params;
        surface->textureFolder = look.surface.textureFolder;
        surface->materials = look.surface.materials;
    }
}

} // namespace mmdx
