#pragma once
// Studio effect stack data: one entry of the studio's effect stack plus its keyed parameters. Pure data (no renderer /
// imgui includes): StudioDoc holds the list, projects persist it (StudioProject.cpp) and the renderer consumes the
// evaluated EffectStackEntry per frame (EvaluateEffect).
#include "render/ShaderPack.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mmdx::studio {

// One key of a param track: `stepped` holds the value until the next key instead of interpolating.
struct EffectParamKey {
    int32_t frame = 0;
    float value = 0;
    bool stepped = false;
};

// The keys of one param of an effect (param = the pack.json param key). Keys are sorted, one per frame.
struct EffectParamTrack {
    std::string param;
    std::vector<EffectParamKey> keys;
};

struct StudioEffect {
    uint32_t uid = 0;                 // StudioDoc::nextEffectUid, not saved
    EffectStackEntry entry;           // pack id, enabled, static params, texture folder
    std::vector<EffectParamTrack> tracks;   // keyed params (param = the pack.json param key)
    size_t ApproxBytes() const;
    bool operator==(const StudioEffect&) const;   // ignores uid
};

// The value of `track` at `frame`: no keys = `fallback`; before the first key = first value; after the last = last
// value; between keys linear, or the earlier key's value when it is stepped.
float EvaluateParamTrack(const EffectParamTrack& track, float frame, float fallback);
// `effect.entry` with every tracked param replaced by its evaluated value (fallback = the entry's static value if
// present, else the track's first key).
EffectStackEntry EvaluateEffect(const StudioEffect& effect, float frame);
// Inserts or replaces the key at its frame (frames stay sorted).
void UpsertParamKey(EffectParamTrack& track, EffectParamKey key);
// Removes every key whose frame is in `frames`.
void EraseParamKeys(EffectParamTrack& track, const std::vector<int32_t>& frames);

} // namespace mmdx::studio
