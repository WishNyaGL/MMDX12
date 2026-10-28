#pragma once
// Studio-only "arm rest-pose correction": some VMD motions were authored for a model whose arms rest horizontally
// (T-pose), so on an A-pose model they drop the arms ~38 deg too far. This rewrites the motion's arm keys so the
// motion looks on the A-pose model as it looked on the T-pose one. Pure data + math (no UI); the undoable apply
// lives in the App (UiStudioRestPose.cpp); unit-tested by tools/restpose_retarget_test.cpp.
#include "asset/PmxModel.h"
#include "studio/StudioMotion.h"
#include <DirectXMath.h>

namespace mmdx::studio {

struct ArmRestGuess {
    bool likely = false;   // the motion looks T-pose-authored
    float medianDeg = 0;   // median arm angle (see below), degrees
    int samples = 0;       // number of 腕 keys measured (both sides)
};

// Whether `motion` looks authored for a T-pose rig: the median angle of its 腕 keys (0 = horizontal outward,
// 90 = straight down, > 90 = inside the body) at or past the calibrated threshold.
ArmRestGuess GuessTPoseMotion(const MotionData& motion);

struct ArmRetargetOptions {
    float sourceArmDeg = 0.0f;   // the arm rest angle the motion was authored for (0 = T-pose), below horizontal
    bool side[2] = {true, true}; // [0] left (左), [1] right (右)
};
struct ArmRetargetResult {
    bool applied = false;
    float rotationDeg[2] = {0, 0}; // the correction angle used per side (0 = side skipped)
    int bonesChanged = 0;          // tracks touched
    int keysChanged = 0;           // keys rewritten or inserted
};

// Rewrites the arm keys of `motion` (腕 / ひじ and their subtrees: 捩 helpers, wrists, fingers) so a motion authored
// for a rig whose arms rest `opt.sourceArmDeg` below horizontal looks the same on `model` (already A-pose).
ArmRetargetResult RetargetArmRest(const PmxModel& model, MotionData& motion, const ArmRetargetOptions& opt);

} // namespace mmdx::studio
