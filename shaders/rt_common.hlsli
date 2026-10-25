// Inline ray tracing helpers (RayQuery, shader model 6.5, compiled with DXC).
// Mirror of the RtVertex / RtGeometry layouts in src/render/ShaderInterop.h.
//
// Bindings (ComputePipeline in RenderPass.h, and the ScenePass root signature):
//   t0 space1  RaytracingAccelerationStructure (TLAS)
//   t1 space1  StructuredBuffer<RtGeometry>, entry = InstanceID + GeometryIndex
//   t0 space2  Texture2D[]         unbounded, index = SrvHeap index
//   t0 space3  ByteAddressBuffer[] unbounded, index = SrvHeap index
//   s4         linear wrap sampler
// Include common.hlsli first is not required; it is included here.
#ifndef MMDX_RT_COMMON_HLSLI
#define MMDX_RT_COMMON_HLSLI
#include "common.hlsli"

#ifndef MAT_HAS_TEXTURE
#define MAT_HAS_TEXTURE 1u
#define MAT_HAS_TOON    2u
#define MAT_SPHERE_MUL  4u
#define MAT_SPHERE_ADD  8u
#define MAT_RECEIVE     16u
#define MAT_STAGE       32u
#define MAT_TOON_MAP    64u
#define MAT_FLAT        128u
#endif
#ifndef MAT_HAS_SPHERE_TEX
#define MAT_HAS_SPHERE_TEX 256u   // a real sphere-slot texture is bound (not the white fallback)
#endif
#ifndef MAT_AUTO_LUMINOUS
#define MAT_AUTO_LUMINOUS 512u    // そぼろ AutoLuminous: this material emits (see AutoLumEmission in common.hlsli)
#endif
#ifndef MAT_ADDITIVE
#define MAT_ADDITIVE      1024u   // the model's own .emd effect is additive (ScenePass uses an additive PSO)
#endif
#ifndef MAT_NO_DEPTH_WRITE
#define MAT_NO_DEPTH_WRITE 2048u  // the model's own .emd effect draws with ZWriteEnable = false (no depth write)
#endif
#define RTG_CAST_SHADOW (1u << 16)
#define RTG_ALPHA_TEST  (1u << 17)
#define RTG_CHARACTER   (1u << 18)
#define RTG_PT_PACK     (1u << 19)
#define RTG_TRANSMISSIVE (1u << 20)   // S2b mesh dielectric (RtGeom_Transmissive): refracts / tints shadows

struct RtPtPackRecord {
    uint materialClass;
    uint headValid;
    uint texBase;
    uint texInfo;
    float4 headRight;
    float4 headUp;
    float4 headForward;
    float4 params[4];
    float4 headPos;   // xyz world head bone position, w = headScale
    uint extSlotBase;   // params.ext slots in gPackExtSlots (pt_pack_glue.hlsli)
    uint extSlotCount;
};

struct RtGeometry {
    uint vertexSrv;
    uint indexSrv;
    uint indexStart;
    uint flags;
    float4 diffuse;
    float3 specular; float specularPower;
    float3 ambient;  float reflectivity;
    uint textureSrv;
    uint sphereSrv;
    uint toonSrv;
    uint packSrv;
    uint addUvSrv;   // raw ByteAddressBuffer of the add-UV stream; 0xFFFFFFFF = none
    float alphaKey;  // pack alpha key (0 = off): cut pixels whose texture red < alphaKey at alpha evaluation
    uint pbrSrv;     // SrvHeap index of the model's GpuPbrMaterial StructuredBuffer (0xFFFFFFFF = none)
    uint pbrIndex;   // material index into that buffer (0xFFFFFFFF = no PBR record)
    float4 texMul, texAdd, sphereMul, sphereAdd, toonMul, toonAdd;   // material morph factors
};

// World packs (world.hlsl, world.lights) use a separate set of registers: space2 already holds the particle buffers
// there, so the TLAS / geometry / bindless bindings live at t20 / t21 space0 and t0 space7 / space8. The helpers
// below are identical.
#if defined(RT_COMMON_ALT_SPACES)
RaytracingAccelerationStructure gTlas : register(t20, space0);
StructuredBuffer<RtGeometry> gGeometries : register(t21, space0);
Texture2D gBindlessTex[] : register(t0, space7);
ByteAddressBuffer gBindlessBuf[] : register(t0, space8);
#else
RaytracingAccelerationStructure gTlas : register(t0, space1);
StructuredBuffer<RtGeometry> gGeometries : register(t1, space1);
Texture2D gBindlessTex[] : register(t0, space2);
ByteAddressBuffer gBindlessBuf[] : register(t0, space3);
#endif
SamplerState gRtWrap : register(s4);

static const uint kRtVertexStride = 48;

struct RtSurface {
    float3 pos;        // world position
    float3 prevPos;    // previous frame world position
    float3 normal;     // interpolated shading normal (not flipped toward the ray)
    float3 faceNormal; // geometric normal from the triangle winding (not flipped)
    float2 uv;
    float4 uv1;        // additional UV1 (zero without them)
    float4 uv2;        // additional UV2 (zero without them)
    uint3 tri;         // vertex indices of the hit triangle (for LoadAddUv callers)
};

RtGeometry LoadGeometry(uint instanceId, uint geometryIndex) {
    return gGeometries[instanceId + geometryIndex];
}

uint3 LoadTriangle(RtGeometry g, uint prim) {
    ByteAddressBuffer ib = gBindlessBuf[NonUniformResourceIndex(g.indexSrv)];
    return ib.Load3((g.indexStart + prim * 3) * 4);
}

float2 LoadUv(RtGeometry g, uint3 tri, float2 bary) {
    ByteAddressBuffer vb = gBindlessBuf[NonUniformResourceIndex(g.vertexSrv)];
    float2 uv0 = asfloat(vb.Load2(tri.x * kRtVertexStride + 24));
    float2 uv1 = asfloat(vb.Load2(tri.y * kRtVertexStride + 24));
    float2 uv2 = asfloat(vb.Load2(tri.z * kRtVertexStride + 24));
    return uv0 * (1.0 - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;
}

// Per-vertex positions and UVs of a hit triangle (for the triangle-UV tangent frame of a mapped normal).
void LoadTriPosUv(RtGeometry g, uint prim, out float3 p0, out float3 p1, out float3 p2,
                  out float2 uv0, out float2 uv1, out float2 uv2) {
    ByteAddressBuffer vb = gBindlessBuf[NonUniformResourceIndex(g.vertexSrv)];
    uint3 tri = LoadTriangle(g, prim);
    uint3 a = tri * kRtVertexStride;
    p0 = asfloat(vb.Load3(a.x)); p1 = asfloat(vb.Load3(a.y)); p2 = asfloat(vb.Load3(a.z));
    uv0 = asfloat(vb.Load2(a.x + 24));
    uv1 = asfloat(vb.Load2(a.y + 24));
    uv2 = asfloat(vb.Load2(a.z + 24));
}

static const uint kAddUvStride = 32;   // GpuAddUv (uv1 at 0, uv2 at 16)

// Additional UV1 / UV2 at a hit (same triangle / barycentrics as FetchSurface). Zeros when the
// model has no additional-UV stream (g.addUvSrv == 0xFFFFFFFF).
void LoadAddUv(RtGeometry g, uint3 tri, float2 bary, out float4 uv1, out float4 uv2) {
    uv1 = 0;
    uv2 = 0;
    if (g.addUvSrv == 0xFFFFFFFFu) return;
    ByteAddressBuffer adduv = gBindlessBuf[NonUniformResourceIndex(g.addUvSrv)];
    float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    uv1 = asfloat(adduv.Load4(tri.x * kAddUvStride + 0)) * w.x +
          asfloat(adduv.Load4(tri.y * kAddUvStride + 0)) * w.y +
          asfloat(adduv.Load4(tri.z * kAddUvStride + 0)) * w.z;
    uv2 = asfloat(adduv.Load4(tri.x * kAddUvStride + 16)) * w.x +
          asfloat(adduv.Load4(tri.y * kAddUvStride + 16)) * w.y +
          asfloat(adduv.Load4(tri.z * kAddUvStride + 16)) * w.z;
}

RtSurface FetchSurface(RtGeometry g, uint prim, float2 bary) {
    ByteAddressBuffer vb = gBindlessBuf[NonUniformResourceIndex(g.vertexSrv)];
    uint3 tri = LoadTriangle(g, prim);
    float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    uint3 a = tri * kRtVertexStride;
    float3 p0 = asfloat(vb.Load3(a.x)), p1 = asfloat(vb.Load3(a.y)), p2 = asfloat(vb.Load3(a.z));
    float3 n0 = asfloat(vb.Load3(a.x + 12)), n1 = asfloat(vb.Load3(a.y + 12)), n2 = asfloat(vb.Load3(a.z + 12));
    float2 t0 = asfloat(vb.Load2(a.x + 24)), t1 = asfloat(vb.Load2(a.y + 24)), t2 = asfloat(vb.Load2(a.z + 24));
    float3 q0 = asfloat(vb.Load3(a.x + 32)), q1 = asfloat(vb.Load3(a.y + 32)), q2 = asfloat(vb.Load3(a.z + 32));
    RtSurface s;
    s.pos = p0 * w.x + p1 * w.y + p2 * w.z;
    s.prevPos = q0 * w.x + q1 * w.y + q2 * w.z;
    float3 n = n0 * w.x + n1 * w.y + n2 * w.z;
    float3 fn = cross(p1 - p0, p2 - p0);
    // MMD winding is clockwise front (D3D default): the face normal of a clockwise triangle in
    // a left-handed system is cross(p1 - p0, p2 - p0).
    s.faceNormal = dot(fn, fn) > 1e-20 ? normalize(fn) : float3(0, 1, 0);
    s.normal = dot(n, n) > 1e-12 ? normalize(n) : s.faceNormal;
    s.uv = t0 * w.x + t1 * w.y + t2 * w.z;
    s.tri = tri;
    LoadAddUv(g, tri, bary, s.uv1, s.uv2);
    return s;
}

float4 SampleBaseTexture(RtGeometry g, float2 uv, float lod) {
    if ((g.flags & MAT_HAS_TEXTURE) == 0) return float4(1, 1, 1, 1);
    return ApplyTexFactor(gBindlessTex[NonUniformResourceIndex(g.textureSrv)].SampleLevel(gRtWrap, uv, lod),
                          g.texMul, g.texAdd);
}

// Coverage of a candidate (diffuse alpha * texture alpha), mip 0. A pack alpha key cuts the pixel out
// (returns 0) when the texture red is below the key, the same colour key the raster PSPack applies.
float CandidateAlpha(RtGeometry g, uint prim, float2 bary) {
    if ((g.flags & MAT_HAS_TEXTURE) == 0) return g.diffuse.a;
    float2 uv = LoadUv(g, LoadTriangle(g, prim), bary);
    float4 tex = SampleBaseTexture(g, uv, 0);
    if (g.alphaKey > 0.0 && tex.r < g.alphaKey) return 0.0;
    return g.diffuse.a * tex.a;
}

// Albedo in gamma space, the same model as the raster pass (mmd.hlsl PSMain).
float3 MaterialAlbedo(RtGeometry g, float3 tex) {
    return (g.flags & MAT_STAGE) ? g.diffuse.rgb * tex : saturate(g.ambient + g.diffuse.rgb) * 0.8 * tex;
}

// MMD "fully lit" colour in gamma space: saturate(ambient + diffuse * light) * texture.
float3 MaterialLit(RtGeometry g, float3 tex) {
    return saturate(g.ambient + g.diffuse.rgb * gLightColor) * tex;
}

#define RT_MASK_STAGE     0x01u
#define RT_MASK_CHARACTER 0x02u

// 1 = unoccluded. Honours RTG_CAST_SHADOW and alpha (>= 0.5) on non-opaque geometry.
// `mask` selects instances (RT_MASK_*).
float TraceShadowRayMasked(float3 origin, float3 dir, float tMax, uint mask) {
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, RAY_FLAG_NONE, mask, r);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            RtGeometry g = LoadGeometry(q.CandidateInstanceID(), q.CandidateGeometryIndex());
            if ((g.flags & RTG_CAST_SHADOW) != 0 &&
                CandidateAlpha(g, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()) >= 0.5)
                q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}

float TraceShadowRay(float3 origin, float3 dir, float tMax) {
    return TraceShadowRayMasked(origin, dir, tMax, 0xFFu);
}

// S2b: TraceShadowRayTinted (tinted visibility through transmissive mesh dielectrics) lives in pbr_common.hlsli, next
// to the PBR record decoders it needs; this header is included before them, so it cannot host the function itself.

struct RtHit {
    uint instanceId, geometryIndex, prim;
    float2 bary;
    float t;
    bool frontFace;
};

// Decal priority. A PMX often layers an alpha-blended decal (tattoo, mask, makeup, emblem) exactly
// on top of an opaque material: the same triangles / positions, drawn later in material order. The
// closest query commits the opaque triangle, and the decal's candidate arrives at the same (or a
// numerically equal) t so it is never reported and the decal disappears. This short second query
// restricted to non-opaque geometry (RAY_FLAG_CULL_OPAQUE, same back-face culling as the caller)
// looks for an accepted candidate within a small window around the committed hit t and returns the
// one that raster would show: the higher geometry index within the same instance, which is material
// order in the RtScene geometry table (see src/render/RayTracing.cpp EnsureModelResources).
// `hit` carries the opaque committed hit and is overwritten only when a decal is accepted. The
// window is relative to t (a fixed world-space epsilon is wrong across the ~+-500 unit scene span)
// with a small floor for very close hits.
// Cost: one extra inline ray query per primary/mirror hit whose committed geometry is opaque; the
// query is empty (and cheap) when the instance has no non-opaque geometry.
bool CommitDecalNear(float3 origin, float3 dir, float tMin, float tHit, uint opaqueInstance,
                     uint opaqueGeom, float alphaThreshold, inout RtHit hit) {
    const float kWindowRel = 2e-4;   // window half-width relative to t
    const float kWindowMin = 5e-4;   // floor so an almost-zero t still has a usable window
    const float eps = max(tHit * kWindowRel, kWindowMin);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_BACK_FACING_TRIANGLES |
             RAY_FLAG_CULL_OPAQUE> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = max(tMin, tHit - eps);
    r.TMax = tHit + eps;
    q.TraceRayInline(gTlas, RAY_FLAG_NONE, 0xFF, r);
    uint bestGeom = opaqueGeom;
    bool found = false;
    while (q.Proceed()) {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        // Only the same instance (same model / BLAS part) can decal the opaque hit, and raster
        // draws materials in ascending order: a candidate at a lower geometry index sits under
        // the opaque surface, so it must not win.
        if (q.CandidateInstanceID() != opaqueInstance || q.CandidateGeometryIndex() <= bestGeom) continue;
        RtGeometry g = LoadGeometry(q.CandidateInstanceID(), q.CandidateGeometryIndex());
        // S2b: a transmissive mesh is non-opaque in the BLAS but it is not a decal: it must not win over the opaque
        // surface it happens to touch (a glass sheet flush against a wall stays a separate surface).
        if ((g.flags & RTG_TRANSMISSIVE) != 0) continue;
        if (CandidateAlpha(g, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()) < alphaThreshold)
            continue;
        // Among coplanar candidates the later-drawn one wins; keep the highest geometry index.
        found = true;
        bestGeom = q.CandidateGeometryIndex();
        hit.instanceId = q.CandidateInstanceID();
        hit.geometryIndex = q.CandidateGeometryIndex();
        hit.prim = q.CandidatePrimitiveIndex();
        hit.bary = q.CandidateTriangleBarycentrics();
        hit.t = tHit;
        hit.frontFace = q.CandidateTriangleFrontFace();
    }
    return found;
}

// Closest hit. Non-opaque candidates are accepted when their alpha >= alphaThreshold
// (0.5 for a binary alpha test, a random number in (0,1] for stochastic transparency).
// Back faces of single-sided materials are culled like the raster pass does (stages are often
// closed rooms whose ceiling or walls the camera looks through from outside, and some layer a
// back-facing copy under a visible surface). Double-sided materials live in instances with
// TRIANGLE_CULL_DISABLE (RtScene), so the ray flag only affects single-sided ones.
bool TraceClosest(float3 origin, float3 dir, float tMin, float tMax, float alphaThreshold, out RtHit hit) {
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = tMin;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            RtGeometry g = LoadGeometry(q.CandidateInstanceID(), q.CandidateGeometryIndex());
            if (CandidateAlpha(g, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()) >= alphaThreshold)
                q.CommitNonOpaqueTriangleHit();
        }
    }
    hit.instanceId = 0;
    hit.geometryIndex = 0;
    hit.prim = 0;
    hit.bary = float2(0, 0);
    hit.t = tMax;
    hit.frontFace = true;
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    hit.instanceId = q.CommittedInstanceID();
    hit.geometryIndex = q.CommittedGeometryIndex();
    hit.prim = q.CommittedPrimitiveIndex();
    hit.bary = q.CommittedTriangleBarycentrics();
    hit.t = q.CommittedRayT();
    hit.frontFace = q.CommittedTriangleFrontFace();
    // The committed hit is opaque (RTG_ALPHA_TEST unset => opaque in the BLAS): a coplanar decal at
    // the same t was not reported by the loop above, so let it win here. Decals are alpha-tested at a
    // fixed 0.5, not the caller's stochastic threshold: a decal that flickers per sample hands the
    // denoiser a different albedo than the radiance it demodulates (speckles + a bright fringe).
    if ((LoadGeometry(hit.instanceId, hit.geometryIndex).flags & RTG_ALPHA_TEST) == 0)
        CommitDecalNear(origin, dir, tMin, hit.t, hit.instanceId, hit.geometryIndex, 0.5, hit);
    return true;
}

// Distance to any hit (alpha >= 0.5 on non-opaque geometry), or -1 when nothing is hit.
float TraceAnyHitDistance(float3 origin, float3 dir, float tMax) {
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            RtGeometry g = LoadGeometry(q.CandidateInstanceID(), q.CandidateGeometryIndex());
            if (CandidateAlpha(g, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()) >= 0.5)
                q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
}

// ---- sampling -----------------------------------------------------------------------

uint PcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint RngSeed(uint2 pixel, uint frame, uint salt) {
    return PcgHash(pixel.x + PcgHash(pixel.y + PcgHash(frame * 16u + salt)));
}
float Rand(inout uint state) {
    state = PcgHash(state);
    return (state >> 8) * (1.0 / 16777216.0);
}
// A random value from the CURRENT state, without advancing it: for a roulette that must not consume a draw on the
// paths that skip it (a material property decides whether the draw happens at all).
float RandValue(uint state) {
    return (PcgHash(state) >> 8) * (1.0 / 16777216.0);
}

void BuildBasis(float3 n, out float3 t, out float3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

float3 CosineSampleHemisphere(float2 u, float3 n) {
    float r = sqrt(u.x);
    float phi = 6.2831853 * u.y;
    float3 t, b;
    BuildBasis(n, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u.x)));
}

// Uniform direction inside a cone of half-angle acos(cosMax) around `axis`.
float3 SampleCone(float2 u, float3 axis, float cosMax) {
    float cosT = 1.0 - u.x * (1.0 - cosMax);
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi = 6.2831853 * u.y;
    float3 t, b;
    BuildBasis(axis, t, b);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + axis * cosT);
}

// Offset along the geometric normal to avoid self-intersection, scaled with distance from the
// origin (MMD scenes span roughly +-500 units).
float3 OffsetRayOrigin(float3 p, float3 geoNormal) {
    float scale = 0.002 + 1.5e-5 * max(max(abs(p.x), abs(p.y)), abs(p.z));
    return p + geoNormal * scale * 8.0;
}

#endif
