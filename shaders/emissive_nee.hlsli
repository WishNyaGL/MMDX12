// S8a: emissive-triangle next-event estimation (LED screens / AutoLuminous / PBR emissive) for the path tracers.
//
// The host (Renderer::BuildEmissiveTable) collects every triangle of every RtScene geometry whose material emits and
// uploads three raw buffers through the bindless buffer table (SceneConstants::gEmissive):
//   gEmissive.x  RawBuffer of GpuEmissiveTri (uint2: geometry id, triangle index inside that geometry), -1 = none
//   gEmissive.y  RawBuffer of float: the pick CDF (normalised inclusive running sum of area x emission estimate)
//   gEmissive.z  entry count
//   gEmissive.w  RawBuffer of uint: per geometry id the geometry's first entry, or 0xFFFFFFFF (does not emit)
// The triangle list is read through the same skinned vertex / index buffers the hit path reads, so a character's
// emitter triangles move with the pose its BLAS moves with.
//
// Requires rt_common.hlsli and pbr_common.hlsli (ScreenEmission / AutoLumEmission / PbrEmissive), i.e. include it after
// pt_pbr.hlsli.
#ifndef MMDX_EMISSIVE_NEE_HLSLI
#define MMDX_EMISSIVE_NEE_HLSLI
#include "common.hlsli"

struct EmissiveTri {
    uint geometry;
    uint prim;   // the triangle's index inside that geometry
};

// True when the frame has an emissive pick table (a frame without emitters leaves every path below untouched).
bool EmissiveOn() { return gEmissive.x >= 0.0 && gEmissive.z >= 1.0; }

// The geometry's first entry in the pick list, or 0xFFFFFFFF when it does not emit.
uint EmissiveBase(uint geometryId) {
    if (!EmissiveOn()) return 0xFFFFFFFFu;
    ByteAddressBuffer base = gBindlessBuf[NonUniformResourceIndex((uint)gEmissive.w)];
    return base.Load(geometryId * 4u);
}

EmissiveTri EmissiveTriAt(uint entry) {
    ByteAddressBuffer tris = gBindlessBuf[NonUniformResourceIndex((uint)gEmissive.x)];
    uint2 v = tris.Load2(entry * 8u);
    EmissiveTri t;
    t.geometry = v.x;
    t.prim = v.y;
    return t;
}

// The CDF share of entry `i` (its discrete probability).
float EmissiveCdfAt(uint i) {
    ByteAddressBuffer cdf = gBindlessBuf[NonUniformResourceIndex((uint)gEmissive.y)];
    return asfloat(cdf.Load(i * 4u));
}

// Picks an entry by `u` (binary search on the CDF). Returns its index (0 when the table is unusable) and its discrete
// probability `pdfTri` (0 = nothing to sample).
uint EmissivePick(float u, out float pdfTri, out EmissiveTri tri) {
    pdfTri = 0.0;
    tri = (EmissiveTri)0;
    if (!EmissiveOn()) return 0u;
    const uint count = (uint)gEmissive.z;
    uint lo = 0u, hi = count - 1u;
    [loop] while (lo < hi) {
        uint mid = (lo + hi) >> 1u;
        if (EmissiveCdfAt(mid) < u) lo = mid + 1u;
        else hi = mid;
    }
    const float below = lo == 0u ? 0.0 : EmissiveCdfAt(lo - 1u);
    pdfTri = max(EmissiveCdfAt(lo) - below, 0.0);
    tri = EmissiveTriAt(lo);
    return lo;
}

// The emission an emissive geometry adds at a point: the same terms the hit path adds (ScreenEmission replaces the
// material's emission, AutoLuminous adds, a PBR record carries its own). One code path for both sides of MIS.
//   g        the geometry
//   texLin   the base texture at the point's uv, gamma-decoded and saturated; the CALLER samples it so both sides read
//            the same mip (a screen's emission is exactly this value)
//   pbrEmis  the PBR record's emissive at the uv (0 when the geometry has no record / the hit is not PBR-shaded)
//   pbrHit   true when the hit path shaded the geometry through its PBR record
float3 EmissionAt(RtGeometry g, float3 texLin, float3 pbrEmis, bool pbrHit) {
    if (pbrHit) return pbrEmis;
    if ((g.flags & MAT_SCREEN) != 0) return ScreenEmission(g.flags, texLin, g.reflectivity);
    return AutoLumEmission(g.flags, g.diffuse, g.specularPower, texLin);
}

// World positions and UVs of an emissive entry's triangle (the current skinned vertices, like LoadTriPosUv).
void EmissiveTrianglePosUv(EmissiveTri t, out float3 p0, out float3 p1, out float3 p2,
                           out float2 uv0, out float2 uv1, out float2 uv2) {
    RtGeometry g = gGeometries[t.geometry];
    LoadTriPosUv(g, t.prim, p0, p1, p2, uv0, uv1, uv2);
}

// Solid-angle pdf of the point `lightPos` on a triangle seen from `fromPos`: pdfTri / area * dist^2 / |cos| of the
// light's normal against the direction back to the shading point (at that very point, not the centroid: a large LED
// panel next to the floor varies a lot over its area). 0 for a degenerate / edge-on triangle.
float EmissiveSolidAnglePdf(float pdfTri, float3 p0, float3 p1, float3 p2, float3 lightPos, float3 fromPos) {
    float3 e1 = p1 - p0, e2 = p2 - p0;
    float3 c = cross(e1, e2);
    const float len = length(c);
    if (len <= 1e-16) return 0.0;
    const float area = 0.5 * len;
    float3 nLight = c / len;
    float3 toLight = lightPos - fromPos;
    const float dist2 = max(dot(toLight, toLight), 1e-8);
    const float cosLight = abs(dot(nLight, normalize(toLight)));
    if (cosLight <= 1e-6) return 0.0;
    return pdfTri / area * dist2 / cosLight;
}

// ---- MIS (power heuristic) ------------------------------------------------------------------
// Weight of a light-sampling estimate against the BSDF-sampling one: pL^2 / (pL^2 + pB^2).
float MisWeightLight(float pdfLight, float pdfBsdf) {
    const float a = pdfLight * pdfLight;
    const float b = pdfBsdf * pdfBsdf;
    return a + b > 0.0 ? a / (a + b) : 1.0;
}
// Weight of an emitter radiance found by a BSDF-sampled direction (depth >= 1, non-delta bounce).
float MisWeightBsdf(float pdfBsdf, float pdfLight) {
    const float a = pdfBsdf * pdfBsdf;
    const float b = pdfLight * pdfLight;
    return a + b > 0.0 ? a / (a + b) : 1.0;
}

// The light-sampling pdf (solid angle) of a BSDF-sampled hit of geometry `geometryId` / triangle `prim`, from that
// triangle's own CDF entry, at the hit point `hitPos`. 0 when the geometry does not emit (then there is no MIS: the hit
// keeps weight 1). The caller skips back-face hits: the NEE never samples a back face.
float EmissiveHitPdf(uint geometryId, uint prim, float3 hitPos, float3 fromPos) {
    const uint base = EmissiveBase(geometryId);
    if (base == 0xFFFFFFFFu) return 0.0;
    const uint entry = base + prim;
    if (entry >= (uint)gEmissive.z) return 0.0;
    const float below = entry == 0u ? 0.0 : EmissiveCdfAt(entry - 1u);
    const float pdfTri = max(EmissiveCdfAt(entry) - below, 0.0);
    if (pdfTri <= 0.0) return 0.0;
    EmissiveTri t = EmissiveTriAt(entry);
    if (t.geometry != geometryId || t.prim != prim) return 0.0;   // the entry holds another triangle
    float3 p0, p1, p2;
    float2 uv0, uv1, uv2;
    EmissiveTrianglePosUv(t, p0, p1, p2, uv0, uv1, uv2);
    return EmissiveSolidAnglePdf(pdfTri, p0, p1, p2, hitPos, fromPos);
}

// Visibility of a sampled emitter point the way a path ray would find it: TraceClosest over every geometry (shadow
// casting or not; glass / transmissive surfaces block, because the path crosses them as delta events whose emitter hits
// keep weight 1) with the path's stochastic alpha. A shadow ray (TraceShadowRayTinted) would let the light through
// geometry the BSDF side is stopped by, and the two halves of the MIS would disagree (measured: +9 % on arena_live's
// floor). Stops 5 cm short of the emitter so its own triangle does not occlude it.
float EmissiveVisible(float3 origin, float3 wi, float dist, float alphaThreshold) {
    RtHit h;
    return TraceClosest(origin, wi, 0.0, max(dist - 0.05, 0.0), alphaThreshold, h) ? 0.0 : 1.0;
}

// ---- the NEE sample --------------------------------------------------------------------------
struct EmissiveSample {
    EmissiveTri tri;
    float3 pos;      // the sampled point on the triangle
    float3 nLight;   // the triangle's geometric normal (winding as the hit path reads it)
    float2 uv;
    float dist;
    float pdf;       // solid angle
};

// One uniform-area sample of the pick table (sqrt barycentrics). False when the frame has no emitters or the sample is
// degenerate. The caller traces the shadow ray and applies the one-sided rule (only the winding side emits).
bool EmissiveSamplePoint(float u, float2 u2, float3 fromPos, out EmissiveSample s) {
    s = (EmissiveSample)0;
    float pdfTri = 0.0;
    EmissiveTri t;
    EmissivePick(u, pdfTri, t);
    if (pdfTri <= 0.0) return false;
    s.tri = t;
    float3 p0, p1, p2;
    float2 uv0, uv1, uv2;
    EmissiveTrianglePosUv(t, p0, p1, p2, uv0, uv1, uv2);
    const float su = sqrt(u2.x);
    const float b0 = 1.0 - su;
    const float b1 = su * (1.0 - u2.y);
    const float b2 = su * u2.y;
    s.pos = p0 * b0 + p1 * b1 + p2 * b2;
    s.uv = uv0 * b0 + uv1 * b1 + uv2 * b2;
    float3 c = cross(p1 - p0, p2 - p0);
    const float len = length(c);
    if (len <= 1e-12) return false;
    s.nLight = c / len;
    s.dist = length(s.pos - fromPos);
    s.pdf = EmissiveSolidAnglePdf(pdfTri, p0, p1, p2, s.pos, fromPos);
    return s.pdf > 0.0 && s.dist > 1e-4;
}

// The emission at the sampled point: the material's base texture at `uv` (the caller's path lod), through EmissionAt.
float3 EmissiveSampleRadiance(EmissiveTri t, float2 uv, float lod) {
    RtGeometry g = gGeometries[t.geometry];
    const float4 tex = SampleBaseTexture(g, uv, lod);
    // Coverage: a ray finds a non-opaque geometry (alpha test, no shadow cast, transmissive; RtScene's OPAQUE flag rule)
    // only with probability = its alpha (the stochastic alpha of TraceClosest), so a light sample on a cut-out texel
    // (glow stick sprites, sign letters) carries that share; opaque geometry ignores alpha in the hit path too.
    const bool opaque = (g.flags & RTG_CAST_SHADOW) != 0 && (g.flags & (RTG_ALPHA_TEST | RTG_TRANSMISSIVE)) == 0;
    float coverage = 1.0;
    if (!opaque) {
        coverage = (g.flags & MAT_HAS_TEXTURE) ? g.diffuse.a * tex.a : g.diffuse.a;
        if ((g.flags & MAT_HAS_TEXTURE) && g.alphaKey > 0.0 && tex.r < g.alphaKey) coverage = 0.0;
        coverage = saturate(coverage);
    }
    if (coverage <= 0.0) return 0.0;
    // A PBR record's emissive is its own emission term (the hit path adds pbr.emissive); a screen replaces everything.
    if ((g.pbrIndex != 0xFFFFFFFFu) && (g.flags & MAT_SCREEN) == 0) {
        GpuPbrMaterial m = PbrLoadMaterial((int)g.pbrSrv, g.pbrIndex);
        if ((m.flags & PBR_FLAG_EMISSIVE) != 0u)
            return coverage * EmissionAt(g, float3(0, 0, 0), PbrEmissive(m, uv), true);
    }
    float3 texLin = SrgbToLinear(saturate(tex.rgb));
    return coverage * EmissionAt(g, texLin, 0.0, false);
}

// ---- sampling pdfs for MIS -------------------------------------------------------------------
// Uniform cone lobe of the non-PBR specular continuation (SampleCone around reflect(dir, n), half-angle rough * 0.5):
// its solid-angle pdf is 1 / omega.
float ConeLobeOmega(float rough) {
    const float cosHalfAngle = cos(rough * 0.5);
    return max(2.0 * PI * (1.0 - cosHalfAngle), 1e-6);
}

// GGX VNDF pdf of a reflection direction (PtPbrSampleSpecular's distribution): D (n.h) * G1 (n.v) / (4 n.v).
float PbrGgxPdf(PtPbrSurface p, float3 V, float3 L) {
    float3 n = p.N;
    if (dot(n, V) < 0.0) n = -n;
    const float NoV = saturate(dot(n, V));
    const float NoL = saturate(dot(n, L));
    if (NoV <= 0.0 || NoL <= 0.0) return 0.0;
    float3 H = normalize(V + L);
    const float NoH = saturate(dot(n, H));
    const float alpha = max(p.rough, 1e-3);
    const float a2 = alpha * alpha;
    const float d = a2 / (PI * pow(NoH * NoH * (a2 - 1.0) + 1.0, 2.0));
    return d * PtGgxG1(NoV, alpha) / (4.0 * NoV);
}

// BRDF x cos of the record's specular continuation as PtPbrSampleSpecular realises it (isotropic GGX, VNDF weight
// F G2 / G1 over the pdf D G1 / (4 n.v)): F D G2 / (4 n.v). The light-sampling half of the emissive MIS uses this, not
// PtPbrEvalSpecular (the punctual lobe), so both halves estimate the same integral.
float3 PbrSpecCosine(PtPbrSurface p, float3 V, float3 L) {
    float3 n = p.N;
    if (dot(n, V) < 0.0) n = -n;
    const float NoV = saturate(dot(n, V));
    const float NoL = saturate(dot(n, L));
    if (NoV <= 0.0 || NoL <= 0.0) return 0.0;
    float3 H = normalize(V + L);
    const float NoH = saturate(dot(n, H));
    const float alpha = max(p.rough, 1e-3);
    const float a2 = alpha * alpha;
    const float d = a2 / (PI * pow(NoH * NoH * (a2 - 1.0) + 1.0, 2.0));
    const float3 F = PtPbrFresnel(p.f0, saturate(dot(V, H)));
    return F * d * PtGgxG2(NoV, NoL, alpha) / (4.0 * NoV);
}

// The pdf of the path's continuation sampler at a vertex it shaded: the specular lobe with probability pSpec (GGX VNDF
// for a PBR record, the uniform cone of the non-PBR lobe) and the cosine hemisphere otherwise. Both sides of the
// emissive MIS weigh against this. `wo` is the direction the path arrived from (-dir), `wi` the direction sampled.
float PathBsdfPdf(bool pbr, PtPbrSurface pb, float rough, float pSpec, float3 n, float3 wo, float3 wi) {
    const float cosTheta = saturate(dot(n, wi));
    const float pdfDiffuse = cosTheta / PI;
    float pdfSpec;
    if (pbr) {
        pdfSpec = PbrGgxPdf(pb, wo, wi);
    } else {
        const float cosHalfAngle = cos(rough * 0.5);
        pdfSpec = dot(reflect(-wo, n), wi) >= cosHalfAngle ? 1.0 / ConeLobeOmega(rough) : 0.0;
    }
    return saturate(1.0 - pSpec) * pdfDiffuse + pSpec * pdfSpec;
}

// The specular lobe alone (pSpec x its pdf): offline GI's camera vertex takes its diffuse indirect from the irradiance
// cache and traces only the specular lobe, so that is all the BSDF side can find there.
float PathSpecPdf(bool pbr, PtPbrSurface pb, float rough, float pSpec, float3 n, float3 wo, float3 wi) {
    return PathBsdfPdf(pbr, pb, rough, pSpec, n, wo, wi) - saturate(1.0 - pSpec) * saturate(dot(n, wi)) / PI;
}

#endif
