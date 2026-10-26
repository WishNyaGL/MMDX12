// S9c: the offline GI path tracer's participating medium (in-path media). One medium for the whole frame, described by
// a single GpuMedium element (t5 space1) plus the local fog volumes (t4 space1, the same GpuFogVolume buffer the
// single-scatter pass reads), so both modes describe one medium:
//   extinction (grey) sigma_t(p) = height fog sigma0 * exp(-falloff * max(y, 0)) inside the haze sphere
//                                  + every fog volume's density * sigma0 * shape(p) (soft edge included)
//   scattering albedo = the tint (the height fog's colour, a volume's colour inside it, blended by their sigma share), so
//     a coloured fog absorbs the other channels and the light it scatters carries its colour
//   phase = Henyey-Greenstein; the caller picks g (the fog's, or a light's own as in the single pass)
// Free-flight distance sampling is delta tracking against a per-segment majorant (the height fog's maximum on the
// segment plus the densities of the volumes it overlaps); transmittance is closed form for the height fog alone and
// ratio tracking with the same majorant once a volume is involved. A ray only travels through the medium inside the
// haze sphere (VolParams::mediumRadius of the single pass), so the sky beyond it is not fogged.
#ifndef MMDX_MEDIUM_COMMON_HLSLI
#define MMDX_MEDIUM_COMMON_HLSLI
#include "fog_shapes.hlsli"

// Mirror of GpuMedium (src/render/ShaderInterop.h, 48 bytes, one element).
struct GpuMediumH {
    float sigma0;   // height fog extinction at y <= 0 (the density slider already applied)
    float falloff;  // height falloff
    float radius;   // the haze sphere around the stage
    float g;        // the fog's anisotropy for the path continuation (the punctual lights' default)
    float3 tint;    // the height fog's scattering albedo
    uint mode;      // 0 = off (the paths ignore the medium), 1 = full
    uint numFog;    // local fog volumes in gMedFog (0 = the height fog alone, no buffer read)
    uint maxEvents; // medium scattering events per path (0 = kMediumMaxEvents)
    float sunG;     // the sun's anisotropy (g is the fog's: punctual lights and the path continuation)
    float sunVolumetric;  // the sun's share of the haze
};
StructuredBuffer<GpuMediumH> gMedium : register(t5, space1);
StructuredBuffer<VolFog> gMedFog : register(t4, space1);

static const uint kMediumMaxEvents = 32u;    // scattering events per path, then transmittance only
static const uint kMediumTrackSteps = 64u;   // null collisions (a majorant overestimate only costs steps)

bool MediumOn() { return gMedium[0].mode == 1u; }
uint MediumMaxEvents() { return gMedium[0].maxEvents > 0u ? gMedium[0].maxEvents : kMediumMaxEvents; }

// ---- the medium at a point -------------------------------------------------------------------
// Grey extinction at wp (0 outside every shape).
float MediumSigma(float3 wp) {
    GpuMediumH m = gMedium[0];
    float s = m.sigma0 > 0.0 ? m.sigma0 * exp(-m.falloff * max(wp.y, 0.0)) : 0.0;
    for (uint i = 0; i < m.numFog; ++i) {
        VolFog f = gMedFog[i];
        if (f.density <= 0.0) continue;
        s += f.density * m.sigma0 * VolFogShape(f, wp);
    }
    return s;
}

// Scattering albedo at wp: the sigma-weighted blend of the height fog's tint and the volumes' colours (<= 1 per
// channel; the rest of the grey extinction absorbs).
float3 MediumAlbedo(float3 wp) {
    GpuMediumH m = gMedium[0];
    float3 scat = m.sigma0 > 0.0 ? m.tint * (m.sigma0 * exp(-m.falloff * max(wp.y, 0.0))) : 0.0;
    for (uint i = 0; i < m.numFog; ++i) {
        VolFog f = gMedFog[i];
        if (f.density <= 0.0) continue;
        scat += f.color * (f.density * m.sigma0 * VolFogShape(f, wp));
    }
    float s = max(MediumSigma(wp), 1e-8);
    return saturate(scat / s);
}

// ---- the segment -----------------------------------------------------------------------------
// The medium's parameter range along o + d t within [0, tMax] (the haze sphere); empty when x >= y.
float2 MediumRange(float3 o, float3 d, float tMax) {
    GpuMediumH m = gMedium[0];
    float3 oc = o;
    float b = dot(oc, d);
    float h = b * b - (dot(oc, oc) - m.radius * m.radius);
    if (h <= 0.0) return float2(1, 0);
    h = sqrt(h);
    float2 seg = float2(max(-b - h, 0.0), min(-b + h, tMax));
    return seg.x < seg.y ? seg : float2(1, 0);
}

// ---- S9c-2: punctual light ranges + equiangular sampling ---------------------------------------
// Copies of volumetric_common.hlsli's VolRaySphere / VolRayCone: that file declares the single pass's own VolLight /
// VolParams buffers, so the two clip helpers the medium NEE needs are repeated here (same maths).
//
// Ray / sphere: [t0, t1] clipped to [0, tMax]; empty when t0 >= t1.
float2 MediumRaySphere(float3 o, float3 d, float3 c, float r, float tMax) {
    float3 oc = o - c;
    float b = dot(oc, d);
    float h = b * b - (dot(oc, oc) - r * r);
    if (h <= 0.0) return float2(1, 0);
    h = sqrt(h);
    return float2(max(-b - h, 0.0), min(-b + h, tMax));
}

// Ray / forward cone (apex a, unit axis, cos of the half angle): the parameter interval where the ray is inside the
// cone, clipped to [lo, hi]; empty when x >= y.
float2 MediumRayCone(float3 o, float3 d, float3 apex, float3 axis, float cosA, float lo, float hi) {
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

// Equiangular (Kulla & Fajardo 2012) distance sample inside [segA, segB] of o + d t around the punctual light at `pos`:
// with delta = the light's distance along the ray and D = its perpendicular distance, th = atan((t - delta) / D) is
// uniform over [thA, thB], so t = delta + D tan(th) and the distance pdf is
//   p(t) = D / ((thB - thA) * (D^2 + (t - delta)^2)).
float MediumEquiangular(float3 o, float3 d, float3 pos, float segA, float segB, float u, out float pdf) {
    const float delta = dot(pos - o, d);
    const float D = max(length(o + d * delta - pos), 1e-3);
    const float thA = atan2(segA - delta, D), thB = atan2(segB - delta, D);
    const float t = clamp(delta + D * tan(lerp(thA, thB, u)), segA, segB);
    const float dt = t - delta;
    const float norm = (thB - thA) * (D * D + dt * dt);
    pdf = norm > 1e-8 ? D / norm : 0.0;
    return t;
}

// The height fog's maximum on the segment (its density at the segment's lowest point) plus the densities of the volumes
// the segment overlaps (their shape is <= 1, so their full density bounds them). 0 = no medium on this segment.
float MediumMajorant(float3 o, float3 d, float2 seg) {
    GpuMediumH m = gMedium[0];
    float yLow = d.y < 0.0 ? o.y + d.y * seg.y : o.y + d.y * seg.x;
    float major = m.sigma0 > 0.0 ? m.sigma0 * exp(-m.falloff * max(yLow, 0.0)) : 0.0;
    for (uint i = 0; i < m.numFog; ++i) {
        VolFog f = gMedFog[i];
        if (f.density <= 0.0) continue;
        const float3 e = max(abs(f.extent), 1e-4);
        const float2 s = f.sphere != 0 ? VolRayEllipsoidForward(o, d, f.center, e)
                                       : VolRayBoxForward(o, d, f.center, e);
        if (min(s.y, seg.y) > max(s.x, seg.x)) major += f.density * m.sigma0;
    }
    return major;
}

// ---- free-flight sampling --------------------------------------------------------------------
// Delta tracking on [0, tMax] (clipped to the haze sphere): true = a real collision at `t` (absolute distance along o + d
// from the caller's origin) with the medium's albedo at that point; false = the segment passes (its transmittance is in
// the collision probability, so there is nothing to multiply).
bool MediumSampleDistance(float3 o, float3 d, float tMax, inout uint rng, out float t, out float3 albedo) {
    t = tMax;
    albedo = 1.0;
    const float2 seg = MediumRange(o, d, tMax);
    if (seg.x >= seg.y) return false;
    const float major = MediumMajorant(o, d, seg);
    if (major <= 0.0) return false;
    float tc = seg.x;
    [loop] for (uint i = 0; i < kMediumTrackSteps; ++i) {
        tc -= log(max(1.0 - Rand(rng), 1e-6)) / major;
        if (tc >= seg.y) return false;
        float3 p = o + d * tc;
        if (Rand(rng) * major < MediumSigma(p)) {   // real collision (a null collision continues)
            t = tc;
            albedo = MediumAlbedo(p);
            return true;
        }
    }
    return false;
}

// ---- transmittance ---------------------------------------------------------------------------
// Height fog optical depth along o + d t, t in [0, t1] (d normalized) — the closed form of
// volumetric_common.hlsli VolOpticalDepthAbove, from a segment that starts at or above the y = 0 plane split.
float MediumHeightOpticalDepth(float y0, float dy, float t, float sigma0, float falloff) {
    float base = sigma0 * exp(-falloff * y0);
    float k = falloff * dy;
    return abs(k * t) < 1e-3 ? base * t * (1.0 - 0.5 * k * t) : base * (1.0 - exp(-k * t)) / k;
}

// Height fog optical depth over [0, tMax] (no local volumes), clipped to the haze sphere: the single pass's
// VolOpticalDepth (the volume part left out).
float MediumHeightTransmittance(float3 o, float3 d, float tMax) {
    GpuMediumH m = gMedium[0];
    if (m.sigma0 <= 0.0) return 1.0;
    const float2 seg = MediumRange(o, d, tMax);
    if (seg.x >= seg.y) return 1.0;
    o += d * seg.x;
    const float t = seg.y - seg.x;
    float tc = abs(d.y) > 1e-6 ? -o.y / d.y : -1.0;   // crossing of the y = 0 plane
    float depth;
    if (o.y >= 0.0) {
        if (d.y < 0.0 && tc < t) depth = MediumHeightOpticalDepth(o.y, d.y, tc, m.sigma0, m.falloff) + m.sigma0 * (t - tc);
        else depth = MediumHeightOpticalDepth(o.y, d.y, t, m.sigma0, m.falloff);
    } else if (d.y > 0.0 && tc < t) {
        depth = m.sigma0 * tc + MediumHeightOpticalDepth(0.0, d.y, t - tc, m.sigma0, m.falloff);
    } else {
        depth = m.sigma0 * t;
    }
    return exp(-depth);
}

// Medium transmittance over [0, t] along o + d (1 = clear, 1 for mode 0 / no medium on the segment): closed form for the
// height fog alone, ratio tracking with the segment's majorant once a local volume is involved.
float MediumTransmittance(float3 o, float3 d, float tMax, inout uint rng) {
    if (!MediumOn()) return 1.0;
    GpuMediumH m = gMedium[0];
    if (m.numFog == 0u) return MediumHeightTransmittance(o, d, tMax);
    const float2 seg = MediumRange(o, d, tMax);
    if (seg.x >= seg.y) return 1.0;
    const float major = MediumMajorant(o, d, seg);
    if (major <= 0.0) return 1.0;
    float T = 1.0;
    float tc = seg.x;
    [loop] for (uint i = 0; i < kMediumTrackSteps; ++i) {
        tc -= log(max(1.0 - Rand(rng), 1e-6)) / major;
        if (tc >= seg.y) break;
        T *= max(1.0 - MediumSigma(o + d * tc) / major, 0.0);   // a null collision leaves the weight; a real one ends it
    }
    return T;
}

// ---- Henyey-Greenstein -----------------------------------------------------------------------
float HgEval(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

// Cosine of the scattering angle for a uniform u in [0, 1) (the analytic HG inversion), forward scattering around dir.
float HgCos(float u, float g) {
    if (abs(g) < 1e-3) return 1.0 - 2.0 * u;
    const float s = (1.0 - g * g) / max(1.0 - g + 2.0 * g * u, 1e-6);
    return (1.0 + g * g - s * s) / (2.0 * g);
}

// A direction sampled from the HG phase around dir (pdf = HgEval(dot(dir, result), g)).
float3 HgSample(float3 dir, float g, float2 u) {
    const float cosTheta = clamp(HgCos(u.x, g), -1.0, 1.0);
    const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    const float phi = 2.0 * PI * u.y;
    float3 t, b;
    BuildBasis(dir, t, b);
    return normalize(dir * cosTheta + (t * cos(phi) + b * sin(phi)) * sinTheta);
}

#endif
