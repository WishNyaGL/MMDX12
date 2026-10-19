#include "render/WorldTypes.h"

#include <algorithm>
#include <cmath>

namespace mmdx {

void WorldColliderTrack::Sample(double t, std::vector<WorldCollider>& out) const {
    out.resize(perSample);
    if (perSample == 0 || sampleCount == 0 || samples.empty()) {
        for (WorldCollider& c : out) c = WorldCollider{};
        return;
    }
    const double span = rate > 0.0f ? 1.0 / (double)rate : 0.0;
    if (span <= 0.0) {
        const WorldCollider* s = &samples[0];
        for (uint32_t i = 0; i < perSample; ++i) out[i] = s[i];
        return;
    }
    double f = (t - start) / span;
    f = std::clamp(f, 0.0, (double)(sampleCount - 1));
    const uint32_t i0 = (uint32_t)f;
    const uint32_t i1 = std::min(i0 + 1, sampleCount - 1);
    const float frac = (float)(f - (double)i0);
    const WorldCollider* a = &samples[(size_t)i0 * perSample];
    const WorldCollider* b = &samples[(size_t)i1 * perSample];
    for (uint32_t i = 0; i < perSample; ++i) {
        WorldCollider c;
        c.pos.x = a[i].pos.x + (b[i].pos.x - a[i].pos.x) * frac;
        c.pos.y = a[i].pos.y + (b[i].pos.y - a[i].pos.y) * frac;
        c.pos.z = a[i].pos.z + (b[i].pos.z - a[i].pos.z) * frac;
        c.vel.x = a[i].vel.x + (b[i].vel.x - a[i].vel.x) * frac;
        c.vel.y = a[i].vel.y + (b[i].vel.y - a[i].vel.y) * frac;
        c.vel.z = a[i].vel.z + (b[i].vel.z - a[i].vel.z) * frac;
        c.radius = frac < 0.5f ? a[i].radius : b[i].radius;
        c.flags = frac < 0.5f ? a[i].flags : b[i].flags;
        out[i] = c;
    }
}

} // namespace mmdx
