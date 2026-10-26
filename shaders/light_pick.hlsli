// S7a: punctual-light pick from the frame's power CDF (Renderer::FillLightCdf, uploaded as raw floats and bound
// through the bindless buffer table: SceneConstants::lightCdfParams = (SrvHeap index, light count)).
//
// The CDF is a normalised inclusive running sum of the pick weight (intensity * luminance(colour) * range^2, times the
// spot cone's solid-angle proxy). Sampling it and dividing by the picked light's probability gives an unbiased
// estimate of the all-lights sum with fewer samples than the uniform pick. A frame without a CDF (index < 0) or with
// a single light keeps the caller's old uniform pick.
#ifndef MMDX_LIGHT_PICK_HLSLI
#define MMDX_LIGHT_PICK_HLSLI

// Requires rt_common.hlsli (gBindlessBuf = ByteAddressBuffer[], t0 space3 / the world's t0 space8).
// The CDF entry of light `i` (one float per light).
float LightCdfAt(ByteAddressBuffer cdf, uint i) { return asfloat(cdf.Load(i * 4u)); }

// Returns the picked light index and its discrete probability (0 when the CDF is unusable).
uint LightCdfPick(float u, uint lightCount, out float pdf) {
    pdf = 0.0;
    const int idx = (int)gLightCdf.x;
    if (idx < 0 || lightCount == 0u) return 0u;
    ByteAddressBuffer cdf = gBindlessBuf[NonUniformResourceIndex((uint)idx)];
    // binary search for the first entry whose running sum reaches u
    uint lo = 0u, hi = lightCount - 1u;
    [loop] while (lo < hi) {
        uint mid = (lo + hi) >> 1u;
        if (LightCdfAt(cdf, mid) < u) lo = mid + 1u;
        else hi = mid;
    }
    const float below = lo == 0u ? 0.0 : LightCdfAt(cdf, lo - 1u);
    const float above = LightCdfAt(cdf, lo);
    pdf = max(above - below, 0.0);
    return lo;
}

#endif
