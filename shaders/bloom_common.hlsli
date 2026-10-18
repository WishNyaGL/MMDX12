// Bloom threshold shared by the prefilter (bloom.hlsl) and the energy-conserving blend (post.hlsl): the share of a
// colour that scatters, a soft knee of half the threshold on its brightest channel.
#ifndef MMDX_BLOOM_COMMON_HLSLI
#define MMDX_BLOOM_COMMON_HLSLI
float BloomContribution(float3 c, float threshold) {
    float knee = threshold * 0.5;
    float br = max(c.r, max(c.g, c.b));
    float rq = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    rq = rq * rq / (4.0 * knee + 1e-4);
    return max(rq, br - threshold) / max(br, 1e-4);
}
#endif
