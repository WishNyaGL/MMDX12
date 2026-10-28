#include "studio/RigSolver.h"

#include "anim/ModelInstance.h"
#include "studio/AutoIk.h"

using namespace DirectX;

namespace mmdx::studio {

namespace {

// Rotation of a rigid bone world matrix (rows normalised, so a display scale does not leak in).
XMVECTOR RotationOf(const XMFLOAT4X4& m) {
    XMMATRIX r = XMLoadFloat4x4(&m);
    r.r[0] = XMVector3Normalize(r.r[0]);
    r.r[1] = XMVector3Normalize(r.r[1]);
    r.r[2] = XMVector3Normalize(r.r[2]);
    r.r[3] = XMVectorSet(0, 0, 0, 1);
    return XMQuaternionNormalize(XMQuaternionRotationMatrix(r));
}

// World position of a bone joint (skinning matrix times its rest position).
XMFLOAT3 JointWorld(const PmxModel& m, const ModelInstance& inst, int bone) {
    const auto& skin = inst.SkinMatrices();
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3TransformCoord(XMLoadFloat3(&m.bones[(size_t)bone].position),
                                                XMLoadFloat4x4(&skin[(size_t)bone])));
    return out;
}

} // namespace

bool ResolveRigChain(const RigDefinition& rig, RigRole rootRole, RigRole midRole, RigRole endRole, RigChain& out) {
    out = {};
    out.root = rig.BoneFor(rootRole);
    out.mid = rig.BoneFor(midRole);
    out.end = rig.BoneFor(endRole);
    return out.root >= 0 && out.mid >= 0 && out.end >= 0;
}

RigChainDrag BeginRigChainDrag(const ModelInstance& inst, const RigChain& chain) {
    RigChainDrag d;
    d.chain = chain;
    if (chain.root < 0 || chain.mid < 0 || chain.end < 0) return d;
    d.rootPos = JointWorld(inst.Model(), inst, chain.root);
    d.midPos = JointWorld(inst.Model(), inst, chain.mid);
    d.endPos = JointWorld(inst.Model(), inst, chain.end);
    XMStoreFloat4(&d.rootWorld, RotationOf(inst.BoneWorld(chain.root)));
    XMStoreFloat4(&d.midWorld, RotationOf(inst.BoneWorld(chain.mid)));
    XMStoreFloat4(&d.endWorld, RotationOf(inst.BoneWorld(chain.end)));
    const PmxModel& m = inst.Model();
    const auto parentWorldOf = [&](int b) {
        const int p = m.bones[(size_t)b].parentIndex;
        return p >= 0 && p < (int)m.bones.size() ? RotationOf(inst.BoneWorld(p)) : XMQuaternionIdentity();
    };
    XMStoreFloat4(&d.rootParentWorld, parentWorldOf(chain.root));
    XMStoreFloat4(&d.midParentWorld, parentWorldOf(chain.mid));
    XMStoreFloat4(&d.endParentWorld, parentWorldOf(chain.end));
    d.bendNormal = AutoIkBendNormal(m, chain.root, chain.mid, chain.end, d.rootPos, d.midPos, d.endPos);
    d.valid = true;
    return d;
}

bool SolveRigChainDrag(const RigChainDrag& drag, const DirectX::XMFLOAT3& target, RigChainResult* out) {
    if (!out || !drag.valid) return false;
    // Reuse the analytic two-bone solve (same bend-plane / reach semantics as the viewport auto-IK).
    XMVECTOR qRoot, qMid;
    if (!SolveTwoBone(XMLoadFloat3(&drag.rootPos), XMLoadFloat3(&drag.midPos), XMLoadFloat3(&drag.endPos),
                      XMLoadFloat3(&target), XMLoadFloat3(&drag.bendNormal), qRoot, qMid))
        return false;
    // Mirror StudyAutoIkDrag: a world delta q at a bone sets world' = world * q; descendants compose qRoot then qMid.
    const XMVECTOR rootWorld = XMLoadFloat4(&drag.rootWorld);
    const XMVECTOR midWorld = XMLoadFloat4(&drag.midWorld);
    const XMVECTOR endWorld = XMLoadFloat4(&drag.endWorld);
    const XMVECTOR rootParentWorld = XMLoadFloat4(&drag.rootParentWorld);
    const XMVECTOR midParentWorld = XMLoadFloat4(&drag.midParentWorld);
    const XMVECTOR endParentWorld = XMLoadFloat4(&drag.endParentWorld);
    const XMVECTOR qRootThenMid = XMQuaternionMultiply(qRoot, qMid);
    const XMVECTOR rootWorldNew = XMQuaternionMultiply(rootWorld, qRoot);
    const XMVECTOR midParentWorldNew = XMQuaternionMultiply(midParentWorld, qRoot);
    const XMVECTOR midWorldNew = XMQuaternionMultiply(XMQuaternionMultiply(midWorld, qRoot), qMid);
    const XMVECTOR endParentWorldNew = XMQuaternionMultiply(endParentWorld, qRootThenMid);
    XMStoreFloat4(&out->rootLocal, XMQuaternionNormalize(XMQuaternionMultiply(rootWorldNew, XMQuaternionInverse(rootParentWorld))));
    XMStoreFloat4(&out->midLocal, XMQuaternionNormalize(XMQuaternionMultiply(midWorldNew, XMQuaternionInverse(midParentWorldNew))));
    // the end bone keeps its WORLD orientation: local' = endWorld * inverse(parentWorld')
    XMStoreFloat4(&out->endLocal, XMQuaternionNormalize(XMQuaternionMultiply(endWorld, XMQuaternionInverse(endParentWorldNew))));
    out->ok = true;
    return true;
}

} // namespace mmdx::studio
