#pragma once
// App-side collider track for world packs (src/render/WorldTypes.h): the cast's feet and hands sampled at a fixed
// rate over the whole timeline, posed without physics (motion + IK + append), so world entities can interact with the
// scene at any time (viewport, video, offline GI). Built on a worker thread; the result is immutable.
#include "asset/PmxModel.h"
#include "render/WorldTypes.h"
#include <DirectXMath.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mmdx {

class BoundMotion;

// One character the collider track poses. `motionVersion` is the caller's version counter of the motion (the
// studio's StudioModel::motionVersion); `root` places the model in the world (identity for play mode).
struct CharacterSource {
    std::shared_ptr<const PmxModel> pmx;
    std::shared_ptr<BoundMotion> motion;
    DirectX::XMFLOAT4X4 root{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float scale = 1.0f;
    float fps = 30.0f;   // the motion's time base (60 for 60 fps VMDs)
    uint64_t motionVersion = 0;
    std::string id;   // library id / display label (hashed into the track version)
};

// Builds the collider track of `cast` over [start, end] seconds at `rate` samples/s. Runs on a worker thread (the
// caller owns the thread); the returned track is immutable. Left / right ankles (足首) and toes (つま先, or the ankle
// when missing) at radius 0.6, wrists (手首) at radius 0.4; missing bones are skipped.
std::shared_ptr<const WorldColliderTrack> BuildColliderTrack(const std::vector<CharacterSource>& cast, double start,
                                                             double end, float rate);

} // namespace mmdx
