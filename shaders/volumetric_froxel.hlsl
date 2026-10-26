// S9d: froxel volumetric fog, the real-time path's default mode (the per-pixel march of volumetric.hlsl stays the
// alternative: RenderSettings::volumetricMode 1 / ini volumetricMode=1 / --volumetric-mode march).
//   CSFroxelLight      one thread per froxel: medium (volumetric_common.hlsli) + in-scattering from the sun and the
//                      froxel's S7c cluster list (light_cluster.hlsli), optionally the 3-octave multiple-scatter
//                      approximation (Wrenninge 2015) -> u0 (rgb = sigma_s x in-scattered radiance, a = extinction)
//   CSFroxelTemporal   blends with the previous frame's grid reprojected through prevViewProjNoJitter -> u0
//   CSFroxelIntegrate  one thread per (x, y) column: front-to-back integration storing the prefix (L, T) of every slice
//                      (volumetric_froxel_apply.hlsl reads the column trilinearly at the pixel's own view depth)
// The grid is kFroxelX x kFroxelY screen tiles x kFroxelZ exponential view-depth slices from nearZ to the frame's haze
// distance, sized independently of the render resolution (PassVolumetric.cpp).
//
// gP0 = (sigma0, heightFalloff, maxDistance, sunG)          gP1 = (punctualG, numFogVolumes, sunVolumetric, radius)
// gP2 = (medium tint rgb, ambient scale)                    gP3 = (multiple scattering 0/1, gridX, gridY, gridZ)
// Temporal: gP0 = (history valid, current weight, previous nearZ, previous maxDistance), gP1 = (this frame's
// maxDistance, numFog, sunVolumetric, radius), same gP2 / gP3. Integrate: gP0 = (maxDistance, 0, 0, 0), same gP3.
#include "rt_common.hlsli"
// light_cluster.hlsli reaches the cluster block through the bindless buffer table (rt_common.hlsli's gBindlessBuf).
#define gPbrBuf gBindlessBuf
#include "light_cluster.hlsli"

cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gLinearClamp : register(s1);

Texture3D<float4> gFroxelSrc : register(t0);       // CSFroxelLight output / the integrate source
Texture3D<float4> gFroxelHistory : register(t1);   // the previous frame's temporally blended grid
Texture2DArray<float> gShadowMap : register(t2);   // raster: sun cascades
Texture2DArray<float> gSpotShadowMap : register(t3);   // raster: spot shadow slices
RWTexture3D<float4> gFroxelOut : register(u0);

// ---- visibility terms of volumetric_common.hlsli ---------------------------------------------
// The raster frame reads the cascaded sun shadow map and the spot shadow slices (the march's own look); a ray-traced /
// path-traced frame (VOL_RT_SHADOWS, the TLAS bound) traces them, so the froxel medium sees the same occluders the
// surfaces do.
#ifndef VOL_RT_SHADOWS
// Four point compares of the 2x2 footprint (smooth shaft edges), as volumetric.hlsl does.
float CompareGather(Texture2DArray<float> map, float2 uv, float slice, float z) {
    float4 d = map.GatherRed(gLinearClamp, float3(uv, slice));
    float4 lit = z <= d ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);
    return dot(lit, float4(0.25, 0.25, 0.25, 0.25));
}
#endif

#include "volumetric_common.hlsli"

float VolSunVisibility(float3 wp) {
    if (gSunShadowParams.x < 0.5) return 1.0;
#ifdef VOL_RT_SHADOWS
    return TraceShadowRay(wp, -gLightDir, 4000.0);
#else
    float viewZ = mul(float4(wp, 1.0), gView).z;
    int c = viewZ < gCascadeSplits.x ? 0 : (viewZ < gCascadeSplits.y ? 1 : 2);
    float4 sp = mul(float4(wp, 1.0), gShadowViewProj[c]);
    float3 q = sp.xyz / sp.w;
    float2 suv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    if (any(suv < 0.0) || any(suv > 1.0) || q.z > 1.0) return 1.0;
    return CompareGather(gShadowMap, suv, c, q.z - 0.0015);
#endif
}

float VolSpotVisibility(VolLight l, float3 wp) {
    if (l.shadowType == 0.0) return 1.0;
#ifdef VOL_RT_SHADOWS
    float3 dv = l.pos - wp;
    float dL = length(dv);
    return TraceShadowRay(wp, dv / max(dL, 1e-4), max(dL - 0.05, 0.0));
#else
    float slice = l.shadowSlice;
    if (slice < 0.0 || slice >= gSpotShadowParams.x) return 1.0;
    float4 sp = mul(float4(wp, 1.0), gSpotViewProj[(uint)slice]);
    if (sp.w <= 0.0) return 1.0;
    float3 q = sp.xyz / sp.w;
    float2 suv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    if (any(suv < 0.0) || any(suv > 1.0) || q.z > 1.0) return 1.0;
    return CompareGather(gSpotShadowMap, suv, slice, q.z - 0.00005);
#endif
}

// ---- the froxel grid ------------------------------------------------------------------------
// Slice k spans view depths [z0 r^k, z0 r^(k+1)], r = (z1 / z0)^(1 / z): exponential slices keep the near froxels thin
// and let the far ones be coarse (the haze is smooth there).
uint FroxelGridX() { return (uint)gP3.y; }
uint FroxelGridY() { return (uint)gP3.z; }
uint FroxelGridZ() { return (uint)gP3.w; }

float FroxelZ0() { return max(gNearZ, 0.01); }
float FroxelZ1(float fallback) { const float z0 = FroxelZ0(); return max(fallback, z0 * 1.05); }
// Continuous slice coordinate of a view depth (texel k's centre is k + 0.5).
float FroxelSliceCoord(float z, float z0, float z1, float d) {
    return saturate(log(max(z, z0) / z0) / max(log(z1 / z0), 1e-4)) * d;
}

// ---- multiple scattering (Wrenninge 2015, three octaves) ------------------------------------
// Octave n carries a^n of the single-scattered energy, with the extinction b^n and the anisotropy g c^n (a = b = c =
// 0.5). A froxel stores one extinction channel, which stays the medium's own so the grid's transmittance (and with it
// the haze level / fogging of surfaces) is exactly the march's in both modes: with a = b the octaves' b^n extinction
// weights are the numbers kMsEnergy already carries, so the single channel loses no order's weight.
static const float kMsEnergy[3] = {1.0, 0.5, 0.25};   // a^n (== b^n, a = b = 0.5)
static const float kMsPhase[3] = {1.0, 0.5, 0.25};    // c^n

// The octaves' Henyey-Greenstein phase sum (exactly HG of the single scattering when the switch is off).
float VolPhaseMS(float c, float g) {
    if (gP3.x < 0.5) return VolHG(c, g);
    float sum = 0.0;
    [unroll] for (int n = 0; n < 3; ++n) sum += kMsEnergy[n] * VolHG(c, g * kMsPhase[n]);
    return sum;
}

// The frame's medium parameters: the march's own fields (PassVolumetric.cpp fills the same values).
void FroxelParams(out VolParams p) {
    p.sigma0 = gP0.x;
    p.falloff = gP0.y;
    p.sunG = gP0.w;
    p.punctualG = gP1.x;
    p.spotBoost = 6.0f;     // volumetric.hlsl's constants (gP1.z = 6, gP2.x = 0.25 ambient, gP2.y = 0.45 points)
    p.pointBoost = 0.45f;
    p.sunShadowDist = 1e9;  // a froxel tests the visibility at its centre, not along a march
    p.sunSteps = 0; p.spotSteps = 0; p.pointSteps = 0;
    p.sunStepLen = 1.0; p.spotStepLen = 1.0;
    p.jitter = 0.5;
    p.ambient = (gSkyZenith + gSkyHorizon + gGroundColor) * (1.0 / 3.0) * gSunIntensity * gP2.w;
    p.mediumCenter = float3(0.0, 0.0, 0.0);
    p.mediumRadius = gP1.w;
    p.numFog = (uint)max(gP1.y, 0.0);
    p.sunVolumetric = gP1.z;
    p.tint = gP2.rgb;
}

// The view-space ray through a froxel tile's centre (the same inverse projection the passes use).
float3 FroxelViewDir(uint2 tile) {
    const float2 uv = (float2(tile) + 0.5) / float2((float)FroxelGridX(), (float)FroxelGridY());
    const float4 cv = mul(float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0), gInvProj);
    return normalize(cv.xyz / cv.w);
}

// The froxel centre, jittered along z by this frame's sample offset (the temporal pass averages the slices' steps).
float3 FroxelWorldPos(uint2 tile, uint tz, float jitter, float z1) {
    const float d = (float)FroxelGridZ();
    const float3 vd = FroxelViewDir(tile);
    const float z = FroxelZ0() * pow(FroxelZ1(z1) / FroxelZ0(), ((float)tz + jitter) / d);
    return mul(float4(vd * (z / max(vd.z, 1e-4)), 1.0), gInvView).xyz;
}

[numthreads(8, 8, 1)]
void CSFroxelLight(uint3 id : SV_DispatchThreadID) {
    const uint w = FroxelGridX(), h = FroxelGridY(), d = FroxelGridZ();
    if (id.x >= w || id.y >= h * d) return;
    const uint2 tile = uint2(id.x, id.y % h);
    const uint tz = id.y / h;

    VolParams vp;
    FroxelParams(vp);
    // R2 low-discrepancy offset of this frame along z (the slices are sampled at different depths each frame)
    const float jitter = frac(0.5 + (float)((uint)gFrameIndex & 63u) * 0.7548776662);
    const float3 wp = FroxelWorldPos(tile, tz, jitter, gP0.z);

    float3 sigmaS; float sigmaT;
    VolMediumAt(wp, vp, sigmaS, sigmaT);
    if (length(wp - vp.mediumCenter) > vp.mediumRadius) {
        // outside the haze sphere (distant scenery / sky): no medium, so no light to gather there either
        gFroxelOut[uint3(tile, tz)] = 0.0;
        return;
    }

    const float3 dirW = normalize(wp - gEyePos);
    // In-scattered radiance at the froxel: the isotropic sky term, the sun, then the punctual lights of its cluster list.
    // The ambient term is already the cheap stand-in for the sky dome's multiple scattering (as in the march), so the
    // octaves of the multiple-scatter approximation apply to the directional lights only: their extra energy shows as
    // the beams' wider, softer halo without lifting the whole haze level above the march's.
    float3 Ls = vp.ambient;
    const float3 sunCol = SrgbToLinear(gLightColor) / SrgbToLinear(float3(0.6, 0.6, 0.6)) * gSunIntensity;
    Ls += sunCol * (VolPhaseMS(dot(dirW, -gLightDir), vp.sunG) * vp.sunVolumetric * VolSunVisibility(wp));

    // S7c: the froxel's clustered light list; without clustering this frame, every light (as the march, capped at
    // kVolMaxLights, so a scene with fewer lights sums the same lights in the same order)
    const LightList list = LightListFor(mul(float4(wp, 1.0), gView).xyz);
    const uint count = min(list.count, kVolMaxLights);
    for (uint k = 0; k < count; ++k) {
        const VolLight l = gVolLights[LightListAt(list, k)];
        if (l.volumetric <= 0.0) continue;   // invisible in the medium
        float3 ld;
        const float atten = VolPunctualAtten(l, wp, ld);
        if (atten <= 0.0) continue;          // out of the light's range (the falloff is exactly 0 there)
        const bool spot = l.cosOuter > -1.0;
        const float g = l.aniso > -0.99 ? l.aniso : vp.punctualG;
        const float vis = spot ? VolSpotVisibility(l, wp) : 1.0;
        Ls += l.color * ((spot ? vp.spotBoost : vp.pointBoost) * l.volumetric * atten * vis) *
              VolPhaseMS(dot(dirW, ld), g);
    }
    // the medium tint scales the whole in-scattering, exactly as the march's final L * p.tint does
    gFroxelOut[uint3(tile, tz)] = float4(sigmaS * Ls * vp.tint, sigmaT);
}

// History reprojection: the froxel's world position through last frame's view projection. Rejected outside the
// previous frustum or when the pass reports no usable history (a camera cut / a frame gap), which then keeps `cur`.
[numthreads(8, 8, 1)]
void CSFroxelTemporal(uint3 id : SV_DispatchThreadID) {
    const uint w = FroxelGridX(), h = FroxelGridY(), d = FroxelGridZ();
    if (id.x >= w || id.y >= h * d) return;
    const uint2 tile = uint2(id.x, id.y % h);
    const uint tz = id.y / h;
    const float4 cur = gFroxelSrc.Load(int4(int3(tile, tz), 0));
    if (gP0.x < 0.5) {
        gFroxelOut[uint3(tile, tz)] = cur;
        return;
    }
    // the froxel centre at the slice's midpoint (any jitter cancels over the two frames it is blended across)
    const float3 wp = FroxelWorldPos(tile, tz, 0.5, gP1.x);   // gP1.x = this frame's maxDistance
    const float4 clip = mul(float4(wp, 1.0), gPrevViewProjNoJitter);
    const float prevZ0 = max(gP0.z, 0.01), prevZ1 = max(gP0.w, prevZ0 * 1.05);
    const float2 puv = clip.w > 0.0 ? clip.xy / clip.w * float2(0.5, -0.5) + 0.5 : float2(-1.0, -1.0);
    if (clip.w <= 0.0 || any(puv < 0.0) || any(puv > 1.0)) {
        gFroxelOut[uint3(tile, tz)] = cur;
        return;
    }
    // texel k holds the froxel at slice coordinate t = k + 0.5, so the texture coordinate is t / d
    const float slice = FroxelSliceCoord(clip.w, prevZ0, prevZ1, (float)d) / (float)d;
    const float4 hist = gFroxelHistory.SampleLevel(gLinearClamp, float3(puv, slice), 0);
    gFroxelOut[uint3(tile, tz)] = lerp(hist, cur, gP0.y);
}

// Front to back over the column: L += T S (1 - exp(-sigma dz)) / sigma, T *= exp(-sigma dz), storing the prefix so the
// apply pass reads the (L, T) of the pixel's own depth instead of the froxel's.
[numthreads(8, 8, 1)]
void CSFroxelIntegrate(uint3 id : SV_DispatchThreadID) {
    const uint w = FroxelGridX(), h = FroxelGridY(), d = FroxelGridZ();
    if (id.x >= w || id.y >= h) return;
    const float z0 = FroxelZ0(), z1 = FroxelZ1(gP0.x);
    const float ratio = pow(z1 / z0, 1.0 / (float)d);
    float T = 1.0, z = z0;
    float3 L = 0.0;
    for (uint k = 0; k < d; ++k) {
        const float dz = z * (ratio - 1.0);
        const float4 s = gFroxelSrc.Load(int4(int3(id.xy, k), 0));
        const float st = max(s.a, 0.0);
        const float tr = st > 1e-6 ? (1.0 - exp(-st * dz)) / st : dz;
        L += T * s.rgb * tr;
        T *= exp(-st * dz);
        gFroxelOut[uint3(id.xy, k)] = float4(L, T);
        z *= ratio;
    }
}
