// shaders/world.hlsl: Host shader for MMDX12 World Packs.
// Compiled per world pack with MMDX_PACK pointing to the pack's world.hlsl.
#include "common.hlsli"
#include "world_api.hlsli"

Texture2DArray<float> gWorldShadowMap : register(t15, space0);
SamplerComparisonState gWorldShadowCmp : register(s3);

// Sun visibility from the scene's cascaded shadow map (raster camera views; 1 where no cascade covers the point,
// e.g. ray-traced / path-traced frames, which do not render the map).
float PackSunShadow(float3 worldPos) {
    if (gSunDir.w <= 0.5 || gCascadeSplits.w < 0.5) return 1.0;
    float viewZ = mul(float4(worldPos, 1.0), gView).z;
    if (viewZ > gCascadeSplits.z) return 1.0;
    int c = viewZ < gCascadeSplits.x ? 0 : (viewZ < gCascadeSplits.y ? 1 : 2);
    float4 sp = mul(float4(worldPos - gLightDir * gCascadeTexel[c] * 1.5, 1.0), gShadowViewProj[c]);
    float3 p = sp.xyz / sp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0) return 1.0;
    float texel = gShadowParams.x;
    float sum = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sum += gWorldShadowMap.SampleCmpLevelZero(gWorldShadowCmp, float3(uv + float2(x, y) * texel, c), p.z - 0.0004);
    float fade = saturate((gCascadeSplits.z - viewZ) / (gCascadeSplits.z * 0.15));
    return lerp(1.0, sum / 9.0, fade);
}

#if defined(WORLD_CLEAR)
RWStructuredBuffer<PackParticle> gParticles : register(u0);
RWStructuredBuffer<float4> gPrevPos : register(u1);

[numthreads(64, 1, 1)]
void CSWorldClear(uint3 id : SV_DispatchThreadID) {
    uint pId = id.x;
    if (pId >= (uint)gWorldParams.w) return;
    PackParticle p = (PackParticle)0;
    p.age = -1.0;
    gParticles[pId] = p;
    gPrevPos[pId] = float4(0, 0, 0, 0);
}
#endif

#include MMDX_PACK

#if defined(WORLD_SIMULATE)
RWStructuredBuffer<PackParticle> gParticles : register(u0);
RWStructuredBuffer<float4> gPrevPos : register(u1);

[numthreads(64, 1, 1)]
void CSWorldSimulate(uint3 id : SV_DispatchThreadID) {
    uint pId = id.x;
    if (pId >= (uint)gWorldParams.w) return;
    PackParticle p = gParticles[pId];
    gPrevPos[pId] = float4(p.pos, p.size);
    PackSimInput s;
    s.id = pId;
    s.dt = gWorldTime.y;
    s.time = gWorldTime.x;
    s.posLocal = PackWorldToLocal(p.pos);
    WORLD_SIMULATE_FN(p, s);
    gParticles[pId] = p;
}
#endif

#if defined(WORLD_SPAWN)
RWStructuredBuffer<PackParticle> gParticles : register(u0);
RWStructuredBuffer<float4> gPrevPos : register(u1);

[numthreads(64, 1, 1)]
void CSWorldSpawn(uint3 id : SV_DispatchThreadID) {
    uint pId = id.x;
    if (pId >= (uint)gWorldParams.w) return;
    PackParticle p = WORLD_SPAWN_FN(pId, gWorldTime.x);
    gPrevPos[pId] = float4(p.pos - p.vel * gWorldTime.y, p.size);
    gParticles[pId] = p;
}
#endif

#if defined(WORLD_FIELD_UPDATE)
Texture2D<float4> gFieldPrev : register(t0, space1);
RWTexture2D<float4> gFieldNext : register(u0);

[numthreads(8, 8, 1)]
void CSWorldFieldUpdate(uint3 id : SV_DispatchThreadID) {
    uint2 texel = id.xy;
    // worldParams.z = field index (+16 on the first step of a re-simulation: the field starts from zero)
    uint fieldIndex = (uint)gWorldParams.z & 15u;
    bool reset = (uint)gWorldParams.z >= 16u;
    uint size = (uint)gFieldExtent[fieldIndex].x;
    if (texel.x >= size || texel.y >= size) return;
    float2 uv = (float2(texel) + 0.5) / float(size);
    float extent = gFieldExtent[fieldIndex].z;
    float2 posLocal = (uv - 0.5) * extent;
    PackFieldInput f;
    f.uv = uv;
    f.posLocal = posLocal;
    f.prev = reset ? float4(0, 0, 0, 0) : gFieldPrev.Load(int3(texel, 0));
    f.dt = gWorldTime.y;
    f.time = gWorldTime.x;
    f.texel = texel;
    gFieldNext[texel] = WORLD_FIELD_UPDATE_FN(f);
}
#endif

#if defined(WORLD_SURFACE)
struct SurfaceVsOut {
    float4 posH : SV_Position;
    float3 posWorld : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 curClip : TEXCOORD3;
    float4 prevClip : TEXCOORD4;
};

SurfaceVsOut VSWorldSurface(uint vertexId : SV_VertexID) {
    SurfaceVsOut o = (SurfaceVsOut)0;
    uint grid = (uint)gWorldParams.w; // grid quads per side
    if (grid == 0) return o;
    uint quadId = vertexId / 6;
    uint corner = vertexId % 6;
    uint2 cornerOffsets[6] = {
        uint2(0, 0), uint2(1, 0), uint2(0, 1),
        uint2(0, 1), uint2(1, 0), uint2(1, 1)
    };
    uint2 c = cornerOffsets[corner];
    uint qx = quadId % grid;
    uint qz = quadId / grid;
    float2 gridVertex = float2(qx + c.x, qz + c.y);
    float2 uv = gridVertex / float(grid);
    float extent = gFieldExtent[(uint)gWorldParams.z & 3u].z;   // worldParams.z = the surface's field
    float2 posLocalXZ = (uv - 0.5) * extent;

    PackSurfaceIn sin;
    sin.uv = uv;
    sin.posLocal = posLocalXZ;
    sin.time = gWorldTime.x;
    float localY = WORLD_SURFACE_HEIGHT_FN(sin);

    // Gradient normal in local space
    float eps = (grid > 0) ? (extent / float(grid) * 0.5) : 0.01;
    PackSurfaceIn sR = sin; sR.uv.x += eps / extent; sR.posLocal.x += eps;
    PackSurfaceIn sU = sin; sU.uv.y += eps / extent; sU.posLocal.y += eps;
    float hR = WORLD_SURFACE_HEIGHT_FN(sR);
    float hU = WORLD_SURFACE_HEIGHT_FN(sU);
    float3 dX = float3(eps, hR - localY, 0.0);
    float3 dZ = float3(0.0, hU - localY, eps);
    float3 normLocal = normalize(cross(dZ, dX));
    float3 normWorld = normalize(PackLocalToWorldVec(normLocal));

    float3 posLocal = float3(posLocalXZ.x, localY, posLocalXZ.y);
    float3 posWorld = PackLocalToWorld(posLocal);

    o.posWorld = posWorld;
    o.normal = normWorld;
    o.uv = uv;
    o.posH = mul(float4(posWorld, 1.0), gViewProj);
    o.curClip = mul(float4(posWorld, 1.0), gViewProjNoJitter);
    o.prevClip = mul(float4(posWorld, 1.0), gPrevViewProjNoJitter);
    return o;
}

struct SurfacePsOut {
    float4 color : SV_Target0;
    float4 normal : SV_Target1;
    float4 velocity : SV_Target2;
};

SurfacePsOut PSWorldSurface(SurfaceVsOut i) {
    SurfacePsOut o;
    PackSurfaceShadeIn sin;
    sin.uv = i.uv;
    sin.posLocal = PackWorldToLocal(i.posWorld);
    sin.posWorld = i.posWorld;
    sin.normal = normalize(i.normal);
    sin.viewDir = normalize(gEyePos.xyz - i.posWorld);
    sin.time = gWorldTime.x;

    PackSurfaceOut sout = WORLD_SURFACE_SHADE_FN(sin);

    float3 L = PackSunDir();
    float NdotL = saturate(dot(sout.normal, L));
    float shadow = PackSunShadow(i.posWorld);
    float3 sunColor = PackSunColor();
    // Ambient: the scene's sky / ground hemisphere (the same terms the lit scene shader uses), so the surface
    // matches the stage in every render path; the composite adds nothing on top (nt.z = -1 below).
    float3 hemi = lerp(gGroundColor, gSkyZenith, saturate(sout.normal.y * 0.5 + 0.5)) * gHemiStrength;
    float3 ambient = max(PackAmbient(), hemi);
    float3 lit = sout.albedo * (sunColor * (NdotL * shadow) + ambient) + sout.emission;

    o.color = float4(lit, sout.alpha);
    float3 viewN = normalize(mul(sout.normal, (float3x3)gView));
    // normal target: view normal, reflectivity -1 = no SSAO / SSR (the host already lit it), coverage 1
    o.normal = float4(OctEncode(viewN), -1.0, 1.0);

    float2 curNdc = i.curClip.xy / max(i.curClip.w, 1e-5);
    float2 prevNdc = i.prevClip.xy / max(i.prevClip.w, 1e-5);
    float2 vel = (curNdc - prevNdc) * float2(0.5, -0.5);
    o.velocity = float4(vel, 0.0, 0.0);
    return o;
}
#endif

#if defined(WORLD_PARTICLE)
// Post-composite particle draw: the resolved depth buffer (R32_FLOAT raw device depth) is sampled to fade particles
// that approach / pass behind scene surfaces (soft particles).
Texture2D<float> gSceneDepth : register(t0, space6);

#if defined(WORLD_STATELESS)
// stateless uses WORLD_SPAWN_FN directly
#else
StructuredBuffer<PackParticle> gParticles : register(t0, space2);
StructuredBuffer<float4> gPrevPos : register(t1, space2);
#endif

struct ParticleVsOut {
    float4 posH : SV_Position;
    float2 uv : TEXCOORD0;
    float3 viewDir : TEXCOORD1;
    float3 posWorld : TEXCOORD2;
    float4 color : TEXCOORD3;
    float4 data : TEXCOORD4;
    float size : TEXCOORD5;
    float age : TEXCOORD6;
    float4 curClip : TEXCOORD7;
    float4 prevClip : TEXCOORD8;
};

ParticleVsOut VSWorldParticle(uint vertexId : SV_VertexID) {
    ParticleVsOut o = (ParticleVsOut)0;
    uint pId = vertexId / 6;
    uint corner = vertexId % 6;
    if (pId >= (uint)gWorldParams.w) {
        o.posH = float4(0, 0, 0, 0);
        return o;
    }

#if defined(WORLD_STATELESS)
    PackParticle p = WORLD_SPAWN_FN(pId, gWorldTime.x);
    float3 prevPos = p.pos - p.vel * gWorldTime.y;
#else
    PackParticle p = gParticles[pId];
    float3 prevPos = gPrevPos[pId].xyz;
#endif

    if (p.age < 0.0 || p.color.a <= 0.0 || p.size <= 0.0) {
        o.posH = float4(0, 0, 0, 0);
        return o;
    }

    float2 cornerOffsets[6] = {
        float2(-0.5, -0.5), float2(0.5, -0.5), float2(-0.5, 0.5),
        float2(-0.5, 0.5),  float2(0.5, -0.5), float2(0.5, 0.5)
    };
    float2 offset = cornerOffsets[corner];
    float3 camRight = gInvView[0].xyz;
    float3 camUp = gInvView[1].xyz;
    float3 posWorld = p.pos + (camRight * offset.x + camUp * offset.y) * p.size;

    o.uv = offset + 0.5;
    o.posWorld = posWorld;
    o.viewDir = normalize(gEyePos.xyz - posWorld);
    o.color = p.color;
    o.data = p.data;
    o.size = p.size;
    o.age = p.age;
    o.posH = mul(float4(posWorld, 1.0), gViewProj);
    o.curClip = mul(float4(posWorld, 1.0), gViewProjNoJitter);

    float3 prevWorldPos = prevPos + (camRight * offset.x + camUp * offset.y) * p.size;
    o.prevClip = mul(float4(prevWorldPos, 1.0), gPrevViewProjNoJitter);
    return o;
}

struct ParticlePsOut {
    float4 color : SV_Target0;
    float4 normal : SV_Target1;
    float4 velocity : SV_Target2;
};

ParticlePsOut PSWorldParticle(ParticleVsOut i) {
    if (i.posH.w <= 0.0) discard;
    ParticlePsOut o;
    PackParticle p;
    p.pos = i.posWorld;
    p.size = i.size;
    p.vel = 0.0;
    p.age = i.age;
    p.color = i.color;
    p.data = i.data;

    PackParticleShadeIn sin;
    sin.uv = i.uv;
    sin.particle = p;
    sin.viewDir = i.viewDir;
    sin.posWorld = i.posWorld;

    float4 col = WORLD_PARTICLE_SHADE_FN(sin);
    if (col.a <= 0.001) discard;

    o.color = col;
    // nt.z = -1 tells SSAO / SSR not to affect this pixel
    o.normal = float4(0, 0, -1, 0);

    float2 curNdc = i.curClip.xy / max(i.curClip.w, 1e-5);
    float2 prevNdc = i.prevClip.xy / max(i.prevClip.w, 1e-5);
    float2 vel = (curNdc - prevNdc) * float2(0.5, -0.5);
    o.velocity = float4(vel, 0.0, 0.0);
    return o;
}

// Post-composite variant: one render target (lit), no velocity, soft depth fade against the resolved scene depth.
// Depth is raw device depth from RenderTargets::depth (R32_FLOAT).
struct ParticlePostPsOut {
    float4 color : SV_Target0;
};

// Fade distance in view-space units (MMD units): a few hundredths, enough to hide a flake's ground intersection.
static const float kParticleDepthFade = 0.25;

ParticlePostPsOut PSWorldParticlePost(ParticleVsOut i) {
    ParticlePostPsOut o;
    o.color = 0.0;
    if (i.posH.w <= 0.0) discard;

    // Soft depth test: i.posH.w is the particle's view depth in this LH perspective projection.
    int2 pixel = int2(i.posH.xy);
    float sceneDepth = gSceneDepth.Load(int3(pixel, 0));
    float sceneZ = LinearZ(sceneDepth);
    float particleZ = i.posH.w;
    float fade = saturate((sceneZ - particleZ) / kParticleDepthFade);
    if (fade <= 0.0) discard;

    PackParticle p;
    p.pos = i.posWorld;
    p.size = i.size;
    p.vel = 0.0;
    p.age = i.age;
    p.color = i.color;
    p.data = i.data;

    PackParticleShadeIn sin;
    sin.uv = i.uv;
    sin.particle = p;
    sin.viewDir = i.viewDir;
    sin.posWorld = i.posWorld;

    float4 col = WORLD_PARTICLE_SHADE_FN(sin);
    if (col.a <= 0.001) discard;

#if defined(WORLD_PARTICLE_ADD)
    // Additive: the destination RGB is what fades; alpha rides along for consistency.
    col.rgb *= fade;
    col.a *= fade;
#else
    col.a *= fade;
#endif
    o.color = col;
    return o;
}
#endif
