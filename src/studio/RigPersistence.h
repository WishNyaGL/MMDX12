#pragma once
// Rig persistence: a RigDefinition is stored as its own UTF-8 JSON file (".mmdxrig") next to the project, and the
// project references it by relative path. Storing bone names (not just indices) lets a rig be re-validated against a
// PMX after model replacement / version changes.
#include "studio/RigDefinition.h"

#include <filesystem>
#include <string>

namespace mmdx {
struct PmxModel;
}

namespace mmdx::studio {

inline constexpr const wchar_t* kRigExtension = L".mmdxrig";
inline constexpr int kRigFormatVersion = 1;

// Writes `rig` to `file` (JSON, atomic). Returns false with *error on failure.
bool SaveRig(const std::filesystem::path& file, const RigDefinition& rig, std::string* error);

// Reads `file` into `out`. Fails (false, *error) when missing, not JSON, wrong format name, or a newer version.
bool LoadRig(const std::filesystem::path& file, RigDefinition& out, std::string* error);

// Re-validates a loaded rig against `model`: each binding is re-resolved by stored bone name (index refreshed), so a
// model swap / bone reorder keeps the rig correct; bones that no longer exist are cleared.
void RebindRig(RigDefinition& rig, const PmxModel& model);

} // namespace mmdx::studio
