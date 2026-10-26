// S9b-2: the sky environment buffer (render/SkyEnv.h's SkyEnvBuilder). One DEFAULT-heap raw buffer per renderer, 8296 floats:
//   cond   [0, W*H)     per row i the normalised inclusive CDF over the columns j (cond[i*W + W-1] = 1)
//   marg   [8192, 8192+H) the normalised inclusive CDF over the rows (marg[H-1] = 1)
//   total  [8256]       the sum of every texel's f
//   sh     [8260, 8296) 9 float4: the order-2 SH radiance coefficients L_k (rgb, w = 0)
// with W = 128, H = 64, texel (i, j) at v = (i + 0.5) / H, u = (j + 0.5) / W, dir = (sin theta cos phi, cos theta,
// sin theta sin phi) and f = (luminance(SkyColor(dir)) + 1e-4) * sin theta. shaders/sky_env.hlsli samples it (the path
// tracers' environment NEE) and evaluates its irradiance (the raster / RT ambient).
//
// Four compute entries, one group per row / one group of 64 for the marginal:
//   CSSkyEnvLum   [128,1,1] group i writes f(i, j) into cond (Dispatch(H))
//   CSSkyEnvRows  [128,1,1] group i prefix-sums its row, writes the row sum into marg and the row's CDF into cond
//   CSSkyEnvMarg  [64,1,1]  prefix-sums the 64 row sums, normalises them, writes the total (Dispatch(1))
//   CSSkySh       [256,1,1] integrates every texel's radiance into the 9 SH coefficients (Dispatch(1))
// The UAV is gSkyEnv (u0 of the ComputePipeline's UAV table); b0 is the frame's (or the offline image's)
// SceneConstants, so SkyColor reads the very sky the scene draws. The host runs the four passes in this order on one
// command list with a UAV barrier between them, then moves the buffer to ALL_SHADER_RESOURCE.
#include "common.hlsli"

RWByteAddressBuffer gSkyEnv : register(u0);
// S9b-3: the sky light's image, sampled by SkyColor's image branch. The ComputePipeline's unbounded t0 space2 range is
// the bindless texture table (its root signature binds heap slot 0, which is what a descriptor-table range covers
// here); s2 is its static linear wrap sampler.
Texture2D gSkyTex[] : register(t0, space2);
SamplerState gSkyMapWrap : register(s2);
#define SKY_MAP_TEX(i) gSkyTex[NonUniformResourceIndex((uint)(i))]
#define SKY_MAP_SAMPLER gSkyMapWrap
#include "sky_map.hlsli"

#define SKY_ENV_W 128u
#define SKY_ENV_H 64u
#define SKY_ENV_MARG_OFFSET 8192u
#define SKY_ENV_TOTAL_OFFSET 8256u
#define SKY_ENV_SH_OFFSET 8260u

#define SKY_ENV_THREADS 256u

// The texel direction of a grid cell's centre.
float3 SkyEnvCellDir(uint i, uint j) {
    const float theta = PI * (((float)i + 0.5) / (float)SKY_ENV_H);
    const float phi = 2.0 * PI * (((float)j + 0.5) / (float)SKY_ENV_W);
    return float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}

// A texel's solid-angle weighted luminance (the distribution the CDFs are built from).
float SkyEnvCellF(uint i, uint j) {
    const float theta = PI * (((float)i + 0.5) / (float)SKY_ENV_H);
    return (Luminance(SkyColor(SkyEnvCellDir(i, j))) + 1e-4) * sin(theta);
}

[numthreads(SKY_ENV_W, 1, 1)]
void CSSkyEnvLum(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint i = gid.x;
    const uint j = tid.x;
    gSkyEnv.Store((i * SKY_ENV_W + j) * 4u, asuint(SkyEnvCellF(i, j)));
}

groupshared float gsRow[SKY_ENV_W];

[numthreads(SKY_ENV_W, 1, 1)]
void CSSkyEnvRows(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint i = gid.x;
    const uint j = tid.x;
    gsRow[j] = asfloat(gSkyEnv.Load((i * SKY_ENV_W + j) * 4u));
    GroupMemoryBarrierWithGroupSync();
    // inclusive prefix sum (Hillis-Steele)
    [unroll] for (uint s = 1u; s < SKY_ENV_W; s <<= 1u) {
        const float below = j >= s ? gsRow[j - s] : 0.0;
        GroupMemoryBarrierWithGroupSync();
        gsRow[j] += below;
        GroupMemoryBarrierWithGroupSync();
    }
    const float rowSum = gsRow[SKY_ENV_W - 1u];
    gSkyEnv.Store((i * SKY_ENV_W + j) * 4u, asuint(gsRow[j] / max(rowSum, 1e-20)));
    if (j == 0u) gSkyEnv.Store((SKY_ENV_MARG_OFFSET + i) * 4u, asuint(rowSum));
}

groupshared float gsMarg[SKY_ENV_H];

[numthreads(SKY_ENV_H, 1, 1)]
void CSSkyEnvMarg(uint3 tid : SV_GroupThreadID) {
    const uint i = tid.x;
    gsMarg[i] = asfloat(gSkyEnv.Load((SKY_ENV_MARG_OFFSET + i) * 4u));
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 1u; s < SKY_ENV_H; s <<= 1u) {
        const float below = i >= s ? gsMarg[i - s] : 0.0;
        GroupMemoryBarrierWithGroupSync();
        gsMarg[i] += below;
        GroupMemoryBarrierWithGroupSync();
    }
    const float total = gsMarg[SKY_ENV_H - 1u];
    gSkyEnv.Store((SKY_ENV_MARG_OFFSET + i) * 4u, asuint(gsMarg[i] / max(total, 1e-20)));
    if (i == 0u) gSkyEnv.Store(SKY_ENV_TOTAL_OFFSET * 4u, asuint(total));
}

// 27 floats per thread (9 rgb coefficients): the reduction is over the flat array, one plane per component.
groupshared float gsSh[9u * 3u * SKY_ENV_THREADS];

[numthreads(SKY_ENV_THREADS, 1, 1)]
void CSSkySh(uint gi : SV_GroupIndex) {
    float3 L[9];
    [unroll] for (uint kInit = 0u; kInit < 9u; ++kInit) L[kInit] = 0.0;
    const float dOmega = (2.0 * PI / (float)SKY_ENV_W) * (PI / (float)SKY_ENV_H);
    [loop] for (uint t = gi; t < SKY_ENV_W * SKY_ENV_H; t += SKY_ENV_THREADS) {
        const uint i = t / SKY_ENV_W;
        const uint j = t % SKY_ENV_W;
        const float3 d = SkyEnvCellDir(i, j);
        const float3 radiance = SkyColor(d);
        const float theta = PI * (((float)i + 0.5) / (float)SKY_ENV_H);
        const float w = dOmega * sin(theta);
        const float basis[9] = {
            0.282095,
            0.488603 * d.y,
            0.488603 * d.z,
            0.488603 * d.x,
            1.092548 * d.x * d.y,
            1.092548 * d.y * d.z,
            0.315392 * (3.0 * d.z * d.z - 1.0),
            1.092548 * d.x * d.z,
            0.546274 * (d.x * d.x - d.y * d.y),
        };
        [unroll] for (uint kAcc = 0u; kAcc < 9u; ++kAcc) L[kAcc] += radiance * basis[kAcc] * w;
    }
    [unroll] for (uint kOut = 0u; kOut < 9u; ++kOut) {
        gsSh[(kOut * 3u + 0u) * SKY_ENV_THREADS + gi] = L[kOut].x;
        gsSh[(kOut * 3u + 1u) * SKY_ENV_THREADS + gi] = L[kOut].y;
        gsSh[(kOut * 3u + 2u) * SKY_ENV_THREADS + gi] = L[kOut].z;
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = SKY_ENV_THREADS / 2u; s > 0u; s >>= 1u) {
        if (gi < s) {
            [unroll] for (uint coef = 0u; coef < 27u; ++coef)
                gsSh[coef * SKY_ENV_THREADS + gi] += gsSh[coef * SKY_ENV_THREADS + gi + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0u) {
        [unroll] for (uint kWrite = 0u; kWrite < 9u; ++kWrite) {
            const float3 l = float3(gsSh[(kWrite * 3u + 0u) * SKY_ENV_THREADS],
                                    gsSh[(kWrite * 3u + 1u) * SKY_ENV_THREADS],
                                    gsSh[(kWrite * 3u + 2u) * SKY_ENV_THREADS]);
            gSkyEnv.Store4((SKY_ENV_SH_OFFSET + kWrite * 4u) * 4u, asuint(float4(l, 0.0)));
        }
    }
}
