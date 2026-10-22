#pragma once
// MocapSolver: retargets video motion capture landmarks (MocapClip) to an MMD PMX model,
// generating a VMD motion with bone keys, morph keys, and IK enable keys.
//
// Math & Architecture:
// 1. Direction-Based Retargeting:
//    - Calibration: For any PMX model (regardless of rest pose or proportions), bone segment
//      vectors are computed from model rest positions: v_rest = child.position - bone.position.
//    - For each driven bone in hierarchy order (parent solved before child):
//      * Compute target world segment direction v_target = child_lm - bone_lm.
//      * Compute a rotation aligning v_rest to v_target.
//      * Resolve roll/twist using a secondary reference normal (e.g. elbow hinge plane for arms,
//        knee plane for legs, shoulder span for torso, palm plane for wrist).
//      * Convert world rotation R_world to bone local rotation R_local = R_world * inv(R_parent_world).
// 2. Torso & Head:
//    - 下半身 (lower body): oriented from hip span and hip-to-spine vector.
//    - 上半身 / 上半身2 (upper body): oriented from shoulder span and spine vector, sharing bend.
//    - 首 / 頭 (neck / head): oriented from face landmarks when available, else pose head points.
// 3. Limbs:
//    - Arms: 腕 (upper arm), ひじ (elbow), 手首 (wrist).
//    - Legs: 左足/右足 (upper leg), 左ひざ/右ひざ (knee), 左足首/右足首 (ankle).
//    - IK: Frame 0 emits VmdIkKey disabling foot IK (左足ＩＫ, 右足ＩＫ, toe IK) so FK controls legs.
// 4. Fingers:
//    - Hand landmarks provide flexion angles between segments. Rotations applied about bone rest curl axes,
//      clamped to anatomical limits (-10 deg to 100 deg).
// 5. Root Motion:
//    - センター translation: X/Z tracked from hip landmark motion relative to initial frame,
//      scaled to MMD units.
//    - Y (vertical): ground contact constraint ensuring lowest foot point stays >= 0 above floor.
// 6. Face Morphs & Eye Gaze:
//    - MediaPipe 52 ARKit blendshapes mapped to PMX morphs (あ, い, う, え, お, まばたき, ウィンク, etc.).
//    - Eye gaze mapped to 両目 bone rotation (clamped to +/-15 deg).
// 7. Smoothing & Key Reduction:
//    - One-Euro filtering for zero-lag jitter removal; quaternion sign continuity.
//    - Keyframe reduction drops keys that linear interpolation reproduces within tolerances.

#include "asset/PmxModel.h"
#include "asset/VmdMotion.h"
#include "mocap/MocapTypes.h"
#include <string>
#include <vector>

namespace mmdx::mocap {

struct SolveOptions {
    bool body = true, fingers = true, face = true, head = true;
    bool rootMotion = true;             // センター translation (horizontal from the image, vertical from foot contact)
    float smoothing = 0.5f;             // 0 = raw, 1 = heavy (maps to One-Euro filter cutoffs)
    float rotationTolerance = 0.5f;     // degrees, keyframe reduction
    float translationTolerance = 0.05f; // MMD units
    bool mirrorBlendshapeSides = false; // swap Left/Right blendshapes (MediaPipe side convention, verified later)
};

struct SolveReport {
    int frames = 0;
    int boneKeys = 0;
    int morphKeys = 0;
    std::vector<std::string> missingBones;
    std::vector<std::string> missingMorphs;
};

bool SolveMotion(const MocapClip& clip, const PmxModel& model, const SolveOptions& opts, VmdMotion& out,
                 SolveReport* report = nullptr, std::string* error = nullptr);

} // namespace mmdx::mocap
