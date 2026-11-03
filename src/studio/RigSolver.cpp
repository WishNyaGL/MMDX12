#include "studio/RigSolver.h"

#include "anim/ModelInstance.h"
#include "studio/AutoIk.h"
#include "core/Log.h"

#ifndef RIG_SOLVER_LOG
#define RIG_SOLVER_LOG(...) LOG_INFO(__VA_ARGS__)
#endif

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

XMFLOAT3 Store(XMVECTOR v) { XMFLOAT3 r; XMStoreFloat3(&r, v); return r; }

// World position of a bone joint (skinning matrix times its rest position).
XMFLOAT3 JointWorld(const PmxModel& m, const ModelInstance& inst, int bone) {
    const auto& skin = inst.SkinMatrices();
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3TransformCoord(XMLoadFloat3(&m.bones[(size_t)bone].position),
                                                XMLoadFloat4x4(&skin[(size_t)bone])));
    return out;
}

// Inspector's bone-rotation convention: q = RollPitchYaw(x, y, z), Euler degrees with
// pitch = asin(-m._32), yaw = atan2(m._31, m._33), roll = atan2(m._12, m._22).
XMFLOAT3 EulerDegLocal(const XMFLOAT4& q) {
    XMFLOAT4X4 rm;
    XMStoreFloat4x4(&rm, XMMatrixRotationQuaternion(XMLoadFloat4(&q)));
    const float pitch = std::asin(std::clamp(-rm._32, -1.0f, 1.0f));
    const float yaw = std::atan2(rm._31, rm._33);
    const float roll = std::atan2(rm._12, rm._22);
    return {pitch, yaw, roll};
}
XMVECTOR QuatFromEulerDegLocal(const XMFLOAT3& e) {
    return XMQuaternionRotationRollPitchYaw(e.x, e.y, e.z);
}

} // namespace

bool ResolveRigChain(const RigDefinition& rig, RigRole rootRole, RigRole midRole, RigRole endRole, RigChain& out) {
    out = {};
    out.root = rig.BoneFor(rootRole);
    out.mid = rig.BoneFor(midRole);
    out.end = rig.BoneFor(endRole);
    return out.root >= 0 && out.mid >= 0 && out.end >= 0;
}

std::vector<int> BuildRigChainByHierarchy(const PmxModel& model, int rootBone, int endBone) {
    std::vector<int> out;
    const int n = (int)model.bones.size();
    if (rootBone < 0 || rootBone >= n || endBone < 0 || endBone >= n) return out;
    // A twist bone (FixedAxis flag, or a name carrying 捩/twist) is a locked-axis joint: it
    // must never be part of the solved chain — its hand-set angle would be re-derived from
    // geometry on every move (accumulating). Treat it as transparent when walking the
    // hierarchy.
    const auto isTwistBone = [](const std::string& name) {
        return name.find("捩") != std::string::npos ||
               name.find("twist") != std::string::npos ||
               name.find("Twist") != std::string::npos;
    };
    // walk from the end up the parents to the root, keeping rotatable, non-twist bones
    std::vector<int> collected;
    int cur = endBone;
    int guard = 0;
    while (cur >= 0 && cur < n && guard++ < n) {
        const PmxBone& b = model.bones[(size_t)cur];
        const bool isTwist = (b.flags & PmxBone_FixedAxis) != 0 || isTwistBone(b.name);
        if (cur == endBone || cur == rootBone || ((b.flags & PmxBone_Rotatable) && !isTwist))
            collected.push_back(cur);
        if (cur == rootBone) break;
        cur = b.parentIndex;
    }
    // collected is end..root; reverse to root..end
    for (auto it = collected.rbegin(); it != collected.rend(); ++it) out.push_back(*it);
    // ensure root first and end last
    const auto has = [&](int x) { for (int y : out) if (y == x) return true; return false; };
    if (!has(rootBone)) out.insert(out.begin(), rootBone);
    if (!has(endBone)) out.push_back(endBone);
    return out;
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
    XMStoreFloat4(&out->rootLocal, XMQuaternionNormalize(XMQuaternionMultiply(rootWorldNew, XMQuaternionInverse(rootParentWorld))));
    XMStoreFloat4(&out->midLocal, XMQuaternionNormalize(XMQuaternionMultiply(midWorldNew, XMQuaternionInverse(midParentWorldNew))));
    // The hand (end bone) keeps its OWN rotation relative to the forearm: only the
    // shoulder/elbow rotate to place the wrist, so the hand's local rotation is
    // unchanged by the move. This lets the hand hold a twist and still respond to a
    // later rotation edit (it does not chase the forearm vector).
    XMStoreFloat4(&out->endLocal, XMQuaternionNormalize(XMQuaternionMultiply(endWorld, XMQuaternionInverse(endParentWorld))));
    out->ok = true;
    return true;
}

RigFkDrag BeginRigFkDrag(const ModelInstance& inst, int bone, float pitchSign) {
    RigFkDrag fk;
    fk.bone = bone;
    fk.pitchSign = pitchSign;
    const PmxModel& m = inst.Model();
    if (bone < 0 || bone >= (int)m.bones.size()) return fk;
    const int parent = m.bones[(size_t)bone].parentIndex;
    const XMVECTOR parentRot = parent >= 0 ? RotationOf(inst.BoneWorld(parent)) : XMQuaternionIdentity();
    const XMVECTOR world = RotationOf(inst.BoneWorld(bone));
    const XMVECTOR local = XMQuaternionNormalize(XMQuaternionMultiply(world, XMQuaternionInverse(parentRot)));
    XMStoreFloat4(&fk.startLocal, local);
    return fk;
}

DirectX::XMFLOAT4 SolveRigFkDrag(const RigFkDrag& fk, const DirectX::XMFLOAT3& viewRight,
                                 const DirectX::XMFLOAT3& viewUp, const DirectX::XMFLOAT4& parentWorld,
                                 float dxPx, float dyPx) {
    constexpr float kAng = 0.01f;  // radians per screen pixel
    XMFLOAT4 out = fk.startLocal;
    const XMVECTOR qYaw = XMQuaternionRotationAxis(XMLoadFloat3(&viewUp), -dxPx * kAng);
    const XMVECTOR qPitch = XMQuaternionRotationAxis(XMLoadFloat3(&viewRight), fk.pitchSign * dyPx * kAng);
    const XMVECTOR q = XMQuaternionMultiply(qPitch, qYaw);
    const XMVECTOR parentRot = XMLoadFloat4(&parentWorld);
    const XMVECTOR oldWorld = XMQuaternionMultiply(parentRot, XMLoadFloat4(&fk.startLocal));
    const XMVECTOR newWorld = XMQuaternionNormalize(XMQuaternionMultiply(q, oldWorld));
    const XMVECTOR invParent = XMQuaternionInverse(parentRot);
    const XMVECTOR newLocal = XMQuaternionNormalize(XMQuaternionMultiply(invParent, newWorld));
    XMStoreFloat4(&out, newLocal);
    return out;
}

RigVirtualIkDrag BeginRigVirtualIkDrag(const ModelInstance& inst, const std::vector<int>& chain,
                                       const std::vector<RigBoneLimit>& limits, int targetBone) {
    RigVirtualIkDrag d;
    d.chain = chain;
    d.limits = limits;
    d.targetBone = targetBone;
    const PmxModel& m = inst.Model();
    const int n = (int)chain.size();
    if (n < 2) return d;
    d.startPos.resize(n + 1);
    d.startWorld.resize(n);
    d.parentWorld.resize(n);
    d.startLocal.resize(n);
    d.gapBones.resize(n);
    d.gapStartLocal.resize(n);
    d.bendNormals.resize(n - 1);
    for (int i = 0; i < n; ++i) {
        const int b = chain[(size_t)i];
        if (b < 0 || b >= (int)m.bones.size()) return d;
        d.startPos[(size_t)i] = JointWorld(m, inst, b);
        XMStoreFloat4(&d.startWorld[(size_t)i], RotationOf(inst.BoneWorld(b)));
        const int p = m.bones[(size_t)b].parentIndex;
        XMStoreFloat4(&d.parentWorld[(size_t)i], p >= 0 ? RotationOf(inst.BoneWorld(p)) : XMQuaternionIdentity());
        XMStoreFloat4(&d.startLocal[(size_t)i],
                      XMQuaternionNormalize(XMQuaternionMultiply(
                          RotationOf(inst.BoneWorld(b)), XMQuaternionInverse(XMLoadFloat4(&d.parentWorld[(size_t)i])))));
        // Intermediate (twist) bones between the previous chain joint and this one.
        if (i > 0) {
            // Walk parents of this bone up to the previous chain bone, collecting the skipped
            // bones top-down (from the previous chain joint toward this one).
            std::vector<int> up;
            int cur = m.bones[(size_t)b].parentIndex;
            const int prev = chain[(size_t)i - 1];
            while (cur >= 0 && cur != prev && (int)up.size() < n) {
                up.push_back(cur);
                cur = m.bones[(size_t)cur].parentIndex;
            }
            for (auto it = up.rbegin(); it != up.rend(); ++it) {
                const int g = *it;
                if (g < 0 || g >= (int)m.bones.size()) continue;
                XMFLOAT4 gWorld{0, 0, 0, 1};
                const int gp = m.bones[(size_t)g].parentIndex;
                XMStoreFloat4(&gWorld, gp >= 0 ? RotationOf(inst.BoneWorld(gp)) : XMQuaternionIdentity());
                d.gapBones[(size_t)i].push_back(g);
                XMFLOAT4 gLocal{0, 0, 0, 1};
                XMStoreFloat4(&gLocal, XMQuaternionNormalize(XMQuaternionMultiply(
                    RotationOf(inst.BoneWorld(g)), XMQuaternionInverse(XMLoadFloat4(&gWorld)))));
                d.gapStartLocal[(size_t)i].push_back(gLocal);            }
        }
    }
    d.startPos[(size_t)n] = JointWorld(m, inst, targetBone >= 0 ? targetBone : chain.back());
    // bend plane normals from the rest pose per joint
    for (int i = 0; i < n - 1; ++i) {
        const XMVECTOR a = XMLoadFloat3(&d.startPos[(size_t)i]);
        const XMVECTOR b = XMLoadFloat3(&d.startPos[(size_t)i + 1]);
        const XMVECTOR c = XMLoadFloat3(&d.startPos[(size_t)i + 2]);
        XMVECTOR normal = XMVector3Cross(XMVectorSubtract(b, a), XMVectorSubtract(c, b));
        XMStoreFloat3(&d.bendNormals[(size_t)i],
                      XMVectorGetX(XMVector3LengthSq(normal)) > 1e-8f ? XMVector3Normalize(normal)
                                                                     : XMVectorSet(0, 0, -1, 0));
    }
    d.valid = true;
    RIG_SOLVER_LOG("VK begin chain=%d targetBone=%d", n, targetBone);
    for (int i = 0; i < n; ++i)
        RIG_SOLVER_LOG("  startWorld[%d]=(%.3f,%.3f,%.3f,%.3f) parent=(%.3f,%.3f,%.3f,%.3f)",
                       i,
                       d.startWorld[(size_t)i].x, d.startWorld[(size_t)i].y,
                       d.startWorld[(size_t)i].z, d.startWorld[(size_t)i].w,
                       d.parentWorld[(size_t)i].x, d.parentWorld[(size_t)i].y,
                       d.parentWorld[(size_t)i].z, d.parentWorld[(size_t)i].w);
    return d;
}

bool SolveRigVirtualIkDrag(const RigVirtualIkDrag& d, const DirectX::XMFLOAT3& target,
                           std::vector<DirectX::XMFLOAT4>& outLocal) {
    if (!d.valid) return false;
    const int n = (int)d.chain.size();
    if (n < 2) return false;
    // Input contract (see RigVirtualIkDrag): one start point per chain bone plus the
    // target (n+1), plus n world/parent rotations and n-1 bend normals. Reject a
    // malformed capture here instead of walking out of bounds.
    if ((int)d.startPos.size() != n + 1) return false;
    if ((int)d.startWorld.size() < n) return false;
    if ((int)d.parentWorld.size() < n) return false;
    if ((int)d.bendNormals.size() < n - 1) return false;

    // ---- FABRIK (positional) ----
    // Fixed rest-pose segment lengths. The chain bends to reach the target.
    std::vector<XMFLOAT3> pos = d.startPos;
    std::vector<float> seg((size_t)n);
    for (int i = 0; i < n; ++i)
        seg[(size_t)i] = XMVectorGetX(XMVector3Length(XMVectorSubtract(
            XMLoadFloat3(&d.startPos[(size_t)i + 1]), XMLoadFloat3(&d.startPos[(size_t)i]))));
    const XMVECTOR targetV = XMLoadFloat3(&target);
    pos[(size_t)n] = Store(targetV);
    for (int iter = 0; iter < 8; ++iter) {
        // backward pass toward the root (root fixed)
        pos[0] = d.startPos[0];
        for (int i = 1; i <= n; ++i) {
            const float len = seg[(size_t)i - 1];
            XMVECTOR dir = XMVectorSubtract(XMLoadFloat3(&pos[(size_t)i]), XMLoadFloat3(&pos[(size_t)i - 1]));
            const float dl = XMVectorGetX(XMVector3Length(dir));
            if (dl > 1e-9f) dir = XMVectorScale(dir, 1.0f / dl);
            pos[(size_t)i] = Store(XMVectorAdd(XMLoadFloat3(&pos[(size_t)i - 1]), XMVectorScale(dir, len)));
        }
        // forward pass: pull the end to the target
        pos[(size_t)n] = Store(targetV);
        for (int i = n - 1; i >= 0; --i) {
            const float len = seg[(size_t)i];
            XMVECTOR dir = XMVectorSubtract(XMLoadFloat3(&pos[(size_t)i + 1]), XMLoadFloat3(&pos[(size_t)i]));
            const float dl = XMVectorGetX(XMVector3Length(dir));
            if (dl > 1e-9f) dir = XMVectorScale(dir, 1.0f / dl);
            pos[(size_t)i] = Store(XMVectorSubtract(XMLoadFloat3(&pos[(size_t)i + 1]), XMVectorScale(dir, len)));
        }
    }
    // ---- bend-side guard ----
    // FABRIK alone can flip an interior joint (elbow/knee) through the singularity when the
    // target crosses the shoulder/hip line, inverting the chain by ~180°. Keep every interior
    // joint on the same side of the line joining its neighbours as in the rest pose.
    for (int i = 1; i < n; ++i) {
        const XMFLOAT3& A = pos[(size_t)i - 1];
        const XMFLOAT3& B = pos[(size_t)i + 1];
        const XMFLOAT3& P = pos[(size_t)i];
        const XMFLOAT3& rA = d.startPos[(size_t)i - 1];
        const XMFLOAT3& rB = d.startPos[(size_t)i + 1];
        const XMFLOAT3& rP = d.startPos[(size_t)i];
        const XMVECTOR rAb = XMVectorSubtract(XMLoadFloat3(&rB), XMLoadFloat3(&rA));
        const XMVECTOR rAp = XMVectorSubtract(XMLoadFloat3(&rP), XMLoadFloat3(&rA));
        const XMVECTOR nRest = XMVector3Cross(rAb, rAp);
        if (XMVectorGetX(XMVector3LengthSq(nRest)) < 1e-12f) continue;
        const XMVECTOR ab = XMVectorSubtract(XMLoadFloat3(&B), XMLoadFloat3(&A));
        const XMVECTOR side = XMVector3Dot(XMVector3Cross(ab, XMVectorSubtract(XMLoadFloat3(&P), XMLoadFloat3(&A))), nRest);
        if (XMVectorGetX(side) >= 0.0f) continue;
        const float abLen2 = XMVectorGetX(XMVector3LengthSq(ab));
        if (abLen2 < 1e-12f) continue;
        const float t = XMVectorGetX(XMVector3Dot(XMVectorSubtract(XMLoadFloat3(&P), XMLoadFloat3(&A)), ab)) / abLen2;
        const XMVECTOR proj = XMVectorAdd(XMLoadFloat3(&A), XMVectorScale(ab, t));
        const XMVECTOR refl = XMVectorSubtract(XMVectorScale(proj, 2.0f), XMLoadFloat3(&P));
        XMStoreFloat3(&pos[(size_t)i], refl);
    }

    // ---- reconstruct local rotations (preserving twist) ----
    // For each chain joint, the REAL parent is not the previous chain element (there may be
    // skipped twist bones between them): it is the last twist before the joint. We propagate
    // the previous chain joint's new world rotation through the intermediate twists (each
    // keeping its captured local angle), then compute the joint's local rotation against that
    // real parent. This prevents folding a twist into the joint's local angle and
    // accumulating it on every move.
    outLocal.resize(n);
    std::vector<DirectX::XMFLOAT4> worldNewArr(n);
    const auto realParentAt = [&](int i, const std::vector<DirectX::XMFLOAT4>& wna) {
        XMVECTOR rp;
        if (i == 0) {
            rp = XMLoadFloat4(&d.parentWorld[0]);
        } else {
            rp = XMLoadFloat4(&wna[(size_t)i - 1]);
            if (i < (int)d.gapBones.size()) {
                const auto& gs = d.gapStartLocal[(size_t)i];
                for (const auto& gl : gs)
                    rp = XMQuaternionNormalize(XMQuaternionMultiply(rp, XMLoadFloat4(&gl)));
            }
        }
        return rp;
    };
    const auto startLocalAt = [&](int i) {
        XMVECTOR loc;
        if (i < (int)d.startLocal.size())
            loc = XMLoadFloat4(&d.startLocal[(size_t)i]);
        else
            loc = XMQuaternionNormalize(XMQuaternionMultiply(
                XMLoadFloat4(&d.startWorld[(size_t)i]),
                XMQuaternionInverse(XMLoadFloat4(&d.parentWorld[(size_t)i]))));
        return loc;
    };
    for (int i = 0; i < n; ++i) {
        const bool isEnd = (i == n - 1 && n >= 2);
        const XMVECTOR realParent = realParentAt(i, worldNewArr);
        XMVECTOR worldNew, local;
        if (isEnd) {
            // End bone keeps its captured local angle; its world follows the real parent.
            local = startLocalAt(i);
            worldNew = XMQuaternionNormalize(XMQuaternionMultiply(realParent, local));
        } else {
            XMVECTOR startDir = XMVectorSubtract(XMLoadFloat3(&d.startPos[(size_t)i + 1]),
                                                 XMLoadFloat3(&d.startPos[(size_t)i]));
            XMVECTOR newDir = XMVectorSubtract(XMLoadFloat3(&pos[(size_t)i + 1]),
                                               XMLoadFloat3(&pos[(size_t)i]));
            const float ls = XMVectorGetX(XMVector3LengthSq(startDir));
            const float ln = XMVectorGetX(XMVector3LengthSq(newDir));
            XMVECTOR q = XMQuaternionIdentity();
            if (ls > 1e-12f && ln > 1e-12f) {
                startDir = XMVector3Normalize(startDir);
                newDir = XMVector3Normalize(newDir);
                const float dot = XMVectorGetX(XMVector3Dot(startDir, newDir));
                if (dot < 1.0f - 1e-7f) {
                    XMVECTOR axis = XMVector3Cross(startDir, newDir);
                    if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-12f)
                        axis = XMVector3Cross(startDir, XMVectorSet(0, 1, 0, 0));
                    q = XMQuaternionRotationAxis(XMVector3Normalize(axis), std::acos(std::clamp(dot, -1.0f, 1.0f)));
                }
            }
            worldNew = XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&d.startWorld[(size_t)i]), q));
            local = XMQuaternionNormalize(XMQuaternionMultiply(worldNew, XMQuaternionInverse(realParent)));
            // The arm (first joint) must not twist about its own axis: it only bends.
            // Preserve the LOCAL X Euler angle (the inspector's convention: q = RollPitchYaw(x,
            // y, z)) from the start pose, letting Y/Z follow the IK. This keeps the roll shown
            // in the inspector flat (0 stays 0, a manual roll stays).
            if (i == 0 && !d.startLocal.empty()) {
                const XMFLOAT3 startE = EulerDegLocal(d.startLocal[(size_t)0]);
                XMFLOAT4 lf;
                XMStoreFloat4(&lf, local);
                XMFLOAT3 curE = EulerDegLocal(lf);
                curE.x = startE.x;
                local = XMQuaternionNormalize(QuatFromEulerDegLocal(curE));
            }
        }
        XMStoreFloat4(&worldNewArr[(size_t)i], worldNew);
        if (i < (int)d.limits.size()) {
            const RigBoneLimit& L = d.limits[(size_t)i];
            if (L.lockX || L.lockY || L.lockZ) {
                const XMVECTOR startLocal = XMQuaternionNormalize(XMQuaternionMultiply(
                    XMLoadFloat4(&d.startWorld[(size_t)i]), XMQuaternionInverse(XMLoadFloat4(&d.parentWorld[(size_t)i]))));
                XMFLOAT4 sl, lc;
                XMStoreFloat4(&sl, startLocal);
                XMStoreFloat4(&lc, local);
                if (L.lockX) lc.x = sl.x;
                if (L.lockY) lc.y = sl.y;
                if (L.lockZ) lc.z = sl.z;
                local = XMQuaternionNormalize(XMLoadFloat4(&lc));
            }
        }
        XMStoreFloat4(&outLocal[(size_t)i], local);
    }
    // TEMP full diagnostic: start/new joint positions, segment directions before/after,
    // start world, reconstructed world, the parent used in reconstruction vs the REAL parent
    // (d.parentWorld[i] — captured at drag start, e.g. the twist bone for an elbow), start
    // local and output local.
    for (int i = 0; i < n; ++i) {
        XMFLOAT3 dirStart{0, 0, 0}, dirNew{0, 0, 0};
        if (i + 1 < (int)pos.size()) {
            XMVECTOR ds = XMVectorSubtract(XMLoadFloat3(&d.startPos[(size_t)i + 1]),
                                           XMLoadFloat3(&d.startPos[(size_t)i]));
            XMVECTOR dn = XMVectorSubtract(XMLoadFloat3(&pos[(size_t)i + 1]),
                                           XMLoadFloat3(&pos[(size_t)i]));
            const float lds = XMVectorGetX(XMVector3Length(ds));
            const float ldn = XMVectorGetX(XMVector3Length(dn));
            if (lds > 1e-9f) ds = XMVectorScale(ds, 1.0f / lds);
            if (ldn > 1e-9f) dn = XMVectorScale(dn, 1.0f / ldn);
            XMStoreFloat3(&dirStart, ds);
            XMStoreFloat3(&dirNew, dn);
        }
        XMFLOAT4 usedParent{0, 0, 0, 1};
        XMStoreFloat4(&usedParent, realParentAt(i, worldNewArr));
        XMFLOAT4 locStart{0, 0, 0, 1};
        if (i < (int)d.startLocal.size()) locStart = d.startLocal[(size_t)i];
        RIG_SOLVER_LOG(
            "VK[%d] bone=%d posStart=(%.3f,%.3f,%.3f) posAfter=(%.3f,%.3f,%.3f) "
            "dirStart=(%.3f,%.3f,%.3f) dirNew=(%.3f,%.3f,%.3f) "
            "wStart=(%.3f,%.3f,%.3f,%.3f) wNew=(%.3f,%.3f,%.3f,%.3f) "
            "parentUsed=(%.3f,%.3f,%.3f,%.3f) parentReal=(%.3f,%.3f,%.3f,%.3f) "
            "locStart=(%.3f,%.3f,%.3f,%.3f) locOut=(%.3f,%.3f,%.3f,%.3f)",
            i, d.chain[(size_t)i],
            d.startPos[(size_t)i].x, d.startPos[(size_t)i].y, d.startPos[(size_t)i].z,
            pos[(size_t)i].x, pos[(size_t)i].y, pos[(size_t)i].z,
            dirStart.x, dirStart.y, dirStart.z, dirNew.x, dirNew.y, dirNew.z,
            d.startWorld[(size_t)i].x, d.startWorld[(size_t)i].y,
            d.startWorld[(size_t)i].z, d.startWorld[(size_t)i].w,
            worldNewArr[(size_t)i].x, worldNewArr[(size_t)i].y,
            worldNewArr[(size_t)i].z, worldNewArr[(size_t)i].w,
            usedParent.x, usedParent.y, usedParent.z, usedParent.w,
            d.parentWorld[(size_t)i].x, d.parentWorld[(size_t)i].y,
            d.parentWorld[(size_t)i].z, d.parentWorld[(size_t)i].w,
            locStart.x, locStart.y, locStart.z, locStart.w,
            outLocal[(size_t)i].x, outLocal[(size_t)i].y,
            outLocal[(size_t)i].z, outLocal[(size_t)i].w);
    }
    return true;
}

} // namespace mmdx::studio
