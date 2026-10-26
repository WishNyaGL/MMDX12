#include "anim/Motion.h"
#include "core/I18n.h"
#include "app/Lighting.h"
#include "render/PhysicalSky.h"
#include "render/Renderer.h"
#include <algorithm>
#include <cmath>

namespace mmdx {

namespace {

DirectX::XMFLOAT3 Srgb(float r, float g, float b) {
    auto f = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    return {f(r), f(g), f(b)};
}

// linear -> sRGB, the inverse of Srgb above (LightParams::color is the MMD light colour, gamma space).
DirectX::XMFLOAT3 SrgbOut(const DirectX::XMFLOAT3& c) {
    auto f = [](float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(std::max(v, 0.0f), 1.0f / 2.4f) - 0.055f; };
    return {f(c.x), f(c.y), f(c.z)};
}

} // namespace

const char* LightingPresetName(LightingPreset p) {
    switch (p) {
    case LightingPreset::Studio: return Tr("스튜디오");
    case LightingPreset::Sunset: return Tr("노을");
    case LightingPreset::Concert: return Tr("콘서트");
    case LightingPreset::Night: return Tr("밤");
    case LightingPreset::Stage: return Tr("스테이지 조명");
    }
    return "";
}

namespace {

// The concert spots' animated sway (shared with the scene lights' Sway aim mode): a truss above the
// stage, the beams sweep with the song time. `phase` staggers the spots. The offset from the sway centre.
void SwayOffset(double t, float phase, float& dx, float& dz) {
    const float sway = (float)std::sin(t * 0.9 + phase) * 16.0f;
    const float swayZ = (float)std::cos(t * 0.7 + phase * 0.6) * 10.0f;
    dx = sway * 0.8f;
    dz = swayZ;
}

DirectX::XMFLOAT3 SwayTarget(double t, float phase, const DirectX::XMFLOAT3& focus) {
    float dx, dz;
    SwayOffset(t, phase, dx, dz);
    return {focus.x + dx, 0.0f, focus.z + dz};
}

} // namespace

void BuildLighting(LightingPreset preset, double t, const DirectX::XMFLOAT3& focus, LightParams& out) {
    out = LightParams{};
    switch (preset) {
    case LightingPreset::Studio:
        out.direction = {-0.45f, -1.0f, 0.62f};
        out.color = {0.6f, 0.6f, 0.6f};
        out.sunIntensity = 1.0f;
        out.skyZenith = Srgb(0.62f, 0.78f, 0.93f);
        out.skyHorizon = Srgb(0.90f, 0.94f, 0.97f);
        out.groundColor = Srgb(0.70f, 0.72f, 0.75f);
        out.hemiStrength = 0.16f;
        out.rimStrength = 0.32f;
        out.rimColor = {1.0f, 0.98f, 0.95f};
        break;
    case LightingPreset::Sunset:
        out.direction = {0.85f, -0.38f, 0.42f};
        out.color = {0.68f, 0.56f, 0.46f};
        out.sunIntensity = 1.0f;
        out.skyZenith = Srgb(0.38f, 0.42f, 0.66f);
        out.skyHorizon = Srgb(0.98f, 0.70f, 0.52f);
        out.groundColor = Srgb(0.42f, 0.34f, 0.36f);
        out.hemiStrength = 0.20f;
        out.rimStrength = 0.7f;
        out.rimColor = Srgb(1.0f, 0.78f, 0.55f);
        break;
    case LightingPreset::Night:
        out.direction = {0.3f, -0.85f, 0.55f};
        out.color = {0.44f, 0.48f, 0.56f};
        out.sunIntensity = 0.85f;
        out.skyZenith = Srgb(0.05f, 0.07f, 0.16f);
        out.skyHorizon = Srgb(0.16f, 0.21f, 0.36f);
        out.groundColor = Srgb(0.08f, 0.09f, 0.12f);
        out.hemiStrength = 0.22f;
        out.rimStrength = 0.55f;
        out.rimColor = Srgb(0.62f, 0.78f, 1.0f);
        break;
    case LightingPreset::Concert: {
        out.direction = {-0.2f, -1.0f, 0.35f};
        out.color = {0.46f, 0.47f, 0.52f};
        out.sunIntensity = 0.8f;
        out.skyZenith = Srgb(0.03f, 0.03f, 0.08f);
        out.skyHorizon = Srgb(0.10f, 0.08f, 0.20f);
        out.groundColor = Srgb(0.05f, 0.05f, 0.08f);
        out.hemiStrength = 0.12f;
        out.rimStrength = 0.45f;
        out.rimColor = Srgb(0.55f, 0.95f, 0.92f);
        // Six moving spots from a truss above the stage, Miku teal / magenta / white.
        const DirectX::XMFLOAT3 colors[3] = {Srgb(0.22f, 0.77f, 0.73f), Srgb(0.95f, 0.35f, 0.62f), Srgb(1.0f, 0.95f, 0.88f)};
        for (int i = 0; i < 6; ++i) {
            const float side = (i % 2 == 0) ? -1.0f : 1.0f;
            const float px = side * (14.0f + 9.0f * (float)(i / 2));
            const float phase = (float)i * 1.3f;
            PunctualLight s;
            s.position = {focus.x + px, 58.0f, focus.z - 18.0f + 6.0f * (float)(i / 2)};
            DirectX::XMFLOAT3 target = SwayTarget(t, phase, focus);
            target.x -= px * 0.25f;
            s.direction = {target.x - s.position.x, target.y - s.position.y, target.z - s.position.z};
            s.range = 140.0f;
            s.color = colors[i % 3];
            s.intensity = 2.6f;
            s.spotCosOuter = std::cos(0.24f);
            s.spotCosInner = std::cos(0.15f);
            s.shadow = LightShadowType::Soft;   // stage spots: soft-edged shadows (default softness)
            out.punctual.push_back(s);
        }
        // a warm front fill so faces never go dark between spot passes
        PunctualLight fill;
        fill.position = {focus.x, focus.y + 22.0f, focus.z - 40.0f};
        fill.range = 120.0f;
        fill.color = Srgb(1.0f, 0.92f, 0.86f);
        fill.intensity = 0.55f;
        out.punctual.push_back(fill);
        break;
    }
    case LightingPreset::Stage:
        // The loaded MMDX stage's own rig (BuildSceneLighting with the stage's lights) is what the caller uses; this
        // fallback is the Studio preset so a stage without lights never renders black.
        BuildLighting(LightingPreset::Studio, t, focus, out);
        break;
    }
}

void BuildSceneLighting(const std::vector<studio::SceneLight>& lights, const std::vector<studio::LightKf>& cameraLight,
                        double t, const LightAnchors& anchors, LightParams& out) {
    using studio::AimMode;
    using studio::LightKind;
    out = LightParams{};
    // the frame as a float like the VMD light sample's: a whole-frame time (frame / 30.0) must not floor to the frame before
    const float frameF = (float)(t * kMmdFps);
    const int frame = (int)std::floor(frameF);

    const studio::SceneLight* sun = nullptr;
    const studio::SceneLight* ambient = nullptr;
    const studio::SceneLight* sky = nullptr;
    for (const studio::SceneLight& l : lights) {
        if (!l.enabled) continue;
        if (l.kind == LightKind::Sun && !sun) sun = &l;
        else if (l.kind == LightKind::Ambient && !ambient) ambient = &l;
        else if (l.kind == LightKind::Sky && !sky) sky = &l;
    }

    if (sun) {
        const studio::LightValues v = studio::SampleLightValues(*sun, frame);
        out.color = v.color;
        out.direction = v.direction;
        out.sunIntensity = v.intensity;
        out.rimStrength = sun->rimStrength;
        out.rimColor = sun->rimColor;
        out.sunShadow = (LightShadowType)sun->shadow;
        out.sunShadowSoftness = sun->shadowSoftness;
        out.sunShadowDensity = sun->shadowDensity;
        out.sunShadowColor = sun->shadowColor;
        // S9: the sun's share of the haze and its own scattering anisotropy
        out.sunVolumetric = v.volumetric;
        out.sunVolumetricG = sun->volumetricG;
        // linked to the camera VMD: its light track decides the colour and direction while it has keys
        if (sun->vmdLink && !cameraLight.empty()) {
            const studio::LightKf k = studio::SampleLight(cameraLight, frameF);
            out.color = k.color;
            // a zero direction (broken file) keeps the sun's
            if (std::fabs(k.direction.x) + std::fabs(k.direction.y) + std::fabs(k.direction.z) > 1e-4f)
                out.direction = k.direction;
        }
    } else {
        // no sun: a black key light. sunIntensity stays 1 (it also scales the sky backdrop, the floor and the fog).
        out.color = {0.0f, 0.0f, 0.0f};
        out.sunIntensity = 1.0f;
        out.rimStrength = 0.0f;
        out.sunShadow = LightShadowType::NoCast;
        out.sunShadowSoftness = 0.5f;
        out.sunShadowDensity = 0.0f;
        out.sunShadowColor = {0.0f, 0.0f, 0.0f};
    }

    if (ambient) {
        out.skyZenith = ambient->skyZenith;
        out.skyHorizon = ambient->skyHorizon;
        out.groundColor = ambient->groundColor;
        out.hemiStrength = ambient->v.intensity;
    } else {
        out.hemiStrength = 0.0f;  // the default sky colours stay
    }

    // S9b-1: the Physical Sky light (at most one, the first enabled). Its model replaces the hand-set gradient in
    // every render path and owns the sun: the direction comes from the time of day unless the scene has a Sun light
    // (then that light's direction is used and only its colour is replaced). The model also writes the ambient
    // colours above, so the hemisphere fill, the floor and the haze follow the sky.
    if (sky) {
        const studio::LightValues v = studio::SampleLightValues(*sky, frame);
        PhysicalSkyInput in;
        in.timeOfDay = v.skyTimeOfDay;
        in.northDeg = v.skyNorthDeg;
        in.maxElevationDeg = v.skyMaxElevationDeg;
        in.turbidity = v.skyTurbidity;
        in.haze = v.skyHaze;
        in.intensity = v.skyIntensity;
        in.sunIntensity = v.skySunIntensity;
        in.ground = v.skyGround;
        in.useSunDir = sun != nullptr;
        in.sunDirToScene = out.direction;
        EvaluatePhysicalSky(in, out.sky);
        out.physicalSky = true;
        out.skyZenith = out.sky.skyZenith;
        out.skyHorizon = out.sky.skyHorizon;
        out.groundColor = out.sky.groundColor;
        // S9b-3: the sky's IMAGE (the renderer decodes it; FillSceneConstants writes the map's averages over these
        // ambient colours once the texture is up, so the hemisphere fill and the haze follow the image).
        out.skyMap = sky->skyMap;
        studio::SkyMapEncoding enc = studio::SkyMapEncoding::Auto;
        studio::ParseSkyMapEncoding(sky->skyMapEncoding, enc);
        out.skyMapEncoding = (int)enc;
        out.skyMapMirror = sky->skyMapMirror;
        out.skyMapRotationRad = DirectX::XMConvertToRadians(v.skyMapRotationDeg);
        out.skyMapIntensity = std::max(0.0f, v.skyMapIntensity);
        // S9b-4: the cloud layer. Only the procedural physical sky draws it: an image sky keeps coverage at 0, so the
        // sun dimming below follows the gate too. The wind offset is the layer's drift at the scene time (sin / cos of
        // the direction, so 0 degrees blows toward +z).
        const bool imageSky = !sky->skyMap.empty();
        out.cloudCoverage = imageSky ? 0.0f : std::clamp(v.cloudCoverage, 0.0f, 1.0f);
        out.cloudOpacity = std::clamp(v.cloudOpacity, 0.0f, 1.0f);
        out.cloudSoftness = std::clamp(v.cloudSoftness, 0.02f, 1.0f);
        out.cloudHeightKm = std::clamp(v.cloudHeightKm, 0.3f, 12.0f);
        out.cloudScaleKm = std::clamp(v.cloudScaleKm, 0.2f, 20.0f);
        const float windRad = DirectX::XMConvertToRadians(v.cloudWindDeg);
        const float windKm = std::clamp(v.cloudWindSpeed, 0.0f, 100.0f) * (float)t / 1000.0f;
        out.cloudWindOffset = {std::sin(windRad) * windKm, std::cos(windRad) * windKm};
        if (!sun) out.direction = out.sky.sunDirToScene;
        // The sun colour: LightParams::color is gamma space (the shaders SrgbToLinear it), so the model's linear colour
        // is encoded here. With no Sun light the sky light also owns the sun's magnitude: it stays under 0.6 (the
        // model's own limit) so it rides in the colour and sunIntensity stays 1, which keeps the sky itself unscaled.
        // A Sun light in the scene keeps its own intensity (it is the brightness control), so the colour is normalized
        // there and its magnitude dropped.
        const DirectX::XMFLOAT3 c = out.sky.sunColor;
        const float m = std::max(c.x, std::max(c.y, c.z));
        if (m > 1e-5f) {
            // no Sun light: the transmittance (c / 0.6, <= 1) is encoded, then scaled so a white noon sun is MMD's
            // default 0.6 in gamma space (encoding 0.6 linear itself would brighten it to ~0.8)
            out.color = sun ? SrgbOut({c.x / m, c.y / m, c.z / m})
                            : [&] {
                                  const DirectX::XMFLOAT3 e = SrgbOut({c.x / 0.6f, c.y / 0.6f, c.z / 0.6f});
                                  return DirectX::XMFLOAT3{e.x * 0.6f, e.y * 0.6f, e.z * 0.6f};
                              }();
            if (!sun) out.sunIntensity = 1.0f;
        } else {
            out.color = {0.0f, 0.0f, 0.0f};
            if (!sun) out.sunIntensity = 1.0f;
        }
        // S9b-4: clouds dim the sun on average (no cloud shadows on the ground). Coverage 0 leaves the colour untouched.
        const float cloudDim = 1.0f - 0.6f * out.cloudCoverage * out.cloudOpacity;
        out.color = {out.color.x * cloudDim, out.color.y * cloudDim, out.color.z * cloudDim};
        if (!sun) {
            // the sky's own sun casts like a default sun light; a dark sun (night, skySunIntensity 0 with an image
            // sky whose own sun lights the scene) casts nothing (a hard shadow with no light was a ghost shadow)
            out.rimStrength = 0.0f;
            const DirectX::XMFLOAT3& sc = out.sky.sunColor;
            out.sunShadow = std::max(sc.x, std::max(sc.y, sc.z)) > 1e-5f ? LightShadowType::Hard
                                                                        : LightShadowType::NoCast;
            out.sunShadowSoftness = 0.5f;
            out.sunShadowDensity = 1.0f;
            out.sunShadowColor = {0.0f, 0.0f, 0.0f};
        }
    }

    for (const studio::SceneLight& l : lights) {
        if (!l.enabled) continue;
        if (l.kind == studio::LightKind::FogVolume) {
            if (out.fogVolumes.size() >= Renderer::kMaxFogVolumes) continue;
            const studio::LightValues v = studio::SampleLightValues(l, frame);
            FogVolume f;
            f.position = v.position;
            f.extent = v.extent;
            f.color = v.color;
            f.density = v.intensity;
            f.edge = std::clamp(l.fogEdge, 0.0f, 1.0f);
            f.sphere = l.fogShape == studio::FogShape::Sphere;
            out.fogVolumes.push_back(f);
            continue;
        }
        if (l.kind != LightKind::Point && l.kind != LightKind::Spot && l.kind != LightKind::Area) continue;
        // A full punctual list skips further punctual lights but keeps looking: S9 fog volumes may follow them in the
        // list (a stage's rig appends them after its lights).
        if (out.punctual.size() >= Renderer::kMaxPunctualLights) continue;
        const studio::LightValues v = studio::SampleLightValues(l, frame);
        PunctualLight p;
        p.position = v.position;
        p.range = v.range;
        p.color = v.color;
        p.intensity = v.intensity;
        p.volumetric = v.volumetric;
        p.volumetricG = l.volumetricG;
        p.shadow = (LightShadowType)l.shadow;
        p.shadowSoftness = l.shadowSoftness;
        p.shadowDensity = l.shadowDensity;
        p.shadowColor = l.shadowColor;
        p.falloff = (LightFalloffType)l.falloff;
        p.affectDiffuse = l.affectDiffuse;
        p.affectSpecular = l.affectSpecular;
        if (l.kind == LightKind::Point) {
            p.castPointShadow = (l.shadow != studio::ShadowType::NoCast);
        } else if (l.kind == LightKind::Spot) {
            DirectX::XMFLOAT3 target = ResolveSpotAim(l, v, t, anchors);
            p.direction = {target.x - v.position.x, target.y - v.position.y, target.z - v.position.z};
            if (p.direction.x * p.direction.x + p.direction.y * p.direction.y + p.direction.z * p.direction.z < 1e-6f)
                p.direction = {0.0f, -1.0f, 0.0f};
            p.spotCosOuter = std::cos(std::clamp(v.coneOuter, 0.02f, 1.5f));
            p.spotCosInner = std::cos(std::clamp(v.coneInner, 0.01f, std::max(v.coneOuter, 0.01f)));
            // S7b: the gobo (only a spot has a cone to project it through)
            p.gobo = l.gobo;
            p.goboRotation = l.goboRotation;
            p.goboScale = l.goboScale;
            p.goboSpin = l.goboSpin;
            p.goboFocus = l.goboFocus;
        } else if (l.kind == LightKind::Area) {
            p.castPointShadow = (l.shadow != studio::ShadowType::NoCast);
            p.areaSize = v.size;
            p.direction = {v.aim.x - v.position.x, v.aim.y - v.position.y, v.aim.z - v.position.z};
            float lenSq = p.direction.x * p.direction.x + p.direction.y * p.direction.y + p.direction.z * p.direction.z;
            if (lenSq < 1e-6f) {
                p.direction = {0.0f, -1.0f, 0.0f};
            } else {
                float invLen = 1.0f / std::sqrt(lenSq);
                p.direction.x *= invLen;
                p.direction.y *= invLen;
                p.direction.z *= invLen;
            }
        }
        out.punctual.push_back(p);
    }
}

DirectX::XMFLOAT3 ResolveSpotAim(const studio::SceneLight& l, const studio::LightValues& v,
                                 double songSeconds, const LightAnchors& anchors) {
    using studio::AimMode;
    DirectX::XMFLOAT3 target = v.aim;
    if (l.aimMode == AimMode::Target) {
        // the character (0 or an unknown uid: the performer)
        const bool head = l.targetPart == studio::TargetPart::Head;
        target = head ? anchors.head : anchors.focus;
        if (l.targetUid != 0)
            for (const LightAnchors::Character& c : anchors.characters)
                if (c.uid == l.targetUid) {
                    target = head ? c.head : c.centre;
                    break;
                }
    } else if (l.aimMode == AimMode::Sway) {
        float dx, dz;
        SwayOffset(songSeconds, l.swayPhase, dx, dz);
        target = {v.aim.x + dx, v.aim.y, v.aim.z + dz};
    }
    return target;
}

} // namespace mmdx

