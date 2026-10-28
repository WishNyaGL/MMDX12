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

} // namespace mmdx::studio
