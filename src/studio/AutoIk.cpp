// Auto-IK posing: temporary IK chains for FK limbs (see AutoIk.h). Pure DirectXMath, no App dependency.
#include "studio/AutoIk.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace mmdx::studio {

namespace {

constexpr float kEps = 1e-6f;

XMFLOAT3 Store3(FXMVECTOR v) { XMFLOAT3 r; XMStoreFloat3(&r, v); return r; }
XMFLOAT4 Store4(FXMVECTOR v) { XMFLOAT4 r; XMStoreFloat4(&r, v); return r; }

// A bone that the auto-IK chain may rotate: rotatable and not fixed-axis (twist bones 腕捩 / 手捩 are left untouched).
bool Chainable(const PmxBone& b) { return (b.flags & PmxBone_Rotatable) && !(b.flags & PmxBone_FixedAxis); }

// Rotation that turns unit vector `from` onto unit vector `to` (both must be non-degenerate).
XMVECTOR RotationBetween(FXMVECTOR from, FXMVECTOR to) {
    const float d = XMVectorGetX(XMVector3Dot(from, to));
    if (d >= 1.0f - 1e-7f) return XMQuaternionIdentity();
    XMVECTOR axis = XMVector3Cross(from, to);
    if (d <= -1.0f + 1e-7f || XMVectorGetX(XMVector3LengthSq(axis)) < 1e-12f) {
        // opposite (or numerically parallel): any axis perpendicular to `from` gives a 180 deg turn
        XMVECTOR ref = std::fabs(XMVectorGetY(from)) < 0.9f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
        axis = XMVector3Cross(from, ref);
        if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-12f) axis = XMVectorSet(1, 0, 0, 0);
        return XMQuaternionRotationAxis(XMVector3Normalize(axis), 3.14159265358979f);
    }
    const float angle = std::acos(std::clamp(d, -1.0f, 1.0f));
    return XMQuaternionRotationAxis(XMVector3Normalize(axis), angle);
}

} // namespace

bool FindAutoIkChain(const PmxModel& model, int endBone, int& root, int& mid) {
    root = mid = -1;
    const int n = (int)model.bones.size();
    if (endBone < 0 || endBone >= n) return false;
    // a PMX IK already moves this bone (a foot under 足IK): leave it to the model's own IK
    for (const PmxBone& b : model.bones) {
        if (!(b.flags & PmxBone_IK)) continue;
        if (b.ikTargetIndex == endBone) return false;
        for (const PmxIkLink& link : b.ikLinks)
            if (link.boneIndex == endBone) return false;
    }
    int kept = 0;
    int p = model.bones[(size_t)endBone].parentIndex;
    int guard = 0;
    while (p >= 0 && p < n && guard++ < n) {
        if (Chainable(model.bones[(size_t)p])) {
            if (kept == 0) mid = p;
            else { root = p; ++kept; break; }
            ++kept;
        }
        p = model.bones[(size_t)p].parentIndex;
    }
    return root >= 0 && mid >= 0;
}

XMFLOAT3 AutoIkBendNormal(const PmxModel& model, int root, int mid, int end, const XMFLOAT3& rootPos,
                          const XMFLOAT3& midPos, const XMFLOAT3& endPos) {
    const XMVECTOR a = XMLoadFloat3(&midPos), b = XMLoadFloat3(&endPos), r = XMLoadFloat3(&rootPos);
    XMVECTOR normal = XMVector3Cross(XMVectorSubtract(a, r), XMVectorSubtract(b, a));
    if (XMVectorGetX(XMVector3LengthSq(normal)) > 1e-8f) return Store3(XMVector3Normalize(normal));
    // straight chain: the rest-pose bend from the PMX bone positions
    if (root >= 0 && mid >= 0 && end >= 0 && root < (int)model.bones.size() && mid < (int)model.bones.size() &&
        end < (int)model.bones.size()) {
        const XMVECTOR ra = XMLoadFloat3(&model.bones[(size_t)root].position);
        const XMVECTOR ma = XMLoadFloat3(&model.bones[(size_t)mid].position);
        const XMVECTOR ea = XMLoadFloat3(&model.bones[(size_t)end].position);
        normal = XMVector3Cross(XMVectorSubtract(ma, ra), XMVectorSubtract(ea, ma));
        if (XMVectorGetX(XMVector3LengthSq(normal)) > 1e-8f) return Store3(XMVector3Normalize(normal));
    }
    return {0, 0, -1};
}

bool SolveTwoBone(FXMVECTOR rootPos, FXMVECTOR midPos, FXMVECTOR endPos, FXMVECTOR target, FXMVECTOR bendNormal,
                  XMVECTOR& qRoot, XMVECTOR& qMid) {
    qRoot = XMQuaternionIdentity();
    qMid = XMQuaternionIdentity();
    const float lab = XMVectorGetX(XMVector3Length(XMVectorSubtract(midPos, rootPos)));
    const float lbc = XMVectorGetX(XMVector3Length(XMVectorSubtract(endPos, midPos)));
    if (lab < kEps || lbc < kEps) return false;

    XMVECTOR toTarget = XMVectorSubtract(target, rootPos);
    float d = XMVectorGetX(XMVector3Length(toTarget));
    XMVECTOR u;
    if (d < kEps) {
        // target sits on the root: fold toward the current end direction (a degenerate reach)
        u = XMVector3Normalize(XMVectorSubtract(endPos, rootPos));
        d = kEps;
    } else {
        u = XMVectorScale(toTarget, 1.0f / d);
    }
    const float minD = std::fabs(lab - lbc) + 1e-4f;
    const float maxD = lab + lbc - 1e-4f;  // keep a hair off full extension so acos and the plane stay finite
    d = std::clamp(d, std::min(minD, maxD), maxD);

    // in-plane offset of the elbow from the root->target line, on the current bend side
    XMVECTOR bend = XMVectorZero();
    {
        const XMVECTOR a = XMVectorSubtract(midPos, rootPos);
        bend = XMVectorSubtract(a, XMVectorScale(u, XMVectorGetX(XMVector3Dot(a, u))));
        if (XMVectorGetX(XMVector3LengthSq(bend)) < 1e-10f) {
            const XMVECTOR bn = XMVector3Normalize(bendNormal);
            bend = XMVector3Cross(bn, u);
            if (XMVectorGetX(XMVector3LengthSq(bend)) < 1e-10f) {
                const XMVECTOR ref = std::fabs(XMVectorGetY(u)) < 0.9f ? XMVectorSet(0, 1, 0, 0) : XMVectorSet(1, 0, 0, 0);
                bend = XMVector3Cross(u, ref);
            }
        }
    }
    bend = XMVector3Normalize(bend);

    const float x = (lab * lab + d * d - lbc * lbc) / (2.0f * d);
    const float h = std::sqrt(std::max(0.0f, lab * lab - x * x));
    const XMVECTOR newMid = XMVectorAdd(rootPos, XMVectorAdd(XMVectorScale(u, x), XMVectorScale(bend, h)));

    qRoot = RotationBetween(XMVector3Normalize(XMVectorSubtract(midPos, rootPos)), XMVector3Normalize(XMVectorSubtract(newMid, rootPos)));

    // where the end went after the root turned, then the mid correction onto the target
    const XMVECTOR endAfterRoot = XMVectorAdd(newMid, XMVector3Rotate(XMVectorSubtract(endPos, midPos), qRoot));
    XMVECTOR toEnd = XMVectorSubtract(endAfterRoot, newMid);
    XMVECTOR toTarget2 = XMVectorSubtract(target, newMid);
    if (XMVectorGetX(XMVector3LengthSq(toEnd)) > 1e-12f && XMVectorGetX(XMVector3LengthSq(toTarget2)) > 1e-12f)
        qMid = RotationBetween(XMVector3Normalize(toEnd), XMVector3Normalize(toTarget2));
    return true;
}

bool SolveAutoIkDrag(const AutoIkDrag& drag, const XMFLOAT3& target, AutoIkResult* out) {
    if (!out || !drag.valid) return false;
    XMVECTOR qRoot, qMid;
    if (!SolveTwoBone(XMLoadFloat3(&drag.rootPos), XMLoadFloat3(&drag.midPos), XMLoadFloat3(&drag.endPos),
                      XMLoadFloat3(&target), XMLoadFloat3(&drag.bendNormal), qRoot, qMid))
        return false;

    const XMVECTOR rootWorld = XMLoadFloat4(&drag.rootWorld);
    const XMVECTOR midWorld = XMLoadFloat4(&drag.midWorld);
    const XMVECTOR endWorld = XMLoadFloat4(&drag.endWorld);
    const XMVECTOR rootParentWorld = XMLoadFloat4(&drag.rootParentWorld);
    const XMVECTOR midParentWorld = XMLoadFloat4(&drag.midParentWorld);
    const XMVECTOR endParentWorld = XMLoadFloat4(&drag.endParentWorld);

    // a world delta q at a bone leaves world' = Multiply(world, q); descendants compose qRoot then qMid
    const XMVECTOR qRootThenMid = XMQuaternionMultiply(qRoot, qMid);
    const XMVECTOR rootWorldNew = XMQuaternionMultiply(rootWorld, qRoot);
    const XMVECTOR midParentWorldNew = XMQuaternionMultiply(midParentWorld, qRoot);
    const XMVECTOR midWorldNew = XMQuaternionMultiply(XMQuaternionMultiply(midWorld, qRoot), qMid);
    const XMVECTOR endParentWorldNew = XMQuaternionMultiply(endParentWorld, qRootThenMid);

    out->rootLocal = Store4(XMQuaternionNormalize(XMQuaternionMultiply(rootWorldNew, XMQuaternionInverse(rootParentWorld))));
    out->midLocal = Store4(XMQuaternionNormalize(XMQuaternionMultiply(midWorldNew, XMQuaternionInverse(midParentWorldNew))));
    // the end bone keeps its WORLD orientation: local' = endWorld * inverse(parentWorld')
    out->endLocal = Store4(XMQuaternionNormalize(XMQuaternionMultiply(endWorld, XMQuaternionInverse(endParentWorldNew))));
    out->ok = true;
    return true;
}

} // namespace mmdx::studio
