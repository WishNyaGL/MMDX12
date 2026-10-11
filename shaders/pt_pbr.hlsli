#ifndef MMDX_PT_PBR_HLSLI
#define MMDX_PT_PBR_HLSLI
// Per-material PBR support for the ray paths (work unit P4): decode an RtGeometry's PBR record into a
// ready-to-shade surface, the GGX visible-normal (VNDF) specular sampler of Heitz 2018, and the
// specular/diffuse lobe choice. Included by pathtrace.hlsl and offline_gi.hlsl after rt_common.hlsli
// (which declares the bindless tables pbr_common.hlsli reuses). The analytic direct terms stay in each
// shader (their signatures differ); the decode / sampling / lobe policy live here.
//
// Rule (raster P3): a hit shaded by an active shader pack ignores its PBR record. Callers clear the
// decoded surface when a PT pack class != OFF wins.
//
// Semantics: docs/raymmd_semantics.md §1/§2/§4. The look mirrors mmd.hlsl MmdShadePbr ("MMD look on top").
#include "pbr_common.hlsli"

struct PtPbrSurface {
    float3 N;         // mapped world normal (NORMAL + NORMAL_SUB, triangle-UV tangent frame)
    float3 diffuse;   // linear albedo * (1 - metal)
    float3 f0;        // linear F0 (lerp(specular, albedo, metal) for dielectrics/metals)
    float3 emissive;  // linear emissive (0 unless PBR_FLAG_EMISSIVE)
    float3 baseColor; // lerp(diffuse, f0, metal): the denoiser albedo (metals must not demodulate to ~0)
    float3 T;         // world tangent (ANISOTROPY lobe)
    float3 customB;   // CUSTOM_B (sheen fuzz / shift tangent / shadow colour)
    float rough;
    float ao;
    float metal;
    float customA;    // CUSTOM_A (anisotropic strength / sheen amount / curvature)
    float specScale;  // SKIN attenuates its sun specular (0.15)
    uint model;
    bool celLike;     // model 8/9: MMD shading + mapped normal + emissive only, no GGX
    bool specOn;      // GGX sun/punctual specular enabled (not CEL/TONEBASED)
    bool skipSpec;    // SKIN: no specular continuation
};

// Schlick Fresnel with the controller's f90 = saturate(lum(f0) * 50) (docs/raymmd_semantics.md §4).
float3 PtPbrFresnel(float3 f0, float vh) {
    float f90 = saturate(dot(f0, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)) * 50.0);
    return lerp(f0, f90, pow(1.0 - vh, 5.0));
}

// World-space mapped normal from the hit triangle's UV frame (the RT counterpart of PbrWorldNormal).
float3 PtPbrWorldNormal(GpuPbrMaterial m, float3 baseN, float3 p0, float3 p1, float3 p2,
                        float2 uv0, float2 uv1, float2 uv2, float2 uv) {
    if (m.slots[PBR_SLOT_NORMAL].textureSrv < 0) return baseN;
    float3 nt = PbrDecodeNormal(m.slots[PBR_SLOT_NORMAL], uv);
    if (m.slots[PBR_SLOT_NORMAL_SUB].textureSrv >= 0)
        nt = PbrBlendRnm(nt, PbrDecodeNormal(m.slots[PBR_SLOT_NORMAL_SUB], uv));
    float3 N = PbrTangentToWorldTri(baseN, p0, p1, p2, uv0, uv1, uv2, nt);
    if (m.slots[PBR_SLOT_NORMAL].uvFlip == 4u) N.x = -N.x;
    return normalize(N);
}

// Decode a hit's record (docs/raymmd_semantics.md §1/§2). `baseN` is the interpolated shading normal;
// `pmxDiffuse` = PMX material diffuse (RtGeometry.diffuse.rgb, gamma) for ALBEDO_MAP_APPLY_DIFFUSE.
PtPbrSurface PtPbrDecode(uint prim, RtGeometry g, RtSurface sf, float3 baseN, float3 pmxDiffuse) {
    GpuPbrMaterial m = PbrLoadMaterial((int)g.pbrSrv, g.pbrIndex);
    PtPbrSurface p;
    p.model = m.shadingModel;
    p.celLike = (p.model == 8u || p.model == 9u);

    float3 p0, p1, p2;
    float2 uv0, uv1, uv2;
    LoadTriPosUv(g, prim, p0, p1, p2, uv0, uv1, uv2);
    p.N = PtPbrWorldNormal(m, baseN, p0, p1, p2, uv0, uv1, uv2, sf.uv);

    // Plain tangent direction for the anisotropic lobe (the triangle UV frame before the normal blend).
    float3 e1 = p1 - p0, e2 = p2 - p0;
    float2 d1 = uv1 - uv0, d2 = uv2 - uv0;
    float det = d1.x * d2.y - d1.y * d2.x;
    p.T = abs(det) < 1e-12 ? normalize(cross(p.N, float3(0, 1, 0)) + float3(1e-4, 0, 0))
                           : normalize((e1 * d2.y - e2 * d1.y) / det);

    float3 albedoLin = PbrAlbedo(m, sf.uv, pmxDiffuse);   // linear
    float metal = p.celLike ? 0.0 : saturate(PbrSlotMetalness(m.slots[PBR_SLOT_METALNESS], sf.uv));
    float3 f0mma = PbrSlotF0(m.slots[PBR_SLOT_SPECULAR], sf.uv);
    p.metal = metal;
    p.diffuse = albedoLin * (1.0 - metal);
    p.f0 = p.celLike ? f0mma : lerp(f0mma, max(0.02, albedoLin), metal);
    p.rough = max(PbrSlotRoughness(m.slots[PBR_SLOT_SMOOTHNESS], sf.uv), 0.03);
    p.ao = PbrSlotAo(m.slots[PBR_SLOT_OCCLUSION], sf.uv);
    p.emissive = saturate(PbrEmissive(m, sf.uv));
    p.customA = PbrCustomA(m, sf.uv);
    p.customB = PbrCustomB(m, sf.uv);
    p.baseColor = lerp(p.diffuse, p.f0, metal);
    p.specScale = 1.0;
    p.specOn = !p.celLike;   // CEL (8) / TONEBASED (9): MMD shading + mapped normal + emissive only
    p.skipSpec = false;
    if (p.model == 1u) {   // SKIN: keep the cel look, never glossy (roughness floor, dimmed specular)
        p.rough = max(p.rough, 0.6);
        p.specScale = 0.15;
        p.skipSpec = true;
    }
    return p;
}

// Per-model specular lobe for NEE (mirrors MmdShadePbr: ANISOTROPY anisotropic GGX, CLOTH GGX->sheen,
// otherwise isotropic GGX).
float3 PtPbrEvalSpecular(PtPbrSurface p, float3 V, float3 L) {
    if (p.model == 3u) return PbrSpecularGGXAniso(p.N, V, L, p.rough, p.f0, p.customA, p.T, p.customB.r);
    if (p.model == 5u) {
        float3 ggx = PbrSpecularGGX(p.N, V, L, p.rough, p.f0);
        float3 sheen = PbrSpecularSheen(p.N, V, L, p.rough, p.customB);
        return lerp(ggx, sheen, saturate(p.customA));
    }
    return PbrSpecularGGX(p.N, V, L, p.rough, p.f0);
}

// Specular continuation probability from Schlick F(N.V) luminance vs the diffuse weight (plan P4 §4).
float PtPbrSpecProb(PtPbrSurface p, float3 V) {
    if (!p.specOn || p.skipSpec) return 0.0;
    float NoV = saturate(dot(p.N, V));
    float fs = Luminance(PtPbrFresnel(p.f0, NoV));
    float fd = Luminance(p.diffuse);
    if (fs <= 1e-5) return 0.0;
    if (fd <= 1e-5) return 1.0;
    return clamp(fs / (fs + fd), 0.05, 0.95);
}

// Smith G1 / height-correlated G2 for the VNDF sampling weight. The NEE lobe (PbrSpecularGGX) uses an
// approximate Smith pair; the sampler uses the standard correlated Smith so direction and weight stay
// consistent (both use GGX D with alpha = rough).
float PtGgxG1(float NoX, float alpha) {
    float a2 = alpha * alpha;
    return 2.0 * NoX / max(NoX + sqrt(a2 + (1.0 - a2) * NoX * NoX), 1e-6);
}
float PtGgxG2(float NoV, float NoL, float alpha) {
    float a2 = alpha * alpha;
    float gv = NoV * sqrt(a2 + (1.0 - a2) * NoL * NoL);
    float gl = NoL * sqrt(a2 + (1.0 - a2) * NoV * NoV);
    return 2.0 * NoV * NoL / max(gv + gl, 1e-6);
}

// GGX visible-normal sampling (Heitz 2018) with alpha = rough. Returns the continuation direction and the
// throughput weight F * G2 / G1.
void PtPbrSampleSpecular(PtPbrSurface p, float3 V, float2 u, out float3 L, out float3 weight) {
    float3 n = p.N;
    if (dot(n, V) < 0.0) n = -n;   // shade the side the incoming ray sees
    float3 t, b;
    BuildBasis(n, t, b);
    // View in the local frame, stretched by alpha (isotropic: alpha_x = alpha_y = rough).
    float3 Ve = float3(dot(V, t), dot(V, b), max(dot(V, n), 1e-5));
    float alpha = max(p.rough, 1e-3);
    float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1, 0, 0);
    float3 T2 = cross(Vh, T1);
    float r = sqrt(u.x);
    float phi = 6.2831853 * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    float3 Ne = normalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z)));
    float3 H = normalize(t * Ne.x + b * Ne.y + n * Ne.z);
    L = reflect(-V, H);
    float NoV = saturate(dot(n, V));
    float NoL = saturate(dot(n, L));
    float3 F = PtPbrFresnel(p.f0, saturate(dot(V, H)));
    float G1 = PtGgxG1(NoV, alpha);
    float G2 = PtGgxG2(NoV, NoL, alpha);
    weight = F * (G2 / max(G1, 1e-5));
}

#endif // MMDX_PT_PBR_HLSLI
