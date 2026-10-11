#pragma once
// Generic model-data traits, shared by the app (render/GpuModel) and tools/pack_suggest_test. Shader packs declare in
// pack.json ("suggest") which traits they fit; the engine only evaluates them (ShaderPack::Suggests) and never knows
// a pack or a game.
#include "asset/ImageLoader.h"
#include "asset/PmxModel.h"

#include <vector>

namespace mmdx {

// A toon ramp is a vertical gradient: every row is (near) one colour. Some MME shaders
// put a UV-mapped shadow-colour texture in the toon slot instead.
bool IsToonRampImage(const ImageRGBA8& img);

struct ModelTraits {
    int additionalUvCount = 0;   // PMX additional UV channels (0..4)
    int toonMapMaterials = 0;    // materials whose own toon slot holds a UV-mapped (non-ramp) image
    int materials = 0;
};

// `textures` is indexed like pmx.textures (an empty image = not loaded). A toon-map material has a non-shared toon
// with a valid, loaded, non-ramp image.
ModelTraits ComputeModelTraits(const PmxModel& pmx, const std::vector<ImageRGBA8>& textures);

} // namespace mmdx
