// Scene pass: MMD toon materials with modern lighting (cascaded shadows through the toon
// ramp, punctual lights, hemispheric fill, rim), plus the sky backdrop and studio floor.
// Writes MRT: 0 = linear HDR colour (alpha = coverage), 1 = oct view normal / reflectivity /
// coverage, 2 = velocity (uv current - uv previous).
#include "common.hlsli"
#include "skinning.hlsli"
#ifdef RT_SHADOWS
#include "rt_common.hlsli"
#endif
#include "pbr_common.hlsli"

cbuffer MaterialCB : register(b1) {
    float4 gDiffuse; float3 gSpecular; float gSpecularPower; float3 gAmbient; float gEdgeSize;
    float4 gEdgeColor; uint gFlags; float gReflectivity;
    uint gPackClass; uint gPackHeadBone;   // shader packs only (pack_api.hlsli); the default shading ignores them
    // material morph factors (ApplyTexFactor): texture, sphere, toon
    float4 gTexMul; float4 gTexAdd; float4 gSphereMul; float4 gSphereAdd; float4 gToonMul; float4 gToonAdd;
    float4 gPackHead;        // xyz = head bone bind position (model space), w = 1 when the model has a head bone
    float4 gPackParams[4];   // the pack's 16 parameters (pack.json order)
};

// Per-material PBR root constants (b3, root param 14 in ScenePass). Set every draw; gPbrActive is 0 unless the run
// uses --material-source ray, so the default raster stays byte-identical (the branch is not taken and the buffer
// reads dead-code eliminate). A shader pack shades from its own path and ignores these.
cbuffer PbrRoot : register(b3) {
    uint gPbrSrv;      // SrvHeap index of the model's GpuPbrMaterial buffer (0xFFFFFFFF = none)
    uint gPbrIndex;    // material index into that buffer (0xFFFFFFFF = no record)
    uint gPbrDebug;    // PbrDebug channel 0..7
    uint gPbrActive;   // 0 = no PBR record for this draw
};
#ifndef MAT_HAS_TEXTURE
#define MAT_HAS_TEXTURE 1u
#define MAT_HAS_TOON    2u
#define MAT_SPHERE_MUL  4u
#define MAT_SPHERE_ADD  8u
#define MAT_RECEIVE     16u
#define MAT_STAGE       32u
#define MAT_TOON_MAP    64u
#define MAT_FLAT        128u
#define MAT_HAS_SPHERE_TEX 256u
#endif

// Shader-pack material class: the PackClass lives in bits 0..7 of gPackClass, bits 8..15 hold the class's alpha key
// (round(key * 255), pack.json "alphaKey"). Every consumer masks the class with PackClassLow; the key is PackAlphaKey.
#define PackClassLow(gc)    ((gc) & 0xFFu)
#define PackAlphaKey(gc)    (float(((gc) >> 8) & 0xFFu) * (1.0 / 255.0))

// FXC ignores pack_matrix for structured-buffer elements: state row_major explicitly.
struct BoneMatrix { row_major float4x4 m; };
StructuredBuffer<BoneMatrix> gBones : register(t0);
Texture2D gTexture : register(t1);
Texture2D gSphere  : register(t2);
Texture2D gToon    : register(t3);
StructuredBuffer<BoneMatrix> gPrevBones : register(t4);
Texture2DArray<float> gShadowMap : register(t5);
struct Light {
    float3 pos; float invRange;
    float3 color; float cosOuter;
    float3 dir; float cosInner;
    float shadowSlice; float shadowType; float shadowSoftness; float shadowDensity;
    float3 shadowColor; float falloff;
    float affectDiffuse; float affectSpecular; float pointShadowSlice; float pad;
    float areaWidth; float areaHeight; float isArea; float pad2;
};
StructuredBuffer<Light> gLights : register(t6);
Texture2DArray<float> gSpotShadowMap : register(t7);
Texture2DArray<float> gPointShadowMap : register(t8);
SamplerState gWrap  : register(s0);
SamplerState gClamp : register(s1);
SamplerComparisonState gShadowCmp : register(s2);

struct VSIn {
    float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0;
    uint4 bones : BLENDINDICES; float4 weights : BLENDWEIGHT; float edge : TEXCOORD1;
    float3 morph : TEXCOORD2; float3 prevMorph : TEXCOORD3;
    float3 sdefC : TEXCOORD4; float3 sdefR0 : TEXCOORD5; float3 sdefR1 : TEXCOORD6; float sdef : TEXCOORD7;
    float4 uv1 : TEXCOORD8; float4 uv2 : TEXCOORD9;   // additional UV1/UV2 (zero without them)
};
struct VSOut {
    float4 pos : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 nrm : NORMAL;
    float2 uv : TEXCOORD1;
    float4 curClip : TEXCOORD2;
    float4 prevClip : TEXCOORD3;
    float viewZ : TEXCOORD4;
    float4 uv1 : TEXCOORD5;   // additional UV1 (zero without)
    float4 uv2 : TEXCOORD6;   // additional UV2 (zero without)
};
struct PSOut {
    float4 color : SV_Target0;
    float4 normal : SV_Target1;
    float2 velocity : SV_Target2;
};

// Current / previous frame skinning (skinning.hlsli: linear blend or SDEF).
void Skin(VSIn v, out float3 wp, out float3 wn) {
    SkinVertex(gBones[v.bones.x].m, gBones[v.bones.y].m, gBones[v.bones.z].m, gBones[v.bones.w].m, v.weights,
               v.pos + v.morph, v.nrm, v.sdef, v.sdefC, v.sdefR0, v.sdefR1, wp, wn);
}
float3 PrevSkinPosition(VSIn v) {
    float3 wp, wn;
    SkinVertex(gPrevBones[v.bones.x].m, gPrevBones[v.bones.y].m, gPrevBones[v.bones.z].m, gPrevBones[v.bones.w].m,
               v.weights, v.pos + v.prevMorph, v.nrm, v.sdef, v.sdefC, v.sdefR0, v.sdefR1, wp, wn);
    return wp;
}

float2 Velocity(float4 curClip, float4 prevClip) {
    float2 c = curClip.xy / curClip.w;
    float2 p = prevClip.xy / prevClip.w;
    return (c - p) * float2(0.5, -0.5);
}

VSOut VSMain(VSIn v) {
    VSOut o;
    float3 wp3, wn;
    Skin(v, wp3, wn);
    float4 wp = float4(wp3, 1.0);
    float4 pwp = float4(PrevSkinPosition(v), 1.0);
    o.pos = mul(wp, gViewProj);
    o.worldPos = wp.xyz;
    o.nrm = wn;
    o.uv = v.uv;
    o.curClip = mul(wp, gViewProjNoJitter);
    o.prevClip = mul(pwp, gPrevViewProjNoJitter);
    o.viewZ = mul(wp, gView).z;
    o.uv1 = v.uv1;
    o.uv2 = v.uv2;
    return o;
}

// ---- shadows ------------------------------------------------------------------

static const float2 kPoisson[12] = {
    float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696, 0.457), float2(-0.203, 0.621),
    float2(0.962, -0.195), float2(0.473, -0.480), float2(0.519, 0.767), float2(0.185, -0.893),
    float2(0.507, 0.064), float2(0.896, 0.412), float2(-0.322, -0.933), float2(-0.792, -0.598)
};

// ---- soft shadows (PCSS) ----
// Soft shadow maps match the ray paths' cone (SoftShadowAngle): a blocker search, then a PCF disk as wide as the
// penumbra an angular light of that size casts from the average blocker distance (contact hardening).
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
float PcssFilter(Texture2DArray<float> map, float2 uv, float slice, float z, float radiusUv, float zPerUv, float rot) {
    float sum = 0;
    [unroll] for (int k = 0; k < 32; ++k) {
        float2 o = VogelTap(k, 32, rot);
        sum += map.SampleCmpLevelZero(gShadowCmp, float3(uv + o * radiusUv, slice), z - length(o) * radiusUv * zPerUv);
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
float PerspectivePcss(Texture2DArray<float> map, float2 uv, float slice, float z, float nearZ, float farZ, float tanHalf,
                      float softness, float ndl, float rot, float invMapSize) {
    float tanA = tan(SoftShadowAngle(0.0, softness));
    float minUv = 0.75 * invMapSize;
    float d = nearZ * farZ / max(farZ - z * (farZ - nearZ), 1e-4);
    float worldPerUv = 2.0 * d * tanHalf;
    float searchUv = clamp(tanA / (2.0 * tanHalf), minUv, kPcssMaxUv);
    float zb = PcssBlocker(map, uv, slice, z, searchUv, rot, 1.0 / invMapSize);
    if (zb < 0.0) return 1.0;
    float db = nearZ * farZ / max(farZ - zb * (farZ - nearZ), 1e-4);
    float radius = clamp(max(d - db, 0.0) * tanA / worldPerUv, minUv, kPcssMaxUv);
    float zPerUv = worldPerUv * ReceiverSlope(ndl) * farZ * nearZ / ((farZ - nearZ) * d * d);
    return PcssFilter(map, uv, slice, z, radius, zPerUv, rot);
}

// Per-position rotation, constant over time: TAA is often off (and always for raster videos), so a per-frame
// rotation would shimmer.
float PositionRotation(float3 wp) { return frac(sin(dot(wp, float3(12.9898, 78.233, 37.719))) * 43758.5453) * 6.2831853; }

float ShadowCascade(float3 wp, float3 n, int c, float2 pixel) {
    float3 L = -gLightDir;
    float ndl = saturate(dot(n, L));
    float offset = gCascadeTexel[c] * gShadowParams.y * (1.5 - ndl);
    float4 sp = mul(float4(wp + n * offset, 1.0), gShadowViewProj[c]);
    float3 p = sp.xyz / sp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0) return 1.0;
    float a = Ign(pixel) * 6.2831853;
    if (gSunShadowParams.x > 1.5) {
        // Soft: orthographic cascade, so depth is linear over (2 * radius + reach) world units
        float tanA = tan(SoftShadowAngle(0.0, gSunShadowParams.y));
        float worldPerUv = gCascadeTexel[c] / gShadowParams.x;    // 2 * cascade radius
        float depthRange = worldPerUv + gCascadeTexel.w;
        float minUv = gShadowParams.z * gShadowParams.x;
        float z = p.z - 0.0004;
        float searchUv = clamp(tanA * 20.0 / worldPerUv, minUv, kPcssMaxUv);   // blockers up to ~20 units away
        float zb = PcssBlocker(gShadowMap, uv, c, z, searchUv, a, 1.0 / gShadowParams.x);
        if (zb < 0.0) return 1.0;
        float radius = clamp((z - zb) * depthRange * tanA / worldPerUv, minUv, kPcssMaxUv);
        return PcssFilter(gShadowMap, uv, c, z, radius, worldPerUv * ReceiverSlope(ndl) / depthRange, a);
    }
    float2x2 rot = float2x2(cos(a), -sin(a), sin(a), cos(a));
    float radius = gShadowParams.z * gShadowParams.x;
    float sum = 0;
    [unroll] for (int i = 0; i < 12; ++i) {
        float2 o = mul(kPoisson[i], rot) * radius;
        sum += gShadowMap.SampleCmpLevelZero(gShadowCmp, float3(uv + o, c), p.z - 0.0004);
    }
    return sum / 12.0;
}

float Shadow(float3 wp, float3 n, float viewZ, float2 pixel) {
    if (gCascadeSplits.w < 0.5) return 1.0;
    int c = viewZ < gCascadeSplits.x ? 0 : (viewZ < gCascadeSplits.y ? 1 : 2);
    if (viewZ > gCascadeSplits.z) return 1.0;
    float s = ShadowCascade(wp, n, c, pixel);
    // fade out at the far end of the last cascade
    float fade = saturate((gCascadeSplits.z - viewZ) / (gCascadeSplits.z * 0.15));
    return lerp(1.0, s, fade);
}

#ifdef RT_SHADOWS
// Two cone-jittered rays toward the sun (soft penumbra), honouring alpha-tested casters.
float ShadowRt(float3 wp, float3 n, float viewZ, float2 pixel) {
    if (gCascadeSplits.w < 0.5 || gSunShadowParams.x < 0.5) return 1.0;
    float3 L = -gLightDir;
    float3 origin = wp + n * (0.02 + viewZ * 0.0004) + L * 0.01;
    uint rng = RngSeed((uint2)pixel, (uint)gFrameIndex, 7u);
    float vis = 0;
    float cosCone = SunShadowConeCos(0.99993);
    // Soft cones reach 15 degrees: four rays keep the wide penumbra from going grainy
    int rays = (gSunShadowParams.x > 1.5) ? 4 : 2;
    [loop] for (int k = 0; k < rays; ++k)
        vis += TraceShadowRay(origin, SampleCone(float2(Rand(rng), Rand(rng)), L, cosCone), 2000.0);
    return vis / (float)rays;
}
#define SHADOW_TERM(wp, n, viewZ, pixel) ShadowRt(wp, n, viewZ, pixel)
#else
#define SHADOW_TERM(wp, n, viewZ, pixel) Shadow(wp, n, viewZ, pixel)
#endif

// ---- punctual lights ------------------------------------------------------------

// Spot shadow map (perspective slice per spot): normal offset scaled with the texel footprint,
// 2x2 hardware PCF taps (Hard) or 12 Poisson taps (Soft).
float SpotShadow(Light l, float3 wp, float3 n, float dist) {
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
        return PerspectivePcss(gSpotShadowMap, uv, slice, p.z - 0.00002, max(0.05, range * 0.004), max(range, 1.0), tanHalf,
                               l.shadowSoftness, dot(n, ld), PositionRotation(wp), gSpotShadowParams.y);
    } else { // Hard (today)
        float r = 0.75 * gSpotShadowParams.y;
        float sum = 0;
        [unroll] for (int k = 0; k < 4; ++k) {
            float2 o = float2((k & 1) ? r : -r, (k & 2) ? r : -r);
            sum += gSpotShadowMap.SampleCmpLevelZero(gShadowCmp, float3(uv + o, slice), p.z - 0.00002);
        }
        return sum * 0.25;
    }
}

// Point shadow map (cube-like depth array, 6 slices per light): major axis determines face,
// UV and projected depth derived analytically, 2x2 PCF (Hard) or 12 Poisson taps (Soft).
float PointShadow(Light l, float3 wp, float3 n, float dist) {
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
            float z = gPointShadowMap.Load(int4(min((int2)(suv * mapSize), (int)mapSize - 1), (int)slice, 0));
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
            sum += gPointShadowMap.SampleCmpLevelZero(gShadowCmp, float3(uv + o, slice), pz - 0.00002);
            float2 o2 = float2(-o.y, o.x) * 0.7;
            sum += gPointShadowMap.SampleCmpLevelZero(gShadowCmp, float3(uv + o2, slice), pz - 0.00002);
        }
        return sum / 24.0;
    } else if (l.shadowType > 1.5) { // Soft: PCSS over the 90-degree face
        return PerspectivePcss(gPointShadowMap, uv, slice, pz - 0.00002, nearZ, farZ, 1.0, l.shadowSoftness, dot(n, ld),
                               PositionRotation(wp), invMapSize);
    } else { // Hard
        float r = 0.75 * invMapSize;
        float sum = 0;
        [unroll] for (int k = 0; k < 4; ++k) {
            float2 o = float2((k & 1) ? r : -r, (k & 2) ? r : -r);
            sum += gPointShadowMap.SampleCmpLevelZero(gShadowCmp, float3(uv + o, slice), pz - 0.00002);
        }
        return sum * 0.25;
    }
}

#ifdef RT_SHADOWS
// Two cone-jittered rays toward the point light, or random points on the area light, honouring alpha-tested casters.
float PointShadowRt(Light l, float3 wp, float3 n, float dist, uint lightIndex) {
    if (l.shadowType < 0.5 || l.pointShadowSlice < 0.0) return 1.0;
    float3 ld = (l.pos - wp) / max(dist, 1e-4);
    float3 origin = wp + n * 0.02 + ld * 0.01;
    uint2 seedCoord = (uint2)abs(float2(wp.x * 37.0 + wp.y * 17.0, wp.z * 37.0 + wp.y * 11.0));
    uint rng = RngSeed(seedCoord, (uint)gFrameIndex, 17u + lightIndex);
    float vis = 0;
    if (l.isArea > 0.5) {
        float3 norm = l.dir;
        float3 c = cross(float3(0, 1, 0), norm);
        float cLen = length(c);
        float3 right = (cLen > 1e-4) ? (c / cLen) : float3(1, 0, 0);
        float3 up = cross(norm, right);
        float softMul = (l.shadowType > 1.5) ? (1.0 + l.shadowSoftness * 1.5) : 1.0;
        float w = l.areaWidth * softMul;
        float h = l.areaHeight * softMul;
        [unroll] for (int k = 0; k < 2; ++k) {
            float u = (Rand(rng) - 0.5) * w;
            float v = (Rand(rng) - 0.5) * h;
            float3 samplePos = l.pos + right * u + up * v;
            float3 rayDelta = samplePos - origin;
            float rayDist = length(rayDelta);
            float3 rayDir = rayDelta / max(rayDist, 1e-4);
            vis += TraceShadowRayMasked(origin, rayDir, max(rayDist - 0.05, 0.0), RT_MASK_CHARACTER);
        }
        return vis * 0.5;
    }
    float cosCone = (l.shadowType > 1.5) ? SoftShadowConeCos(0.99993, l.shadowSoftness) : 0.99993;
    [unroll] for (int k = 0; k < 2; ++k) {
        float3 rayDir = SampleCone(float2(Rand(rng), Rand(rng)), ld, cosCone);
        vis += TraceShadowRayMasked(origin, rayDir, max(dist - 0.05, 0.0), RT_MASK_CHARACTER);
    }
    return vis * 0.5;
}
#endif

float3 PunctualDiffuse(float3 wp, float3 n, float toonSoft, float flat) {
    float3 sum = 0;
    uint count = (uint)gNumLights;
    for (uint i = 0; i < count; ++i) {
        Light l = gLights[i];
        if (l.affectDiffuse <= 0.0) continue;
        float3 d = l.pos - wp;
        float dist = length(d);
        float3 ld = d / max(dist, 1e-4);
        float atten = PunctualFalloff(dist, l.invRange, l.falloff);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
        float rawVis = 1.0;
        if (atten > 0.0) {
            if (l.cosOuter > -1.0) {
                rawVis = SpotShadow(l, wp, n, dist);
            } else {
#ifdef RT_SHADOWS
                rawVis = PointShadowRt(l, wp, n, dist, i);
#else
                rawVis = PointShadow(l, wp, n, dist);
#endif
            }
        }
        float3 shadowTerm = ShadowTransmission(rawVis, l.shadowDensity, l.shadowColor);
        float ndl = dot(n, ld);
        float diff = lerp(saturate(ndl), smoothstep(-0.05, 0.25, ndl), toonSoft);
        diff = lerp(diff, saturate(ndl * 0.3 + 0.7), flat);
        sum += l.color * shadowTerm * atten * diff;
    }
    return sum;
}

float3 PunctualSpecular(float3 wp, float3 n, float3 V, float power) {
    float3 sum = 0;
    uint count = (uint)gNumLights;
    for (uint i = 0; i < count; ++i) {
        Light l = gLights[i];
        if (l.affectSpecular <= 0.0) continue;
        float3 d = l.pos - wp;
        float dist = length(d);
        float3 ld = d / max(dist, 1e-4);
        float atten = PunctualFalloff(dist, l.invRange, l.falloff);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
        float rawVis = 1.0;
        if (atten > 0.0) {
            if (l.cosOuter > -1.0) {
                rawVis = SpotShadow(l, wp, n, dist);
            } else {
#ifdef RT_SHADOWS
                rawVis = PointShadowRt(l, wp, n, dist, i);
#else
                rawVis = PointShadow(l, wp, n, dist);
#endif
            }
        }
        float3 shadowTerm = ShadowTransmission(rawVis, l.shadowDensity, l.shadowColor);
        float3 h = normalize(ld + V);
        sum += l.color * shadowTerm * atten * pow(saturate(dot(n, h)), power) * saturate(dot(n, ld));
    }
    return sum;
}

float3 Hemisphere(float3 n) {
    return lerp(gGroundColor, gSkyZenith, saturate(n.y * 0.5 + 0.5)) * gSunIntensity;
}

// GGX specular for the punctual lights (P3): the same attenuation / shadow term as PunctualSpecular, but with the
// GGX lobe instead of Blinn. Added + scaled by the caller (SunIntensity lives in the light colour there).
float3 PunctualSpecularGGX(float3 wp, float3 n, float3 V, float rough, float3 f0) {
    float3 sum = 0;
    uint count = (uint)gNumLights;
    for (uint i = 0; i < count; ++i) {
        Light l = gLights[i];
        if (l.affectSpecular <= 0.0) continue;
        float3 d = l.pos - wp;
        float dist = length(d);
        float3 ld = d / max(dist, 1e-4);
        float atten = PunctualFalloff(dist, l.invRange, l.falloff);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
        if (atten <= 0.0) continue;
        float rawVis;
        if (l.cosOuter > -1.0) {
            rawVis = SpotShadow(l, wp, n, dist);
        } else {
#ifdef RT_SHADOWS
            rawVis = PointShadowRt(l, wp, n, dist, i);
#else
            rawVis = PointShadow(l, wp, n, dist);
#endif
        }
        float3 shadowTerm = ShadowTransmission(rawVis, l.shadowDensity, l.shadowColor);
        sum += SrgbToLinear(saturate(l.color)) * shadowTerm * atten * PbrSpecularGGX(n, V, ld, rough, f0);
    }
    return sum;
}

PSOut PackOutput(float3 color, float alpha, float3 worldNormal, float reflectivity, float4 curClip, float4 prevClip) {
    PSOut o;
    o.color = float4(color, alpha);
    float3 vn = normalize(mul(worldNormal, (float3x3)gView));
    o.normal = float4(OctEncode(vn), reflectivity, alpha);
    o.velocity = Velocity(curClip, prevClip);
    return o;
}

// ---- MMD material -----------------------------------------------------------------

// ---- per-material PBR debug view (work unit P2) ---------------------------------------------
// A material with a PBR record outputs one channel (CLI --pbr-debug); materials without one render as before.
// The buffer is read through the model's SRV (absolute SrvHeap index), as the ray paths do.
PSOut PbrDebugOutput(in VSOut i, in GpuPbrMaterial m, float3 n, float alpha) {
    float2 uv = i.uv;
    float3 outColor;
    if (gPbrDebug == 1u) {          // albedo (Ray's linear albedo)
        outColor = saturate(PbrAlbedo(m, uv, gDiffuse.rgb));
    } else if (gPbrDebug == 2u) {   // normal: tangent-space normal into the world
        outColor = PbrWorldNormal(m, n, i.worldPos, uv) * 0.5 + 0.5;
    } else if (gPbrDebug == 3u) {   // roughness = SmoothnessToRoughness(smoothness)
        outColor = PbrSlotRoughness(m.slots[PBR_SLOT_SMOOTHNESS], uv).xxx;
    } else if (gPbrDebug == 4u) {   // metalness
        outColor = PbrSlotMetalness(m.slots[PBR_SLOT_METALNESS], uv).xxx;
    } else if (gPbrDebug == 5u) {   // ambient occlusion
        outColor = PbrSlotAo(m.slots[PBR_SLOT_OCCLUSION], uv).xxx;
    } else if (gPbrDebug == 6u) {   // emissive
        outColor = saturate(PbrEmissive(m, uv));
    } else {                        // shading model as a distinct hue
        outColor = PbrModelHue(m.shadingModel);
    }
    return PackOutput(outColor, alpha, n, gReflectivity, i.curClip, i.prevClip);
}

// MMD colour model, unchanged (P2 keeps the default look byte-identical).
PSOut MmdShade(VSOut i, bool front, float3 n, float3 L, float3 V, float4 tex, float alpha) {
    // MMD colour model (gamma space): saturate(ambient + diffuse * light) * texture * sphere.
    float3 lit = saturate(gAmbient + gDiffuse.rgb * gLightColor) * tex.rgb;
    // Albedo for the modern fill terms. MMD diffuse is a light-response factor, and rigs use
    // either d=1.0/a=0.5 or d=0.8/a=0.6 for the same "fully lit = texture" look; base the
    // characters' albedo on that fully-lit colour so both conventions get the same fill.
    float3 albedo = (gFlags & MAT_STAGE) ? gDiffuse.rgb * tex.rgb
                                         : saturate(gAmbient + gDiffuse.rgb) * 0.8 * tex.rgb;
    if (gFlags & (MAT_SPHERE_MUL | MAT_SPHERE_ADD)) {
        float3 nv = normalize(mul(n, (float3x3)gView));
        float2 suv = nv.xy * float2(0.5, -0.5) + 0.5;
        float3 s = ApplyTexFactor3(gSphere.Sample(gClamp, suv).rgb, gSphereMul, gSphereAdd, (gFlags & MAT_SPHERE_MUL) ? 1.0 : 0.0);
        if (gFlags & MAT_SPHERE_MUL) { lit *= s; albedo *= s; } else { lit += s; }
    }

    float rawSh = (gFlags & MAT_RECEIVE) ? SHADOW_TERM(i.worldPos, n, i.viewZ, i.pos.xy) : 1.0;
    float occ = (1.0 - rawSh) * gSunShadowParams.z;
    float sh = 1.0 - occ;
    float ndl = dot(n, L);
    float flat = (gFlags & MAT_FLAT) ? 1.0 : 0.0;
    float3 c;
    if (gFlags & MAT_TOON_MAP) {
        // Project Sekai layout: the "toon" is the painted shadow colour at the same UV.
        // Hard terminator around N.L = 0 (their _SekaiShadowThreshold 0.5 on half-Lambert).
        float3 shadowTex = ApplyTexFactor3(gToon.Sample(gWrap, i.uv).rgb, gToonMul, gToonAdd, 1.0);
        float term = sh * lerp(smoothstep(-0.03, 0.06, ndl), 1.0, flat);
        float3 light = saturate(gAmbient + gDiffuse.rgb * gLightColor);
        c = lerp(light * shadowTex, lit, term);
    } else if (gFlags & MAT_HAS_TOON) {
        // The cast shadow pushes the lookup toward the dark end of the material's own ramp.
        float v = lerp(1.0, saturate(0.5 - 0.5 * ndl), sh);
        c = lit * ApplyTexFactor3(gToon.Sample(gClamp, float2(0.5, v)).rgb, gToonMul, gToonAdd, 1.0);
    } else if (gFlags & MAT_STAGE) {
        c = lerp(lit * 0.62, lit, sh);
    } else {
        c = lit;   // unmodelled (flat) character material: only cast shadows below
    }
    if (!(gFlags & (MAT_STAGE | MAT_TOON_MAP))) {
        // Hue-preserving shadow (c * c, slightly cooled): light MMD toon ramps alone read as
        // unlit; this keeps skin warm and hair rich on the shaded side. Flat materials skip
        // the N.L terminator and take a softened cast shadow only.
        float term = lerp(sh * smoothstep(-0.12, 0.22, ndl), lerp(1.0, sh, 0.8), flat);
        float3 shade = c * lerp(1.0, saturate(c), 0.55) * float3(0.90, 0.90, 0.96);
        c = lerp(shade, c, term);
    }
    c += lit * gSunShadowColor.rgb * occ;
    if (gSpecularPower > 0.0) {
        float3 h = normalize(L + V);
        c += pow(saturate(dot(h, n)), gSpecularPower) * gSpecular * gLightColor * sh;
    }

    float3 albedoLin = SrgbToLinear(saturate(albedo));
    float3 color = SrgbToLinear(saturate(c)) * gSunIntensity;
    // Flat materials take an even fill (no sky/ground gradient modelling the face).
    float3 hemi = lerp(Hemisphere(n), lerp(gGroundColor, gSkyZenith, 0.65) * gSunIntensity, flat * 0.85);
    color += albedoLin * hemi * gHemiStrength;
    color += albedoLin * PunctualDiffuse(i.worldPos, n, 1.0, flat);

    float reflectivity = gReflectivity;
    if (gFlags & MAT_STAGE) {
        float floorMask = smoothstep(0.82, 0.97, n.y);
        reflectivity = saturate(reflectivity + gFloorGloss * floorMask);
        color += PunctualSpecular(i.worldPos, n, V, 48.0) * reflectivity;
    } else {
        // anime rim light: grazing angles on the side the key light comes from
        float rim = pow(1.0 - saturate(dot(n, V)), 4.0);
        float side = smoothstep(-0.2, 0.5, dot(n, L) + 0.25);
        color += gRimColor * rim * side * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
    }
    return PackOutput(color, alpha, n, alpha > 0.9 ? reflectivity : 0.0, i.curClip, i.prevClip);
}

// ---- PBR material shading (P3) --------------------------------------------------------------
// Analytic split-sum environment BRDF (Karis 2014 after Lazarov 2013): F0 * A + B for perceptual roughness `pr`.
float3 PbrEnvBrdfApprox(float3 f0, float pr, float nv) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = pr * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// "PBR on top of the MMD look": the MMD structure above stays; for a material with a PBR record only its inputs /
// terms change (mapped normal, albedo / alpha, sphere ignored, metalness, GGX + env specular, AO, emissive, and the
// per-model lobes). Sun / hemispheric / punctual diffuse keeps the MMD look.
PSOut MmdShadePbr(VSOut i, bool front, GpuPbrMaterial m, float3 n0, float4 tex, float alpha) {
    const uint model = m.shadingModel;
    const bool celLike = (model == 8u || model == 9u);   // CEL / TONEBASED: MMD shading + normal + emissive only
    float2 uv = i.uv;

    // Mapped normal (NORMAL + NORMAL_SUB) replaces n for every term, including the written normal.
    float3 n = PbrWorldNormal(m, n0, i.worldPos, uv);
    float3 L = -gLightDir;
    float3 V = normalize(gEyePos - i.worldPos);

    // ALPHA slot cut-out: only the Cutout variants of the material file enforce a hard alpha threshold (0.2/0.5/
    // 0.75, PBR_FLAG_CUTOUT). The standard 0.999 threshold is an opaque-pass detail that would otherwise kill
    // blended materials in this single-pass engine; those keep the caller's gDiffuse.a * texture alpha coverage.
    if ((m.flags & PBR_FLAG_CUTOUT) && PbrAlpha(m, uv) < m.alphaThreshold) discard;

    // ALBEDO (+ sub blend) replaces the texture; a slot that reads the PMX texture (FROM 3) is unchanged by
    // construction. Sphere map is ignored for PBR materials (Ray-MMD does not use it). CEL / TONEBASED keep the
    // MMD shading, so they keep the plain texture as the diffuse colour. The MMD colour pipeline below is
    // gamma-space, so the linear PBR albedo goes back through LinearToSrgb for the `c` term.
    float3 pbrAlbedo = PbrAlbedo(m, uv, gDiffuse.rgb);          // linear
    float3 pbrAlbedoGamma = LinearToSrgb(saturate(pbrAlbedo));
    float3 celAlbedo = (gFlags & MAT_STAGE) ? gDiffuse.rgb * tex.rgb
                                            : saturate(gAmbient + gDiffuse.rgb) * 0.8 * tex.rgb;
    float3 diffAlbedo = celLike ? celAlbedo : pbrAlbedoGamma;   // gamma, for the MMD diffuse term

    float rawSh = (gFlags & MAT_RECEIVE) ? SHADOW_TERM(i.worldPos, n, i.viewZ, i.pos.xy) : 1.0;
    float occAmt = (1.0 - rawSh) * gSunShadowParams.z;
    float sh = 1.0 - occAmt;
    float ndl = dot(n, L);
    const float flat = (gFlags & MAT_FLAT) ? 1.0 : 0.0;
    const float3 mddLight = saturate(gAmbient + gDiffuse.rgb * gLightColor);   // MMD diffuse * light (gamma)
    const float3 sunLin = SrgbToLinear(saturate(gLightColor)) * gSunIntensity;

    // TONEBASED (9): the MMD shading is kept, but the diffuse is replaced by a hardness toon toward the customB
    // shadow colour. Normal + emissive only otherwise, per the plan.
    if (model == 9u) {
        float hard = saturate(PbrCustomA(m, uv));
        float3 shadowColor = PbrCustomB(m, uv);
        float litAmt = saturate(ndl * hard + 1.0 - hard) * sh;
        float3 c9 = SrgbToLinear(saturate(mddLight * tex.rgb)) * lerp(shadowColor, 1.0, litAmt) * gSunIntensity;
        float3 rim9 = pow(1.0 - saturate(dot(n, V)), 4.0) * smoothstep(-0.2, 0.5, dot(n, L) + 0.25);
        c9 += gRimColor * rim9 * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
        c9 += saturate(PbrEmissive(m, uv));
        return PackOutput(c9, alpha, n, alpha > 0.9 ? gReflectivity : 0.0, i.curClip, i.prevClip);
    }

    // MMD diffuse colour model (gamma). CEL keeps the toon ramp.
    float metal = celLike ? 0.0 : saturate(PbrSlotMetalness(m.slots[PBR_SLOT_METALNESS], uv));
    float3 cAlbedo = diffAlbedo * (1.0 - metal);   // metalness removes the diffuse
    float3 c = mddLight * cAlbedo;
    if (model == 8u) {
        float v = lerp(1.0, saturate(0.5 - 0.5 * ndl), sh);
        c = mddLight * tex.rgb * ApplyTexFactor3(gToon.Sample(gClamp, float2(0.5, v)).rgb, gToonMul, gToonAdd, 1.0);
    } else if (!celLike) {
        // Hue-preserving shadow, kept for the PBR albedo too (MMD look).
        float term = lerp(sh * smoothstep(-0.12, 0.22, ndl), lerp(1.0, sh, 0.8), flat);
        float3 shade = c * lerp(1.0, saturate(c), 0.55) * float3(0.90, 0.90, 0.96);
        c = lerp(shade, c, term);
    }
    c += mddLight * cAlbedo * gSunShadowColor.rgb * occAmt;

    float3 color = SrgbToLinear(saturate(c)) * gSunIntensity;

    // Lambert metalness: the fill terms use (1 - metalness) too.
    float3 f0mma = PbrSlotF0(m.slots[PBR_SLOT_SPECULAR], uv);
    float3 albedoLin = celLike ? SrgbToLinear(saturate(celAlbedo)) : saturate(pbrAlbedo);
    float3 diffLin = albedoLin * (1.0 - metal);
    float3 f0 = celLike ? f0mma : lerp(f0mma, max(0.02, albedoLin), metal);
    float rough = max(PbrSlotRoughness(m.slots[PBR_SLOT_SMOOTHNESS], uv), 0.03);
    float ao = PbrSlotAo(m.slots[PBR_SLOT_OCCLUSION], uv);

    bool specularOn = (model != 8u);
    bool envSpecOn = !celLike;
    float specScale = 1.0;
    if (model == 1u) {   // SKIN: strict look policy (soft SSS tint, specular x <= 0.15, no env specular)
        float curve = PbrCustomA(m, uv);
        float3 trans = PbrCustomB(m, uv);
        float ss = pow(1.0 - saturate(ndl), 2.0) * pow(1.0 - saturate(-ndl), 3.0 / (curve + 0.001)) *
                   saturate(curve - 0.04);
        color += trans * (ss * 0.7) * sunLin * sh;
        rough = max(rough, 0.6);
        specScale = 0.15;
        envSpecOn = false;
    }

    // Hemispheric + punctual diffuse fill, scaled by (1 - metalness) and the PBR AO.
    float3 hemi = lerp(Hemisphere(n), lerp(gGroundColor, gSkyZenith, 0.65) * gSunIntensity, flat * 0.85);
    color += diffLin * hemi * gHemiStrength * ao;
    color += diffLin * PunctualDiffuse(i.worldPos, n, 1.0, flat) * ao;

    // Environment specular: the sky the path tracer reflects (SkyColor at full radiance, not the hemiStrength fill),
    // seen along the GGX dominant direction and pre-blurred toward the normal's sky for rough lobes, weighted by the
    // analytic split-sum DFG (Karis, "Physically Based Shading on Mobile") x AO. Metals get their colour from here.
    if (envSpecOn) {
        float nv = saturate(dot(n, V));
        float pr = sqrt(saturate(rough));   // perceptual roughness (rough is GGX alpha)
        float3 R = reflect(-V, n);
        float3 Rd = normalize(lerp(n, R, (1.0 - pr) * (sqrt(1.0 - pr) + pr)));
        float3 env = lerp(SkyColor(Rd), Hemisphere(n), pr * pr);
        color += PbrEnvBrdfApprox(f0, pr, nv) * env * ao;
    }

    // Sun specular (replaces the MMD Blinn), x N.L x light colour x shadow, per shading model.
    float nl = saturate(ndl);
    if (specularOn) {
        float3 spec;
        if (model == 3u) {                       // ANISOTROPY (hair): anisotropic GGX along the tangent
            float3 T, B;
            PbrTangentFrame(n, i.worldPos, uv, T, B);
            float aniso = PbrCustomA(m, uv);
            float3 cb = PbrCustomB(m, uv);
            float shift = cb.r;
            spec = PbrSpecularGGXAniso(n, V, L, rough, f0, aniso, T, shift);
        } else if (model == 5u) {                // CLOTH: GGX lerped with a sheen lobe (customB = fuzz colour)
            float3 fuzz = PbrCustomB(m, uv);
            float sheenAmt = PbrCustomA(m, uv);
            float3 ggx = PbrSpecularGGX(n, V, L, rough, f0);
            float3 sheen = PbrSpecularSheen(n, V, L, rough, fuzz);
            spec = lerp(ggx, sheen, saturate(sheenAmt));
        } else {
            spec = PbrSpecularGGX(n, V, L, rough, f0);
        }
        color += spec * sh * sunLin * nl * specScale;
        color += PunctualSpecularGGX(i.worldPos, n, V, rough, f0) * specScale;
    }

    // Reflectivity output (SSR / RT reflections): F0 luminance scaled by smoothness, so smooth / metal materials
    // mirror and rough dielectrics barely do. CEL keeps the MMD value. A stage with a record drops its floor gloss
    // (floorGloss is only for stage materials without a PBR record, per the plan).
    float reflectivity = celLike ? gReflectivity
                                 : saturate(dot(f0, float3(0.2126, 0.7152, 0.0722)) * (1.0 - rough));
    if (gFlags & MAT_STAGE) {
        color += PunctualSpecular(i.worldPos, n, V, 48.0) * reflectivity;
    } else {
        // Rim stays; metals tint it by F0 (lerp toward F0's luminance).
        float rim = pow(1.0 - saturate(dot(n, V)), 4.0);
        float side = smoothstep(-0.2, 0.5, dot(n, L) + 0.25);
        float3 rimTint = lerp(float3(1, 1, 1), dot(f0, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)).xxx, metal);
        color += gRimColor * rimTint * rim * side * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
    }

    // Emissive (linear * intensity) after lighting. EMISSIVE promotion (custom 2) is the same term; the value is
    // 0 for the library materials that leave EMISSIVE_ENABLE off. PbrEmissive returns linear already.
    color += saturate(PbrEmissive(m, uv));
    return PackOutput(color, alpha, n, alpha > 0.9 ? reflectivity : 0.0, i.curClip, i.prevClip);
}

// Per-material PBR branch entry (gPbrActive is 0 unless the run has Ray records for this draw). --pbr-debug wins
// over the PBR shading; the lit path only. The default run never enters either, so its frame is unchanged.
PSOut PSMain(VSOut i, bool front : SV_IsFrontFace) {
    PbrSetUvGradients(ddx(i.uv), ddy(i.uv));
    float3 n = normalize(i.nrm);
    if (!front) n = -n;
    float3 L = -gLightDir;
    float3 V = normalize(gEyePos - i.worldPos);

    float4 tex = (gFlags & MAT_HAS_TEXTURE) ? ApplyTexFactor(gTexture.Sample(gWrap, i.uv), gTexMul, gTexAdd)
                                            : float4(1, 1, 1, 1);
    float alpha = gDiffuse.a * tex.a;
    if (alpha < 0.004) discard;

    if (gPbrActive != 0u && gShading < 0.5) {
        GpuPbrMaterial pbr = PbrLoadMaterial(gPbrSrv, gPbrIndex);
        if (gPbrDebug != 0u) return PbrDebugOutput(i, pbr, n, alpha);
        return MmdShadePbr(i, front, pbr, n, tex, alpha);
    }

    // Unlit (gShading 1): texture * material diffuse colour only, still alpha-tested/blended.
    if (gShading > 0.5 && gShading < 1.5)
        return PackOutput(SrgbToLinear(saturate(gDiffuse.rgb * tex.rgb)), alpha, n, gReflectivity,
                          i.curClip, i.prevClip);

    return MmdShade(i, front, n, L, V, tex, alpha);
}

// ---- shader packs ---------------------------------------------------------------------
// Compiled only for a pack's PSO: MMDX_PACK is the pack's surface.hlsl (a quoted include path), which implements
// PackShade (contract: pack_api.hlsli). PSMain above is never affected.
#ifdef MMDX_PACK
#include "pack_api.hlsli"
#include MMDX_PACK

// The pack sets PACK_HAS_EDGE itself (surface.hlsl); without it the edge pass is unchanged.
#ifndef PACK_HAS_EDGE
#define PACK_HAS_EDGE 0
#endif

PSOut PSPack(VSOut i, bool front : SV_IsFrontFace) {
    PackSurface s;
    s.N = normalize(i.nrm);
    if (!front) s.N = -s.N;
    s.worldPos = i.worldPos;
    s.V = normalize(gEyePos - i.worldPos);
    s.L = -gLightDir;
    s.uv = i.uv;
    s.uv1 = i.uv1;
    s.uv2 = i.uv2;
    s.pixel = i.pos.xy;
    s.viewZ = i.viewZ;
    s.tex = (gFlags & MAT_HAS_TEXTURE) ? ApplyTexFactor(gTexture.Sample(gWrap, i.uv), gTexMul, gTexAdd) : float4(1, 1, 1, 1);
    s.alpha = gDiffuse.a * s.tex.a;
    if (s.alpha < 0.004) discard;
    // pack alpha key (colour-key cut-out): cut pixels whose texture red is below the key, before the pack shading
    const float alphaKey = PackAlphaKey(gPackClass);
    if (alphaKey > 0.0 && s.tex.r < alphaKey) discard;
    s.materialClass = (PackClassLow(gPackClass) == PACK_CLASS_OFF) ? PACK_BODY : PackClassLow(gPackClass);
    float rawShadow = (gFlags & MAT_RECEIVE) ? SHADOW_TERM(i.worldPos, s.N, i.viewZ, i.pos.xy) : 1.0;
    float occ = (1.0 - rawShadow) * gSunShadowParams.z;
    s.shadow = 1.0 - occ;
    PackHeadFrame(s);
    PackResult r = PackShade(s);
    r.color += SrgbToLinear(saturate(gDiffuse.rgb * s.tex.rgb)) * gSunShadowColor.rgb * occ * gSunIntensity;
    // negative reflectivity = "no ambient occlusion here" (composite.hlsl); SSR / RT reflections skip it as well
    float refl = r.noAo ? -1.0 : (r.alpha > 0.9 ? r.reflectivity : 0.0);
    return PackOutput(r.color, r.alpha, s.N, refl, i.curClip, i.prevClip);
}
#endif

// ---- inverted-hull edges ------------------------------------------------------------

struct EdgeOut { float4 pos : SV_Position; float4 curClip : TEXCOORD0; float4 prevClip : TEXCOORD1; };

float4 ExpandEdge(float4 clip, float3 wn, float px) {
    float2 dirPx = mul(float4(wn, 0.0), gViewProj).xy * gViewportSize;   // clip-space normal -> pixel space
    float len = length(dirPx);
    dirPx = len > 1e-6 ? dirPx / len : float2(0, 0);
    clip.xy += dirPx * px * 2.0 / gViewportSize * clip.w;
    return clip;
}

EdgeOut VSEdge(VSIn v) {
    EdgeOut o;
    float3 wp3, wn;
    Skin(v, wp3, wn);
    wn = normalize(wn);
    float4 wp = float4(wp3, 1.0);
    float4 pwp = float4(PrevSkinPosition(v), 1.0);
    float px = gEdgeSize * v.edge * gEdgeScale;                           // outline width in pixels
#if PACK_HAS_EDGE
    // pack outlines (PackEdge): per-material width scale; PackEdgeUv2 carries this vertex's uv2
    PackEdgeUv2 = v.uv2;
    uint edgeClass = (PackClassLow(gPackClass) == PACK_CLASS_OFF) ? PACK_BODY : PackClassLow(gPackClass);
    px *= max(PackEdge(edgeClass, gEdgeColor, gEdgeSize).widthScale, 0.0);
#endif
    o.pos = ExpandEdge(mul(wp, gViewProj), wn, px);
    o.curClip = ExpandEdge(mul(wp, gViewProjNoJitter), wn, px);
    o.prevClip = ExpandEdge(mul(pwp, gPrevViewProjNoJitter), wn, px);
    return o;
}

PSOut PSEdge(EdgeOut i) {
    PSOut o;
    o.color = float4(SrgbToLinear(gEdgeColor.rgb) * gSunIntensity * 0.85, gEdgeColor.a);
    o.normal = float4(OctEncode(float3(0, 0, -1)), 0.0, gEdgeColor.a);
    o.velocity = Velocity(i.curClip, i.prevClip);
    return o;
}

// Depth only (path tracer outlines, ScenePass in PT mode): the edges' depth pre-pass, the alpha test of the ray
// tracer's binary alpha (>= 0.5), as offline_edge.hlsl PSDepth. Colour writes are masked off by the PSO.
// Pack alpha key (pack.json "alphaKey"): a keyed material does not write depth for its cut pixels.
void PSDepthAlpha(VSOut i) {
    float4 tex = (gFlags & MAT_HAS_TEXTURE) ? gTexture.Sample(gWrap, i.uv) : float4(1, 1, 1, 1);
    float a = gDiffuse.a * tex.a;
    clip(a - 0.5);
    const float alphaKey = PackAlphaKey(gPackClass);
    if (alphaKey > 0.0) clip(tex.r - alphaKey);
}

// Pack outlines (PACK_HAS_EDGE in the pack's surface.hlsl): PackEdge sets the colour and width scale.
// Compiled only for the pack's edge PSO (MMDX_PACK); otherwise the default PSEdge above draws.
#if PACK_HAS_EDGE
PSOut PSEdgePack(EdgeOut i) {
    uint edgeClass = (PackClassLow(gPackClass) == PACK_CLASS_OFF) ? PACK_BODY : PackClassLow(gPackClass);
    PackEdgeResult e = PackEdge(edgeClass, gEdgeColor, gEdgeSize);
    PSOut o;
    o.color = float4(SrgbToLinear(saturate(e.color.rgb)) * gSunIntensity * 0.85, saturate(e.color.a));
    o.normal = float4(OctEncode(float3(0, 0, -1)), 0.0, saturate(e.color.a));
    o.velocity = Velocity(i.curClip, i.prevClip);
    return o;
}
#endif

// ---- sky backdrop -----------------------------------------------------------------

struct SkyOut { float4 pos : SV_Position; float2 ndc : TEXCOORD0; };
SkyOut VSSky(uint id : SV_VertexID) {
    SkyOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.pos = float4(o.ndc, 1.0, 1.0);
    return o;
}

PSOut PSSky(SkyOut i) {
    float4 v = mul(float4(i.ndc, 1.0, 1.0), gInvProj);
    float3 viewDir = normalize(v.xyz / v.w);
    float3 dir = normalize(mul(viewDir, (float3x3)gInvView));
    PSOut o;
    o.color = float4(SkyColor(dir), gTransparentBg > 0.5 ? 0.0 : 1.0);
    o.normal = float4(0, 0, 0, 0);
    float4 cur = mul(float4(dir, 0.0), gViewProjNoJitter);
    float4 prev = mul(float4(dir, 0.0), gPrevViewProjNoJitter);
    o.velocity = Velocity(cur, prev);
    return o;
}

// ---- studio floor (no stage) ---------------------------------------------------------

static const float kFloorExtent = 900.0;

VSOut VSFloor(uint id : SV_VertexID) {
    // two triangles: (0,1,2) (2,1,3) over a square at y = 0
    uint k = id < 3 ? id : (id == 3 ? 2 : (id == 4 ? 1 : 3));
    float2 c = float2((k & 1) ? 1.0 : -1.0, (k & 2) ? 1.0 : -1.0) * kFloorExtent;
    float4 wp = float4(c.x, 0.0, c.y, 1.0);
    VSOut o;
    o.pos = mul(wp, gViewProj);
    o.worldPos = wp.xyz;
    o.nrm = float3(0, 1, 0);
    o.uv = c;
    o.curClip = mul(wp, gViewProjNoJitter);
    o.prevClip = mul(wp, gPrevViewProjNoJitter);
    o.viewZ = mul(wp, gView).z;
    o.uv1 = 0;
    o.uv2 = 0;
    return o;
}

PSOut PSFloor(VSOut i) {
    float3 n = float3(0, 1, 0);
    float3 L = -gLightDir;
    float sh = SHADOW_TERM(i.worldPos, n, i.viewZ, i.pos.xy);
    float r = length(i.worldPos.xz);
    // Unlit (gShading 1): flat albedo, no sun / hemispheric / punctual lighting; the fade into
    // the sky at the edge stays so there is no visible horizon seam.
    if (gShading > 0.5 && gShading < 1.5) {
        float3 unlit = lerp(float3(0.80, 0.83, 0.86), gSkyHorizon * 0.95, smoothstep(40.0, 420.0, r));
        float3 viewDir = normalize(i.worldPos - gEyePos);
        unlit = lerp(unlit, SkyColor(float3(viewDir.x, 0.0, viewDir.z)), smoothstep(260.0, 820.0, r));
        return PackOutput(SrgbToLinear(unlit), 1.0, n, 0.0, i.curClip, i.prevClip);
    }
    // cyclorama: brightest under the performer, easing into the horizon colour
    float3 albedo = lerp(float3(0.80, 0.83, 0.86), gSkyHorizon * 0.95, smoothstep(40.0, 420.0, r));
    float3 sunLin = SrgbToLinear(gLightColor) * 1.65;
    float ndl = saturate(dot(n, L));
    float3 color = albedo * (sunLin * ndl * lerp(0.42, 1.0, sh) + Hemisphere(n) * 0.55) * gSunIntensity;
    color += albedo * PunctualDiffuse(i.worldPos, n, 0.0, 0.0);
    float3 V = normalize(gEyePos - i.worldPos);
    color += PunctualSpecular(i.worldPos, n, V, 64.0) * 0.35;
    float reflectivity = 0.42 * (1.0 - smoothstep(120.0, 600.0, r));
    // dissolve into the sky toward the edge so there is no visible horizon seam
    float3 viewDir = normalize(i.worldPos - gEyePos);
    color = lerp(color, SkyColor(float3(viewDir.x, 0.0, viewDir.z)), smoothstep(260.0, 820.0, r));
    return PackOutput(color, 1.0, n, reflectivity, i.curClip, i.prevClip);
}

// ---- wireframe shading mode (ViewShading::Wireframe, raster only) ------------------

// The wireframe PSOs reuse the lit MRT layout (PSOut) and VSM's VSMain so the resolve /
// composite passes run unchanged; the fill mode draws the triangle edges instead. Lines are
// flat so nothing lights them, and alpha-tested/blended materials keep their coverage.

PSOut PSWire(VSOut i) {
    // dark grey on light, light grey on dark backgrounds read equally; HDR linear value < 1
    // so the composite/post passes pass it through without blooming.
    return PackOutput(float3(0.09, 0.10, 0.12), 1.0, float3(0, 1, 0), 0.0, i.curClip, i.prevClip);
}

// Flat background over a wireframe scene: same fullscreen far-plane draw as the sky, one colour.
PSOut PSWireBg(SkyOut i) {
    PSOut o;
    // mid grey (linear); the post pass tonemaps it to a neutral paper-like value
    o.color = float4(0.72, 0.74, 0.76, gTransparentBg > 0.5 ? 0.0 : 1.0);
    o.normal = float4(0, 0, 0, 0);
    o.velocity = float2(0, 0);   // uniform colour: no visible parallax, keep it temporally stable
    return o;
}

PSOut PSWireFloor(VSOut i) {
    return PackOutput(float3(0.16, 0.17, 0.19), 1.0, float3(0, 1, 0), 0.0, i.curClip, i.prevClip);
}

// ---- shadow map ------------------------------------------------------------------
// (left out by includers that use b2 themselves: offline_edge_pack.hlsl)
#ifndef MMDX_NO_SHADOW_PASS

cbuffer ShadowCB : register(b2) {
    uint gCascade;
    uint3 gShadowPad;
    row_major float4x4 gPointViewProj;
};
struct ShadowOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

ShadowOut VSShadow(VSIn v) {
    ShadowOut o;
    float3 wp3, wn;
    Skin(v, wp3, wn);
    float4 wp = float4(wp3, 1.0);
    float4x4 m = gCascade < 3 ? gShadowViewProj[gCascade]
               : (gCascade < 19 ? gSpotViewProj[gCascade - 3]
               : gPointViewProj);
    o.pos = mul(wp, m);
    o.uv = v.uv;
    return o;
}

void PSShadowAlpha(ShadowOut i) {
    float4 tex = (gFlags & MAT_HAS_TEXTURE) ? gTexture.Sample(gWrap, i.uv) : float4(1, 1, 1, 1);
    float a = gDiffuse.a * tex.a;
    clip(a - 0.5);
    const float alphaKey = PackAlphaKey(gPackClass);
    if (alphaKey > 0.0) clip(tex.r - alphaKey);
}
#endif
