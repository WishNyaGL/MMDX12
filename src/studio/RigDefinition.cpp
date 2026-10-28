#include "studio/RigDefinition.h"

namespace mmdx::studio {

const char* RigRoleName(RigRole role) {
    switch (role) {
        case RigRole::Pelvis: return "pelvis";
        case RigRole::Groove: return "groove";
        case RigRole::Spine: return "spine";
        case RigRole::Chest: return "chest";
        case RigRole::Neck: return "neck";
        case RigRole::Head: return "head";
        case RigRole::LeftShoulder: return "shoulder_l";
        case RigRole::LeftUpperArm: return "upper_arm_l";
        case RigRole::LeftForearm: return "forearm_l";
        case RigRole::LeftHand: return "hand_l";
        case RigRole::RightShoulder: return "shoulder_r";
        case RigRole::RightUpperArm: return "upper_arm_r";
        case RigRole::RightForearm: return "forearm_r";
        case RigRole::RightHand: return "hand_r";
        case RigRole::LeftThigh: return "thigh_l";
        case RigRole::LeftShin: return "shin_l";
        case RigRole::LeftFoot: return "foot_l";
        case RigRole::LeftToe: return "toe_l";
        case RigRole::RightThigh: return "thigh_r";
        case RigRole::RightShin: return "shin_r";
        case RigRole::RightFoot: return "foot_r";
        case RigRole::RightToe: return "toe_r";
        default: return "?";
    }
}

bool RigRoleIsLeft(RigRole role) {
    return role == RigRole::LeftShoulder || role == RigRole::LeftUpperArm || role == RigRole::LeftForearm ||
           role == RigRole::LeftHand || role == RigRole::LeftThigh || role == RigRole::LeftShin ||
           role == RigRole::LeftFoot || role == RigRole::LeftToe;
}
bool RigRoleIsRight(RigRole role) {
    return role == RigRole::RightShoulder || role == RigRole::RightUpperArm || role == RigRole::RightForearm ||
           role == RigRole::RightHand || role == RigRole::RightThigh || role == RigRole::RightShin ||
           role == RigRole::RightFoot || role == RigRole::RightToe;
}

int RigDefinition::BoneFor(RigRole role) const {
    const int i = (int)role;
    if (i < 0 || i >= (int)bindings.size()) return -1;
    return bindings[(size_t)i].boneIndex;
}

int RigDefinition::IkBoneFor(RigRole role) const {
    for (const RigIkBone& b : ikBones)
        if (b.targetRole == role) return b.boneIndex;
    return -1;
}

void RigDefinition::Clear() {
    for (RigBoneBinding& b : bindings) b = RigBoneBinding{};
    ikBones.clear();
    sourceModelName.clear();
}

} // namespace mmdx::studio
