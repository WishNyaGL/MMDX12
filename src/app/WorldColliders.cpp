#include "app/WorldColliders.h"
#include "anim/Motion.h"
#include "anim/ModelInstance.h"

#include <algorithm>
#include <cmath>

namespace mmdx {

namespace {
// MMD standard skeleton names (UTF-8).
constexpr const char* kLeftAnkle = "\xE5\xB7\xA6\xE8\xB6\xB3\xE9\xA6\x96";       // 左足首
constexpr const char* kRightAnkle = "\xE5\x8F\xB3\xE8\xB6\xB3\xE9\xA6\x96";      // 右足首
constexpr const char* kLeftToe = "\xE5\xB7\xA6\xE3\x81\xA4\xE3\x81\xBE\xE5\x85\x88";    // 左つま先
constexpr const char* kRightToe = "\xE5\x8F\xB3\xE3\x81\xA4\xE3\x81\xBE\xE5\x85\x88";   // 右つま先
constexpr const char* kLeftWrist = "\xE5\xB7\xA6\xE6\x89\x8B\xE9\xA6\x96";       // 左手首
constexpr const char* kRightWrist = "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96";      // 右手首

struct BoneCollider {
    int bone = -1;
    float radius = 0.6f;
    uint32_t flags = 0;
};

uint64_t HashCombine(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t HashString(uint64_t h, const std::string& s) {
    for (unsigned char c : s) h = HashCombine(h, c);
    return h;
}
} // namespace

std::shared_ptr<const WorldColliderTrack> BuildColliderTrack(const std::vector<CharacterSource>& cast, double start,
                                                             double end, float rate) {
    auto track = std::make_shared<WorldColliderTrack>();
    if (rate <= 0.0f) rate = 30.0f;
    if (end < start) std::swap(start, end);
    track->rate = rate;
    track->start = start;

    const double span = 1.0 / (double)rate;
    const uint32_t sampleCount = (uint32_t)std::floor((end - start) / span + 1e-6) + 1;
    track->sampleCount = sampleCount;

    uint64_t version = HashCombine(0, (uint64_t)std::llround(start * 1000.0));
    version = HashCombine(version, (uint64_t)std::llround(end * 1000.0));
    version = HashCombine(version, (uint64_t)std::llround((double)rate * 1000.0));

    struct CharacterColliders {
        CharacterSource source;
        std::vector<BoneCollider> bones;
    };
    std::vector<CharacterColliders> characters;
    characters.reserve(cast.size());
    uint32_t total = 0;
    for (const CharacterSource& src : cast) {
        if (!src.pmx) continue;
        version = HashString(version, src.id.empty() ? src.pmx->sourcePath.string() : src.id);
        version = HashCombine(version, src.motionVersion);

        CharacterColliders cc;
        cc.source = src;
        const auto add = [&](const char* name, float radius, uint32_t flags) {
            const int b = src.pmx->FindBone(name);
            if (b >= 0) cc.bones.push_back({b, radius, flags});
        };
        const int leftAnkle = src.pmx->FindBone(kLeftAnkle);
        const int rightAnkle = src.pmx->FindBone(kRightAnkle);
        const int leftToe = src.pmx->FindBone(kLeftToe);
        const int rightToe = src.pmx->FindBone(kRightToe);
        if (leftAnkle >= 0) {
            cc.bones.push_back({leftAnkle, 0.6f, kColliderFoot | kColliderLeft});
            cc.bones.push_back({leftToe >= 0 ? leftToe : leftAnkle, 0.6f, kColliderFoot | kColliderLeft});
        }
        if (rightAnkle >= 0) {
            cc.bones.push_back({rightAnkle, 0.6f, kColliderFoot});
            cc.bones.push_back({rightToe >= 0 ? rightToe : rightAnkle, 0.6f, kColliderFoot});
        }
        add(kLeftWrist, 0.4f, kColliderHand | kColliderLeft);
        add(kRightWrist, 0.4f, kColliderHand);
        total += (uint32_t)cc.bones.size();
        if (total > kWorldMaxColliders) break;
        characters.push_back(std::move(cc));
    }
    if (total > kWorldMaxColliders) {
        total = kWorldMaxColliders;
        uint32_t kept = 0;
        for (CharacterColliders& cc : characters) {
            if (kept + cc.bones.size() > total) {
                cc.bones.resize(total - kept);
            }
            kept += (uint32_t)cc.bones.size();
            if (kept >= total) break;
        }
    }
    track->perSample = total;
    track->version = version;

    if (total == 0) {
        track->samples.clear();
        return track;
    }

    // Pose a private instance per character once per sample; only the recorded bones are kept.
    std::vector<std::unique_ptr<ModelInstance>> instances;
    instances.reserve(characters.size());
    for (CharacterColliders& cc : characters) {
        instances.push_back(std::make_unique<ModelInstance>(cc.source.pmx));
        instances.back()->EnablePhysics(false);
    }

    track->samples.resize((size_t)sampleCount * total);
    std::vector<std::vector<DirectX::XMFLOAT3>> positions(characters.size());
    for (size_t c = 0; c < characters.size(); ++c) positions[c].resize(characters[c].bones.size());

    for (uint32_t s = 0; s < sampleCount; ++s) {
        const double t = start + (double)s * span;
        for (size_t c = 0; c < characters.size(); ++c) {
            ModelInstance& inst = *instances[c];
            const float frame = (float)(t * characters[c].source.fps);
            if (characters[c].source.motion) characters[c].source.motion->Evaluate(frame, inst);
            else inst.ResetPose();
            inst.SetScale(characters[c].source.scale);
            inst.SetRootTransform(characters[c].source.root);
            inst.UpdatePose(0.0f);
            for (size_t b = 0; b < characters[c].bones.size(); ++b)
                positions[c][b] = inst.BoneWorldPosition(characters[c].bones[b].bone);
        }
        size_t out = (size_t)s * total;
        const size_t endOut = out + total;
        for (size_t c = 0; c < characters.size() && out < endOut; ++c) {
            for (size_t b = 0; b < characters[c].bones.size() && out < endOut; ++b) {
                WorldCollider& col = track->samples[out++];
                col.pos = positions[c][b];
                col.radius = characters[c].bones[b].radius;
                col.flags = characters[c].bones[b].flags;
            }
        }
    }

    // Velocity = central difference of the positions (one-sided at both ends).
    const float dt = span > 0.0 ? (float)span : 0.0f;
    for (uint32_t s = 0; s < sampleCount; ++s) {
        const uint32_t s0 = s > 0 ? s - 1 : s;
        const uint32_t s1 = s + 1 < sampleCount ? s + 1 : s;
        const float denom = (float)(s1 - s0) * dt;
        if (denom <= 0.0f) continue;
        for (uint32_t i = 0; i < total; ++i) {
            const WorldCollider& a = track->samples[(size_t)s0 * total + i];
            const WorldCollider& b = track->samples[(size_t)s1 * total + i];
            WorldCollider& c = track->samples[(size_t)s * total + i];
            c.vel.x = (b.pos.x - a.pos.x) / denom;
            c.vel.y = (b.pos.y - a.pos.y) / denom;
            c.vel.z = (b.pos.z - a.pos.z) / denom;
        }
    }
    return track;
}

} // namespace mmdx
