// world_api.hlsli: HLSL contract and helper API for MMDX12 World Packs (type: world).
// See docs/shader_world_api.md.
#ifndef WORLD_API_HLSLI
#define WORLD_API_HLSLI

// Particle representation (world space, 64 bytes)
struct PackParticle {
    float3 pos;
    float size;
    float3 vel;
    float age;
    float4 color;
    float4 data;
};

// Simulation step input for stateful particles
struct PackSimInput {
    uint id;
    float dt;
    float time;
    float3 posLocal;
};

// 2D field update input (per texel per field step, ping-pong)
struct PackFieldInput {
    float2 uv;
    float2 posLocal;   // xz in world units centred on origin
    float4 prev;
    float dt;
    float time;
    uint2 texel;
};

// Surface height evaluation input
struct PackSurfaceIn {
    float2 uv;
    float2 posLocal;   // xz in world units
    float time;
};

// Surface shading output
struct PackSurfaceOut {
    float3 albedo;
    float alpha;
    float roughness;
    float3 normal;     // default from height gradient
    float3 emission;
};

// Surface shading input
struct PackSurfaceShadeIn {
    float2 uv;
    float3 posLocal;
    float3 posWorld;
    float3 normal;     // gradient normal in world space
    float3 viewDir;
    float time;
};

// Particle shading input
struct PackParticleShadeIn {
    float2 uv;
    PackParticle particle;
    float3 viewDir;
    float3 posWorld;
};

// World constant buffer
cbuffer WorldCB : register(b1) {
    float4x4 gWorldToLocal;
    float4x4 gLocalToWorld;
    float4 gWorldTime;      // x: time, y: dt, z: simTime, w: stepIndex
    float4 gWorldParams;    // x: colliderCount, y: fieldCount, z: particleSystemIndex, w: particleCount
    float4 gSunDir;         // xyz: sunDir, w: hasSun
    float4 gSunColor;       // rgb: sunColor, w: shadowDistance
    float4 gAmbient;        // rgb: ambientColor, w: 0
    float4 gFieldExtent[4]; // xy: size, z: extent, w: 1/extent
};

#define gTime (gWorldTime.x)

// Extended parameter buffer (128 slots float4)
cbuffer PackParamsExt : register(b4) {
    float4 gPackParamSlots[128];
};

#define PackParam(i) (gPackParamSlots[(i) & 127u].x)
inline int PackParamInt(uint i) { return (int)round(gPackParamSlots[i & 127u].x); }
inline bool PackParamBool(uint i) { return (gPackParamSlots[i & 127u].x > 0.5); }
inline float3 PackParamColor(uint i) { return gPackParamSlots[i & 127u].rgb; }
inline float4 PackParamColor4(uint i) { return gPackParamSlots[i & 127u]; }
inline float3 PackParamVec3(uint i) { return gPackParamSlots[i & 127u].xyz; }
inline uint PackParamCount() { return 128u; }

// Colliders
struct PackColliderData {
    float3 pos;
    float radius;
    float3 vel;
    uint flags;
};

StructuredBuffer<PackColliderData> gColliders : register(t10, space0);
inline uint PackColliderCount() { return (uint)gWorldParams.x; }
inline PackColliderData PackCollider(uint i) { return gColliders[i]; }

// Transforms
inline float3 PackWorldToLocal(float3 posWorld) {
    return mul(float4(posWorld, 1.0), gWorldToLocal).xyz;
}

inline float3 PackLocalToWorld(float3 posLocal) {
    return mul(float4(posLocal, 1.0), gLocalToWorld).xyz;
}

inline float3 PackWorldToLocalVec(float3 vecWorld) {
    return mul(float4(vecWorld, 0.0), gWorldToLocal).xyz;
}

inline float3 PackLocalToWorldVec(float3 vecLocal) {
    return mul(float4(vecLocal, 0.0), gLocalToWorld).xyz;
}

// Lighting helpers
inline float3 PackSunDir() { return gSunDir.xyz; }
inline float3 PackSunColor() { return gSunColor.rgb; }
inline float3 PackAmbient() { return gAmbient.rgb; }

// Forward declaration of PackSunShadow, implemented by host
float PackSunShadow(float3 worldPos);

// Samplers
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);
SamplerState gWrapLinear : register(s2);

// Pack textures (up to 32)
#ifndef PACK_TEX_COUNT
#define PACK_TEX_COUNT 0
#endif
#ifndef PACK_TEX_CLAMP_MASK
#define PACK_TEX_CLAMP_MASK 0u
#endif
#ifndef PACK_TEX_SRGB_MASK
#define PACK_TEX_SRGB_MASK 0u
#endif

Texture2D gPackTex[32] : register(t0, space5);

inline float4 PackSampleTex(uint i, float2 uv) {
    if (i >= 32u || i >= (uint)PACK_TEX_COUNT) return float4(1, 1, 1, 1);
    i &= 31u;
    if ((PACK_TEX_CLAMP_MASK >> i) & 1u) return gPackTex[i].SampleLevel(gLinear, uv, 0);
    return gPackTex[i].SampleLevel(gWrapLinear, uv, 0);
}

inline float4 PackSampleTexLevel(uint i, float2 uv, float lod) {
    if (i >= 32u || i >= (uint)PACK_TEX_COUNT) return float4(1, 1, 1, 1);
    i &= 31u;
    if ((PACK_TEX_CLAMP_MASK >> i) & 1u) return gPackTex[i].SampleLevel(gLinear, uv, lod);
    return gPackTex[i].SampleLevel(gWrapLinear, uv, lod);
}

inline uint2 PackTexSize(uint i) {
    if (i >= 32u || i >= (uint)PACK_TEX_COUNT) return uint2(0, 0);
    uint2 s;
    gPackTex[i & 31u].GetDimensions(s.x, s.y);
    return s;
}

inline uint PackTexCount() { return (uint)PACK_TEX_COUNT; }

// Fields (up to 4 2D textures)
Texture2D<float4> gPackFields[4] : register(t0, space4);

inline float4 PackFieldSample(uint fieldIndex, float2 posLocalXZ) {
    if (fieldIndex >= 4u) return 0.0;
    float extent = gFieldExtent[fieldIndex].z;
    if (extent <= 0.0) return 0.0;
    float2 uv = (posLocalXZ / extent) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;
    return gPackFields[fieldIndex].SampleLevel(gLinear, uv, 0);
}

inline float4 PackFieldLoad(uint fieldIndex, int2 texel) {
    if (fieldIndex >= 4u) return 0.0;
    return gPackFields[fieldIndex].Load(int3(texel, 0));
}

// Deterministic hashing / random functions (PCG)
inline uint PackHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

inline float PackRandom(uint seed) {
    return float(PackHash(seed)) * (1.0 / 4294967296.0);
}

inline float PackRandom(uint seed, uint salt) {
    return float(PackHash(seed ^ PackHash(salt))) * (1.0 / 4294967296.0);
}

inline float PackRandom(uint seed, uint salt1, uint salt2) {
    return float(PackHash(seed ^ PackHash(salt1 ^ PackHash(salt2)))) * (1.0 / 4294967296.0);
}

inline float2 PackRandom2(uint seed, uint salt) {
    return float2(PackRandom(seed, salt), PackRandom(seed ^ 0x9e3779b9u, salt));
}

inline float3 PackRandom3(uint seed, uint salt) {
    return float3(PackRandom(seed, salt), PackRandom(seed ^ 0x9e3779b9u, salt), PackRandom(seed ^ 0x517cc1b7u, salt));
}

#endif // WORLD_API_HLSLI
