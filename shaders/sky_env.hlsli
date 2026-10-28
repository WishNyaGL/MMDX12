// S9b-2: the sky as a light (see shaders/sky_env.hlsl for the buffer's layout and how it is built).
//
// SceneConstants::gSkyEnvParams (C++: SceneConstants::skyEnvParams):
//   x = SrvHeap index of the raw ByteAddressBuffer holding the sky environment (-1 = none this frame: every function
//       here returns 0 / false at one compare, so the frame is the one without the feature byte for byte),
//   y = W, z = H (128 x 64), w = unused.
//
// The buffer holds the sky's luminance distribution over the sphere as an equirectangular grid:
//   cond[i*W + j]  per row i the normalised inclusive CDF over the columns (cond[i*W + W-1] = 1),
//   marg[i]        the normalised inclusive CDF over the rows (marg[H-1] = 1),
//   total          the sum of every texel's f,
//   sh[k]          9 float4: the order-2 SH radiance coefficients L_k (rgb, w = 0).
// f(i, j) = (luminance(SkyColor(dir)) + 1e-4) * sin(theta) is the texel's solid-angle weighted luminance, so an
// importance sample of it is a cosine-free sample of the sky's radiance distribution and its pdf is the solid-angle
// pdf the MIS below weighs against.
//
// SkyShIrradiance is the raster / RT ambient (Hemisphere in mmd.hlsl): E(n) = sum_k A_k L_k Y_k(n), the Lambertian
// irradiance of the environment up to order 2.
#ifndef MMDX_SKY_ENV_HLSLI
#define MMDX_SKY_ENV_HLSLI

#include "common.hlsli"

// The buffer through the bindless buffer table, like shaders/screen_light.hlsli: gBindlessBuf in the ray paths,
// gPbrBuf in the raster / RT scene shaders.
#ifdef MMDX_RT_COMMON_HLSLI
#define SKY_ENV_BUF(i) gBindlessBuf[NonUniformResourceIndex((uint)(i))]
#else
#define SKY_ENV_BUF(i) gPbrBuf[NonUniformResourceIndex((uint)(i))]
#endif

static const uint kSkyEnvW = 128u;
static const uint kSkyEnvH = 64u;
static const uint kSkyEnvMargOffset = 8192u;
static const uint kSkyEnvShOffset = 8260u;

bool SkyEnvOn() { return gSkyEnvParams.x >= 0.0; }

// The texel direction of a grid cell's centre: theta = PI * v from +Y, phi = 2 * PI * u.
float3 SkyEnvDir(float v, float u) {
    const float theta = PI * saturate(v);
    const float phi = 2.0 * PI * u;
    return float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
}

// E(n) = sum_k A_k L_k Y_k(n) with A = (PI, 2PI/3 x3, PI/4 x5): the Lambertian irradiance of the sky, clamped at 0.
// SkyColor already carries gSunIntensity, so the coefficients do too (the caller must not scale by it again).
float3 SkyShIrradiance(float3 n) {
    const ByteAddressBuffer b = SKY_ENV_BUF(SafeSrv(gSkyEnvParams.x));
    const float3 d = n;
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
    const float band[9] = {PI, 2.0 * PI / 3.0, 2.0 * PI / 3.0, 2.0 * PI / 3.0,
                           PI / 4.0, PI / 4.0, PI / 4.0, PI / 4.0, PI / 4.0};
    float3 e = 0.0;
    [unroll] for (uint k = 0u; k < 9u; ++k) {
        const float3 l = asfloat(b.Load4((kSkyEnvShOffset + k * 4u) * 4u)).rgb;
        e += band[k] * l * basis[k];
    }
    return max(e, 0.0);
}

// One importance sample of the sky's luminance distribution: `u` in [0,1)^2, the direction it lands on and the
// direction's solid-angle pdf (0 = nothing to sample, the caller skips).
bool SkyEnvSample(float2 u, out float3 dir, out float pdf) {
    dir = float3(0.0, 1.0, 0.0);
    pdf = 0.0;
    if (!SkyEnvOn()) return false;
    const ByteAddressBuffer b = SKY_ENV_BUF(SafeSrv(gSkyEnvParams.x));
    // row: the first index whose marginal CDF passes u.x
    uint lo = 0u, hi = kSkyEnvH - 1u;
    [loop] while (lo < hi) {
        const uint mid = (lo + hi) >> 1u;
        if (asfloat(b.Load((kSkyEnvMargOffset + mid) * 4u)) < u.x) lo = mid + 1u;
        else hi = mid;
    }
    const uint i = lo;
    const float m1 = asfloat(b.Load((kSkyEnvMargOffset + i) * 4u));
    const float m0 = i == 0u ? 0.0 : asfloat(b.Load((kSkyEnvMargOffset + i - 1u) * 4u));
    const float fv = (u.x - m0) / max(m1 - m0, 1e-20);
    // column: the first index of the row whose CDF passes u.y
    lo = 0u;
    hi = kSkyEnvW - 1u;
    [loop] while (lo < hi) {
        const uint mid = (lo + hi) >> 1u;
        if (asfloat(b.Load((i * kSkyEnvW + mid) * 4u)) < u.y) lo = mid + 1u;
        else hi = mid;
    }
    const uint j = lo;
    const float c1 = asfloat(b.Load((i * kSkyEnvW + j) * 4u));
    const float c0 = j == 0u ? 0.0 : asfloat(b.Load((i * kSkyEnvW + j - 1u) * 4u));
    const float fu = (u.y - c0) / max(c1 - c0, 1e-20);
    const float v = ((float)i + fv) / (float)kSkyEnvH;
    dir = SkyEnvDir(v, ((float)j + fu) / (float)kSkyEnvW);
    const float theta = PI * v;
    // the cell's solid angle: (2 PI / W) * (PI / H) * sin(theta)
    const float pTexel = (m1 - m0) * (c1 - c0);
    pdf = pTexel * (float)(kSkyEnvW * kSkyEnvH) / (2.0 * PI * PI * max(sin(theta), 1e-6));
    return pdf > 0.0;
}

// The solid-angle pdf SkyEnvSample gives a direction (the BSDF side of the MIS): the texel that holds it, its CDF
// share and the same conversion. 0 when the direction has no sample (the top / bottom row's poles are covered).
float SkyEnvPdf(float3 dir) {
    if (!SkyEnvOn()) return 0.0;
    const ByteAddressBuffer b = SKY_ENV_BUF(SafeSrv(gSkyEnvParams.x));
    const float theta = acos(clamp(dir.y, -1.0, 1.0));
    float phi = atan2(dir.z, dir.x);
    if (phi < 0.0) phi += 2.0 * PI;
    const uint i = min((uint)(theta / PI * (float)kSkyEnvH), kSkyEnvH - 1u);
    const uint j = min((uint)(phi / (2.0 * PI) * (float)kSkyEnvW), kSkyEnvW - 1u);
    const float m1 = asfloat(b.Load((kSkyEnvMargOffset + i) * 4u));
    const float m0 = i == 0u ? 0.0 : asfloat(b.Load((kSkyEnvMargOffset + i - 1u) * 4u));
    const float c1 = asfloat(b.Load((i * kSkyEnvW + j) * 4u));
    const float c0 = j == 0u ? 0.0 : asfloat(b.Load((i * kSkyEnvW + j - 1u) * 4u));
    const float pTexel = (m1 - m0) * (c1 - c0);
    if (pTexel <= 0.0) return 0.0;
    return pTexel * (float)(kSkyEnvW * kSkyEnvH) / (2.0 * PI * PI * max(sin(theta), 1e-6));
}

#endif
