#include "asset/ModelTraits.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace mmdx {

bool IsToonRampImage(const ImageRGBA8& img) {
    if (img.Empty()) return true;
    const ImageRGBA8::Level& l = img.mips[0];
    if (l.width <= 2) return true;
    const uint32_t step = std::max(1u, l.height / 64);
    uint64_t dev = 0, n = 0;
    for (uint32_t y = 0; y < l.height; y += step) {
        const uint8_t* row = &l.pixels[(size_t)y * l.width * 4];
        for (int ch = 0; ch < 3; ++ch) {
            uint32_t sum = 0;
            for (uint32_t x = 0; x < l.width; ++x) sum += row[x * 4 + ch];
            const int mean = (int)(sum / l.width);
            for (uint32_t x = 0; x < l.width; ++x) dev += (uint32_t)std::abs(row[x * 4 + ch] - mean);
            n += l.width;
        }
    }
    return dev < n * 4;  // mean absolute deviation along x below ~1.5%
}

ModelTraits ComputeModelTraits(const PmxModel& pmx, const std::vector<ImageRGBA8>& textures) {
    ModelTraits t;
    t.additionalUvCount = pmx.additionalUvCount;
    t.materials = (int)pmx.materials.size();
    for (const PmxMaterial& m : pmx.materials) {
        if (m.sharedToon || m.toonIndex < 0 || (size_t)m.toonIndex >= textures.size()) continue;
        if (textures[m.toonIndex].Empty()) continue;
        if (!IsToonRampImage(textures[m.toonIndex])) ++t.toonMapMaterials;
    }
    return t;
}

} // namespace mmdx
