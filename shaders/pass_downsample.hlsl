// Intermediate pass mip downsample (passes.mips capability)
// Samples mip N-1 with bilinear filter into mip N.
Texture2D<float4> gSrc : register(t0);
RWTexture2D<float4> gDst : register(u0);
SamplerState gLinear : register(s0);

cbuffer CB : register(b0) {
    uint2 gDstSize;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gDstSize.x || id.y >= gDstSize.y) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(gDstSize);
    gDst[id.xy] = gSrc.SampleLevel(gLinear, uv, 0);
}
