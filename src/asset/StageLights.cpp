#include "asset/StageLights.h"

#include <algorithm>
#include <cmath>

namespace mmdx {

namespace {

// A name the rig does not use yet: the glTF node name, else the kind, with " 2", " 3", ... appended on collision.
std::string UniqueName(const std::string& base, std::vector<std::string>& taken) {
    const std::string stem = base.empty() ? std::string("glTF 조명") : base;
    std::string name = stem;
    for (int n = 2; std::find(taken.begin(), taken.end(), name) != taken.end(); ++n)
        name = stem + " " + std::to_string(n);
    taken.push_back(name);
    return name;
}

} // namespace

studio::SceneLight ImportedLightToSceneLight(const ImportedLight& light) {
    studio::SceneLight out;
    out.name = light.name;
    out.enabled = true;
    out.v.color = light.color;
    out.v.intensity = std::max(0.0f, light.intensity);
    switch (light.kind) {
    case ImportedLight::Kind::Directional:
        out.kind = studio::LightKind::Sun;
        out.v.direction = light.direction;
        out.vmdLink = false;   // a stage's own sun is not driven by the camera VMD's light track
        break;
    case ImportedLight::Kind::Spot:
        out.kind = studio::LightKind::Spot;
        out.v.position = light.position;
        // The engine aims a spot at a point: put it far enough along the glTF direction that the angle is exact.
        out.v.aim = {light.position.x + light.direction.x * 100.0f, light.position.y + light.direction.y * 100.0f,
                     light.position.z + light.direction.z * 100.0f};
        out.v.range = light.range > 0.0f ? light.range : 140.0f;
        out.v.coneInner = std::clamp(light.innerCone, 0.01f, 1.5f);
        out.v.coneOuter = std::clamp(std::max(light.outerCone, out.v.coneInner), 0.02f, 1.55f);
        out.aimMode = studio::AimMode::Manual;
        out.shadow = studio::ShadowType::Soft;
        break;
    case ImportedLight::Kind::Point:
    default:
        out.kind = studio::LightKind::Point;
        out.v.position = light.position;
        out.v.range = light.range > 0.0f ? light.range : 140.0f;
        out.shadow = studio::ShadowType::Soft;
        break;
    }
    return out;
}

std::vector<studio::SceneLight> PartGltfLights(const StageManifest& manifest, const PmxModel& part,
                                               std::vector<std::string>& taken) {
    std::vector<studio::SceneLight> out;
    if (!manifest.gltfLights || part.importedLights.empty()) return out;
    out.reserve(part.importedLights.size());
    for (const ImportedLight& light : part.importedLights) {
        studio::SceneLight sceneLight = ImportedLightToSceneLight(light);
        sceneLight.name = UniqueName(light.name, taken);
        out.push_back(std::move(sceneLight));
    }
    return out;
}

} // namespace mmdx
