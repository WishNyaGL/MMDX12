// S7c: clustered light cull (Forward+ with depth slices). One thread group per froxel: every light's bounding sphere is
// tested against the cluster's view-space AABB, and the survivors are written in ascending light order so the scene
// shaders accumulate their per-light sums in exactly the order of the old full loop (identical image).
//
// Grid: gP0 = (tilesX, tilesY, slices, light count), gP1 = (nearZ, farZ, log-slice scale, log-slice bias); the slices are
// exponential between nearZ and farZ, the same packing the scene shaders decode from
// SceneConstants::lightClusterParams (shaders/light_cluster.hlsli, src/render/ShaderInterop.h kCluster*).
//   gClusters: per cluster one header uint (count, or 0xFFFF = "too many: the shaders loop over every light") followed by
//              up to kClusterMaxLights light indices, in ascending order. Cluster c starts at c * (1 + kClusterMaxLights);
//              the cluster order is (slice * tilesY + ty) * tilesX + tx (tx = 0 left, ty = 0 top).
//   GroupID.x = tile x + tile y * tilesX, GroupID.y = slice.
// The light data is the frame's GpuLight buffer (t2 space1 = ComputePipeline root param 6 = pc.lights). No depth
// pre-pass and no TLAS: the cull is conservative, a cluster that cannot be proven empty keeps the light.
#include "shadow_common.hlsli"   // struct Light (the frame's GpuLight layout)
#include "rt_common.hlsli"       // b0 SceneConstants, b1 root constants
// the frame's GpuLight buffer (ComputePipeline root param 6 = t2 space1 = pc.lights)
StructuredBuffer<Light> gLights : register(t2, space1);
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
RWByteAddressBuffer gClusters : register(u0);

static const uint kClusterMaxLights = 64u;      // ShaderInterop.h kClusterMaxLights
static const uint kClusterOverflow = 0xFFFFu;   // a cluster with more lights: the shaders loop over all of them
static const uint kMaxLights = 256u;            // Renderer::kMaxPunctualLights (the bit mask is 256 bits)

groupshared uint gMask[8];   // one bit per light of this cluster (ascending order comes from walking the mask)

// The view-space point on the ray through the NDC point (ndc) at view depth z (the perspective divide cancels the
// scaling, so any depth in the inverse projection gives the same ray).
float3 ViewRay(float2 ndc, float z) {
    float4 v = mul(float4(ndc, 0.5, 1.0), gInvProj);
    float3 p = v.xyz / v.w;
    return abs(p.z) < 1e-6 ? float3(0.0, 0.0, z) : p * (z / p.z);
}

// The bounding sphere of light `i` in view space: centre and radius. The falloff is exactly 0 at distance >= range, so the
// range sphere is the bound; it is grown a hair (1 unit + 0.2%) because 1 / invRange is not bit-exact to range, and a
// cluster must never drop a light that a fragment still sees.
void LightSphere(uint i, out float3 centre, out float radius) {
    Light l = gLights[i];
    centre = mul(float4(l.pos, 1.0), gView).xyz;
    radius = 1.0 / max(l.invRange, 1e-4) * 1.002 + 1.0;
}

// Conservative cone rejection for a spot: the box is entirely outside the cone when its largest direction cosine (the
// box centre's cosine plus the box's angular radius seen from the apex) is below the cone's outer cosine.
bool SpotMissesBox(Light l, float3 lo, float3 hi) {
    if (l.cosOuter <= -1.0) return false;   // not a spot
    const float3 apex = mul(float4(l.pos, 1.0), gView).xyz;
    const float3 axis = mul(l.dir, (float3x3)gView);
    const float3 c = 0.5 * (lo + hi);
    const float3 d = c - apex;
    const float dist = length(d);
    const float boxRadius = 0.5 * length(hi - lo);
    if (dist <= boxRadius) return false;    // the apex is inside / next to the box
    const float cmax = dot(d, axis) / dist + boxRadius / dist;   // upper bound of the direction cosine
    return cmax < clamp(l.cosOuter, -1.0, 1.0);                  // outside the cone: the cone term is 0 everywhere
}

// Sphere vs view-space AABB.
bool SphereHitsBox(float3 c, float r, float3 lo, float3 hi) {
    const float3 d = max(max(lo - c, c - hi), 0.0);
    return dot(d, d) <= r * r;
}

[numthreads(64, 1, 1)]
void CSLightCull(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
    if (tid < 8u) gMask[tid] = 0u;
    // the clear must land before any thread ORs into the mask: a 64-thread group runs as two waves on 32-wide GPUs,
    // and the second wave (lights 32..63) could set its bits before the first wave cleared them (lights dropped at
    // random per frame = flickering lights)
    GroupMemoryBarrierWithGroupSync();

    const uint tilesX = max((uint)gP0.x, 1u);
    const uint tilesY = max((uint)gP0.y, 1u);
    const uint slices = max((uint)gP0.z, 1u);
    const uint lightCount = min((uint)gP0.w, kMaxLights);
    const uint tx = gid.x % tilesX;
    const uint ty = gid.x / tilesX;
    const uint slice = min(gid.y, slices - 1u);

    // This cluster's view-space AABB: the tile's corner rays at the slice's near / far depth. The far end is clamped to
    // the camera's far plane; a small margin on each side absorbs the TAA / upscaler projection jitter, so a fragment
    // classified by its own world position never falls outside the cluster the shader looked up.
    const float logScale = max(gP1.z, 1e-6);
    const float z0 = exp(((float)slice - gP1.w) / logScale);
    const float z1 = min(exp(((float)slice + 1.0 - gP1.w) / logScale), gP1.y);
    const float u0 = (float)tx / (float)tilesX, u1 = (float)(tx + 1u) / (float)tilesX;
    const float v0 = (float)ty / (float)tilesY, v1 = (float)(ty + 1u) / (float)tilesY;
    const float2 n00 = float2(u0 * 2.0 - 1.0, 1.0 - v0 * 2.0);
    const float2 n10 = float2(u1 * 2.0 - 1.0, 1.0 - v0 * 2.0);
    const float2 n01 = float2(u0 * 2.0 - 1.0, 1.0 - v1 * 2.0);
    const float2 n11 = float2(u1 * 2.0 - 1.0, 1.0 - v1 * 2.0);
    float3 lo = ViewRay(n00, z0), hi = lo;
    // the other three corners at both ends of the slice (n00's far end below)
    const float3 a10 = ViewRay(n10, z0), b10 = ViewRay(n10, z1);
    const float3 a01 = ViewRay(n01, z0), b01 = ViewRay(n01, z1);
    const float3 a11 = ViewRay(n11, z0), b11 = ViewRay(n11, z1);
    lo = min(lo, min(min(a10, b10), min(min(a01, b01), min(a11, b11))));
    hi = max(hi, max(max(a10, b10), max(max(a01, b01), max(a11, b11))));
    const float3 n00Far = ViewRay(n00, z1);   // the first corner's far end (only its near end was taken above)
    lo = min(lo, n00Far);
    hi = max(hi, n00Far);
    // a couple of pixels of margin (the projection jitter the fragments were rasterised with): the cluster's width at
    // `z1` is 2 * z1 * tan(fovX / 2), tan = 1 / proj[0][0], divided by the tile's pixel count.
    const float tanX = 1.0 / max(abs(gProj[0][0]), 1e-4);
    const float tanY = 1.0 / max(abs(gProj[1][1]), 1e-4);
    const float2 pad = float2(2.0 * z1 * tanX / max(gViewportSize.x, 1.0),
                              2.0 * z1 * tanY / max(gViewportSize.y, 1.0)) + 0.5;
    lo.xy -= pad;
    hi.xy += pad;
    // the slice boundary itself: the fragment's slice comes from the same log mapping in float, and a light at the
    // boundary must land in both neighbouring clusters
    const float padZ = 0.2 * (z1 - z0) + 0.5;
    lo.z -= padZ;
    hi.z += padZ;

    for (uint i = tid; i < lightCount; i += 64u) {
        float3 c;
        float r;
        LightSphere(i, c, r);
        if (SphereHitsBox(c, r, lo, hi) && !SpotMissesBox(gLights[i], lo, hi))
            InterlockedOr(gMask[i >> 5u], 1u << (i & 31u));
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid != 0u) return;
    // walk the mask in ascending light order; more than kClusterMaxLights hits overflow the cluster (the shaders then
    // loop over every light for that cluster: correct, slow, rare)
    const uint base = (slice * tilesY * tilesX + ty * tilesX + tx) * (1u + kClusterMaxLights);
    uint count = 0u;
    uint outIndex = base + 1u;
    for (uint w = 0u; w < 8u; ++w) {
        uint m = gMask[w];
        while (m != 0u) {
            const uint b = firstbitlow(m);
            m &= m - 1u;
            const uint light = w * 32u + b;
            if (light >= lightCount) continue;
            if (count < kClusterMaxLights) gClusters.Store(outIndex * 4u, light);
            ++count;
            ++outIndex;
        }
    }
    gClusters.Store(base * 4u, count > kClusterMaxLights ? kClusterOverflow : count);
}
