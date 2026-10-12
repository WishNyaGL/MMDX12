#pragma once
// Auto-IK posing: temporary IK chains for FK limbs. Dragging a wrist (or any bone whose parent chain has >= 2
// rotatable bones) solves the 2-bone parent chain so the end bone follows the gizmo, while the end bone keeps its
// WORLD orientation (the hand does not spin while the arm moves). The solved rotations become ordinary PoseLayer bone
// overrides (VMD-local values), so Register / auto-key / undo / mirror / VPD keep working unchanged.
// Pure math: no App state, unit tested in tools/studio_pose_test.cpp.
//
// Composition rules of this codebase (see anim/ModelInstance.cpp and app/UiStudioPose.cpp):
//   XMQuaternionMultiply(a, b) == "a, then b"; a bone's world rotation W = Multiply(local, parentWorld).
//   Applying a world-space delta rotation q to a bone: local' = Multiply(local, Multiply(Multiply(P, q), Inverse(P)))
//   with P = the parent's world rotation, i.e. W' = Multiply(W, q).
#include "asset/PmxModel.h"
#include <DirectXMath.h>
#include <vector>

namespace mmdx::studio {

// Finds the temporary IK chain for dragging `endBone`: `mid` and `root` are its nearest two ancestors that are
// rotatable and not fixed-axis (twist bones 腕捩 / 手捩 are skipped). False when fewer than 2 exist, or when the model
// already has a PMX IK that targets / links `endBone` (legs with 足IK use the model's own IK).
bool FindAutoIkChain(const PmxModel& model, int endBone, int& root, int& mid);

// The elbow's bend plane normal at a drag start: cross(mid-root, end-mid) in world space. When the chain is straight
// (the cross is degenerate), the rest-pose bend from the PMX bone positions, else world -Z.
DirectX::XMFLOAT3 AutoIkBendNormal(const PmxModel& model, int root, int mid, int end,
                                   const DirectX::XMFLOAT3& rootPos, const DirectX::XMFLOAT3& midPos,
                                   const DirectX::XMFLOAT3& endPos);

// Analytic law-of-cosines two-bone solve in world space. The returned quaternions are world-space delta rotations:
// qRoot rotates the root (about rootPos) and its subtree; qMid then rotates the mid (about its moved pivot) and its
// subtree. The bend plane is `bendNormal` (the elbow keeps its side, so it can never invert). An unreachable target
// leaves the chain straight toward it. No NaN at full extension.
bool SolveTwoBone(DirectX::FXMVECTOR rootPos, DirectX::FXMVECTOR midPos, DirectX::FXMVECTOR endPos,
                  DirectX::FXMVECTOR target, DirectX::FXMVECTOR bendNormal,
                  DirectX::XMVECTOR& qRoot, DirectX::XMVECTOR& qMid);

// State captured when an auto-IK drag starts (all world space / start pose).
struct AutoIkDrag {
    int root = -1, mid = -1, end = -1;
    DirectX::XMFLOAT3 rootPos{}, midPos{}, endPos{};        // world joint positions at the start
    DirectX::XMFLOAT4 rootWorld{0, 0, 0, 1}, midWorld{0, 0, 0, 1}, endWorld{0, 0, 0, 1};  // world rotations at the start
    DirectX::XMFLOAT4 rootParentWorld{0, 0, 0, 1}, midParentWorld{0, 0, 0, 1},
                      endParentWorld{0, 0, 0, 1};           // their parents' world rotations at the start
    DirectX::XMFLOAT3 bendNormal{0, 0, -1};                 // resolved bend plane
    bool valid = false;
};

// Solves a drag to `target` (world position of the end joint) and returns the new VMD-local rotations of the three
// bones. The end bone's world orientation is preserved. Translations are the caller's business (unchanged).
struct AutoIkResult {
    DirectX::XMFLOAT4 rootLocal{0, 0, 0, 1}, midLocal{0, 0, 0, 1}, endLocal{0, 0, 0, 1};
    bool ok = false;
};
bool SolveAutoIkDrag(const AutoIkDrag& drag, const DirectX::XMFLOAT3& target, AutoIkResult* out);

} // namespace mmdx::studio
