#include "studio/RestPoseRetarget.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace DirectX;

namespace mmdx::studio {

namespace {

constexpr float kPi = 3.14159265358979f;
// Calibrated over the whole library: the Romeo motions measure 82..87, every normal motion <= 73.
constexpr float kTPoseMedianDeg = 78.0f;
constexpr int kMinSamples = 8;
// MMD's A-pose reference: the upper arm rests 38 degrees below horizontal (the reference direction below).
constexpr float kArmRestDeg = 38.0f;

struct Side {
    const char* arm;
    const char* elbow;
};
constexpr Side kSides[2] = {{"\xE5\xB7\xA6\xE8\x85\x95", "\xE5\xB7\xA6\xE3\x81\xB2\xE3\x81\x98"},   // 左腕, 左ひじ
                            {"\xE5\x8F\xB3\xE8\x85\x95", "\xE5\x8F\xB3\xE3\x81\xB2\xE3\x81\x98"}};  // 右腕, 右ひじ

} // namespace

ArmRestGuess GuessTPoseMotion(const MotionData& motion) {
    ArmRestGuess g;
    std::vector<float> angles;
    for (int i = 0; i < 2; ++i) {
        const float s = i == 0 ? 1.0f : -1.0f;  // left = +x, right = -x
        const XMVECTOR d = XMVectorSet(s * std::cos(kArmRestDeg * kPi / 180.0f), -std::sin(kArmRestDeg * kPi / 180.0f),
                                       0.0f, 0.0f);
        const auto it = motion.bones.find(kSides[i].arm);
        if (it == motion.bones.end()) continue;
        for (const BoneKf& k : it->second) {
            const XMVECTOR v = XMVector3Rotate(d, XMLoadFloat4(&k.r));
            const float angle = std::atan2(-XMVectorGetY(v), s * XMVectorGetX(v)) * 180.0f / kPi;
            angles.push_back(angle);
        }
    }
    g.samples = (int)angles.size();
    if (angles.empty()) return g;
    std::sort(angles.begin(), angles.end());
    const size_t n = angles.size();
    g.medianDeg = n % 2 ? angles[n / 2] : 0.5f * (angles[n / 2 - 1] + angles[n / 2]);
    g.likely = g.samples >= kMinSamples && g.medianDeg >= kTPoseMedianDeg;
    return g;
}

ArmRetargetResult RetargetArmRest(const PmxModel& model, MotionData& motion, const ArmRetargetOptions& opt) {
    ArmRetargetResult res;
    const float src = opt.sourceArmDeg * kPi / 180.0f;
    for (int i = 0; i < 2; ++i) {
        if (!opt.side[i]) continue;
        const int arm = model.FindBone(kSides[i].arm);
        const int elbow = model.FindBone(kSides[i].elbow);
        if (arm < 0 || elbow < 0) continue;
        const XMVECTOR armPos = XMLoadFloat3(&model.bones[(size_t)arm].position);
        const XMVECTOR elbowPos = XMLoadFloat3(&model.bones[(size_t)elbow].position);
        const XMVECTOR dA = XMVector3Normalize(elbowPos - armPos);

        const XMVECTOR flat = XMVectorSet(XMVectorGetX(dA), 0.0f, XMVectorGetZ(dA), 0.0f);
        if (XMVectorGetX(XMVector3Length(flat)) < 1e-4f) continue;  // arm hangs straight down: no usable heading
        const XMVECTOR hA = XMVector3Normalize(flat);
        const XMVECTOR dS = XMVector3Normalize(hA * std::cos(src) + XMVectorSet(0.0f, -std::sin(src), 0.0f, 0.0f));

        const XMVECTOR axis = XMVector3Cross(dA, dS);
        const float axisLen = XMVectorGetX(XMVector3Length(axis));
        const float angle = std::acos(std::clamp(XMVectorGetX(XMVector3Dot(dA, dS)), -1.0f, 1.0f));
        if (angle < 0.5f * kPi / 180.0f || axisLen < 1e-6f) continue;
        const XMVECTOR R = XMQuaternionRotationAxis(XMVector3Normalize(axis), angle);
        const XMVECTOR Rinv = XMQuaternionInverse(R);
        res.rotationDeg[i] = angle * 180.0f / kPi;

        // Subtree: every bone whose parent chain reaches the 腕 bone (twist helpers, wrist, fingers).
        std::vector<int> subtree;
        for (int j = 0; j < (int)model.bones.size(); ++j) {
            if (j == arm) continue;
            int p = model.bones[(size_t)j].parentIndex;
            for (int guard = 0; p >= 0 && guard < (int)model.bones.size(); ++guard) {
                if (p == arm) {
                    subtree.push_back(j);
                    break;
                }
                p = model.bones[(size_t)p].parentIndex;
            }
        }

        const std::string armName = model.bones[(size_t)arm].name;
        auto armTrack = motion.bones.find(armName);
        if (armTrack == motion.bones.end() || armTrack->second.empty()) {
            BoneKf k;
            k.frame = 0;
            XMStoreFloat4(&k.r, XMQuaternionNormalize(R));
            FillLinearInterp(k.interp);
            motion.bones[armName] = {k};  // an un-keyed bone is identity, and identity becomes R
            ++res.bonesChanged;
            ++res.keysChanged;
        } else {
            for (BoneKf& k : armTrack->second) {
                XMStoreFloat4(&k.r, XMQuaternionNormalize(XMQuaternionMultiply(R, XMLoadFloat4(&k.r))));
                ++res.keysChanged;
            }
            ++res.bonesChanged;
        }

        for (int j : subtree) {
            auto track = motion.bones.find(model.bones[(size_t)j].name);
            if (track == motion.bones.end() || track->second.empty()) continue;  // un-keyed subtree bones stay un-keyed
            for (BoneKf& k : track->second) {
                const XMVECTOR r = XMLoadFloat4(&k.r);
                XMStoreFloat4(&k.r, XMQuaternionNormalize(XMQuaternionMultiply(XMQuaternionMultiply(R, r), Rinv)));
                const XMFLOAT3 t = k.t;
                XMStoreFloat3(&k.t, XMVector3Rotate(XMLoadFloat3(&t), Rinv));
                ++res.keysChanged;
            }
            ++res.bonesChanged;
        }
    }
    res.applied = res.keysChanged > 0;
    return res;
}

} // namespace mmdx::studio
