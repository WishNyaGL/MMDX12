// Physically based bloom (Jimenez 2014): 13-tap downsample chain with a Karis-averaged,
// soft-thresholded first step, then 3x3 tent upsampling added back up the chain.
// Alpha carries luminance x linear depth through every level (blurs are linear), so alpha / luminance of the bloom
// is the light-weighted depth of the sources: the depth mask in post.hlsl.
//   t0 source, t1 depth (PSPrefilter).  gP0.xy = source texel size;
//   PSPrefilter: gP0.z = threshold, gP0.w = 1 when t1 is bound;  PSUp: gP0.z = weight of the coarser level, gP0.w = radius scale.
#include "fullscreen.hlsli"
#include "bloom_common.hlsli"

Texture2D<float4> gSrc : register(t0);
Texture2D<float> gBloomDepth : register(t1);

float4 Down13(float2 uv, float2 t) {
    float4 a = gSrc.SampleLevel(gLinear, uv + t * float2(-2, -2), 0);
    float4 b = gSrc.SampleLevel(gLinear, uv + t * float2(0, -2), 0);
    float4 c = gSrc.SampleLevel(gLinear, uv + t * float2(2, -2), 0);
    float4 d = gSrc.SampleLevel(gLinear, uv + t * float2(-2, 0), 0);
    float4 e = gSrc.SampleLevel(gLinear, uv, 0);
    float4 f = gSrc.SampleLevel(gLinear, uv + t * float2(2, 0), 0);
    float4 g = gSrc.SampleLevel(gLinear, uv + t * float2(-2, 2), 0);
    float4 h = gSrc.SampleLevel(gLinear, uv + t * float2(0, 2), 0);
    float4 k = gSrc.SampleLevel(gLinear, uv + t * float2(2, 2), 0);
    float4 l = gSrc.SampleLevel(gLinear, uv + t * float2(-1, -1), 0);
    float4 m = gSrc.SampleLevel(gLinear, uv + t * float2(1, -1), 0);
    float4 n = gSrc.SampleLevel(gLinear, uv + t * float2(-1, 1), 0);
    float4 o = gSrc.SampleLevel(gLinear, uv + t * float2(1, 1), 0);
    return e * 0.125 + (a + c + g + k) * 0.03125 + (b + d + f + h) * 0.0625 + (l + m + n + o) * 0.125;
}

float KarisWeight(float3 c) { return 1.0 / (1.0 + Luminance(c)); }

float4 PSPrefilter(FsOut i) : SV_Target {
    float2 t = gP0.xy;
    // Karis average over the four 2x2 groups suppresses single-pixel fireflies.
    float3 g0 = gSrc.SampleLevel(gLinear, i.uv + t * float2(-1, -1), 0).rgb;
    float3 g1 = gSrc.SampleLevel(gLinear, i.uv + t * float2(1, -1), 0).rgb;
    float3 g2 = gSrc.SampleLevel(gLinear, i.uv + t * float2(-1, 1), 0).rgb;
    float3 g3 = gSrc.SampleLevel(gLinear, i.uv + t * float2(1, 1), 0).rgb;
    float w0 = KarisWeight(g0), w1 = KarisWeight(g1), w2 = KarisWeight(g2), w3 = KarisWeight(g3);
    float3 c = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3) / (w0 + w1 + w2 + w3);
    c *= BloomContribution(c, gP0.z);
    float z = gP0.w > 0.5 ? LinearZ(gBloomDepth.SampleLevel(gPoint, i.uv, 0)) : 0.0;
    return float4(c, Luminance(c) * z);
}

float4 PSDown(FsOut i) : SV_Target {
    return Down13(i.uv, gP0.xy);
}

float4 PSUp(FsOut i) : SV_Target {
    float2 t = gP0.xy * gP0.w;
    float4 s = gSrc.SampleLevel(gLinear, i.uv, 0) * 4.0;
    s += (gSrc.SampleLevel(gLinear, i.uv + t * float2(-1, 0), 0) + gSrc.SampleLevel(gLinear, i.uv + t * float2(1, 0), 0) +
          gSrc.SampleLevel(gLinear, i.uv + t * float2(0, -1), 0) + gSrc.SampleLevel(gLinear, i.uv + t * float2(0, 1), 0)) * 2.0;
    s += gSrc.SampleLevel(gLinear, i.uv + t * float2(-1, -1), 0) + gSrc.SampleLevel(gLinear, i.uv + t * float2(1, -1), 0) +
         gSrc.SampleLevel(gLinear, i.uv + t * float2(-1, 1), 0) + gSrc.SampleLevel(gLinear, i.uv + t * float2(1, 1), 0);
    return s / 16.0 * gP0.z;
}
