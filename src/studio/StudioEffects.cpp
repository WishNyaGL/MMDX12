#include "studio/StudioEffects.h"

#include <algorithm>
#include <cstring>

namespace mmdx::studio {

size_t StudioEffect::ApproxBytes() const {
    size_t bytes = sizeof(StudioEffect) + entry.pack.size() + entry.textureFolder.size();
    for (const auto& [k, v] : entry.params) bytes += k.size() + sizeof(float);
    for (const EffectParamTrack& t : tracks)
        bytes += sizeof(EffectParamTrack) + t.param.size() + t.keys.capacity() * sizeof(EffectParamKey);
    return bytes;
}

bool StudioEffect::operator==(const StudioEffect& o) const {
    if (!(entry == o.entry) || tracks.size() != o.tracks.size()) return false;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const EffectParamTrack& a = tracks[i];
        const EffectParamTrack& b = o.tracks[i];
        if (a.param != b.param || a.keys.size() != b.keys.size()) return false;
        for (size_t k = 0; k < a.keys.size(); ++k) {
            if (a.keys[k].frame != b.keys[k].frame || a.keys[k].value != b.keys[k].value ||
                a.keys[k].stepped != b.keys[k].stepped)
                return false;
        }
    }
    return true;
}

float EvaluateParamTrack(const EffectParamTrack& track, float frame, float fallback) {
    if (track.keys.empty()) return fallback;
    if (frame <= (float)track.keys.front().frame) return track.keys.front().value;
    if (frame >= (float)track.keys.back().frame) return track.keys.back().value;
    for (size_t i = 1; i < track.keys.size(); ++i) {
        const EffectParamKey& b = track.keys[i];
        if (frame > (float)b.frame) continue;
        const EffectParamKey& a = track.keys[i - 1];
        if (a.stepped) return a.value;
        const float span = (float)(b.frame - a.frame);
        if (span <= 0.0f) return b.value;
        const float t = (frame - (float)a.frame) / span;
        return a.value + (b.value - a.value) * t;
    }
    return track.keys.back().value;
}

EffectStackEntry EvaluateEffect(const StudioEffect& effect, float frame) {
    EffectStackEntry out = effect.entry;
    for (const EffectParamTrack& track : effect.tracks) {
        float fallback = 0.0f;
        if (const auto it = out.params.find(track.param); it != out.params.end())
            fallback = it->second;
        else if (!track.keys.empty())
            fallback = track.keys.front().value;
        out.params[track.param] = EvaluateParamTrack(track, frame, fallback);
    }
    return out;
}

void UpsertParamKey(EffectParamTrack& track, EffectParamKey key) {
    const auto it = std::lower_bound(track.keys.begin(), track.keys.end(), key.frame,
                                     [](const EffectParamKey& k, int32_t frame) { return k.frame < frame; });
    if (it != track.keys.end() && it->frame == key.frame) {
        *it = key;
    } else {
        track.keys.insert(it, key);
    }
}

void EraseParamKeys(EffectParamTrack& track, const std::vector<int32_t>& frames) {
    track.keys.erase(std::remove_if(track.keys.begin(), track.keys.end(),
                                    [&](const EffectParamKey& k) {
                                        return std::find(frames.begin(), frames.end(), k.frame) != frames.end();
                                    }),
                     track.keys.end());
}

} // namespace mmdx::studio
