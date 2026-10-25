#pragma once
// S2a glTF lights for MMDX stages: a model's KHR_lights_punctual lights (PmxModel::importedLights) become ordinary
// scene lights of the stage rig when its stage.json says "gltfLights": true (docs/stage_format.md).
//
// Unit conversion (asset/ModelImport.cpp ConvertLights, documented in docs/stage_format.md):
//   point / spot  engine intensity = candela * 4pi / 683 * 0.0005   (a Blender 1000 W light, 54351 cd, reads 0.5)
//   directional   engine intensity = lux * 1 / 683 * 0.5            (a 683 lux sun reads 0.5)
// Positions / ranges are MMD world units (the importer already scaled them with the geometry), directions are unit
// vectors in MMD space, spot cones are radians.
#include "asset/PmxModel.h"
#include "asset/StageManifest.h"
#include "studio/SceneLight.h"

#include <string>
#include <vector>

namespace mmdx {

// One imported light as a scene light of the rig: Point / Spot keep the position, a directional light becomes the
// sun (its direction is the travel direction). `uid` is 0; the caller assigns identities.
studio::SceneLight ImportedLightToSceneLight(const ImportedLight& light);

// S9: one `environment.fogVolumes` entry as a LightKind::FogVolume scene light of the rig (uid 0: the caller assigns
// it; the name is the entry's `id`, else "안개 <index + 1>"). Never keyed: the values of the stage are static.
studio::SceneLight StageFogVolumeLight(const StageFogVolume& volume, size_t index);

// The glTF lights of one loaded part, in the manifest's rig order (empty unless `manifest.gltfLights`). Names follow the
// glTF node, made unique against `taken` (and against each other) with a numeric suffix; `taken` is extended.
std::vector<studio::SceneLight> PartGltfLights(const StageManifest& manifest, const PmxModel& part,
                                               std::vector<std::string>& taken);

} // namespace mmdx
