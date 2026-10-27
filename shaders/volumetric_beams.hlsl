// Spot light beams at the full render resolution (volumetric_common.hlsli), added on top of the froxel grid: the 160 x
// 90 froxel tiles are too coarse for a thin cone, so the spots' in-scattering is marched per pixel here instead
// (volumetric_froxel.hlsl skips them, gP3.x bit 1). The sun, the ambient haze, the point lights and the fog volumes
// stay in the froxel grid.
//   CSBeams        one thread per full-resolution pixel: the spot march of IntegrateVolume with VOL_SPOTS_ONLY (alpha 1,
//                  the pass is additive) -> u0 (rgb = in-scattered radiance)
//     gP0 = (sigma0, heightFalloff, maxDistance, 0), gP1 = (punctualG, numFogVolumes, multiScatter 0/1, mediumRadius)
//     gP2 = (medium tint rgb, 0), gP3 = 0
//   CSBeamTemporal the march is jittered per frame: blend with the reprojected history (3x3 neighbourhood clamp) -> u0
//     gP0 = (history valid 0/1, current weight, maxDistance, 0)
#include "rt_common.hlsli"
// light_cluster.hlsli reaches the cluster block through the bindless buffer table (rt_common.hlsli's gBindlessBuf).
#define gPbrBuf gBindlessBuf
#include "light_cluster.hlsli"
#include "gobo.hlsli"   // S7b: spot gobos in the beam march
#define VOL_GOBO 1      // volumetric_common.hlsli applies the spot gobos (this pass has the bindless tables)
#define VOL_SPOTS_ONLY 1
// the froxel's VolPhaseMS (volumetric_froxel.hlsl): the multiple-scatter halo of the beams when it is on
#define VOL_PUNCTUAL_PHASE(c, g) (gP1.z > 0.5 ? VolHG(c, g) + 0.5 * VolHG(c, 0.5 * (g)) + 0.25 * VolHG(c, 0.25 * (g)) : VolHG(c, g))

cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gLinearClamp : register(s1);

Texture2D<float> gDepthTex : register(t0);          // CSBeams: full-res depth
Texture2D<float4> gSrc : register(t0);              // CSBeamTemporal: this frame's march
Texture2D<float4> gHistory : register(t1);          // CSBeamTemporal: the previous frame's blended beams
Texture2D<float> gDepthTemporal : register(t2);     // CSBeamTemporal: full-res depth
Texture2DArray<float> gShadowMap : register(t2);    // CSBeams: sun cascades
Texture2DArray<float> gSpotShadowMap : register(t3);   // CSBeams: spot shadow slices
RWTexture2D<float4> gOut : register(u0);

// ---- visibility terms of volumetric_common.hlsli (the froxel's raster variants) ----------------
// Four point compares of the 2x2 footprint (smooth shaft edges), as volumetric.hlsl does.
float CompareGather(Texture2DArray<float> map, float2 uv, float slice, float z) {
    float4 d = map.GatherRed(gLinearClamp, float3(uv, slice));
    float4 lit = z <= d ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);
    return dot(lit, float4(0.25, 0.25, 0.25, 0.25));
}

float VolSunVisibility(float3 wp) {
    if (gSunShadowParams.x < 0.5) return 1.0;
    float viewZ = mul(float4(wp, 1.0), gView).z;
    int c = viewZ < gCascadeSplits.x ? 0 : (viewZ < gCascadeSplits.y ? 1 : 2);
    float4 sp = mul(float4(wp, 1.0), gShadowViewProj[c]);
    float3 q = sp.xyz / sp.w;
    float2 suv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    if (any(suv < 0.0) || any(suv > 1.0) || q.z > 1.0) return 1.0;
    return CompareGather(gShadowMap, suv, c, q.z - 0.0015);
}

#include "volumetric_common.hlsli"

float VolSpotVisibility(VolLight l, float3 wp) {
    if (l.shadowType == 0.0) return 1.0;
    float slice = l.shadowSlice;
    if (slice < 0.0 || slice >= gSpotShadowParams.x) return 1.0;
    float4 sp = mul(float4(wp, 1.0), gSpotViewProj[(uint)slice]);
    if (sp.w <= 0.0) return 1.0;
    float3 q = sp.xyz / sp.w;
    float2 suv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    if (any(suv < 0.0) || any(suv > 1.0) || q.z > 1.0) return 1.0;
    return CompareGather(gSpotShadowMap, suv, slice, q.z - 0.00005);
}

[numthreads(8, 8, 1)]
void CSBeams(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 p = id.xy;
    // the view ray of this full-resolution pixel and its march length
    float d = gDepthTex.Load(int3(p, 0));
    float2 uv = (p + 0.5) * gInvViewportSize;
    float3 vp = ViewPosFromDepth(uv, d >= 1.0 ? 1.0 : d);
    float3 dirV = normalize(vp);
    float dist = d >= 1.0 ? gP0.z : min(length(vp), gP0.z);
    float3 dir = normalize(mul(dirV, (float3x3)gInvView));

    VolParams vpo;
    vpo.sigma0 = gP0.x;
    vpo.falloff = gP0.y;
    vpo.punctualG = gP1.x;
    vpo.numFog = (uint)max(gP1.y, 0.0);
    vpo.tint = gP2.rgb;
    vpo.mediumCenter = float3(0.0, 0.0, 0.0);
    vpo.mediumRadius = gP1.w;
    vpo.spotBoost = 6.0;
    vpo.pointBoost = 0.45;
    vpo.sunG = 0.55;
    vpo.sunVolumetric = 0.0;   // the sun is the froxel grid's
    vpo.ambient = 0.0;
    vpo.sunShadowDist = 0.0;
    vpo.sunSteps = 0;
    vpo.pointSteps = 0;
    vpo.spotSteps = 24;
    vpo.sunStepLen = 2.5;
    vpo.spotStepLen = 1.5;
    vpo.jitter = frac(Ign(float2(p) + 5.588238 * (float)((uint)gFrameIndex & 63u)));

    gOut[p] = IntegrateVolume(gEyePos, dir, dist, (uint)gNumLights, gLightDir, float3(0, 0, 0), vpo);
}

[numthreads(8, 8, 1)]
void CSBeamTemporal(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    int2 p = int2(id.xy);
    float4 cur = gSrc.Load(int3(p, 0));
    if (gP0.x < 0.5) {
        gOut[p] = float4(cur.rgb, 1.0);
        return;
    }
    // neighbourhood range of the (jittered, noisy) current march
    float4 mn = cur, mx = cur;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        float4 s = gSrc.Load(int3(clamp(p + int2(x, y), int2(0, 0), int2(w, h) - 1), 0));
        mn = min(mn, s);
        mx = max(mx, s);
    }
    // reproject the march end point (surface, or the far end for the sky) of this pixel
    float d = gDepthTemporal.Load(int3(p, 0));
    float2 uv = (float2(p) + 0.5) * gInvViewportSize;
    float3 vp = ViewPosFromDepth(uv, d >= 1.0 ? 1.0 : d);
    float3 dirV = normalize(vp);
    float dist = d >= 1.0 ? gP0.z : min(length(vp), gP0.z);
    float3 dir = normalize(mul(dirV, (float3x3)gInvView));
    float4 clip = mul(float4(gEyePos + dir * dist, 1.0), gPrevViewProjNoJitter);
    float2 puv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
    if (clip.w <= 0.0 || any(puv < 0.0) || any(puv > 1.0)) {
        gOut[p] = float4(cur.rgb, 1.0);
        return;
    }
    float4 hist = clamp(gHistory.SampleLevel(gLinearClamp, puv, 0), mn, mx);
    gOut[p] = float4(lerp(hist, cur, gP0.y).rgb, 1.0);
}
