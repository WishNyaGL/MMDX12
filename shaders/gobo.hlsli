// S7b: spot gobos (a stencil projected through the cone, docs/stage_format.md "lights" gobo block).
//
// The frame's gobo table (SceneConstants::goboParams, C++: GpuGobo in src/render/ShaderInterop.h) is one float4 per
// punctual light: (gobo texture SrvHeap index or -1, rotation radians incl. spin x time, scale, mip bias from focus).
// gGoboParams.x is the SrvHeap index of that table (-1 = no light of the frame has a gobo: no fetch, no cost).
//
// Projection: in the spot's own frame (+z = the light direction, an up vector picked stably) the point's local xy
// divided by its z gives tan of the off-axis angle; dividing by tan(outer half angle) maps the cone onto the
// [-1, 1] square, then rotation / scale / +0.5 give the uv. uv outside [0, 1] = no light (the sample is 0), so the
// gobo multiplies the light's colour (rgb: coloured gobos work).
//
// Include after common.hlsli (SceneConstants) AND a bindless table. The includer defines
//   GOBO_TEX(i)   the bindless Texture2D[] indexed at SrvHeap index i
//   GOBO_BUF(i)   the bindless raw ByteAddressBuffer[] indexed at SrvHeap index i
//   GOBO_SAMPLER  the linear wrap sampler to sample it with
// or leaves them to the defaults below (rt_common.hlsli's gBindlessTex / gBindlessBuf / gRtWrap when it is included,
// else pbr_common.hlsli's gPbrTex / gPbrBuf / gPbrWrap).
#ifndef MMDX_GOBO_HLSLI
#define MMDX_GOBO_HLSLI

#ifndef GOBO_TEX
#ifdef MMDX_RT_COMMON_HLSLI
#define GOBO_TEX(i) gBindlessTex[NonUniformResourceIndex((uint)(i))]
#define GOBO_BUF(i) gBindlessBuf[NonUniformResourceIndex((uint)(i))]
#define GOBO_SAMPLER gRtWrap
#else
#define GOBO_TEX(i) gPbrTex[NonUniformResourceIndex((uint)(i))]
#define GOBO_BUF(i) gPbrBuf[NonUniformResourceIndex((uint)(i))]
#define GOBO_SAMPLER gPbrWrap
#endif
#endif

// The gobo of light `lightIndex` (the index into the frame's punctual light list): its packed table entry. `on` false =
// no gobo on this light / this frame (the caller keeps the plain cone).
struct GoboRef {
    float srv;
    float rotation;
    float scale;
    float mipBias;
    bool on;
};

GoboRef GoboOf(uint lightIndex) {
    GoboRef g;
    g.srv = -1.0;
    g.rotation = 0.0;
    g.scale = 1.0;
    g.mipBias = 0.0;
    g.on = false;
    const int table = (int)gGoboParams.x;
    if (table < 0) return g;
    // one GpuGobo (16 B) per light: 4 floats at lightIndex * 16 B
    ByteAddressBuffer gobos = GOBO_BUF(SafeSrv(table));
    g.srv = asfloat(gobos.Load(lightIndex * 16u + 0u));
    g.rotation = asfloat(gobos.Load(lightIndex * 16u + 4u));
    g.scale = asfloat(gobos.Load(lightIndex * 16u + 8u));
    g.mipBias = asfloat(gobos.Load(lightIndex * 16u + 12u));
    g.on = g.srv >= 0.0;
    return g;
}

// The gobo's uv for a world point `wp` lit by a spot at `lightPos` pointing along the unit `lightDir` with the cone
// cosines cosOuter / cosInner (the GpuLight / PtLight / VolLight fields). `normalized` is the local xy / z (tan of the
// off-axis angle) divided by tan(outer half angle): the cone's axis is 0, the cone surface is at radius 1.
float2 GoboUv(float3 wp, float3 lightPos, float3 lightDir, float cosOuter, GoboRef g) {
    // the spot's frame: local z = the light direction, local y = world up projected (world x / z when nearly parallel)
    const float3 fwd = normalize(lightDir);
    float3 up = float3(0.0, 1.0, 0.0);
    if (abs(dot(fwd, up)) > 0.98) up = float3(1.0, 0.0, 0.0);
    const float3 right = normalize(cross(up, fwd));
    up = cross(fwd, right);
    const float3 d = wp - lightPos;
    const float z = dot(d, fwd);
    // tan(outer half angle) from its cosine; a degenerate / point light falls back to a wide cone
    const float c = clamp(cosOuter, -0.9999, 0.9999);
    const float tanOuter = sqrt(max(1.0 - c * c, 1e-6)) / max(abs(c), 1e-4);
    const float2 axis = float2(dot(d, right), dot(d, up)) / max(z, 1e-5) / tanOuter;
    const float2 scaled = axis / max(g.scale, 1e-4);
    const float s = sin(g.rotation), cs = cos(g.rotation);
    const float2 rot = float2(scaled.x * cs - scaled.y * s, scaled.x * s + scaled.y * cs);
    return rot * 0.5 + 0.5;
}

// The gobo's factor (linear rgb) at world point `wp` for light `lightIndex`: 1 when the light has no gobo, 0 outside the
// stencil's opaque parts. `loD` adds to the table's focus mip bias (the volume paths pass 1 to limit noise).
float3 GoboSample(GoboRef g, float3 wp, float3 lightPos, float3 lightDir, float cosOuter, float loD) {
    if (!g.on) return 1.0;
    const float2 uv = GoboUv(wp, lightPos, lightDir, cosOuter, g);
    if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;
    return GOBO_TEX(SafeSrv(g.srv)).SampleLevel(GOBO_SAMPLER, uv, max(g.mipBias + loD, 0.0)).rgb;
}

// The same factor for a light the caller already has as (position, direction, cosOuter) - the per-light convenience.
float3 GoboFactor(uint lightIndex, float3 wp, float3 lightPos, float3 lightDir, float cosOuter, float loD = 0.0) {
    return GoboSample(GoboOf(lightIndex), wp, lightPos, lightDir, cosOuter, loD);
}

#endif
