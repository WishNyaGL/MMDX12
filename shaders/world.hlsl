// shaders/world.hlsl: Host shader for MMDX12 World Packs.
// Compiled per world pack with MMDX_PACK pointing to the pack's world.hlsl.
#include "common.hlsli"
#include "shadow_common.hlsli"
#ifdef RT_SHADOWS
// The ray-query shadow path (world.lights in RT / PT / offline GI): the TLAS and its geometry table. The alternate
// register set avoids the particle buffers in space2 (rt_common.hlsli RT_COMMON_ALT_SPACES).
#define RT_COMMON_ALT_SPACES 1
#include "rt_common.hlsli"
#endif
#include "world_api.hlsli"

// The host's own light sample: the public PackLightSample exists only for packs that require world.lights.
struct WorldLightSample {
    float3 L;
    float3 radiance;
};

Texture2DArray<float> gWorldShadowMap : register(t15, space0);
SamplerComparisonState gWorldShadowCmp : register(s3);

// The scene's punctual lights (spots / points) for the world surfaces: the light buffer, the spot / point
// shadow maps and the binding flags. The flags are a root constant (b5): bit0 = the lights buffer is bound,
// bit1 = the spot shadow map is bound, bit2 = the point shadow map is bound.
cbuffer WorldLightCB : register(b5) {
    uint gWorldLightFlags;
};
// The scene's light layout (the Light struct of shadow_common.hlsli, must match the C++ upload, 112 bytes).
StructuredBuffer<Light> gWorldLights : register(t16, space0);
Texture2DArray<float> gWorldSpotShadowMap : register(t17, space0);
Texture2DArray<float> gWorldPointShadowMap : register(t18, space0);

static uint WorldPunctualCount() {
    if ((gWorldLightFlags & 1u) == 0u) return 0u;
    return min((uint)gNumLights, 16u);
}

// One scene punctual light at a world point (the general lighting API, docs/shader_world_api.md "world.lights"):
// the light's direction, colour x intensity x falloff x cone x visibility. Visibility comes from the shared spot /
// point shadow maps in raster frames, from a ray query against the scene TLAS in RT / PT / offline GI frames.
// `biasNormal` is the receiver normal the shadow-map taps are offset along (the public sample has no normal and
// uses up; the host surface lighting passes the shaded normal, so its frames stay exactly as before).
static WorldLightSample WorldPunctualLight(uint i, float3 wp, float3 biasNormal) {
    WorldLightSample s;
    s.L = float3(0.0, 1.0, 0.0);
    s.radiance = 0.0;
    if ((gWorldLightFlags & 1u) == 0u) return s;
    uint count = min((uint)gNumLights, 16u);
    if (i >= count) return s;
    Light l = gWorldLights[i];
    if (l.affectDiffuse <= 0.0) return s;
    float3 d = l.pos - wp;
    float dist = length(d);
    float3 ld = d / max(dist, 1e-4);
    float atten = PunctualFalloff(dist, l.invRange, l.falloff);
    if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
    else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
    if (atten <= 0.0) return s;
    float rawVis = 1.0;
#ifdef RT_SHADOWS
    // A short ray toward the light, with the scene's rules (mmd.hlsl SpotShadowRt / PointShadowRt): only lights that
    // cast shadows, occluded by characters only. The origin is biased a little toward the light.
    if (l.shadowType >= 0.5)
        rawVis = TraceShadowRayMasked(wp + ld * 0.02, ld, max(dist - 0.05, 0.0), RT_MASK_CHARACTER);
#else
    if (l.cosOuter > -1.0) {
        if (gWorldLightFlags & 2u) rawVis = SpotShadow(l, wp, biasNormal, dist, gWorldShadowCmp, gWorldSpotShadowMap);
    } else {
        if (gWorldLightFlags & 4u) rawVis = PointShadow(l, wp, biasNormal, dist, gWorldShadowCmp, gWorldPointShadowMap);
    }
#endif
    s.L = ld;
    s.radiance = l.color * ShadowTransmission(rawVis, l.shadowDensity, l.shadowColor) * atten;
    return s;
}

#if defined(PACK_CAP_WORLD_LIGHTS) && PACK_CAP_WORLD_LIGHTS
static PackLightSample PublishLight(WorldLightSample s) {
    PackLightSample o;
    o.L = s.L;
    o.radiance = s.radiance;
    return o;
}

PackLightSample PackPunctualLight(uint i, float3 wp) {
    return PublishLight(WorldPunctualLight(i, wp, float3(0.0, 1.0, 0.0)));
}

uint PackPunctualCount() {
    return WorldPunctualCount();
}
#endif

// The scene's punctual lights on a world surface (diffuse only): the scene's PunctualDiffuse, with the
// shared spot / point shadow-map visibility (hard or soft, like the scene).
float3 WorldPunctualDiffuse(float3 wp, float3 n) {
    float3 sum = 0;
    uint count = WorldPunctualCount();
    for (uint i = 0; i < count; ++i) {
        WorldLightSample s = WorldPunctualLight(i, wp, n);
        sum += s.radiance * saturate(dot(n, s.L));
    }
    return sum;
}

// Sun visibility: the scene's cascaded shadow map in raster camera views; a ray query against the scene TLAS in
// RT / PT / offline GI (where the cascade map is not rendered). 1 where no cascade covers the point.
float PackSunShadow(float3 worldPos) {
#ifdef RT_SHADOWS
    if (gSunDir.w <= 0.5) return 1.0;
    float3 L = gSunDir.xyz;
    return TraceShadowRay(worldPos + L * 0.02, L, 2000.0);   // the scene's sun ray length (mmd.hlsl)
#else
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
#endif
}

#if defined(PACK_CAP_WORLD_LIGHTS) && PACK_CAP_WORLD_LIGHTS
PackLightSample PackSunLight(float3 posWorld) {
    PackLightSample s;
    s.L = gSunDir.xyz;
    s.radiance = 0.0;
    if (gSunDir.w <= 0.5) return s;
    s.radiance = PackSunColor() * PackSunShadow(posWorld);
    return s;
}
#endif

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
    // The local xz include the field's centre (the anchor), so the pack compares them with the collider positions.
#if defined(PACK_CAP_WORLD_ANCHOR) && PACK_CAP_WORLD_ANCHOR
    float2 posLocal = (uv - 0.5) * extent + PackFieldCentre();
#else
    float2 posLocal = (uv - 0.5) * extent;
#endif
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
#if defined(PACK_CAP_WORLD_GROUND) && PACK_CAP_WORLD_GROUND
    float2 posWorldXZ : TEXCOORD5;
    float3 groundNormal : TEXCOORD6;
    float groundY : TEXCOORD7;   // -1e9: there is no ground under this vertex
#endif
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
    uint fieldIndex = (uint)gWorldParams.z & 3u;
    float extent = gFieldExtent[fieldIndex].z;   // worldParams.z = the surface's field

    float2 localXZ = float2(0.0, 0.0);
    float2 worldXZ = float2(0.0, 0.0);
    float2 uv = float2(0.0, 0.0);
    float cellWorld = 1.0;      // the world size of one cell
    float regionWorld = 1.0;    // the world size of the whole region
    bool degenerate = false;    // a cell a finer level covers: no fragments

#if defined(PACK_CAP_WORLD_GROUND) && PACK_CAP_WORLD_GROUND
    if (gSurfaceParams.x > 0.5) {
        // The camera-centred grid: 3 nested LOD levels, each grid x grid quads.
        //   level 0: the whole square of half-size R (cells of 2R/grid)
        //   level 1: the central quarter of it (cells 4x finer)
        //   level 2: the central sixteenth (cells 16x finer)
        // The cells are snapped to their level's spacing (the grid is anchored to the world, not to the camera, so it
        // does not swim when the camera moves) and a level skips the cells the next finer level covers, so no region is
        // drawn twice. The host rounds the grid to a multiple of 8, so the levels tile exactly.
        uint quads = grid * grid;
        uint level = min(quadId / quads, 2u);
        uint q = quadId - level * quads;
        uint lx = q % grid;
        uint lz = q / grid;

        float radius = max(gSurfaceParams.y, 1.0);
        float base = 2.0 * radius / float(grid);
        float lodScale = (level == 0u) ? 1.0 : (level == 1u ? 0.25 : 0.0625);
        float spacing = base * lodScale;

        int2 camCell = (int2)floor(gEyePos.xz / base);        // the camera's coarse cell
        int2 quads8 = int2((int)(grid / 8), (int)(grid / 8));
        int2 base1 = 4 * (camCell - quads8);                   // the middle level's first cell (fine units)
        int2 base2 = 4 * camCell - quads8;                     // the fine level's first cell, centred (fine units)
        int2 cell;
        if (level == 0u) {
            cell = camCell + int2((int)lx - (int)(grid / 2), (int)lz - (int)(grid / 2));
            int2 lo = camCell - quads8, hi = camCell + quads8;  // the cells the middle level covers
            if (cell.x >= lo.x && cell.x < hi.x && cell.y >= lo.y && cell.y < hi.y) degenerate = true;
        } else if (level == 1u) {
            cell = base1 + int2((int)lx, (int)lz);
            int2 lo = base2, hi = base2 + int2((int)(grid / 4), (int)(grid / 4));   // the cells the fine level covers
            if (cell.x >= lo.x && cell.x < hi.x && cell.y >= lo.y && cell.y < hi.y) degenerate = true;
        } else {
            cell = 4 * base2 + int2((int)lx, (int)lz);
        }
        worldXZ = (float2(cell) + float2(c)) * spacing;
        uv = float2(lx + c.x, lz + c.y) / float(grid);
        cellWorld = spacing;
        regionWorld = 2.0 * radius * lodScale;
        localXZ = PackWorldToLocal(float3(worldXZ.x, 0.0, worldXZ.y)).xz;
    } else
#endif
    {
        // The field's extent (the default): one grid over the field's extent, centred on the entity.
        uint qx = quadId % grid;
        uint qz = quadId / grid;
        float2 gridVertex = float2(qx + c.x, qz + c.y);
        uv = gridVertex / float(grid);
#if defined(PACK_CAP_WORLD_ANCHOR) && PACK_CAP_WORLD_ANCHOR
        localXZ = (uv - 0.5) * extent + PackFieldCentre();
#else
        localXZ = (uv - 0.5) * extent;
#endif
        worldXZ = PackLocalToWorld(float3(localXZ.x, 0.0, localXZ.y)).xz;
        cellWorld = extent / float(grid);
        regionWorld = extent;
    }

    if (degenerate) {   // a zero-area triangle: the rasterizer produces no fragments
        o.posH = float4(0.0, 0.0, 0.0, 1.0);
        return o;
    }

    // The ground under this vertex (the host's height map): the pack's height is then the height ABOVE the ground.
    bool groundMode = false;
    float groundY = 0.0;
    float3 groundNormal = float3(0.0, 1.0, 0.0);
    bool hasGround = false;
#if defined(PACK_CAP_WORLD_GROUND) && PACK_CAP_WORLD_GROUND
    groundMode = gSurfaceParams.z > 0.5;
    if (groundMode) {
        hasGround = PackGround(worldXZ, groundY, groundNormal);
        // A thin / tall feature (a railing, a post, the edge of a step) gets no layer: the grid would drape a sheet
        // over it (or bury a post in snow).
        if (hasGround && PackGroundRange(worldXZ) > PackGroundStepLimit()) hasGround = false;
    }
#endif

    PackSurfaceIn sin;
    sin.uv = uv;
    sin.posLocal = localXZ;
    sin.time = gWorldTime.x;
#if defined(PACK_CAP_WORLD_GROUND) && PACK_CAP_WORLD_GROUND
    sin.posWorldXZ = worldXZ;
    sin.groundNormal = hasGround ? groundNormal : float3(0.0, 1.0, 0.0);
#endif
    float localY = WORLD_SURFACE_HEIGHT_FN(sin);

    // Gradient normal in local space
    float eps = cellWorld * 0.5;
    float duv = regionWorld > 0.0 ? eps / regionWorld : 0.0;
    PackSurfaceIn sR = sin; sR.uv.x += duv; sR.posLocal.x += eps;
    PackSurfaceIn sU = sin; sU.uv.y += duv; sU.posLocal.y += eps;
    float hR = WORLD_SURFACE_HEIGHT_FN(sR);
    float hU = WORLD_SURFACE_HEIGHT_FN(sU);
    float3 dX = float3(eps, hR - localY, 0.0);
    float3 dZ = float3(0.0, hU - localY, eps);
    float3 normLocal = normalize(cross(dZ, dX));
    float3 normWorld = normalize(PackLocalToWorldVec(normLocal));

    float3 posLocal = float3(localXZ.x, localY, localXZ.y);
    float3 posWorld = PackLocalToWorld(posLocal);
#if defined(PACK_CAP_WORLD_GROUND) && PACK_CAP_WORLD_GROUND
    if (groundMode) {
        // The ground's slope joins the pack's own: the surface follows the ground.
        float2 packSlope = float2(-normWorld.x / max(normWorld.y, 1e-4), -normWorld.z / max(normWorld.y, 1e-4));
        float2 groundSlope = float2(-groundNormal.x / max(groundNormal.y, 1e-4),
                                    -groundNormal.z / max(groundNormal.y, 1e-4));
        float2 total = packSlope + groundSlope;
        normWorld = normalize(float3(-total.x, 1.0, -total.y));
        posWorld.y = hasGround ? (groundY + localY) : 0.0;
    }
    o.posWorldXZ = posWorld.xz;
    o.groundNormal = hasGround ? groundNormal : float3(0.0, 1.0, 0.0);
    // A large positive height when the surface does not follow the ground (nothing is discarded then);
    // a large negative one when there is no ground under the vertex (the fragment is discarded).
    o.groundY = groundMode ? (hasGround ? groundY : -1e9) : 1e9;
#endif

    o.posWorld = posWorld;
    o.normal = normWorld;
    o.uv = uv;
    o.posH = WorldClip(posWorld);
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
#if defined(PACK_CAP_WORLD_GROUND) && PACK_CAP_WORLD_GROUND
    if (i.groundY < -1e8) discard;   // there is no ground under this fragment
    if (i.groundY < 1e8) {           // a ground-following surface: per-pixel checks against the real ground
        // a triangle spanning a ground step hangs in the air above the lower floor: keep only what lies on the ground
        float gy = PackGroundHeight(i.posWorldXZ);
        if (gy <= -1e8 || i.posWorld.y > gy + 1.5) discard;
        if (PackGroundRange(i.posWorldXZ) > PackGroundStepLimit()) discard;
    }
#endif
    PackSurfaceShadeIn sin;
    sin.uv = i.uv;
    sin.posLocal = PackWorldToLocal(i.posWorld);
    sin.posWorld = i.posWorld;
    sin.normal = normalize(i.normal);
    sin.viewDir = normalize(gEyePos.xyz - i.posWorld);
    sin.time = gWorldTime.x;
#if defined(PACK_CAP_WORLD_GROUND) && PACK_CAP_WORLD_GROUND
    sin.posWorldXZ = i.posWorldXZ;
    sin.groundNormal = i.groundNormal;
#endif

    PackSurfaceOut sout = WORLD_SURFACE_SHADE_FN(sin);

    float3 L = PackSunDir();
    float NdotL = saturate(dot(sout.normal, L));
    float shadow = PackSunShadow(i.posWorld);
    float3 sunColor = PackSunColor();
    // Ambient: the scene's sky / ground hemisphere (the same terms the lit scene shader uses), so the surface
    // matches the stage in every render path; the composite adds nothing on top (nt.z = -1 below).
    float3 hemi = PackHemiAmbient(sout.normal);
    float3 ambient = max(PackAmbient(), hemi);
    // The scene's punctual lights (spots / points) with their shadow maps, the same terms the stage floor gets.
    float3 litBase = sout.albedo * (sunColor * (NdotL * shadow) + ambient + WorldPunctualDiffuse(i.posWorld, sout.normal));
    float3 lit = litBase + sout.emission;

#if defined(WORLD_SURFACE_OVERLAY) && WORLD_SURFACE_OVERLAY
    // The overlay blend: the premultiplied colour over the already-lit scene colour (the destination keeps its
    // alpha, so the scene's coverage is untouched).
    o.color = float4(litBase * sout.alpha + sout.emission, sout.alpha);
#else
    o.color = float4(lit, sout.alpha);
#endif
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

// The camera-space wrap: the particle is drawn in the box of size `size` around `cam` (the host moves the tile with
// the camera). The interpolation happens in unwrapped space, then both drawn positions are wrapped, so a wrap never
// leaves a streak across the tile edge.
static float3 WorldWrapCamera(float3 p, float size, float3 cam) {
    float3 w;
    w.x = cam.x + (frac((p.x - cam.x) / size + 0.5) - 0.5) * size;
    w.y = p.y;
    w.z = cam.z + (frac((p.z - cam.z) / size + 0.5) - 0.5) * size;
    return w;
}

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
    float3 drawPos = p.pos;
    float3 prevDrawPos = p.pos - p.vel * gWorldTime.y;
    float drawSize = p.size;
#else
    PackParticle p = gParticles[pId];
    // The simulation is fixed-step (step = the simulation rate): the drawn particle interpolates between the last two
    // grid steps, a = 1 - (simTime - time) / stepDt in [0, 1]. A particle that respawned / teleported in the last step
    // (a clear's zero prevPos, a wrap-sized jump, or a fresh age) is drawn at its new position. The previous frame's
    // drawn position (motion vectors) is the same interpolation at time - frameDelta; where that lies before prevPos's
    // step, prevPos is the previous drawn position.
    float4 prev = gPrevPos[pId];
    float a = saturate(gParticleParams.x);
    float aPrev = 1.0 - (gWorldTime.z - (gWorldTime.x - gParticleParams.y)) / max(gWorldTime.y, 1e-6);
    aPrev = aPrev > 1.0 ? 0.0 : saturate(aPrev);
    bool snapped = p.age < gWorldTime.y ||
                   (gParticleParams.w > 0.0 && length(p.pos - prev.xyz) > gParticleParams.w) ||
                   (dot(prev.xyz, prev.xyz) < 1e-12 && dot(p.pos, p.pos) > 1e-12);
    float3 drawPos = snapped ? p.pos : lerp(prev.xyz, p.pos, a);
    float3 prevDrawPos = snapped ? p.pos : lerp(prev.xyz, p.pos, aPrev);
    float drawSize = p.size;
    if (!snapped && prev.w > 0.0) drawSize = lerp(prev.w, p.size, a);   // the size rides in prevPos.w
#endif

#if defined(PACK_CAP_WORLD_CAMERA) && PACK_CAP_WORLD_CAMERA
    // The camera-space wrap: the particle is drawn in the box of gWorldExtra.x world units around the camera (the
    // pack keeps it in its own tile; the host moves the tile with the camera).
    if (gWorldExtra.x > 0.0 && gWorldExtra.y > 0.5) {
        float size = gWorldExtra.x;
        float3 cam = PackWrapCentre(size);   // the box ahead of the camera
        drawPos = WorldWrapCamera(drawPos, size, cam);
        prevDrawPos = WorldWrapCamera(prevDrawPos, size, cam);
    }
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
    float3 posWorld = drawPos + (camRight * offset.x + camUp * offset.y) * drawSize;

    o.uv = offset + 0.5;
    o.posWorld = posWorld;
    o.viewDir = normalize(gEyePos.xyz - posWorld);
    o.color = p.color;
    o.data = p.data;
    o.size = drawSize;
    o.age = p.age;
    o.posH = WorldClip(posWorld);
    o.curClip = mul(float4(posWorld, 1.0), gViewProjNoJitter);

    float3 prevWorldPos = prevDrawPos + (camRight * offset.x + camUp * offset.y) * drawSize;
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
