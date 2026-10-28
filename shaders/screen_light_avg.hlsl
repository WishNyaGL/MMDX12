// S8b: the average linear colour of every screen light's texture (Renderer::UpdateScreenLights).
//
// One 16x16 thread group per screen light (group index = the light's index in the frame's screen-light table): every
// thread samples the screen texture at its grid cell's centre, gamma-decodes the RGBA8 sRGB texel to linear light, the
// group reduces 256 values to one, and thread 0 writes float4(avg.rgb, 1) at 16 * index into the frame's average buffer.
// The scene pixel shaders read that entry (SceneConstants::screenLightParams.z = the buffer's SrvHeap index) and scale
// it by the light's emissive strength: a rectangular area light emits its own average radiance, not a projected image.
//
// The screen textures are in ALL_SHADER_RESOURCE for the frame (video: PIXEL_SHADER_RESOURCE), the state this pass reads
// them in. The table's axisA.w holds each light's texture as an SrvHeap index into the raster bindless texture table
// (t0 space2, the same one the scene pixel shaders use).
//
// Root constants b1 (the ComputePipeline's 16-dword block): gP0 = (screen light count, the screen-light table's SrvHeap
// index, 0, 0). Dispatch: (count, 1, 1).
#include "common.hlsli"

RWByteAddressBuffer gScreenAverage : register(u0);
Texture2D gScreenTex[] : register(t0, space2);    // the raster bindless table (the frame's screen textures)
ByteAddressBuffer gScreenBuf[] : register(t0, space3);   // the raster bindless table (the frame's light table)
SamplerState gScreenClamp : register(s1);         // the scene pass's gClamp slot (linear, clamp)

cbuffer ScreenAvgCB : register(b1) {
    float4 gP0;   // x = screen light count, y = the screen-light table's SrvHeap index
};

#define SCREEN_AVG_GROUP 16u
#define SCREEN_AVG_SAMPLES (SCREEN_AVG_GROUP * SCREEN_AVG_GROUP)

groupshared float3 gSum[SCREEN_AVG_SAMPLES];

// The screen texture of light `light`: GpuScreenLight::axisA.w (byte offset 44 of the 64-byte entry).
uint ScreenTextureSrv(uint light) {
    const ByteAddressBuffer table = gScreenBuf[NonUniformResourceIndex(SafeSrv(gP0.y))];
    return (uint)asfloat(table.Load(light * 64u + 44u));
}

// A texel of linear light: the screen textures are RGBA8 UNORM holding display-referred (sRGB) images.
float3 ScreenSrgbToLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow(max(c, 0.0) * (1.0 / 1.055) + 0.055 / 1.055, 2.4);
}

[numthreads(SCREEN_AVG_GROUP, SCREEN_AVG_GROUP, 1)]
void CSScreenAvg(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint light = gid.x;
    const uint local = tid.y * SCREEN_AVG_GROUP + tid.x;
    float3 c = 0.0;
    if (light < (uint)gP0.x) {
        const Texture2D tex = gScreenTex[NonUniformResourceIndex(ScreenTextureSrv(light))];
        uint w = 0u, h = 0u;
        tex.GetDimensions(w, h);
        if (w > 0u && h > 0u) {
            const float2 uv = (float2(tid.xy) + 0.5) / (float)SCREEN_AVG_GROUP;   // the group's cell centre
            c = ScreenSrgbToLinear(saturate(tex.SampleLevel(gScreenClamp, uv, 0.0).rgb));
        }
    }
    gSum[local] = c;
    GroupMemoryBarrierWithGroupSync();
    // 256 -> 1 over the group's threads
    [unroll] for (uint s = SCREEN_AVG_SAMPLES / 2u; s > 0u; s >>= 1) {
        if (local < s) gSum[local] += gSum[local + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (local == 0u && light < (uint)gP0.x) {
        const float3 avg = gSum[0] / (float)SCREEN_AVG_SAMPLES;
        gScreenAverage.Store4(light * 16u, asuint(float4(avg, 1.0)));   // bit pattern, not a value cast
    }
}
