// S9b-3: the Sky light's image (an equirect HDRI / LDR image / Ray-MMD RGBT dome / DDS cubemap decoded by
// render/SkyMap.cpp into one RGBA16F equirect texture). SkyColor (common.hlsli) calls SkyMapRadiance while
// gSkyParams.x > 1.5; the image is then the background AND, through S9b-2's sky environment buffer, the ambient /
// environment light in every path (that buffer is built from SkyColor).
//
// SceneConstants (C++: ShaderInterop.h skyMapParams / skyMapSize): x = SrvHeap index of the texture (-1 = none),
// y = the rotation around +y in radians, z = the radiance scale (skyMapIntensity), w = mirror (1 = flips the panorama
// left / right); gSkyMapSize.xy = the texture's size in texels.
//
// The mapping matches render/SkyMap.cpp's cube -> equirect resampling and tools/stagegen's .hdr writer:
// u = atan2(dir.x, dir.z) / 2 PI + 0.5 (u = 0.5 faces +Z), v = acos(dir.y) / PI (v = 0 is the zenith).
//
// Include AFTER common.hlsli (the prototype / SceneCB) and after a bindless table, with the includer defining
//   SKY_MAP_TEX(i)    the bindless Texture2D[] indexed at SrvHeap index i
//   SKY_MAP_SAMPLER   the linear wrap sampler to sample it with
// (mmd.hlsl: gPbrTex / gPbrWrap; pathtrace / offline_gi / rtreflect: gBindlessTex / gRtWrap; sky_env.hlsl declares its
// own table).
#ifndef MMDX_SKY_MAP_HLSLI
#define MMDX_SKY_MAP_HLSLI

float3 SkyMapRadiance(float3 dir) {
    if (gSkyMapParams.x < 0.0) return 0.0;
    float u = (atan2(dir.x, dir.z) + gSkyMapParams.y) / (2.0 * PI) + 0.5;
    if (gSkyMapParams.w > 0.5) u = 1.0 - u;
    u = frac(u);
    // the v clamp keeps a bilinear sample off the edge rows (the panorama's zenith / nadir row is one texel)
    float v = clamp(acos(clamp(dir.y, -1.0, 1.0)) / PI, 0.5 / gSkyMapSize.y, 1.0 - 0.5 / gSkyMapSize.y);
    return SKY_MAP_TEX(SafeSrv(gSkyMapParams.x)).SampleLevel(SKY_MAP_SAMPLER, float2(u, v), 0.0).rgb * gSkyMapParams.z;
}

#endif // MMDX_SKY_MAP_HLSLI
