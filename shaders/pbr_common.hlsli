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
// Pixel shaders set the screen-space UV gradients once in uniform control flow (PbrSetUvGradients, before any
// record-dependent branch) so slots sample with mips through SampleGrad; compute users (rtreflect) define
// PBR_SAMPLE_LEVEL0 and read mip 0.
#ifndef PBR_SAMPLE_LEVEL0
static float2 gPbrUvDx = 0, gPbrUvDy = 0;
void PbrSetUvGradients(float2 dx, float2 dy) { gPbrUvDx = dx; gPbrUvDy = dy; }
#endif
float4 PbrSlotTexel(GpuPbrSlot s, float2 uv) {
    if (s.textureSrv >= 0) {
#ifdef PBR_SAMPLE_LEVEL0
        return gPbrTex[NonUniformResourceIndex((uint)s.textureSrv)].SampleLevel(gPbrWrap, PbrSlotUv(s, uv), 0);
#else
        const float2 k = float2(s.loopU, s.loopV) * s.uvScale.xy;   // PbrSlotUv's scale (flips keep the length)
        return gPbrTex[NonUniformResourceIndex((uint)s.textureSrv)].SampleGrad(gPbrWrap, PbrSlotUv(s, uv),
                                                                             gPbrUvDx * k, gPbrUvDy * k);
#endif
    }
    return float4(s.value.xyz, 1.0);
}

float PbrChannel(float4 c, uint swizzle) { return c[swizzle]; }

// roughness from a SMOOTHNESS slot sample per SMOOTHNESS_MAP_TYPE (0 value = smoothness, 1 roughness,
// 2 linear roughness); SmoothnessToRoughness(s) = (1-s)^2. Single-channel slots apply their swizzle; R8 file
// uploads bake the channel into R (swizzle 0 in the record), PMX maps keep the declared channel.
float PbrSlotRoughness(GpuPbrSlot s, float2 uv) {
    float v = PbrChannel(PbrSlotTexel(s, uv), s.swizzle);
    if (s.type == 0u) return pow(max(0.0, 1.0 - v), 2.0);
    if (s.type == 2u) return v * v;   // linear (perceptual) roughness: s = 1 - r, so (1 - s)^2 = r^2 (glTF too)
    return max(0.0, v);               // type 1: s = 1 - sqrt(r), so (1 - s)^2 = r
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

// Sun F0 per docs/raymmd_semantics.md §4 (SPECULAR_MAP_TYPE 0/1/2/3). Types 2/3 are single-channel; R8 file
// uploads bake the swizzle channel into R (swizzle 0), PMX maps use the declared channel.
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

// ---- P3 surface decode (Ray semantics: docs/raymmd_semantics.md §1) -------------------------
// Scalar slots (ALPHA / SMOOTHNESS / METALNESS / OCCLUSION / CUSTOM_A) select their channel with the record's
// swizzle. A File map for such a slot is uploaded R8 with that channel baked into R and the record's swizzle set
// to 0 (GpuModel), while a PMX-sourced map is RGBA and keeps the declared swizzle. A slot without a texture falls
// back to its constant (value.x for a scalar slot).

// <slot>_MAP_APPLY_SCALE for a scalar slot (1 = value * const, 2 = value ^ const).
float PbrApplyScaleS(float v, GpuPbrSlot s) {
    if (s.applyScale == 1u) return v * s.value.x;
    if (s.applyScale == 2u) return pow(max(v, 0.0), s.value.x);
    return v;
}

// <slot>_MAP_APPLY_SCALE for a colour slot.
float3 PbrApplyScaleRgb(float3 c, GpuPbrSlot s) {
    if (s.applyScale == 1u) return c * s.value.xyz;
    if (s.applyScale == 2u) return pow(max(c, 0.0), s.value.xyz);
    return c;
}

// ALBEDO (+ ALBEDO_SUB blend, APPLY_DIFFUSE, APPLY_SCALE), linear. `pmxDiffuse` is the PMX material diffuse
// (gamma, as gDiffuse.rgb). Ray order: sample -> APPLY_DIFFUSE (gamma multiply) -> srgb2linear -> APPLY_SCALE ->
// ALBEDO_SUB blend (linear) -> saturate.
float3 PbrAlbedo(GpuPbrMaterial m, float2 uv, float3 pmxDiffuse) {
    GpuPbrSlot sa = m.slots[PBR_SLOT_ALBEDO];
    float3 a = PbrSlotRgb(sa, uv);
    if (m.params.y > 0.5) a *= pmxDiffuse;                 // ALBEDO_MAP_APPLY_DIFFUSE
    a = SrgbToLinear(saturate(a));
    a = PbrApplyScaleRgb(a, sa);

    const int sub = (int)m.params.x;                        // ALBEDO_SUB_ENABLE
    if (sub > 0) {
        GpuPbrSlot ss = m.slots[PBR_SLOT_ALBEDO_SUB];
        float4 s4 = PbrSlotTexel(ss, uv);
        float3 s = PbrApplyScaleRgb(SrgbToLinear(saturate(s4.rgb)), ss);
        if (sub == 1) a = a * s;
        else if (sub == 2) a = pow(max(a, 0.0), max(s, 1e-4));
        else if (sub == 3) a = a + s;
        else if (sub == 4) a = a * lerp(1.0, a * Luminance(a), s);   // ColorSynthesis
        else if (sub == 5) a = lerp(a, s, s4.a);                     // alpha blend
    }
    return saturate(a);
}

// ALPHA slot (Ray GetAlpha): the sampled/constant channel selected by swizzle, times the PMX diffuse alpha (the
// GPU record does not carry FROM; the library is ALPHA_MAP_FROM 3 almost everywhere, where Ray multiplies by
// MaterialDiffuse.a). R8 file uploads bake the channel into R.
float PbrAlpha(GpuPbrMaterial m, float2 uv) {
    GpuPbrSlot s = m.slots[PBR_SLOT_ALPHA];
    float v = PbrChannel(PbrSlotTexel(s, uv), s.swizzle) * m.diffuseAlpha;
    return saturate(PbrApplyScaleS(v, s));
}

// EMISSIVE colour, linear, with APPLY_SCALE (the morph/blink controls are out of scope in P3).
float3 PbrEmissive(GpuPbrMaterial m, float2 uv) {
    if ((m.flags & PBR_FLAG_EMISSIVE) == 0u) return 0;   // EMISSIVE_ENABLE 0: the slot's value is not light
    GpuPbrSlot s = m.slots[PBR_SLOT_EMISSIVE];
    float3 e = SrgbToLinear(saturate(PbrSlotRgb(s, uv)));
    return PbrApplyScaleRgb(e, s);
}

// CUSTOM_A scalar (curvature / sheen / threshold ...) with COLOR_FLIP.
float PbrCustomA(GpuPbrMaterial m, float2 uv) {
    GpuPbrSlot s = m.slots[PBR_SLOT_CUSTOM_A];
    float v = PbrChannel(PbrSlotTexel(s, uv), s.swizzle);
    v = PbrApplyScaleS(v, s);
    if (s.colorFlip == 1u) v = 1.0 - v;
    return saturate(v);
}

// CUSTOM_B colour (transmittance / fuzz / shadow colour) with COLOR_FLIP (the local library uses
// CUSTOM_B_MAP_TYPE 0 = sRGB decode, handled here).
float3 PbrCustomB(GpuPbrMaterial m, float2 uv) {
    GpuPbrSlot s = m.slots[PBR_SLOT_CUSTOM_B];
    float3 c = SrgbToLinear(saturate(PbrSlotRgb(s, uv)));
    if (s.colorFlip == 1u) c = 1.0 - c;
    return saturate(c);
}

// Screen-derivative tangent frame (Ray-MMD does the same; docs/raymmd_semantics.md §5). P is the world position.
void PbrTangentFrame(float3 N, float3 P, float2 uv, out float3 T, out float3 B) {
    float3 dp1 = ddx(P), dp2 = ddy(P);
    float2 duv1 = ddx(uv), duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    T = dp2perp * duv1.x + dp1perp * duv2.x;
    B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    T *= invmax;
    B *= invmax;
}

// Reoriented normal mapping (RNM) blend of a base and a sub tangent-space normal (Ray RNMBlendUnpacked).
float3 PbrBlendRnm(float3 n1, float3 n2) {
    n1.z += 1.0;
    n2.xy = -n2.xy;
    return normalize(n1 * dot(n1, n2) / max(n1.z, 1e-5) - n2);
}

// World-space mapped normal: NORMAL + NORMAL_SUB (RNM) through the derivative tangent frame. Returns baseN when
// the material has no normal map. NORMAL_MAP_UV_FLIP 4 flips the shading normal X.
float3 PbrWorldNormal(GpuPbrMaterial m, float3 baseN, float3 P, float2 uv) {
    if (m.slots[PBR_SLOT_NORMAL].textureSrv < 0) return baseN;
    float3 nt = PbrDecodeNormal(m.slots[PBR_SLOT_NORMAL], uv);
    if (m.slots[PBR_SLOT_NORMAL_SUB].textureSrv >= 0)
        nt = PbrBlendRnm(nt, PbrDecodeNormal(m.slots[PBR_SLOT_NORMAL_SUB], uv));
    float3 N = PbrTangentToWorld(baseN, P, uv, nt);
    if (m.slots[PBR_SLOT_NORMAL].uvFlip == 4u) N.x = -N.x;
    return normalize(N);
}

// ---- GGX lobe (docs/raymmd_semantics.md §4) -------------------------------------------------
// GGX + the doc's Smith pair + Schlick Fresnel; the geometry already carries N.L / N.V.
float3 PbrSpecularGGX(float3 N, float3 V, float3 L, float rough, float3 f0) {
    float3 H = normalize(L + V);
    float nh = saturate(dot(N, H));
    float nl = saturate(dot(N, L));
    float nv = abs(dot(N, V)) + 1e-5;
    float vh = saturate(dot(V, H));
    float m2 = rough * rough;
    float d = m2 / pow((nh * m2 - nh) * nh + 1.0, 2.0);
    float gv = nl * (nv * (1.0 - rough) + rough);
    float gl = nv * (nl * (1.0 - rough) + rough);
    d *= 0.5 / max(gv + gl, 1e-5);
    if (nl <= 0.0) d = 0.0;   // no light from behind the surface
    float f90 = saturate(dot(f0, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)) * 50.0);
    float3 f = lerp(f0, f90, pow(1.0 - vh, 5.0));
    return f * d;
}

// Anisotropic GGX (docs/raymmd_semantics.md §2 ANISOTROPY): an elliptical lobe along `T`. `aniso` = customA
// strength, `shift` = customB.r tangent shift.
float3 PbrSpecularGGXAniso(float3 N, float3 V, float3 L, float rough, float3 f0, float aniso,
                           float3 T, float shift) {
    float3 X = normalize(T + N * shift);
    float3 Y = cross(N, X);
    float3 H = normalize(L + V);
    float aspect = rsqrt(max(1.0 - aniso * 0.9, 1e-4));
    float ax = max(rough / max(aspect, 1e-4), 1e-3);
    float ay = max(rough * aspect, 1e-3);
    float tx = dot(X, H) / ax;
    float ty = dot(Y, H) / ay;
    float hn = dot(N, H);
    float d = 1.0 / (3.14159265 * ax * ay * pow(tx * tx + ty * ty + hn * hn, 2.0));
    float nv = abs(dot(N, V)) + 1e-5;
    float nl = saturate(dot(N, L));
    float vh = saturate(dot(V, H));
    float f90 = saturate(dot(f0, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)) * 50.0);
    float3 f = lerp(f0, f90, pow(1.0 - vh, 5.0));
    return f * (d * 0.25 / max(nv, 1e-5));
}

// Sheen lobe (docs/raymmd_semantics.md §2 CLOTH): `(2 + 1/rough) * sin^2(nh)^(0.5/rough) / 2 / G`.
float3 PbrSpecularSheen(float3 N, float3 V, float3 L, float rough, float3 fuzz) {
    float3 H = normalize(L + V);
    float nh = saturate(dot(N, H));
    float nl = saturate(dot(N, L));
    float nv = abs(dot(N, V)) + 1e-5;
    float s2 = (1.0 - nh) * (1.0 + nh);
    float g = max(4.0 * (nl + nv - nl * nv), 1e-4);
    float s = (2.0 + 1.0 / max(rough, 1e-3)) * pow(max(s2, 1e-6), 0.5 / max(rough, 1e-3)) / (2.0 * g);
    float vh = saturate(dot(V, H));
    return fuzz * (s * pow(1.0 - vh, 5.0));
}

#endif // MMDX_PBR_COMMON_HLSLI
