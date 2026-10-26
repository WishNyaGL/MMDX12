#pragma once
// S9b-1: the Physical Sky light (studio::LightKind::Sky): Preetham 1999's clear-sky model (Perez distribution in
// Yxy) evaluated on the CPU into the constants the shaders read (shaders/physical_sky.hlsli). One implementation, two
// users: app/Lighting.cpp resolves a Sky light into LightParams (and its sun / ambient colours) and
// Renderer::FillSceneConstants copies the state into SceneConstants.
//
// The state is self-contained: the same numbers drive the shader branch of SkyColor and PhysicalSkyRadiance below (the
// CPU twin, used to write LightParams::skyZenith / skyHorizon / groundColor so the hemisphere fill stays coherent).
#include <DirectXMath.h>

namespace mmdx {

// Where the sky light puts its sun: the time of day (hours 0..24, sunrise 6:00, sunset 18:00) and the compass
// rotation of the arc, or a sun light's own direction when the scene has one (then the time is ignored).
struct PhysicalSkyInput {
    float timeOfDay = 12.0f;
    float northDeg = 0.0f;
    float maxElevationDeg = 60.0f;
    float turbidity = 3.0f;
    float haze = 0.2f;
    float intensity = 1.0f;       // scales the sky's radiance
    float sunIntensity = 1.0f;    // scales the sun's colour
    DirectX::XMFLOAT3 ground{0.35f, 0.35f, 0.35f};  // the ground's albedo (linear): below the horizon
    bool useSunDir = false;       // true = a sun light owns the direction (sunDirToScene), the time is ignored
    DirectX::XMFLOAT3 sunDirToScene{-0.5f, -1.0f, 0.5f};  // LightParams::direction
};

// The model's constants (the SceneConstants sky fields, see ShaderInterop.h).
struct PhysicalSkyState {
    // params: x = 1 (a physical sky this frame), y = the radiance scale (0.04 x the light's intensity),
    // z = horizon haze 0..1, w = the day factor (scales the model's sky: 1 with the sun up, 0 with it below the
    // horizon, where only the night floor's rgb is left so the scene stays readable).
    DirectX::XMFLOAT4 params{0, 0, 0, 0};
    DirectX::XMFLOAT4 perezY{0, 0, 0, 0};      // A, B, C, D of Y
    DirectX::XMFLOAT4 perezX{0, 0, 0, 0};      // A, B, C, D of x
    DirectX::XMFLOAT4 perezYy{0, 0, 0, 0};     // A, B, C, D of y
    DirectX::XMFLOAT4 perezE{0, 0, 0, 0};      // E of Y, x, y
    DirectX::XMFLOAT4 zenithOverF{0, 0, 0, 0}; // zenith Y / F(0, sun), and the same quotients of x and y
    DirectX::XMFLOAT4 sunToward{0, 0, 1, 0};   // the unit vector toward the sun
    DirectX::XMFLOAT4 night{0, 0, 0, 0};       // the night floor (linear rgb), added under the faded sky
    DirectX::XMFLOAT3 sunColor{0, 0, 0};       // linear sun colour (already x sunIntensity; 0 below the horizon)
    DirectX::XMFLOAT3 skyZenith{0, 0, 0};      // sampled at the zenith (linear, the ambient's skyZenith)
    DirectX::XMFLOAT3 skyHorizon{0, 0, 0};     // averaged over the horizon (the ambient's skyHorizon)
    DirectX::XMFLOAT3 groundColor{0, 0, 0};    // the ground albedo x the sky's irradiance (the ambient's groundColor)
    DirectX::XMFLOAT3 sunDirToScene{0, -1, 0}; // the travel direction of the sun toward the scene
};

// Fills st from in. Pure: the same in gives the same st.
void EvaluatePhysicalSky(const PhysicalSkyInput& in, PhysicalSkyState& st);

// The sky's linear radiance in a world direction (up is +y): the CPU twin of the shader's physical_sky.hlsli branch,
// including the horizon haze, the below-horizon ground colour and the night floor.
DirectX::XMFLOAT3 PhysicalSkyRadiance(const PhysicalSkyState& st, DirectX::XMFLOAT3 dir);

} // namespace mmdx
