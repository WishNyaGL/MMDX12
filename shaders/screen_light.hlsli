// S8b: stage screens as rectangular area lights for the raster / RT scene (raster mmd.hlsl and the RT lit PS, which
// shares mmd.hlsl). Not used by the path tracers: they already have S8a's emissive-triangle NEE.
//
// SceneConstants::gScreenLights (C++: SceneConstants::screenLightParams):
//   x = SrvHeap index of the raw ByteAddressBuffer holding one GpuScreenLight per screen light (-1 = no screen light in
//       the frame: every function here returns 0 at one compare, so the frame is the old one byte for byte),
//   y = the count (at most ShaderInterop.h kMaxScreenLights = 16),
//   z = SrvHeap index of the average-colour buffer (shaders/screen_light_avg.hlsl): one float4 per screen light,
//       xyz = the screen texture's average in linear light,
//   w = unused.
//
// GpuScreenLight: xyz = centre (world), w = the screen's emissive strength (the material's reflectivity, the same scale
// ScreenEmission uses); xyz = unit normal, w = area (world^2); axisA.xyz = half axis A (world), axisA.w = the SrvHeap
// index of the screen texture the material shows this frame; axisB.xyz = half axis B (world).
//
// Diffuse: the rectangle's AVERAGE radiance (one texel from the average buffer) over the analytic irradiance of a
// polygon (Lambert's formula): for the unit vectors v_i from the shading point to the corners,
// sum acos(dot(v_i, v_j)) * dot(normalize(cross(v_i, v_j)), n) / (2 pi) is the cosine-weighted fraction of the
// hemisphere the rectangle covers. The formula falls off as 1/r^2 (a distant rectangle's solid angle shrinks), so the
// result is distance-correct without a falloff curve. One-sided: only the side the rectangle's normal points to.
// Specular: the representative point (Karis 2013): the point of the rectangle closest to the reflection ray of the view
// direction, with the existing Blinn / GGX lobe, the lobe widened to the rectangle's angular radius, and the radiance
// scaled by the rectangle's solid angle seen from the shading point (so a small / far screen cannot out-shine a big one).
#ifndef MMDX_SCREEN_LIGHT_HLSLI
#define MMDX_SCREEN_LIGHT_HLSLI

#include "common.hlsli"

// The frame's screen-light table / average buffer (bindless buffer table) and the screen textures (bindless texture
// table). The includer must have declared gPbrBuf / gPbrTex (raster) or gBindlessBuf / gBindlessTex (RT).
#ifdef MMDX_RT_COMMON_HLSLI
#define SCREEN_LIGHT_BUF(i) gBindlessBuf[NonUniformResourceIndex((uint)(i))]
#define SCREEN_LIGHT_TEX(i) gBindlessTex[NonUniformResourceIndex((uint)(i))]
#define SCREEN_LIGHT_SAMPLER gRtWrap
#else
#define SCREEN_LIGHT_BUF(i) gPbrBuf[NonUniformResourceIndex((uint)(i))]
#define SCREEN_LIGHT_TEX(i) gPbrTex[NonUniformResourceIndex((uint)(i))]
#define SCREEN_LIGHT_SAMPLER gPbrWrap
#endif

struct GpuScreenLightHLSL {
    float4 center;   // xyz = centre (world), w = emissive strength
    float4 normal;   // xyz = unit normal, w = area (world^2)
    float4 axisA;    // xyz = half axis A (world), w = the screen texture's SrvHeap index
    float4 axisB;    // xyz = half axis B (world)
};

bool ScreenLightsActive() { return gScreenLights.x >= 0.0 && gScreenLights.y > 0.0; }

GpuScreenLightHLSL ScreenLightOf(uint i) {
    const ByteAddressBuffer table = SCREEN_LIGHT_BUF((uint)gScreenLights.x);
    const uint o = i * 64u;   // GpuScreenLight is 64 B
    GpuScreenLightHLSL l;
    l.center = asfloat(table.Load4(o + 0u));
    l.normal = asfloat(table.Load4(o + 16u));
    l.axisA = asfloat(table.Load4(o + 32u));
    l.axisB = asfloat(table.Load4(o + 48u));
    return l;
}

// The light's radiance (linear rgb): the screen texture's average (the reduction pass) x the emissive strength, which
// is exactly what ScreenEmission gives the surface itself.
float3 ScreenLightAverage(GpuScreenLightHLSL l, uint i) {
    const int avg = (int)gScreenLights.z;
    if (avg < 0) return 0.0;
    const float3 c = asfloat(SCREEN_LIGHT_BUF((uint)avg).Load4(i * 16u)).rgb;
    return max(c, 0.0) * max(l.center.w, 0.0);
}

// The screen's radiance (linear rgb) at a point of its own surface, for the specular reflection (a glossy floor shows
// the LED wall's image, so the reflection reads the texture rather than the average).
float3 ScreenLightColorAt(GpuScreenLightHLSL l, float2 uv, uint i) {
    const int tex = (int)l.axisA.w;
    if (tex < 0) return ScreenLightAverage(l, i);
    const float3 s = saturate(SCREEN_LIGHT_TEX((uint)tex).SampleLevel(SCREEN_LIGHT_SAMPLER, uv, 0.0).rgb);
    return max(SrgbToLinear(s), 0.0) * max(l.center.w, 0.0);
}

// Lambert's polygon irradiance: the cosine-weighted fraction of the hemisphere covered by the rectangle, seen from `wp`
// with the surface normal `n` (1 = a rectangle filling the whole upper hemisphere at `wp`). A corner at the shading
// point is pushed out so the cross products stay finite (the limit of the solid angle is the same).
float ScreenLightIrradiance(GpuScreenLightHLSL l, float3 wp, float3 n) {
    float3 c[4];
    c[0] = l.center.xyz + l.axisA.xyz + l.axisB.xyz - wp;
    c[1] = l.center.xyz - l.axisA.xyz + l.axisB.xyz - wp;
    c[2] = l.center.xyz - l.axisA.xyz - l.axisB.xyz - wp;
    c[3] = l.center.xyz + l.axisA.xyz - l.axisB.xyz - wp;
    [unroll] for (int e = 0; e < 4; ++e) {
        if (dot(c[e], c[e]) < 1e-12) c[e] = float3(0.0, 0.0, 1e-6);
        c[e] = normalize(c[e]);
    }
    float sum = 0.0;
    [unroll] for (int f = 0; f < 4; ++f) {
        const float3 a = c[f];
        const float3 b = c[(f + 1) & 3];
        sum += acos(clamp(dot(a, b), -1.0, 1.0)) * dot(normalize(cross(a, b)), n);
    }
    // The sum's sign follows the corner winding: with A x B along the light's normal (seen from its lit side) the
    // corners run clockwise and the sum is negative, so orient it by the rectangle's own handedness.
    sum *= dot(cross(l.axisA.xyz, l.axisB.xyz), l.normal.xyz) > 0.0 ? -1.0 : 1.0;
    return saturate(sum / (2.0 * PI));
}

// The rectangle's uv of a world point (clamped to its extent), for the specular colour fetch.
float2 ScreenLightUv(GpuScreenLightHLSL l, float3 p) {
    const float3 a = normalize(l.axisA.xyz);
    const float3 b = normalize(l.axisB.xyz);
    const float3 d = p - l.center.xyz;
    const float u = dot(d, a) / max(length(l.axisA.xyz), 1e-4);
    const float v = dot(d, b) / max(length(l.axisB.xyz), 1e-4);
    return clamp(float2(u, v) * 0.5 + 0.5, 0.0, 1.0);
}

// The representative point of the rectangle for the reflection of the view direction V about `n` (Karis, "Physically
// Based Shading on Mobile", 2013): the point of the rectangle closest to that ray, so a mirror-like surface sees the LED
// wall at its own reflected image. `halfAngle` output = the rectangle's angular radius, for the lobe widening.
float3 ScreenLightRepPoint(GpuScreenLightHLSL l, float3 wp, float3 n, float3 V, out float halfAngle) {
    const float3 R = reflect(-V, n);
    const float3 diff = l.center.xyz - wp;
    // closest point on the ray wp + t R to the rectangle's centre, then clamped into the rectangle's plane slab
    const float t = max(dot(diff, R), 0.0);
    const float3 p = wp + R * t;
    float3 d = p - l.center.xyz;
    d -= l.normal.xyz * dot(d, l.normal.xyz);
    const float3 a = normalize(l.axisA.xyz);
    const float3 b = normalize(l.axisB.xyz);
    const float ha = length(l.axisA.xyz);
    const float hb = length(l.axisB.xyz);
    const float u = clamp(dot(d, a), -ha, ha);
    const float v = clamp(dot(d, b), -hb, hb);
    const float3 q = l.center.xyz + a * u + b * v;
    // the rectangle's angular radius seen from the shading point: half its larger extent over the distance
    halfAngle = atan2(max(ha, hb), max(length(q - wp), 1e-4));
    return q;
}

// ---- the sum over the frame's screen lights (what the call sites in mmd.hlsl use) ----

float3 ScreenLightsDiffuse(float3 wp, float3 n) {
    if (!ScreenLightsActive()) return 0.0;
    const uint count = min((uint)gScreenLights.y, (uint)16);
    float3 sum = 0.0;
    for (uint i = 0; i < count; ++i) {
        const GpuScreenLightHLSL l = ScreenLightOf(i);
        // one-sided: only the side the rectangle's normal points to. The polygon formula's sign follows the RECEIVER's
        // normal, so it cannot decide this on its own.
        if (dot(l.normal.xyz, wp - l.center.xyz) <= 0.0) continue;
        const float irr = ScreenLightIrradiance(l, wp, n);
        if (irr <= 0.0) continue;
        sum += ScreenLightAverage(l, i) * irr;
    }
    return sum;
}

// Blinn-Phong lobe toward the representative point, widened to the rectangle's angular radius and weighted by the solid
// angle the rectangle covers from `wp` (a small / far screen contributes a small share of the specular response).
float3 ScreenLightsSpecularBlinn(float3 wp, float3 n, float3 V, float power) {
    if (!ScreenLightsActive()) return 0.0;
    const uint count = min((uint)gScreenLights.y, (uint)16);
    float3 sum = 0.0;
    for (uint i = 0; i < count; ++i) {
        const GpuScreenLightHLSL l = ScreenLightOf(i);
        float halfAngle;
        const float3 q = ScreenLightRepPoint(l, wp, n, V, halfAngle);
        const float3 ld = normalize(q - wp);
        if (dot(l.normal.xyz, -ld) <= 0.0) continue;   // the sample sits on the screen's back: one-sided
        const float ndl = saturate(dot(n, ld));
        if (ndl <= 0.0) continue;
        const float dist = max(length(q - wp), 1e-4);
        const float cosL = saturate(dot(l.normal.xyz, -ld));
        const float omega = min(max(l.normal.w, 0.0) * cosL / (dist * dist), 2.0 * PI);
        const float aPrime = clamp(tan(min(halfAngle, 1.4)), 0.03, 1.0);
        const float p = min(power, max(2.0 / (aPrime * aPrime) - 2.0, 1.0));
        const float3 h = normalize(ld + V);
        sum += ScreenLightColorAt(l, ScreenLightUv(l, q), i) * omega * pow(saturate(dot(n, h)), p) * ndl *
               ((p + 8.0) / (power + 8.0));
    }
    return sum;
}

// GGX lobe toward the representative point, the same solid-angle weight and lobe widening.
// `rough` / `f0` are the PBR material's, as PunctualSpecularGGX takes them.
float3 ScreenLightsSpecularGGX(float3 wp, float3 n, float3 V, float rough, float3 f0) {
    if (!ScreenLightsActive()) return 0.0;
    const uint count = min((uint)gScreenLights.y, (uint)16);
    float3 sum = 0.0;
    for (uint i = 0; i < count; ++i) {
        const GpuScreenLightHLSL l = ScreenLightOf(i);
        float halfAngle;
        const float3 q = ScreenLightRepPoint(l, wp, n, V, halfAngle);
        const float3 ld = normalize(q - wp);
        if (dot(l.normal.xyz, -ld) <= 0.0) continue;   // one-sided
        const float ndl = saturate(dot(n, ld));
        if (ndl <= 0.0) continue;
        const float dist = max(length(q - wp), 1e-4);
        const float cosL = saturate(dot(l.normal.xyz, -ld));
        const float omega = min(max(l.normal.w, 0.0) * cosL / (dist * dist), 2.0 * PI);
        const float aPrime = clamp(max(rough, tan(min(halfAngle, 1.4))), 0.03, 1.0);
        sum += ScreenLightColorAt(l, ScreenLightUv(l, q), i) * (omega / PI) * ndl *
               PbrSpecularGGX(n, V, ld, aPrime, f0);
    }
    return sum;
}

#endif
