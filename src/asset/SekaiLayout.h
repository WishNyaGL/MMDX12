#pragma once
// Project Sekai data-layout detection, shared by the app (render/GpuModel) and tools/sekai_detect_test.
#include "asset/ImageLoader.h"
#include "asset/PmxModel.h"

#include <vector>

namespace mmdx {

// A toon ramp is a vertical gradient: every row is (near) one colour. Models made for the
// Project Sekai MME shader put a UV-mapped shadow-colour texture in the toon slot instead.
bool IsToonRampImage(const ImageRGBA8& img);

// The decision behind GpuModel::HasSekaiLayout, on the CPU side.
struct SekaiLayoutInfo {
    bool sekai = false;
    int additionalUvCount = 0;
    int toonMapMaterials = 0;
    int materials = 0;
};

// Exactly GpuModel's rule: additional UV count >= 2 AND at least one toon-map material.
// `textures` is indexed like pmx.textures (an empty image = not loaded).
SekaiLayoutInfo DetectSekaiLayout(const PmxModel& pmx, const std::vector<ImageRGBA8>& textures);

} // namespace mmdx
