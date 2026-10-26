#pragma once
// Studio scene lights: every light of a studio project is an object (sun, point, spot or ambient) with base values
// and, except for the ambient light, its own keyframes. Pure data (no renderer / imgui / App includes): StudioDoc holds
// the list, projects persist it (StudioProject.cpp) and app/Lighting.cpp resolves it into LightParams with
// BuildSceneLighting. A preset is a list of such objects (PresetLights): applying one replaces the whole list.
//   - Sun: the directional key light (at most one per scene). While `vmdLink` is on and the camera VMD has light keys,
//     that light track drives the sun's colour and direction.
//   - Ambient: the environment (sky, ground, hemisphere strength; at most one per scene). Not keyable.
//   - Point / Spot: punctual lights. A spot aims by hand, at a character, or sways like the concert preset's beams.
//   - FogVolume (S9): a local participating-medium volume (box or sphere): its `intensity` adds to the global haze density
//     inside it and its `color` tints that part of the medium. It never lights a surface and has no shadow.
//
// SceneLightToJson / SceneLightFromJson (asset/SceneLightJson.h) are the ONE implementation of the "lights" file
// schema: a project file's editor.lights array and an MMDX stage's stage.json "lights" array both use them
// (docs/stage_format.md), so a light block can move between the two unchanged. Keys missing from the JSON keep the
// SceneLight defaults.
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace mmdx::studio {

enum class LightKind : uint8_t { Sun = 0, Point = 1, Spot = 2, Ambient = 3, Area = 4, FogVolume = 5 };
// S9: the shape of a FogVolume light's local medium (project-file names "box" / "sphere").
enum class FogShape : uint8_t { Box = 0, Sphere = 1 };
enum class AimMode : uint8_t { Manual = 0, Target = 1, Sway = 2 };
enum class TargetPart : uint8_t { Centre = 0, Head = 1 };
enum class ShadowType : uint8_t { NoCast = 0, Hard = 1, Soft = 2 };
enum class FalloffType : uint8_t { None = 0, Linear = 1, InverseSquare = 2 };

// Project-file names: "sun" "point" "spot" "ambient" / "manual" "target" "sway" / "centre" "head" /
// "nocast" "hard" "soft" / "none" "linear" "inverse_square". Parse* returns false for anything else (out untouched).
const char* LightKindName(LightKind k);
bool ParseLightKind(const std::string& s, LightKind& out);
const char* AimModeName(AimMode m);
bool ParseAimMode(const std::string& s, AimMode& out);
const char* TargetPartName(TargetPart p);
bool ParseTargetPart(const std::string& s, TargetPart& out);
const char* ShadowTypeName(ShadowType t);
bool ParseShadowType(const std::string& s, ShadowType& out);
const char* FalloffTypeName(FalloffType f);
bool ParseFalloffType(const std::string& s, FalloffType& out);
// "box" / "sphere".
const char* FogShapeName(FogShape s);
bool ParseFogShape(const std::string& s, FogShape& out);

// The keyable values. The base values of a light and every key hold all of them; the ones a kind does not use stay at
// their defaults.
struct LightValues {
    DirectX::XMFLOAT3 position{0, 45, -15};          // point, spot, area: world position (MMD space)
    DirectX::XMFLOAT3 aim{0, 0, 0};                  // spot, area: aim point (Manual) or sway centre (Sway)
    DirectX::XMFLOAT3 direction{-0.5f, -1.0f, 0.5f}; // sun: travel direction toward the scene
    DirectX::XMFLOAT3 color{1, 1, 1};                // linear RGB
    float intensity = 1.0f;                          // sun: sunIntensity; point / spot / area: intensity; ambient: hemisphere strength
    float range = 140.0f;                            // point, spot, area: falloff distance
    float coneOuter = 0.24f;                         // spot: outer half-angle, radians
    float coneInner = 0.15f;                         // spot: inner half-angle, radians, <= coneOuter
    DirectX::XMFLOAT2 size{20, 20};                  // area: width, height (MMD units)
    DirectX::XMFLOAT3 extent{20, 10, 20};            // fog volume: half size (box) / radii (sphere), MMD units
    float volumetric = 1.0f;                         // S9: how much of the light shows in the haze (0 = none, 5..10 = bold beam)
    bool operator==(const LightValues& o) const;     // exact, every field
};

struct LightKey {
    int frame = 0;
    LightValues v;
    bool operator==(const LightKey& o) const;
};

struct SceneLight {
    uint32_t uid = 0;                 // from StudioDoc::nextLightUid (separate from the model uids); the identity of the light
    std::string name;                 // UTF-8, saved data
    LightKind kind = LightKind::Point;
    bool enabled = true;
    LightValues v;                    // base values, used where no key holds
    std::vector<LightKey> keys;       // sorted by frame, one key per frame; ignored for Ambient

    // Sun
    bool vmdLink = true;              // colour and direction follow the camera VMD light track while it has keys
    float rimStrength = 0.35f;
    DirectX::XMFLOAT3 rimColor{1.0f, 0.97f, 0.92f};

    // Spot
    AimMode aimMode = AimMode::Manual;
    uint32_t targetUid = 0;           // Target: StudioModel::uid of the character, 0 = the performer (first character).
                                      // In a project file (ProjectEditor::lights) it is 1 + the index into the project's
                                      // models instead, since model uids are not saved (App converts on save / load).
    TargetPart targetPart = TargetPart::Centre;
    float swayPhase = 0.0f;           // Sway: phase of the sway (per spot)

    // S9: per-light haze behaviour. volumetricG is the Henyey-Greenstein anisotropy of this light's in-scattering in
    // -0.9..0.95; -1 = use the fog's own (RenderSettings::volumetricAnisotropy).
    float volumetricG = -1.0f;

    // S9 FogVolume: the shape of the local medium and the soft falloff band at its border (0..1 of the half size).
    FogShape fogShape = FogShape::Box;
    float fogEdge = 0.35f;

    // S7b: a spot's gobo (a stencil projected through the cone, docs/stage_format.md "lights"). `gobo` is the texture
    // file: a stage light's path is relative to the stage folder (the stage runtime resolves it to an absolute path in
    // the rig it hands the renderer); a studio light's is absolute (what the file picker gives). Empty = no gobo.
    // These are base values like the shadow properties: a stage binding (`lights.<name>.gobo.rotation` ...) drives them
    // and the inspector shows them read-only while a stage owns the field.
    std::string gobo;
    float goboRotation = 0.0f;        // degrees, in the cone's own frame
    float goboScale = 1.0f;           // uv scale (1 = the texture fills the cone's inscribed square)
    float goboSpin = 0.0f;            // degrees per second (added to the rotation with the song time)
    float goboFocus = 1.0f;           // 0..1: 1 = sharp, below = blurred (a higher mip)

    // Ambient: the environment. The hemisphere strength is v.intensity.
    DirectX::XMFLOAT3 skyZenith{0.32f, 0.55f, 0.85f};
    DirectX::XMFLOAT3 skyHorizon{0.78f, 0.86f, 0.92f};
    DirectX::XMFLOAT3 groundColor{0.42f, 0.44f, 0.47f};

    // Common properties of every light. Base values only: they are not keyable and SampleLightValues ignores them.
    ShadowType shadow = ShadowType::Hard;
    float shadowSoftness = 0.5f;      // 0..1, meaningful for Soft
    float shadowDensity = 1.0f;       // 0..1, 1 = full shadow
    DirectX::XMFLOAT3 shadowColor{0, 0, 0};  // linear RGB, black = an ordinary shadow
    FalloffType falloff = FalloffType::None; // point and spot only
    bool affectDiffuse = true;
    bool affectSpecular = true;
    bool viewportVisible = true;      // editor only: the gizmo and icon in the viewport; no effect on the render

    // S3 (project files only; a stage.json light leaves both 0 / empty): the MMDX stage the light came from and the
    // manifest light name its `lights.<name>.<field>` targets address. The studio replays that stage's light targets
    // onto the fields a binding drives (docs/stage_format.md "Studio"); a stage.json light is identified by its order.
    uint32_t stageUid = 0;
    std::string stageLightName;

    bool operator==(const SceneLight& o) const;   // every field, exact
    size_t ApproxBytes() const { return sizeof(SceneLight) + name.size() + keys.capacity() * sizeof(LightKey); }
};

// No keys (and always for Ambient): the base values. Before the first key or after the last: that key. Between two
// keys: linear on every field (the sun's direction too, component-wise, as MMD does).
LightValues SampleLightValues(const SceneLight& light, int frame);

// The preset's lights. presetIndex: 0 Studio, 1 Sunset, 2 Concert, 3 Night (the order of LightingPreset in
// app/Lighting.h; clamped). Order: sun, ambient, then (Concert only) six sway spots and the front fill. All enabled.
// `focus` is the performer's centre ({0, 10, 0} without a performer). Each light takes its uid from `nextLightUid`,
// which is incremented. The values equal what BuildLighting produces for the preset (tools/studio_light_test.cpp).
std::vector<SceneLight> PresetLights(int presetIndex, DirectX::XMFLOAT3 focus, uint32_t& nextLightUid);

} // namespace mmdx::studio
