#pragma once
// The "lights" JSON of a studio project's editor object, as reusable functions: one entry is one SceneLight.
// StudioProject.cpp writes / reads its "lights" array with these (a project round trip stays byte-identical) and an
// MMDX stage's stage.json uses the same schema for its named light rig (asset/StageManifest.h, docs/stage_format.md),
// so one block can move between a project and a stage unchanged.
//
// An entry: uid, kind, name, enabled, values {}, sun {}, spot {}, sky {}, shadow {}, falloff, affectDiffuse,
// affectSpecular, viewportVisible, keys [].
#include "studio/SceneLight.h"
#include <json.hpp>
#include <string>
#include <vector>

namespace mmdx::studio {

// Every field, in a fixed key order.
nlohmann::json SceneLightToJson(const SceneLight& light);

// Missing keys keep the SceneLight defaults (a key without some values holds the light's base values). False, with a
// warning in `warnings`, for an unknown "kind": `light` is then untouched.
bool SceneLightFromJson(const nlohmann::json& j, SceneLight& light, std::vector<std::string>* warnings = nullptr);

} // namespace mmdx::studio
