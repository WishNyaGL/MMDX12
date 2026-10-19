#pragma once
// World packs (docs/effect_ecosystem.md §2 item 7, §7): placed, keyable effect entities (falling snow, dust, magic
// circles, ground snow cover ...) with an optional GPU simulation. The contract between the app (placement, keyed
// parameters, the collider track) and the renderer (WorldPass: simulation, drawing in every render path).
//
// Determinism: a world's simulation advances in fixed steps of its pack's step size towards `WorldInstance::time`.
// Seeking (any jump the live step cannot reach) re-simulates: short-lived state over the pack's rewind window before
// the target time, persistent fields (footprints) from the start at their reduced rate. Scene interaction comes from
// the collider track, sampled once per scene / motion version (never from the live pose), so the viewport, videos and
// offline GI give the same result for the same frame.
#include <DirectXMath.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mmdx {

inline constexpr uint32_t kWorldMaxColliders = 64;   // per sample (all characters together)

// One collision sphere at one instant: a character's foot / toe / hand, world space (MMD units).
struct WorldCollider {
    DirectX::XMFLOAT3 pos{0, 0, 0};
    float radius = 0.0f;
    DirectX::XMFLOAT3 vel{0, 0, 0};   // units per second
    uint32_t flags = 0;               // WorldColliderFlags
};
enum WorldColliderFlags : uint32_t { kColliderFoot = 1u, kColliderHand = 2u, kColliderLeft = 4u };

// Every character's colliders sampled at a fixed rate over the whole timeline (without physics: feet and hands are
// driven by motion + IK), so any time can be evaluated without posing the live models.
struct WorldColliderTrack {
    float rate = 30.0f;                    // samples per second
    double start = 0.0;                    // time of sample 0 (seconds)
    uint32_t perSample = 0;                // colliders per sample (<= kWorldMaxColliders)
    uint32_t sampleCount = 0;
    std::vector<WorldCollider> samples;    // sampleCount * perSample, sample-major
    uint64_t version = 0;                  // changes whenever the cast / motions change (worlds rebuild their state)

    // Colliders at `t` (seconds): positions interpolated linearly between samples, velocities from the samples;
    // clamped at both ends. `out` is resized to perSample.
    void Sample(double t, std::vector<WorldCollider>& out) const;
};

// One world entity for one frame.
struct WorldInstance {
    uint32_t uid = 0;                      // stable per entity (studio uid / play-mode stack index + 1)
    std::string pack;                      // world pack id
    DirectX::XMFLOAT4X4 transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // placement (row-vector)
    std::map<std::string, float> params;   // evaluated at this frame; absent keys = pack defaults
    double time = 0.0;                     // simulation target time (seconds on the timeline / song clock)
    bool exact = false;                    // true: offline renders and paused / scrubbed frames re-simulate
                                           // exactly; false: live playback may take the next fixed steps only
    std::shared_ptr<const WorldColliderTrack> colliders;   // may be null (no interaction)
};

} // namespace mmdx
