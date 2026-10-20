// Shared shadow-map code for the punctual lights (spots / points): the light layout, the PCSS kernel and
// the spot / point shadow-map lookups. The scene pass (mmd.hlsl) and the world-surface host (world.hlsl)
// include this file, so a world surface resolves the same shadows as the scene.
#ifndef SHADOW_COMMON_HLSLI
#define SHADOW_COMMON_HLSLI

#include "common.hlsli"

// The scene's punctual light layout (the C++ upload, 112 bytes).
struct Light {
    float3 pos; float invRange;
    float3 color; float cosOuter;
    float3 dir; float cosInner;
    float shadowSlice; float shadowType; float shadowSoftness; float shadowDensity;
    float3 shadowColor; float falloff;
    float affectDiffuse; float affectSpecular; float pointShadowSlice; float pad;
    float areaWidth; float areaHeight; float isArea; float pad2;
};

// Poisson disk taps: the hard-shadow PCF kernel and the area light's blocker search.
static const float2 kPoisson[12] = {
    float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696, 0.457), float2(-0.203, 0.621),
    float2(0.962, -0.195), float2(0.473, -0.480), float2(0.519, 0.767), float2(0.185, -0.893),
    float2(0.507, 0.064), float2(0.896, 0.412), float2(-0.322, -0.933), float2(-0.792, -0.598)
};

// ---- soft shadows (PCSS) ----
// Soft shadow maps match the ray paths' cone (PunctualSoftAngle): a blocker search, then a PCF disk as wide as
// the penumbra an angular light of that size casts from the average blocker distance (contact hardening).
static const float kPcssMaxUv = 0.04;   // widest PCF radius (shadow-map uv): ~80 texels of a 2048 map

// Vogel disk tap k of n, rotated: even coverage at any radius.
float2 VogelTap(int k, int n, float rot) {
    float a = (float)k * 2.39996323 + rot;
    return float2(cos(a), sin(a)) * sqrt(((float)k + 0.5) / (float)n);
}

// Mean raw depth of the texels nearer to the light than z within searchUv, -1 without any.
float PcssBlocker(Texture2DArray<float> map, float2 uv, float slice, float z, float searchUv, float rot, float mapSize) {
    float sum = 0, count = 0;
    [unroll] for (int k = 0; k < 16; ++k) {
        float2 suv = saturate(uv + VogelTap(k, 16, rot) * searchUv);
        float d = map.Load(int4(min((int2)(suv * mapSize), (int)mapSize - 1), (int)slice, 0));
        if (d < z) { sum += d; count += 1.0; }
    }
    return count > 0.0 ? sum / count : -1.0;
}

// PCF disk; zPerUv lowers each tap's reference along the receiver's slope so wide kernels don't shadow their own surface.
float PcssFilter(Texture2DArray<float> map, SamplerComparisonState cmp, float2 uv, float slice, float z, float radiusUv,
                 float zPerUv, float rot) {
    float sum = 0;
    [unroll] for (int k = 0; k < 32; ++k) {
        float2 o = VogelTap(k, 32, rot);
        sum += map.SampleCmpLevelZero(cmp, float3(uv + o * radiusUv, slice), z - length(o) * radiusUv * zPerUv);
    }
    return sum / 32.0;
}

// tan of the angle between the receiver and the light direction (the slope a wide kernel sees), capped.
float ReceiverSlope(float ndl) {
    float c = clamp(ndl, 0.25, 1.0);
    return sqrt(1.0 - c * c) / c;
}

// PCSS on a perspective depth slice (spot cone, point-light face): a world length w at view depth d covers
// w / (2 d tanHalf) of its uv.
float PerspectivePcss(Texture2DArray<float> map, SamplerComparisonState cmp, float2 uv, float slice, float z, float nearZ,
                      float farZ, float tanHalf, float softness, float ndl, float rot, float invMapSize) {
    float tanA = tan(PunctualSoftAngle(softness));
    float minUv = 0.75 * invMapSize;
    float d = nearZ * farZ / max(farZ - z * (farZ - nearZ), 1e-4);
    float worldPerUv = 2.0 * d * tanHalf;
    float searchUv = clamp(tanA / (2.0 * tanHalf), minUv, kPcssMaxUv);
    float zb = PcssBlocker(map, uv, slice, z, searchUv, rot, 1.0 / invMapSize);
    if (zb < 0.0) return 1.0;
    float db = nearZ * farZ / max(farZ - zb * (farZ - nearZ), 1e-4);
    float radius = clamp(max(d - db, 0.0) * tanA / worldPerUv, minUv, kPcssMaxUv);
    float zPerUv = worldPerUv * ReceiverSlope(ndl) * farZ * nearZ / ((farZ - nearZ) * d * d);
    return PcssFilter(map, cmp, uv, slice, z, radius, zPerUv, rot);
}

// Per-position rotation, constant over time: TAA is often off (and always for raster videos), so a per-frame
// rotation would shimmer.
float PositionRotation(float3 wp) { return frac(sin(dot(wp, float3(12.9898, 78.233, 37.719))) * 43758.5453) * 6.2831853; }

// Spot shadow map (perspective slice per spot): normal offset scaled with the texel footprint,
// 2x2 hardware PCF taps (Hard) or 12 Poisson taps (Soft).
float SpotShadow(Light l, float3 wp, float3 n, float dist, SamplerComparisonState cmp, Texture2DArray<float> spotMap) {
    if (l.shadowType < 0.5) return 1.0;
    float slice = l.shadowSlice;
    if (slice < 0.0 || slice >= gSpotShadowParams.x) return 1.0;
    float c = max(l.cosOuter, 0.05);
    float texel = dist * 2.0 * sqrt(1.0 - c * c) / c * gSpotShadowParams.y;
    float3 ld = (l.pos - wp) / max(dist, 1e-4);
    float offset = texel * (2.0 - saturate(dot(n, ld)));
    float4 sp = mul(float4(wp + n * offset, 1.0), gSpotViewProj[(uint)slice]);
    if (sp.w <= 0.0) return 1.0;
    float3 p = sp.xyz / sp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0) return 1.0;
    if (l.shadowType > 1.5) { // Soft: PCSS over the spot's frustum (Renderer.cpp: fov, near, far)
        float range = 1.0 / max(l.invRange, 1e-4);
        float tanHalf = tan(min(acos(clamp(l.cosOuter, -0.99, 1.0)) * 1.08 + 0.01, 1.4835));
        return PerspectivePcss(spotMap, cmp, uv, slice, p.z - 0.00002, max(0.05, range * 0.004), max(range, 1.0),
                               tanHalf, l.shadowSoftness, dot(n, ld), PositionRotation(wp), gSpotShadowParams.y);
    } else { // Hard (today):
        float r = 0.75 * gSpotShadowParams.y;
        float sum = 0;
        [unroll] for (int k = 0; k < 4; ++k) {
            float2 o = float2((k & 1) ? r : -r, (k & 2) ? r : -r);
            sum += spotMap.SampleCmpLevelZero(cmp, float3(uv + o, slice), p.z - 0.00002);
        }
        return sum * 0.25;
    }
}

// Point shadow map (cube-like depth array, 6 slices per light): major axis determines face,
// UV and projected depth derived analytically, 2x2 PCF (Hard) or 12 Poisson taps (Soft).
float PointShadow(Light l, float3 wp, float3 n, float dist, SamplerComparisonState cmp, Texture2DArray<float> pointMap) {
    if (l.shadowType < 0.5 || gPointShadowParams.x < 0.5) return 1.0;
    float baseSlice = l.pointShadowSlice;
    if (baseSlice < 0.0) return 1.0;

    float invMapSize = gPointShadowParams.y;
    float texel = dist * 2.0 * invMapSize;
    float3 ld = (l.pos - wp) / max(dist, 1e-4);
    float offset = texel * (2.0 - saturate(dot(n, ld)));
    float3 d = (wp + n * offset) - l.pos;

    float3 absD = abs(d);
    uint face = 0;
    float2 uv = 0.0;
    float major = 0.0;

    if (absD.x >= absD.y && absD.x >= absD.z) {
        major = absD.x;
        if (d.x > 0.0) {
            face = 0;
            uv = float2(-d.z, -d.y) / d.x;
        } else {
            face = 1;
            uv = float2(d.z, -d.y) / -d.x;
        }
    } else if (absD.y >= absD.x && absD.y >= absD.z) {
        major = absD.y;
        if (d.y > 0.0) {
            face = 2;
            uv = float2(d.x, d.z) / d.y;
        } else {
            face = 3;
            uv = float2(d.x, -d.z) / -d.y;
        }
    } else {
        major = absD.z;
        if (d.z > 0.0) {
            face = 4;
            uv = float2(d.x, -d.y) / d.z;
        } else {
            face = 5;
            uv = float2(-d.x, -d.y) / -d.z;
        }
    }
    uv = uv * 0.5 + 0.5;

    float range = 1.0 / max(l.invRange, 1e-4);
    float nearZ = max(0.05, range * 0.004);
    float farZ = max(range, 1.0);
    float pz = (farZ / (farZ - nearZ)) * (1.0 - nearZ / max(major, 1e-4));
    if (pz > 1.0 || pz < 0.0) return 1.0;

    float slice = baseSlice + (float)face;
    uv = clamp(uv, 0.5 * invMapSize, 1.0 - 0.5 * invMapSize);

    if (l.isArea > 0.5) {
        // PCSS: blocker search over the light's footprint, then a penumbra from the average blocker distance
        // (world size s at major-axis distance m covers s / (2 m) of a face's uv). Soft widens the light.
        float lightSize = 0.5 * (l.areaWidth + l.areaHeight);
        if (l.shadowType > 1.5) lightSize *= 1.0 + l.shadowSoftness * 2.0;
        float mapSize = 1.0 / invMapSize;
        // per-position rotation, constant over time: TAA is usually off (and always for raster videos), so a
        // per-frame rotation would shimmer
        float rot = frac(sin(dot(wp, float3(12.9898, 78.233, 37.719))) * 43758.5453) * 6.2831853;
        float2x2 R = float2x2(cos(rot), -sin(rot), sin(rot), cos(rot));
        float searchUv = clamp(lightSize / (2.0 * major), 2.0 * invMapSize, 0.1);
        float blockerSum = 0, blockers = 0;
        [unroll] for (int i = 0; i < 12; ++i) {
            float2 suv = saturate(uv + mul(kPoisson[i], R) * searchUv);
            float z = pointMap.Load(int4(min((int2)(suv * mapSize), (int)mapSize - 1), (int)slice, 0));
            if (z < pz - 0.00002) {
                blockerSum += nearZ * farZ / max(farZ - z * (farZ - nearZ), 1e-4); // depth -> major-axis distance
                blockers += 1.0;
            }
        }
        if (blockers < 0.5) return 1.0;
        float b = blockerSum / blockers;
        float radius = clamp(lightSize * max(major - b, 0.0) / max(b, 1e-3) / (2.0 * major), 0.75 * invMapSize, 0.1);
        // 24 taps (the Poisson set and its 90-degree turn at 0.7 of the radius) keep wide penumbrae smooth
        float sum = 0;
        [unroll] for (int k = 0; k < 12; ++k) {
            float2 o = mul(kPoisson[k], R) * radius;
            sum += pointMap.SampleCmpLevelZero(cmp, float3(uv + o, slice), pz - 0.00002);
            float2 o2 = float2(-o.y, o.x) * 0.7;
            sum += pointMap.SampleCmpLevelZero(cmp, float3(uv + o2, slice), pz - 0.00002);
        }
        return sum / 24.0;
    } else if (l.shadowType > 1.5) { // Soft: PCSS over the 90-degree face
        return PerspectivePcss(pointMap, cmp, uv, slice, pz - 0.00002, nearZ, farZ, 1.0, l.shadowSoftness, dot(n, ld),
                               PositionRotation(wp), invMapSize);
    } else { // Hard
        float r = 0.75 * invMapSize;
        float sum = 0;
        [unroll] for (int k = 0; k < 4; ++k) {
            float2 o = float2((k & 1) ? r : -r, (k & 2) ? r : -r);
            sum += pointMap.SampleCmpLevelZero(cmp, float3(uv + o, slice), pz - 0.00002);
        }
        return sum * 0.25;
    }
}

#endif
