// Single-scattering integrator shared by the real-time (volumetric.hlsl) and offline
// (offline_volumetric.hlsl) volumetric passes. Include common.hlsli (or rt_common.hlsli) first
// and define, before including this file:
//   float VolSunVisibility(float3 wp)                  0..1 sun visibility at wp
//   float VolSpotVisibility(VolLight l, float3 wp)     0..1 visibility of spot / point light l at wp
//
// Medium: exponential height fog sigma(y) = sigma0 * exp(-falloff * max(y, 0)) (scattering =
// extinction, white). Transmittance is integrated in closed form, so every light sample is
// weighted by the exact T(t) and the unshadowed part of the sun term is exact:
//   integral sigma(t) T(t) dt over [a, b] = T(a) - T(b).
// S9: local fog volumes (gVolFog, t4 space1) add a tinted extinction inside their box / ellipsoid, and every light
// scales its own in-scattering by its volumetric share and its anisotropy (Henyey-Greenstein). VolParams::numFog == 0
// and a share of 1 with the fog's g reproduce the image of a build without S9 bit for bit.
// The medium is bounded by a sphere around the stage (VolParams::mediumRadius).
// Light shafts come from the visibility terms: the sun is marched only where shadow data exists
// (shadow-cascade range, or the whole ray with ray queries); each spot light is marched only
// inside its cone (analytic ray / cone intersection), so a beam gets all of its samples
// regardless of the camera distance, and occluders cut dark shafts out of it.
#ifndef MMDX_VOLUMETRIC_COMMON_HLSLI
#define MMDX_VOLUMETRIC_COMMON_HLSLI

#include "fog_shapes.hlsli"   // S9c: VolFog + the shape tests, shared with the offline GI medium (medium_common.hlsli)

struct VolLight {
    float3 pos;   float invRange;
    float3 color; float cosOuter;   // cosOuter <= -1: point light
    float3 dir;   float cosInner;
    float shadowSlice; float shadowType; float shadowSoftness; float shadowDensity;
    float3 shadowColor; float falloff;
    float affectDiffuse; float affectSpecular; float pointShadowSlice; float volumetric; // S9: haze share
    float areaWidth; float areaHeight; float isArea; float aniso;          // S9: scattering anisotropy (-1: the fog's)
};
StructuredBuffer<VolLight> gVolLights : register(t2, space1);

// S7a: the march considers at most this many punctual lights (the first ones in the light list), so a 48-spot stage
// stays affordable per step.
static const uint kVolMaxLights = 64u;

// S9: local participating-medium volumes (GpuFogVolume in ShaderInterop.h; the shape lives in fog_shapes.hlsli):
// density is added to the global height fog inside the shape (density = a multiple of the base sigma0; box, or
// ellipsoid with sphere != 0) and color tints the medium there. edge is the soft border band.
StructuredBuffer<VolFog> gVolFog : register(t4, space1);

float VolSunVisibility(float3 wp);
float VolSpotVisibility(VolLight l, float3 wp);

struct VolParams {
    float sigma0;        // extinction at y <= 0 (per MMD unit)
    float falloff;       // height falloff (per MMD unit)
    float sunG;          // Henyey-Greenstein anisotropy for the sun
    float punctualG;     // ... for spot / point lights
    float spotBoost;     // spot light in-scattering scale
    float pointBoost;    // point light in-scattering scale (fills would only add a uniform veil)
    float sunShadowDist; // the sun is marched over [0, min(dist, sunShadowDist)], analytic beyond
    uint sunSteps;       // max steps over the sun's shadowed segment
    uint spotSteps;      // max steps per spot cone crossing
    uint pointSteps;     // per point light sphere crossing
    float sunStepLen;    // target step length (MMD units); fewer steps on short segments
    float spotStepLen;
    float jitter;        // 0..1 per pixel / frame sample offset
    float3 ambient;      // isotropic in-scattered radiance from the sky dome (already scaled)
    float3 mediumCenter; // the haze fills a sphere around the stage (none outside), so distant
    float mediumRadius;  // scenery and the sky are not fogged out
    uint numFog;         // S9: local fog volumes in gVolFog (0 = none read: the image is the height fog alone)
    float sunVolumetric; // S9: the sun's share of the haze (LightValues::volumetric)
    float3 tint;         // S9: the medium's scattering tint (a stage environment.fog.color; white = plain haze)
};

float VolHG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// Optical depth of the height fog along o + d t, t in [0, t1] (d normalized).
float VolOpticalDepthAbove(float y0, float dy, float t, VolParams p) {
    float base = p.sigma0 * exp(-p.falloff * y0);
    float k = p.falloff * dy;
    return abs(k * t) < 1e-3 ? base * t * (1.0 - 0.5 * k * t) : base * (1.0 - exp(-k * t)) / k;
}
// ---- S9 local fog volumes --------------------------------------------------
// Defined after the ray / shape tests below; declared here for VolOpticalDepth.
float VolFogOpticalDepth(float3 o, float3 d, float t, VolParams p);

float VolOpticalDepth(float3 o, float3 d, float t, VolParams p) {
    float tc = abs(d.y) > 1e-6 ? -o.y / d.y : -1.0;   // crossing of the y = 0 plane
    float base;
    if (o.y >= 0.0) {
        if (d.y < 0.0 && tc < t) base = VolOpticalDepthAbove(o.y, d.y, tc, p) + p.sigma0 * (t - tc);
        else base = VolOpticalDepthAbove(o.y, d.y, t, p);
    } else if (d.y > 0.0 && tc < t) {
        base = p.sigma0 * tc + VolOpticalDepthAbove(0.0, d.y, t - tc, p);
    } else {
        base = p.sigma0 * t;
    }
    return base + VolFogOpticalDepth(o, d, t, p);
}
float VolSigma(float3 wp, VolParams p) {
    return p.sigma0 * exp(-p.falloff * max(wp.y, 0.0));
}

// Ray / sphere: [t0, t1] clipped to [0, tMax]; empty when t0 >= t1.
float2 VolRaySphere(float3 o, float3 d, float3 c, float r, float tMax) {
    float3 oc = o - c;
    float b = dot(oc, d);
    float h = b * b - (dot(oc, oc) - r * r);
    if (h <= 0.0) return float2(1, 0);
    h = sqrt(h);
    return float2(max(-b - h, 0.0), min(-b + h, tMax));
}

// Ray / forward cone (apex a, unit axis, cos of the half angle): the parameter interval where the
// ray is inside the cone, clipped to [lo, hi]; empty when x >= y.
float2 VolRayCone(float3 o, float3 d, float3 apex, float3 axis, float cosA, float lo, float hi) {
    float3 co = o - apex;
    float c2 = cosA * cosA;
    float da = dot(d, axis), ca = dot(co, axis);
    float a = da * da - c2;
    float b = 2.0 * (da * ca - dot(d, co) * c2);
    float c = ca * ca - dot(co, co) * c2;
    float t0 = -1e30, t1 = 1e30;   // the inside interval on the double cone (forward nappe picked below)
    float disc = b * b - 4.0 * a * c;
    if (abs(a) < 1e-6) {
        // ray parallel to the cone surface: one root
        if (abs(b) < 1e-8) return float2(1, 0);
        float r = -c / b;
        if (b > 0.0) t0 = r; else t1 = r;
    } else if (disc < 0.0) {
        if (a < 0.0) return float2(1, 0);   // never inside
        // always inside the double cone: the forward nappe test below decides
    } else {
        float sq = sqrt(disc);
        float r0 = (-b - sq) / (2.0 * a), r1 = (-b + sq) / (2.0 * a);
        float rl = min(r0, r1), rh = max(r0, r1);
        if (a < 0.0) {
            t0 = rl; t1 = rh;   // inside between the roots
        } else {
            // inside outside the roots: (-inf, rl] and [rh, inf), one per nappe
            float axialHi = ca + da * (rh + 1.0);
            if (axialHi > 0.0) t0 = rh; else t1 = rl;
        }
    }
    t0 = max(t0, lo);
    t1 = min(t1, hi);
    if (t0 >= t1) return float2(1, 0);
    // forward nappe only (apex side along the axis)
    float tm = 0.5 * (t0 + t1);
    if (ca + da * tm < 0.0) return float2(1, 0);
    return float2(t0, t1);
}

// ---- S9 local fog volumes: the tinted extinction they add (the shape tests live in fog_shapes.hlsli) ----
// Extinction the local volumes add at wp, per channel (tinted). Zero when numFog == 0.
float3 VolFogSigma(float3 wp, VolParams p) {
    float3 sum = 0;
    for (uint i = 0; i < p.numFog; ++i) {
        VolFog f = gVolFog[i];
        if (f.density <= 0.0) continue;
        sum += f.color * (f.density * p.sigma0 * VolFogShape(f, wp));   // density = a multiple of the base medium
    }
    return sum;
}

// Optical depth the local volumes add over [0, t] along o + d t: the ray / shape overlap length times the density (the
// soft border band is left out here; the tint shows in the in-scattering, not as a hard transmittance edge).
float VolFogOpticalDepth(float3 o, float3 d, float t, VolParams p) {
    float sum = 0.0;
    for (uint i = 0; i < p.numFog; ++i) {
        VolFog f = gVolFog[i];
        if (f.density <= 0.0) continue;
        const float3 e = max(abs(f.extent), 1e-4);
        const float2 seg = f.sphere != 0 ? VolRayEllipsoidForward(o, d, f.center, e)
                                         : VolRayBoxForward(o, d, f.center, e);
        const float lo = max(seg.x, 0.0), hi = min(seg.y, t);
        if (hi > lo) sum += f.density * p.sigma0 * (hi - lo);
    }
    return sum;
}

float VolPunctualAtten(VolLight l, float3 wp, out float3 ld) {
    float3 dv = l.pos - wp;
    float dL = length(dv);
    ld = dv / max(dL, 1e-4);
    float atten = PunctualFalloff(dL, l.invRange, (int)l.falloff);
    if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
    return atten;
}

// S9d: the medium at one point (the froxel injector, and the march's own light samples via the same functions):
// `sigmaS` = the tinted scattering coefficient the in-scattered light is weighted by (the height fog plus the local
// volumes' colours), `sigmaT` = the scalar extinction of the transmittance (the height fog plus the volumes'
// densities, the density VolFogOpticalDepth integrates; the shape's soft border band is left out there as well).
void VolMediumAt(float3 wp, VolParams p, out float3 sigmaS, out float sigmaT) {
    const float base = VolSigma(wp, p);
    sigmaS = base + VolFogSigma(wp, p);
    float extra = 0.0;
    for (uint i = 0; i < p.numFog; ++i) {
        VolFog f = gVolFog[i];
        if (f.density <= 0.0) continue;
        extra += f.density * VolFogShape(f, wp);
    }
    sigmaT = base + extra * p.sigma0;
}

#ifndef VOL_PUNCTUAL_PHASE
#define VOL_PUNCTUAL_PHASE(c, g) VolHG(c, g)
#endif

// In-scattered radiance (rgb) and transmittance (a) along eye + dir t, t in [0, dist].
float4 IntegrateVolume(float3 eye, float3 dir, float dist, uint numLights, float3 sunDir, float3 sunCol,
                       VolParams p) {
    // clip the ray to the haze volume; t is measured from its entry point from here on
    float2 medium = VolRaySphere(eye, dir, p.mediumCenter, p.mediumRadius, dist);
    if (medium.x >= medium.y) return float4(0, 0, 0, 1);
    eye += dir * medium.x;
    dist = medium.y - medium.x;
    p.sunShadowDist -= medium.x;

    float Tend = exp(-VolOpticalDepth(eye, dir, dist, p));
#ifdef VOL_SPOTS_ONLY
    float3 L = 0.0;   // the beam pass is additive: the ambient haze stays in the froxel grid
#else
    float3 L = p.ambient * (1.0 - Tend);
#endif

#ifndef VOL_SPOTS_ONLY
    // ---- sun: shadowed march over the shadow range, analytic (fully lit) tail beyond
    // S9: sunVolumetric scales the whole term (0 = the sun leaves no haze); sunG is the fog's anisotropy unless the
    // sun light carries its own (VolParams::sunG is already resolved by the caller).
    float3 sunScale = sunCol * VolHG(dot(dir, -sunDir), p.sunG) * p.sunVolumetric;
    float tS = min(dist, p.sunShadowDist);
    if (tS > 0.0 && p.sunSteps > 0) {
        uint n = clamp((uint)ceil(tS / p.sunStepLen), 4u, p.sunSteps);
        float stepLen = tS / (float)n;
        float Tprev = 1.0;
        float sum = 0.0;
        for (uint i = 0; i < n; ++i) {
            float tb = (float)(i + 1) * stepLen;
            float Tb = exp(-VolOpticalDepth(eye, dir, tb, p));
            float vis = VolSunVisibility(eye + dir * ((float)i + p.jitter) * stepLen);
            sum += vis * (Tprev - Tb);
            Tprev = Tb;
        }
        L += sunScale * (sum + (Tprev - Tend));
    } else {
        L += sunScale * (1.0 - Tend);
    }
#endif   // !VOL_SPOTS_ONLY

    // ---- spot cones / point spheres: samples only where the light reaches
    // S7a: the march sees at most kVolMaxLights lights (the first ones in the light list); a scene with fewer lights
    // keeps the exact image (same order, same samples).
    numLights = min(numLights, kVolMaxLights);
    for (uint li = 0; li < numLights; ++li) {
        VolLight l = gVolLights[li];
        // S9: the light's own haze share (0 = invisible in the medium) and scattering anisotropy (-1 = the fog's)
        if (l.volumetric <= 0.0) continue;
        const float g = l.aniso > -0.99 ? l.aniso : p.punctualG;   // -1 = unset (valid g is -0.9..0.95)
        float range = 1.0 / max(l.invRange, 1e-6);
        float2 seg = VolRaySphere(eye, dir, l.pos, range, dist);
        bool spot = l.cosOuter > -1.0;
#ifdef VOL_SPOTS_ONLY
        if (!spot) continue;   // point lights stay in the froxel grid
#endif
        if (spot && seg.x < seg.y) seg = VolRayCone(eye, dir, l.pos, l.dir, l.cosOuter, seg.x, seg.y);
        if (seg.x >= seg.y) continue;
        uint n = spot ? clamp((uint)ceil((seg.y - seg.x) / p.spotStepLen), 3u, max(p.spotSteps, 3u)) : p.pointSteps;
        if (n == 0) continue;
        float stepLen = (seg.y - seg.x) / (float)n;
        float boost = spot ? p.spotBoost : p.pointBoost;
        float3 acc = 0;
        for (uint i = 0; i < n; ++i) {
            float t = seg.x + ((float)i + p.jitter) * stepLen;
            float3 wp = eye + dir * t;
            float3 ld;
            float atten = VolPunctualAtten(l, wp, ld);
            if (atten <= 0.0) continue;
            float vis = spot ? VolSpotVisibility(l, wp) : 1.0;
            float T = exp(-VolOpticalDepth(eye, dir, t, p));
            // S9: the global height fog sigma plus the local volumes' tinted extinction at this sample
            float3 sigma = VolSigma(wp, p) + (p.numFog > 0 ? VolFogSigma(wp, p) : 0.0);
#ifdef VOL_GOBO
            // S7b: a spot's gobo dims / tints its in-scattering at this sample point (+1 mip: the volume is noisy)
            const float3 goboTint = spot ? GoboFactor(li, wp, l.pos, l.dir, l.cosOuter, 1.0) : float3(1, 1, 1);
#else
            const float3 goboTint = float3(1, 1, 1);   // the march (volumetric.hlsl) does not carry gobos
#endif
            acc += (atten * vis * VOL_PUNCTUAL_PHASE(dot(dir, ld), g) * sigma * T) * goboTint;
        }
        L += l.color * (boost * l.volumetric) * acc * stepLen;
    }
#ifdef VOL_SPOTS_ONLY
    // alpha 1: the beam pass is additive (the froxel already applies the transmittance to the surface)
    return float4(L * p.tint, 1.0);
#else
    return float4(L * p.tint, Tend);
#endif
}

#endif
