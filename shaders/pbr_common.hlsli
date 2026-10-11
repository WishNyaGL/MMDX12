#ifndef MMDX_PBR_COMMON_HLSLI
#define MMDX_PBR_COMMON_HLSLI
// Per-material PBR data (work unit P2). Mirror of GpuPbrMaterial in src/render/ShaderInterop.h; the values come
// from Ray-MMD material data decoded by asset/PbrMaterial.h. This is the plumbing only: the P3 lighting uses the
// sampling decoders below.
//
// Rule: a material shaded by an active shader pack ignores its PBR record (the pack wins). The raster pack path is
// PSPack, which never reads this file; only the default PSMain debug branch does.
//
// Resource bindings (must already be declared by the includer):
//   raster: Texture2D gPbrTex[] (t0 space2) + ByteAddressBuffer gPbrBuf[] (t0 space3) + SamplerState gPbrWrap (s4)
//   RT:     include rt_common.hlsli first (it declares gBindlessTex / gBindlessBuf / gRtWrap at the same slots).
// The record buffer's SRV uses an absolute SrvHeap index (its raw descriptors live in the same bindless heap the
// path tracer reads).
#ifndef MMDX_RT_COMMON_HLSLI
// The unbounded gPbrTex / gPbrBuf match the texAll / bufAll tables (params 9/10) the scene root signature declares;
// gPbrWrap is the sampler at s4. Under RT_SHADOWS these map onto rt_common.hlsli's names (see the #else).
Texture2D gPbrTex[] : register(t0, space2);
ByteAddressBuffer gPbrBuf[] : register(t0, space3);
SamplerState gPbrWrap : register(s4);
#else
// rt_common.hlsli already declared the bindless tables under their RT names.
#define gPbrTex gBindlessTex
#define gPbrBuf gBindlessBuf
#define gPbrWrap gRtWrap
#endif

// Slot order mirrors PbrSlotId (asset/PbrMaterial.h): Albedo .. CustomB.
#define PBR_SLOT_ALBEDO      0
#define PBR_SLOT_ALBEDO_SUB  1
#define PBR_SLOT_ALPHA       2
#define PBR_SLOT_NORMAL      3
#define PBR_SLOT_NORMAL_SUB  4
#define PBR_SLOT_SMOOTHNESS  5
#define PBR_SLOT_METALNESS   6
#define PBR_SLOT_SPECULAR    7
#define PBR_SLOT_OCCLUSION   8
#define PBR_SLOT_EMISSIVE    9
#define PBR_SLOT_CUSTOM_A   10
#define PBR_SLOT_CUSTOM_B   11
#define PBR_SLOT_COUNT      12

#define PBR_FLAG_VALID    1u
#define PBR_FLAG_CUTOUT   2u
#define PBR_FLAG_EMISSIVE 4u

// Decoded record: one "slot" (constant + texture) per field; the material header carries the shading model and
// alpha information. The field order matches GpuPbrMaterial / GpuPbrSlot.
struct GpuPbrSlot {
    uint type;         // Ray <PREFIX>_MAP_TYPE
    uint swizzle;      // 0 R, 1 G, 2 B, 3 A
    uint uvFlip;       // 0 none, 1 X, 2 Y, 3 both; NORMAL 4 = flip the shading normal X
    uint applyScale;   // 0 none, 1 map * value, 2 map ^ value
    uint colorFlip;    // CUSTOM_A/B: 1 inverts the value
    int textureSrv;    // absolute SrvHeap index of the slot texture (-1 = none)
    float loopU, loopV;
    float4 value;      // xyz = constant value, w = scale (normalMapScale / ...)
    float4 uvScale;    // xy = <PREFIX>_MAP_UV_SCALE
};

struct GpuPbrMaterial {
    uint flags;
    uint shadingModel;      // CUSTOM_ENABLE (0..9)
    float alphaThreshold;
    float diffuseAlpha;
    float4 params;          // x = ALBEDO_SUB_ENABLE blend mode
    GpuPbrSlot slots[PBR_SLOT_COUNT];
};

GpuPbrSlot PbrLoadSlot(ByteAddressBuffer b, uint base) {
    GpuPbrSlot s;
    s.type = b.Load(base + 0);
    s.swizzle = b.Load(base + 4);
    s.uvFlip = b.Load(base + 8);
    s.applyScale = b.Load(base + 12);
    s.colorFlip = b.Load(base + 16);
    s.textureSrv = asint(b.Load(base + 20));
    s.loopU = asfloat(b.Load(base + 24));
    s.loopV = asfloat(b.Load(base + 28));
    s.value = asfloat(b.Load4(base + 32));
    s.uvScale = asfloat(b.Load4(base + 48));
    return s;
}

GpuPbrMaterial PbrLoadMaterial(int srvIndex, uint index) {
    GpuPbrMaterial m;
    ByteAddressBuffer b = gPbrBuf[NonUniformResourceIndex((uint)srvIndex)];
    const uint base = index * 800u;   // sizeof(GpuPbrMaterial)
    m.flags = b.Load(base + 0);
    m.shadingModel = b.Load(base + 4);
    m.alphaThreshold = asfloat(b.Load(base + 8));
    m.diffuseAlpha = asfloat(b.Load(base + 12));
    m.params = asfloat(b.Load4(base + 16));
    [unroll] for (uint i = 0; i < PBR_SLOT_COUNT; ++i)
        m.slots[i] = PbrLoadSlot(b, base + 32u + i * 64u);
    return m;
}

// ---- slot decoding (Ray semantics: docs/raymmd_semantics.md) --------------------------------

// UV transform: loop counts, optional UV_SCALE, then the flip.
float2 PbrSlotUv(GpuPbrSlot s, float2 uv) {
    uv *= float2(s.loopU, s.loopV);
    uv *= s.uvScale.xy;
    if (s.uvFlip == 1u || s.uvFlip == 3u) uv.x = 1.0 - uv.x;
    if (s.uvFlip == 2u || s.uvFlip == 3u) uv.y = 1.0 - uv.y;
    return uv;
}

// Sampled texel, or the slot's constant when it has no texture (the load-time fallback).
float4 PbrSlotTexel(GpuPbrSlot s, float2 uv) {
    if (s.textureSrv >= 0)
        return gPbrTex[NonUniformResourceIndex((uint)s.textureSrv)].SampleLevel(gPbrWrap, PbrSlotUv(s, uv), 0);
    return float4(s.value.xyz, 1.0);
}

float PbrChannel(float4 c, uint swizzle) { return c[swizzle]; }

// roughness from a SMOOTHNESS slot sample per SMOOTHNESS_MAP_TYPE (0 value = smoothness, 1 roughness,
// 2 linear roughness); SmoothnessToRoughness(s) = (1-s)^2.
float PbrSlotRoughness(GpuPbrSlot s, float2 uv) {
    float v = PbrChannel(PbrSlotTexel(s, uv), s.swizzle);
    if (s.type == 0u) return pow(max(0.0, 1.0 - v), 2.0);
    return max(0.0, v);   // types 1 / 2 store roughness
}

// metalness scalar (METALNESS has no meaningful MAP_TYPE).
float PbrSlotMetalness(GpuPbrSlot s, float2 uv) { return saturate(PbrChannel(PbrSlotTexel(s, uv), s.swizzle)); }

// occlusion scalar; OCCLUSION_MAP_TYPE 1/3 are sRGB-encoded.
float PbrSlotAo(GpuPbrSlot s, float2 uv) {
    float v = PbrChannel(PbrSlotTexel(s, uv), s.swizzle);
    if (s.type == 1u || s.type == 3u) v = SrgbToLinear(v.xxx).x;
    return saturate(v);
}

// Colour for the colour-carrying slots (albedo / sub / emissive / custom B): sRGB source -> linear is left to the
// caller (P3); the debug view shows the raw value.
float3 PbrSlotRgb(GpuPbrSlot s, float2 uv) { return PbrSlotTexel(s, uv).rgb; }

// Tangent-space normal sample per NORMAL_MAP_TYPE (docs/raymmd_semantics.md §1.3), strength folded in.
float3 PbrDecodeNormal(GpuPbrSlot s, float2 uv) {
    float3 c = PbrSlotTexel(s, uv).rgb;
    if (s.type == 4u) return normalize(c * 2.0 - 1.0);           // world-space normal
    float3 n = c * 2.0 - 1.0;
    n.xy *= max(s.value.w, 0.0) > 0.0 ? s.value.w : 1.0;          // normalMapScale
    if (s.type == 1u) {
        n.z = sqrt(saturate(1.0 - dot(n.xy, n.xy)));
    } else if (s.type == 2u || s.type == 3u) {
        n = float3(0, 0, 1);   // grayscale bump: no tangent-space vector (screen-derivative only, P3)
    }
    return normalize(n);
}

// World normal from a tangent-space vector: a screen-derivative tangent frame (Ray-MMD does the same;
// docs/raymmd_semantics.md §5). P is the world position.
float3 PbrTangentToWorld(float3 N, float3 P, float2 uv, float3 nTangent) {
    float3 dp1 = ddx(P), dp2 = ddy(P);
    float2 duv1 = ddx(uv), duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    // bitangent negated as Ray builds the TBN (mat[1] = -normalize(B))
    return normalize(T * nTangent.x * invmax - B * nTangent.y * invmax + N * nTangent.z);
}

// Triangle-UV tangent frame for the ray path: p0/p1/p2 are the hit triangle's world positions, uv0..2 its UVs.
// PMX has no vertex tangents, so the frame comes from the triangle (the RT counterpart of the raster derivative
// frame above).
float3 PbrTangentToWorldTri(float3 N, float3 p0, float3 p1, float3 p2, float2 uv0, float2 uv1, float2 uv2,
                            float3 nTangent) {
    float3 e1 = p1 - p0, e2 = p2 - p0;
    float2 d1 = uv1 - uv0, d2 = uv2 - uv0;
    float det = d1.x * d2.y - d1.y * d2.x;
    if (abs(det) < 1e-12) return N;
    float r = 1.0 / det;
    float3 T = (e1 * d2.y - e2 * d1.y) * r;
    float3 B = (e2 * d1.x - e1 * d2.x) * r;
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    return normalize(T * nTangent.x * invmax - B * nTangent.y * invmax + N * nTangent.z);
}

// Sun F0 per docs/raymmd_semantics.md §4 (SPECULAR_MAP_TYPE 0/1/2/3), grayscale sampling through swizzle.
float3 PbrSlotF0(GpuPbrSlot s, float2 uv) {
    float4 c = PbrSlotTexel(s, uv);
    float3 rgb = c.rgb;
    if (s.type >= 2u) rgb = PbrChannel(c, s.swizzle).xxx;
    if (s.type == 0u || s.type == 2u) rgb *= 0.08;
    else rgb *= 0.16 * rgb;
    return clamp(rgb, 0.01, 1.0);
}

// Distinct hue per shading model (PBR debug "model").
float3 PbrModelHue(uint model) {
    float h = frac((float)model * 0.61803398875);
    float3 k = frac(float3(1.0, 2.0 / 3.0, 1.0 / 3.0) + h);
    return saturate(abs(k * 6.0 - 3.0) - 1.0);
}

#endif // MMDX_PBR_COMMON_HLSLI
