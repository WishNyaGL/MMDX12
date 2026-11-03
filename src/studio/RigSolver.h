#pragma once
// Rig solver (hybrid): a controller is driven as an IK target while dragging (arms/legs via the existing two-bone
// AutoIK), but the authored result is ordinary bone values (the pose layer / keys). Body controllers are single-bone
// FK (they reuse the normal bone gizmo once a handle is picked; no special math).
//
// This file provides the pure-math core: capture a chain-drag start from the current pose/rig, then solve a new
// target into VMD-local bone rotations (end orientation preserved, bend plane kept). No App / UI state.
#include "asset/PmxModel.h"
#include "studio/RigDefinition.h"
#include <DirectXMath.h>

namespace mmdx {
class ModelInstance;
}

namespace mmdx::studio {

// A chain controller's roles (from a RigDefinition): root / mid / end. Returns false when any role is unresolved.
struct RigChain {
    int root = -1, mid = -1, end = -1;
};
bool ResolveRigChain(const RigDefinition& rig, RigRole rootRole, RigRole midRole, RigRole endRole, RigChain& out);
// Builds a virtual IK chain from the actual bone hierarchy between `rootBone` and
// `endBone`: walk from the end up the parents to the root, keeping rotatable,
// non-fixed-axis bones (twist bones шЕХцНй/цЙЛцНй are skipped), and return them root-first.
// Always includes both `rootBone` and `endBone` when they are valid.
std::vector<int> BuildRigChainByHierarchy(const PmxModel& model, int rootBone, int endBone);

// State captured when a chain-controller drag starts (all world space / start pose). Mirrors AutoIkDrag but is built
// from rig roles and works for hands and feet.
struct RigChainDrag {
    RigChain chain;
    DirectX::XMFLOAT3 rootPos{}, midPos{}, endPos{};
    DirectX::XMFLOAT4 rootWorld{0, 0, 0, 1}, midWorld{0, 0, 0, 1}, endWorld{0, 0, 0, 1};
    DirectX::XMFLOAT4 rootParentWorld{0, 0, 0, 1}, midParentWorld{0, 0, 0, 1},
                      endParentWorld{0, 0, 0, 1};
    DirectX::XMFLOAT3 bendNormal{0, 0, -1};
    bool valid = false;
};

// Captures the chain-drag start from a model's current pose.
RigChainDrag BeginRigChainDrag(const ModelInstance& inst, const RigChain& chain);

// Solves the drag so the end joint reaches `target` (world). Returns new VMD-local rotations for root/mid/end (the
// end's world orientation is preserved). False when the chain is invalid.
struct RigChainResult {
    DirectX::XMFLOAT4 rootLocal{0, 0, 0, 1}, midLocal{0, 0, 0, 1}, endLocal{0, 0, 0, 1};
    bool ok = false;
};
bool SolveRigChainDrag(const RigChainDrag& drag, const DirectX::XMFLOAT3& target, RigChainResult* out);

// FK controller drag: rotate a single bone directly. The drag captures the camera
// basis and the start local rotation; the solver converts a screen-space drag
// (dx/dy pixels) into a world rotation about the camera axes.
struct RigFkDrag {
    int bone = -1;
    DirectX::XMFLOAT4 startLocal{0, 0, 0, 1};
    DirectX::XMFLOAT3 viewRight{1, 0, 0}, viewUp{0, 1, 0};
    float pitchSign = 1.0f;   // +1 default, -1 for shoulders/upper arms
};
// Captures an FK drag start from the current pose.
RigFkDrag BeginRigFkDrag(const ModelInstance& inst, int bone, float pitchSign);
// Solves a screen-space drag into a new local rotation for the bone. `viewRight` /
// `viewUp` are the camera basis at the current frame; `parentWorld` is the bone's
// parent world rotation (identity for a root bone); `dxPx` / `dyPx` are the mouse
// delta in pixels from the drag start. Returns the new local rotation.
DirectX::XMFLOAT4 SolveRigFkDrag(const RigFkDrag& fk, const DirectX::XMFLOAT3& viewRight,
                                 const DirectX::XMFLOAT3& viewUp, const DirectX::XMFLOAT4& parentWorld,
                                 float dxPx, float dyPx);

// Virtual IK solver: solves an arbitrary-length chain toward `target` (world). The
// chain bones are rotated in place (root-first), the bend plane per joint is kept
// from the rest pose, and per-bone limits (RigBoneLimit) are applied. Pure math,
// no App state.
struct RigVirtualIkDrag {
    std::vector<int> chain;                       // chain bones, root..target (indices into PmxModel::bones)
    std::vector<DirectX::XMFLOAT3> startPos;      // world joint positions at drag start (per chain bone + target)
    std::vector<DirectX::XMFLOAT4> startWorld;    // world rotations at drag start
    std::vector<DirectX::XMFLOAT4> parentWorld;   // (direct) parent world rotations at drag start
    std::vector<DirectX::XMFLOAT4> startLocal;    // LOCAL rotations at drag start (child relative to its parent)
    // Intermediate bones skipped by the chain (twist bones) between consecutive chain joints:
    // gaps[i] (i in 1..n-1) lists the bones strictly between chain[i-1] and chain[i] in the
    // arm hierarchy, top-down, with their start-local rotations. The reconstruction uses them
    // to compute the REAL parent of each joint (the last twist before it), not the previous
    // chain element — otherwise the twist is folded into the local angle and accumulates.
    std::vector<std::vector<int>> gapBones;
    std::vector<std::vector<DirectX::XMFLOAT4>> gapStartLocal;
    std::vector<DirectX::XMFLOAT3> bendNormals;   // per-joint bend plane normals (rest pose)
    std::vector<RigBoneLimit> limits;             // per-bone limits (same order as chain)
    int targetBone = -1;                          // the chain's end (target) bone index
    bool valid = false;
};
// Captures a virtual-IK drag start from the current pose.
RigVirtualIkDrag BeginRigVirtualIkDrag(const ModelInstance& inst, const std::vector<int>& chain,
                                       const std::vector<RigBoneLimit>& limits, int targetBone);
// Solves toward `target` (world). Returns new local rotations per chain bone
// (same order as `chain`), or false when invalid.
bool SolveRigVirtualIkDrag(const RigVirtualIkDrag& d, const DirectX::XMFLOAT3& target,
                           std::vector<DirectX::XMFLOAT4>& outLocal);

} // namespace mmdx::studio
