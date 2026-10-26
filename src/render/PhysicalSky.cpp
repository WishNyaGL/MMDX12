// S9b-1: the Physical Sky light's CPU model (Preetham 1999, the Perez distribution in Yxy). See PhysicalSky.h.
#include "render/PhysicalSky.h"

#include <algorithm>
#include <cmath>

namespace mmdx {

namespace {

constexpr float kPi = 3.14159265358979f;

// The Preetham zenith luminance is in kcd/m^2; this scale puts a noon (turbidity 3, 60 degree sun) zenith near the
// brightness of the built-in skyZenith (0.32, 0.55, 0.85 linear), i.e. the sky the app had without a Sky light.
constexpr float kRadianceScale = 0.04f;

float Radians(float d) { return d * kPi / 180.0f; }
float Degrees(float r) { return r * 180.0f / kPi; }
float Saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }
float SmoothStep(float a, float b, float x) {
    const float t = Saturate((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

DirectX::XMFLOAT3 Sub(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
DirectX::XMFLOAT3 Add(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
DirectX::XMFLOAT3 Mul(const DirectX::XMFLOAT3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(const DirectX::XMFLOAT3& a) { return std::sqrt(Dot(a, a)); }
DirectX::XMFLOAT3 Normalize(const DirectX::XMFLOAT3& a) {
    const float l = Length(a);
    return l > 1e-8f ? Mul(a, 1.0f / l) : DirectX::XMFLOAT3{0.0f, 1.0f, 0.0f};
}

// Perez's five-term angular distribution: (1 + A e^(B / cos theta)) (1 + C e^(D gamma) + E cos^2 gamma).
float Perez(float a, float b, float c, float d, float e, float cosTheta, float cosGamma) {
    return (1.0f + a * std::exp(b / std::max(cosTheta, 0.01f))) *
           (1.0f + c * std::exp(d * std::acos(std::clamp(cosGamma, -1.0f, 1.0f))) + e * cosGamma * cosGamma);
}

float PerezY(const DirectX::XMFLOAT4& p, float e, float cosTheta, float cosGamma) {
    return Perez(p.x, p.y, p.z, p.w, e, cosTheta, cosGamma);
}

// Yxy -> linear sRGB (the Preetham paper's XYZ -> RGB matrix).
DirectX::XMFLOAT3 YxyToLinearRgb(float y, float x, float yChroma) {
    const float yy = std::max(yChroma, 1e-4f);
    const float X = x / yy * y;
    const float Y = y;
    const float Z = (1.0f - x - yChroma) / yy * y;
    return {3.2406f * X - 1.5372f * Y - 0.4986f * Z, -0.9689f * X + 1.8758f * Y + 0.0415f * Z,
            0.0557f * X - 0.2040f * Y + 1.0570f * Z};
}

// The model's radiance for a direction, before the haze blend and the night floor (step 6 of the S9b-1 note).
DirectX::XMFLOAT3 ModelRadiance(const PhysicalSkyState& st, DirectX::XMFLOAT3 dir) {
    const DirectX::XMFLOAT3 s{st.sunToward.x, st.sunToward.y, st.sunToward.z};
    // 1e-4, not 0: a direction straight down would normalise a zero vector (the below-horizon branch drops this value)
    const DirectX::XMFLOAT3 d = Normalize({dir.x, std::max(dir.y, 1e-4f), dir.z});
    const float cosTheta = std::max(d.y, 0.0f);
    const float cosGamma = std::clamp(Dot(d, s), -1.0f, 1.0f);
    const float Y = st.zenithOverF.x * PerezY(st.perezY, st.perezE.x, cosTheta, cosGamma);
    const float x = st.zenithOverF.y * PerezY(st.perezX, st.perezE.y, cosTheta, cosGamma);
    // + 0.012: Preetham's chromaticities sit slightly magenta of the daylight locus (a noon horizon came out mauve);
    // the shift puts the horizon on D65 and keeps the zenith blue. Mirrored in shaders/physical_sky.hlsli.
    const float y = st.zenithOverF.z * PerezY(st.perezYy, st.perezE.z, cosTheta, cosGamma) + 0.012f;
    const DirectX::XMFLOAT3 rgb = YxyToLinearRgb(Y, x, y);
    return {std::max(rgb.x, 0.0f), std::max(rgb.y, 0.0f), std::max(rgb.z, 0.0f)};
}

} // namespace

void EvaluatePhysicalSky(const PhysicalSkyInput& in, PhysicalSkyState& st) {
    st = PhysicalSkyState{};
    const float turbidity = std::clamp(in.turbidity, 1.7f, 10.0f);

    // 1. the sun's direction (toward the sun): the time of day's arc, or a sun light's own direction
    DirectX::XMFLOAT3 toScene = Normalize(in.sunDirToScene);
    DirectX::XMFLOAT3 s;
    if (in.useSunDir && Length(in.sunDirToScene) > 1e-6f) {
        s = Mul(toScene, -1.0f);
    } else {
        const float phase = (in.timeOfDay - 6.0f) / 12.0f;   // 0 at sunrise 6:00, 1 at sunset 18:00
        const float elev = Radians(in.maxElevationDeg) * std::sin(kPi * phase);
        const float phi = kPi * phase + Radians(in.northDeg);
        const float cosElev = std::cos(elev);
        s = {cosElev * std::cos(phi), std::sin(elev), -cosElev * std::sin(phi)};
        toScene = Normalize(Mul(s, -1.0f));
    }
    st.sunDirToScene = toScene;
    st.sunToward = {s.x, s.y, s.z, 0.0f};

    // 2. the model's sun: a sun below the horizon is clamped to the horizon (the night floor handles the rest)
    const float thetaS = std::acos(Saturate(s.y));

    // 3. the Perez coefficients for this turbidity
    const float T = turbidity;
    st.perezY = {0.1787f * T - 1.4630f, -0.3554f * T + 0.4275f, -0.0227f * T + 5.3251f, 0.1206f * T - 2.5771f};
    st.perezE.x = -0.0670f * T + 0.3703f;
    st.perezX = {-0.0193f * T - 0.2592f, -0.0665f * T + 0.0008f, -0.0004f * T + 0.2125f, -0.0641f * T - 0.8989f};
    st.perezE.y = -0.0033f * T + 0.0452f;
    st.perezYy = {-0.0167f * T - 0.2608f, -0.0950f * T + 0.0092f, -0.0079f * T + 0.2102f, -0.0441f * T - 1.6537f};
    st.perezE.z = -0.0109f * T + 0.0529f;

    // 4. the zenith's luminance (Preetham's chi uses the sun's zenith angle) and chromaticity, already divided by the
    // distribution's value toward the sun (F(0, thetaS)): the shader multiplies it back by F(theta, gamma).
    const float chi = (4.0f / 9.0f - T / 120.0f) * (kPi - 2.0f * std::min(thetaS, kPi * 0.5f));
    const float Yz = std::max((4.0453f * T - 4.9710f) * std::tan(chi) - 0.2155f * T + 2.4192f, 0.0f);
    const float t3 = thetaS * thetaS * thetaS, t2 = thetaS * thetaS, t1 = thetaS;
    const float xz = T * T * (0.00166f * t3 - 0.00375f * t2 + 0.00209f * t1) +
                     T * (-0.02903f * t3 + 0.06377f * t2 - 0.03202f * t1 + 0.00394f) +
                     (0.11693f * t3 - 0.21196f * t2 + 0.06052f * t1 + 0.25886f);
    const float yz = T * T * (0.00275f * t3 - 0.00610f * t2 + 0.00317f * t1) +
                     T * (-0.04214f * t3 + 0.08970f * t2 - 0.04153f * t1 + 0.00516f) +
                     (0.15346f * t3 - 0.26756f * t2 + 0.06670f * t1 + 0.26688f);
    const float f0Y = PerezY(st.perezY, st.perezE.x, 1.0f, s.y);
    const float f0X = PerezY(st.perezX, st.perezE.y, 1.0f, s.y);
    const float f0Yy = PerezY(st.perezYy, st.perezE.z, 1.0f, s.y);
    st.zenithOverF = {Yz / std::max(f0Y, 1e-4f), xz / std::max(f0X, 1e-4f), yz / std::max(f0Yy, 1e-4f), 0.0f};

    // 5. the frame's constants
    // The day factor keeps the model down to a little below the horizon (the sky just after sunset is still the
    // model's warm horizon) and fades it out by ~7 degrees: only the night floor is left then.
    const float nightFade = SmoothStep(-0.12f, -0.005f, s.y);
    st.params = {1.0f, kRadianceScale * std::max(in.intensity, 0.0f), Saturate(in.haze), nightFade};
    st.night = {0.004f * in.intensity, 0.006f * in.intensity, 0.012f * in.intensity, 0.0f};

    // 7. the sun's colour: Rayleigh + Mie optical depth along the elevation (Kasten-Young air mass), normalised to the
    // zenith so a high sun is ~0.6 x sunIntensity; 0 once the sun is below the horizon.
    const float elevDeg = Degrees(std::asin(std::clamp(s.y, -1.0f, 1.0f)));
    if (elevDeg > 0.0f) {
        const float mass = 1.0f / (std::sin(Radians(elevDeg)) + 0.50572f * std::pow(elevDeg + 6.07995f, -1.6364f));
        const float beta = 0.04608f * T - 0.04586f;
        const DirectX::XMFLOAT3 tauR{0.0421f, 0.1002f, 0.2490f};
        const DirectX::XMFLOAT3 tauM{beta * 1.65f, beta * 2.17f, beta * 2.91f};
        auto trans = [&](const DirectX::XMFLOAT3& tau, float m) {
            return DirectX::XMFLOAT3{std::exp(-m * tau.x), std::exp(-m * tau.y), std::exp(-m * tau.z)};
        };
        const DirectX::XMFLOAT3 at1 = trans(Add(tauR, tauM), 1.0f);
        const DirectX::XMFLOAT3 atM = trans(Add(tauR, tauM), mass);
        const float gain = 0.6f * std::max(in.sunIntensity, 0.0f) * SmoothStep(0.0f, 0.05f, s.y);
        st.sunColor = {atM.x / at1.x * gain, atM.y / at1.y * gain, atM.z / at1.z * gain};
    }

    // 8. the CPU samples for the ambient fill: the zenith, the horizon (averaged over the azimuths) and the ground
    // (the light's albedo x the sky's irradiance: the horizon and the zenith plus the sun's share).
    st.skyZenith = PhysicalSkyRadiance(st, {0.0f, 1.0f, 0.0f});
    DirectX::XMFLOAT3 horizon{0, 0, 0};
    for (int i = 0; i < 8; ++i) {
        const float a = (float)i * (2.0f * kPi / 8.0f);
        horizon = Add(horizon, PhysicalSkyRadiance(st, Normalize({std::cos(a), 0.05f, std::sin(a)})));
    }
    st.skyHorizon = Mul(horizon, 1.0f / 8.0f);
    const DirectX::XMFLOAT3 irradiance =
        Add(Mul(Add(st.skyZenith, st.skyHorizon), 0.5f), Mul(st.sunColor, 0.25f * std::max(s.y, 0.0f)));
    st.groundColor = {in.ground.x * irradiance.x, in.ground.y * irradiance.y, in.ground.z * irradiance.z};
}

DirectX::XMFLOAT3 PhysicalSkyRadiance(const PhysicalSkyState& st, DirectX::XMFLOAT3 dir) {
    if (st.params.x <= 0.5f) return {0, 0, 0};
    const float scale = st.params.y * st.params.w;
    auto shaded = [&](const DirectX::XMFLOAT3& d) {
        const DirectX::XMFLOAT3 c = Mul(ModelRadiance(st, d), scale);
        return Add(c, DirectX::XMFLOAT3{st.night.x, st.night.y, st.night.z});
    };
    // the horizon colour in the direction's own azimuth: the same distribution at a shallow elevation
    const DirectX::XMFLOAT3 horiz = shaded(Normalize({dir.x, 0.02f, dir.z}));
    DirectX::XMFLOAT3 sky = shaded(dir);
    // horizon haze: the lower ~15 degrees lean toward the horizon colour
    sky = {sky.x + (horiz.x - sky.x) * (st.params.z * (1.0f - SmoothStep(0.0f, 0.26f, dir.y))),
           sky.y + (horiz.y - sky.y) * (st.params.z * (1.0f - SmoothStep(0.0f, 0.26f, dir.y))),
           sky.z + (horiz.z - sky.z) * (st.params.z * (1.0f - SmoothStep(0.0f, 0.26f, dir.y)))};
    if (dir.y >= 0.0f) return sky;
    // below the horizon: today's blend from the horizon colour to the ground colour
    const float t = Saturate(-dir.y * 6.0f);
    const DirectX::XMFLOAT3& g = st.groundColor;
    return {horiz.x + (g.x - horiz.x) * t, horiz.y + (g.y - horiz.y) * t, horiz.z + (g.z - horiz.z) * t};
}

} // namespace mmdx
