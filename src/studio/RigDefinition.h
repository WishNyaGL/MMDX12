#pragma once
// Rig data model (mmdx12-native, independent of the PMX skeleton). A rig is a set of controller roles bound to
// original PMX bones. It is stored separately from the model/animation and never alters the PMX or VMD data.
#include <string>
#include <vector>

namespace mmdx {
struct PmxModel;
}

namespace mmdx::studio {

// Controller roles a rig can drive. Order is fixed; a RigDefinition always has one entry per role.
enum class RigRole : int {
    Pelvis = 0,
    Groove,
    Spine,
    Chest,
    Neck,
    Head,
    LeftShoulder,
    LeftUpperArm,
    LeftForearm,
    LeftHand,
    RightShoulder,
    RightUpperArm,
    RightForearm,
    RightHand,
    LeftThigh,
    LeftShin,
    LeftFoot,
    LeftToe,
    RightThigh,
    RightShin,
    RightFoot,
    RightToe,
    Waist,
    Count
};

// Human-readable role id (ASCII, stable — used in diagnostics / serialisation).
const char* RigRoleName(RigRole role);
bool RigRoleIsLeft(RigRole role);
bool RigRoleIsRight(RigRole role);

// Controller kind: a limb chain (hands/feet/toes), a body ring (single-bone
// select/drag), or an FK joint (diamond, directly rotates a bone).
enum class RigControllerKind { Chain, Body, Joint };
// Visual shape of the controller (grows in stage 4).
enum class RigControllerShape { Ring, Diamond, Sphere, Cube };
// Solver type / IK mode (stage 2): FK, or IK with a native PMX IK bone or a
// virtual chain.
enum class RigSolverType { FK, IK };
enum class RigIkMode { NativePMX, Virtual };
// Transform constraints (stage 5): axis locks and a movement distance limit.
struct RigControllerConstraints {
    bool lockX = false, lockY = false, lockZ = false;
    bool hasMaxDist = false;
    float maxDist = 0.0f;
};

// One controller = the target architecture's unit. It is an entity resolved to
// one or more bone roles (bindings), with a visual shape and a solver. The
// prototype still drives most behaviour from the role bindings; the controller
// list is the single source of truth for what exists and how it is drawn.
struct RigController {
    std::string id;                              // stable id (also the timeline row label)
    RigControllerKind kind = RigControllerKind::Body;
    RigControllerShape shape = RigControllerShape::Ring;
    std::vector<RigRole> roles;                  // roles this controller binds
    bool draggable = false;                      // body: center/groove drag in the camera plane
    bool rotate = false;                         // joint: directly rotates its bone
    float size = 1.0f;                           // visual size scale
    RigSolverType solverType = RigSolverType::FK;
    RigIkMode ikMode = RigIkMode::NativePMX;
    RigControllerConstraints constraints;
};

// One role -> PMX bone binding.
struct RigBoneBinding {
    RigRole role = RigRole::Pelvis;
    int boneIndex = -1;         // index into PmxModel::bones; -1 = unresolved
    std::string boneName;       // original bone name (for rebinding across model changes)
    float confidence = 0.0f;    // 0..1 match confidence
    bool manuallyAssigned = false;
};

// A model's own IK drive bone that a controller should move as an object (e.g. arm_IK / leg_IKP / toe_IK). The
// `targetRole` is the body role it controls (LeftHand/RightHand/LeftFoot/RightFoot/LeftToe/...).
struct RigIkBone {
    RigRole targetRole = RigRole::Pelvis;
    int boneIndex = -1;
    std::string boneName;
};

// The full rig definition (one binding per role, in RigRole order).
struct RigDefinition {
    int version = 1;
    std::vector<RigBoneBinding> bindings;  // size == (int)RigRole::Count
    std::vector<RigIkBone> ikBones;        // ready-made model IK bones (arm/leg/toe) the controllers can drive
    std::vector<RigController> controllers; // the controller entities (single source of truth for what exists)
    std::string sourceModelName;           // display-only hint (not part of identity)

    RigDefinition() { bindings.resize((size_t)RigRole::Count); }
    int BoneFor(RigRole role) const;
    int IkBoneFor(RigRole role) const;     // index of the model IK bone that drives `role`, -1 if none
    void Clear();
    // Controllers: the single source of truth for what exists. When `controllers`
    // is empty (legacy rig files), callers fall back to the fixed role arrays.
    const RigController* ControllerAt(int i) const {
        return (i >= 0 && i < (int)controllers.size()) ? &controllers[(size_t)i] : nullptr;
    }
    bool HasControllers() const { return !controllers.empty(); }
};

// Diagnostics from generation: warnings plus what got resolved.
struct RigDiagnostics {
    std::vector<std::string> warnings;
};

} // namespace mmdx::studio
