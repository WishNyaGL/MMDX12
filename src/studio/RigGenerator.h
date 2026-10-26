#pragma once
// Skeleton analysis: maps PMX bones to rig roles using a Japanese/English/common name dictionary plus structural
// heuristics. Produces a RigDefinition + diagnostics. Does not modify the PMX or any animation data.
#include "studio/RigDefinition.h"

namespace mmdx {
struct PmxModel;
}

namespace mmdx::studio {

struct RigGenerateResult {
    RigDefinition rig;
    RigDiagnostics diag;
};

// Analyze `model` and build a base rig. `model` is read-only.
RigGenerateResult GenerateRig(const PmxModel& model);

} // namespace mmdx::studio
