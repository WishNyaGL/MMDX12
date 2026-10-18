// Final grade: bloom, exposure, contrast/saturation, Khronos PBR Neutral tonemap (keeps
// MMD albedo colours intact below ~0.8), vignette, sRGB encode with dithering.
//   t0 HDR, t1 bloom (may be null; rgb, a = luminance x linear depth), t2 colour LUT strip (may be null), t3 depth.
//   gP0 = (exposure, bloomIntensity x bloom normalisation, contrast, saturation), gP1 = (vignette, transparentBg, aspect, bloomOn)
//   gP2 = (LUT intensity (0 = off), bloom depth mask 0..1, bloom blend (0 additive, 1 energy-conserving), threshold)
//   gP3.x = bloom intensity (the energy-conserving share)
#include "fullscreen.hlsli"
#include "bloom_common.hlsli"

Texture2D<float4> gHdr : register(t0);
Texture2D<float4> gBloomTex : register(t1);
Texture2D<float4> gLut : register(t2);
Texture2D<float> gDepth : register(t3);

// Bloom at this pixel. The depth mask fades the light of sources behind the pixel's surface (the bloom's depth is
// the light-weighted depth of its sources), so far stage lights do not wash over a nearer character.
float3 BloomAt(FsOut i) {
    float4 b = gBloomTex.SampleLevel(gLinear, i.uv, 0);
    float3 c = max(b.rgb, 0.0);
    if (gP2.y > 0.0) {
        float lum = Luminance(c);
        float zb = b.a / max(lum, 1e-6);
        float zp = LinearZ(gDepth.SampleLevel(gPoint, i.uv, 0));
        float behind = max(zb - zp, 0.0) / max(zp, 1e-3);
        if (lum > 1e-6) c *= lerp(1.0, exp2(-behind * 6.0), gP2.y);
    }
    return c * gP0.y;
}

float3 PbrNeutral(float3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, newPeak.xxx, g);
}

// 32^3 LUT as a (32*32) x 32 strip: x = b * 32 + r, y = g (display-referred sRGB in and out).
float3 SampleLut(float3 c) {
    const float N = 32.0;
    c = saturate(c);
    float b = c.b * (N - 1.0);
    float b0 = floor(b);
    float b1 = min(b0 + 1.0, N - 1.0);
    float2 uv = float2((c.r * (N - 1.0) + 0.5) / (N * N), (c.g * (N - 1.0) + 0.5) / N);
    float3 s0 = gLut.SampleLevel(gLinear, uv + float2(b0 / N, 0.0), 0).rgb;
    float3 s1 = gLut.SampleLevel(gLinear, uv + float2(b1 / N, 0.0), 0).rgb;
    return lerp(s0, s1, b - b0);
}

float4 PSPost(FsOut i) : SV_Target {
    float4 src = gHdr.SampleLevel(gPoint, i.uv, 0);
    float3 c = max(src.rgb, 0.0);
    if (gP1.w > 0.5) {
        float3 b = BloomAt(i);
        // energy-conserving: the scattered share of the bright light leaves the pixel and the bloom brings it back
        // spread out; additive: the bloom is extra light
        if (gP2.z > 0.5) c += b - c * (BloomContribution(c, gP2.w) * saturate(gP3.x));
        else c += b;
    }
    c *= gP0.x;
    // contrast around middle grey in log space, then saturation
    float3 lc = log2(max(c, 1e-5) / 0.18);
    c = 0.18 * exp2(lc * gP0.z);
    float l = Luminance(c);
    c = max(lerp(l.xxx, c, gP0.w), 0.0);
    c = PbrNeutral(c);
    // vignette
    float2 v = (i.uv - 0.5) * float2(gP1.z, 1.0);
    c *= 1.0 - gP1.x * smoothstep(0.35, 1.05, length(v));
    c = LinearToSrgb(saturate(c));
    if (gP2.x > 0.0) c = lerp(c, SampleLut(c), gP2.x);
    c += (Ign(i.pos.xy) - 0.5) / 255.0;
    float a = gP1.y > 0.5 ? saturate(src.a) : 1.0;
    return float4(c, a);
}

// UI backdrop: downsample + separable gaussian of the final LDR image.
//   gP0.xy = source texel size, gP0.zw = blur direction (uv units, 0 for the downsample).
float4 PSBackdrop(FsOut i) : SV_Target {
    if (gP0.z == 0.0 && gP0.w == 0.0) {
        float2 t = gP0.xy;
        float4 s = gHdr.SampleLevel(gLinear, i.uv + t * float2(-1, -1), 0) + gHdr.SampleLevel(gLinear, i.uv + t * float2(1, -1), 0) +
                   gHdr.SampleLevel(gLinear, i.uv + t * float2(-1, 1), 0) + gHdr.SampleLevel(gLinear, i.uv + t * float2(1, 1), 0);
        return s * 0.25;
    }
    static const float w[5] = {0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216};
    float4 s = gHdr.SampleLevel(gLinear, i.uv, 0) * w[0];
    [unroll] for (int k = 1; k < 5; ++k) {
        s += gHdr.SampleLevel(gLinear, i.uv + gP0.zw * k * 1.4, 0) * w[k];
        s += gHdr.SampleLevel(gLinear, i.uv - gP0.zw * k * 1.4, 0) * w[k];
    }
    return s;
}
