// Path-trace denoising: temporal accumulation against the previous history (bilinear,
// depth + normal validated), a-trous edge-aware smoothing, albedo remodulation.
#include "rt_common.hlsli"   // includes common.hlsli
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);
SamplerState gLinearWrap : register(s2);

// ---- temporal accumulation ------------------------------------------------------

Texture2D<float4> gLight : register(t0);
Texture2D<float4> gHistory : register(t1);
Texture2D<float2> gVelocity : register(t2);
Texture2D<float> gDepth : register(t3);
Texture2D<float4> gNormal : register(t4);
Texture2D<float> gPrevDepth : register(t5);
Texture2D<float4> gPrevNormal : register(t6);
RWTexture2D<float4> gHistOut : register(u0);
RWTexture2D<float> gDepthCopy : register(u1);
RWTexture2D<float4> gNormalCopy : register(u2);
Texture2D<float4> gAlbedoPass : register(t7);   // this pass's primary albedo
RWTexture2D<float4> gRadPass : register(u3);     // this pass's radiance = light * albedo (offline video accumulates it)

[numthreads(8, 8, 1)]
void CSTemporal(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gHistOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    int2 p = (int2)id.xy;

    float3 c = min(gLight[p].rgb, 64.0);
    float4 pa = gAlbedoPass[p];
    gRadPass[p] = float4(c * pa.rgb, pa.a);
    float d = gDepth[p];
    float4 nt = gNormal[p];
    gDepthCopy[p] = d;
    gNormalCopy[p] = nt;
    if (gP1.y > 0.5 && gP0.x > 0.5) {
        // another pass of the same video frame: same pose and camera, only the jitter and the random numbers differ,
        // so the history of this very pixel always belongs to it (sky and silhouettes included)
        float4 h = gHistory[p];
        float sl = min(h.a + 1.0, gP0.y);
        gHistOut[p] = float4(lerp(h.rgb, c, max(1.0 / sl, gP0.z)), sl);
        return;
    }
    if (d >= 1.0 || gP0.x < 0.5) { gHistOut[p] = float4(c, 1.0); return; }   // sky or no history

    float2 size = gViewportSize;
    float2 uv = (p + 0.5) / size;
    float2 puv = uv - gVelocity[p];
    float2 pos = puv * size - 0.5;
    int2 base = (int2)floor(pos);
    float2 f = pos - base;
    float z = LinearZ(d);
    float3 n = OctDecode(nt.xy);

    float4 sum = 0;
    float wsum = 0;
    [unroll] for (int j = 0; j < 2; ++j)
    [unroll] for (int i = 0; i < 2; ++i) {
        int2 q = base + int2(i, j);
        if (any(q < 0) || any(q >= (int2)size)) continue;
        float pd = gPrevDepth[q];
        float4 pn = gPrevNormal[q];
        bool valid = pd < 1.0 && abs(LinearZ(pd) - z) < 0.05 * z && dot(OctDecode(pn.xy), n) > 0.8;
        float w = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y);
        if (valid) { sum += gHistory[q] * w; wsum += w; }
    }
    if (wsum < 0.01) { gHistOut[p] = float4(c, 1.0); return; }

    float4 hist = sum / wsum;
    if (gP1.x > 0.5) hist.a = min(hist.a, gP0.w);   // first pass of a video frame: limit cross-frame ghosting
    float len = min(hist.a + 1.0, gP0.y);
    float alpha = max(1.0 / len, gP0.z);
    gHistOut[p] = float4(lerp(hist.rgb, c, alpha), len);
}

// ---- temporal accumulation of the radiance, effective albedo ---------------------------
// The light is demodulated by the albedo of THIS pass, so a primary hit that changes between passes (stochastic
// transparency such as hair over skin, and every silhouette under the sub-pixel jitter) would show one pass's albedo
// as a binary dither and a stair-stepped edge. For offline video (RenderSettings::ptAccumAlbedo) the radiance
// R = light * albedo is accumulated with the light's weights and the albedo handed to the remodulation is
// R_acc / L_acc: exactly the mean radiance before the a-trous filter, and the plain albedo where it never changes.
// (Averaging the albedo itself is biased: light = R / albedo of the first sample is correlated with that albedo.)

Texture2D<float4> gLightHistCur : register(t0);   // history_[cur]: rgb = L_acc, a = the length the light pass just used
Texture2D<float4> gRadCur : register(t1);
Texture2D<float4> gRadPrev : register(t2);
Texture2D<float2> gVelA : register(t3);
Texture2D<float> gDepthA : register(t4);
Texture2D<float4> gNormalA : register(t5);
Texture2D<float> gPrevDepthA : register(t6);
Texture2D<float4> gPrevNormalA : register(t7);
RWTexture2D<float4> gRadOut : register(u0);
RWTexture2D<float4> gAlbEff : register(u1);

float4 RadianceHistory(int2 p, float4 c, float len, float alpha) {
    if (gP0.x < 0.5 || len <= 1.0) return c;   // no history, or the light pass restarted it
    if (gP1.y > 0.5) return lerp(gRadPrev[p], c, alpha);   // same frame: this pixel's own history
    float d = gDepthA[p];
    if (d >= 1.0) return c;
    float2 size = gViewportSize;
    float2 puv = (p + 0.5) / size - gVelA[p];
    float2 pos = puv * size - 0.5;
    int2 base = (int2)floor(pos);
    float2 f = pos - base;
    float z = LinearZ(d);
    float3 n = OctDecode(gNormalA[p].xy);
    float4 sum = 0;
    float wsum = 0;
    [unroll] for (int j = 0; j < 2; ++j)
    [unroll] for (int i = 0; i < 2; ++i) {
        int2 q = base + int2(i, j);
        if (any(q < 0) || any(q >= (int2)size)) continue;
        float pd = gPrevDepthA[q];
        bool valid = pd < 1.0 && abs(LinearZ(pd) - z) < 0.05 * z && dot(OctDecode(gPrevNormalA[q].xy), n) > 0.8;
        float wq = (i ? f.x : 1.0 - f.x) * (j ? f.y : 1.0 - f.y);
        if (valid) { sum += gRadPrev[q] * wq; wsum += wq; }
    }
    if (wsum < 0.01) return c;
    return lerp(sum / wsum, c, alpha);
}

[numthreads(8, 8, 1)]
void CSTemporalAlbedo(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gRadOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    int2 p = (int2)id.xy;

    float4 lacc = gLightHistCur[p];
    float len = max(lacc.a, 1.0);
    float4 r = RadianceHistory(p, gRadCur[p], len, max(1.0 / len, gP0.z));
    gRadOut[p] = r;
    gAlbEff[p] = float4(r.rgb / max(lacc.rgb, 1e-3), r.a);
}

// ---- a-trous filter --------------------------------------------------------------

Texture2D<float4> gIn : register(t0);
Texture2D<float> gDepth2 : register(t1);
Texture2D<float4> gNormal2 : register(t2);
RWTexture2D<float4> gOut : register(u0);

[numthreads(8, 8, 1)]
void CSAtrous(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    int2 p = (int2)id.xy;

    float4 center = gIn[p];
    float d = gDepth2[p];
    if (d >= 1.0) { gOut[p] = center; return; }   // sky

    float z = LinearZ(d);
    float3 n = OctDecode(gNormal2[p].xy);
    float l = Luminance(center.rgb);
    int step = (int)gP0.x;
    static const float k[5] = { 1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0 };

    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int y = -2; y <= 2; ++y)
    [unroll] for (int x = -2; x <= 2; ++x) {
        int2 q = p + int2(x, y) * step;
        if (any(q < 0) || q.x >= (int)w || q.y >= (int)h) continue;
        float dq = gDepth2[q];
        if (dq >= 1.0) continue;
        float wz = exp(-abs(LinearZ(dq) - z) / (0.03 * z * step + 1e-3));
        float wn = pow(saturate(dot(OctDecode(gNormal2[q].xy), n)), 32.0);
        float4 cq = gIn[q];
        float wl = exp(-abs(Luminance(cq.rgb) - l) / (gP0.y * max(l, 0.05) + 1e-4));
        float wgt = k[x + 2] * k[y + 2] * wz * wn * wl;
        sum += cq.rgb * wgt;
        wsum += wgt;
    }
    gOut[p] = float4(sum / max(wsum, 1e-6), center.a);   // the centre tap always contributes (w = k*k)
}

// ---- albedo remodulation ---------------------------------------------------------

Texture2D<float4> gFiltered : register(t0);
Texture2D<float4> gAlbedo : register(t1);
Texture2D<float> gDepth3 : register(t2);
RWTexture2D<float4> gColorOut : register(u0);

[numthreads(8, 8, 1)]
void CSModulate(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gColorOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    int2 p = (int2)id.xy;

    float4 a = gAlbedo[p];
    gColorOut[p] = float4(gFiltered[p].rgb * a.rgb, gP0.x > 0.5 ? a.a : 1.0);
}
