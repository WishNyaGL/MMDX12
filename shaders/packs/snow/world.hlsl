// shaders/packs/snow/world.hlsl
// MMDX12 Built-in Snow World Shader Pack v2.0.0
// Falling snow over the whole map around the camera (the camera-space tile), snow on every upward surface
// (the camera-centred cover above the ground) with the performers' footprints, and kicked-up snow puffs.
//
// Capabilities (declared in pack.json's "requires"): world.camera (the camera-space snowfall),
// world.anchor (the performer-anchored cover field), world.ground (the ground height map and the cover
// that follows the ground).

// ============================================================================
// 0. Shared helpers
// ============================================================================

// The snow cover's height ABOVE the ground at a local xz: the depth, the drifts and the footprint carve.
// The cover field is performer-anchored: outside the field the cover is untouched snow (PackFieldSample returns 0)
// and PackFieldCoverage fades the carve out over the field's border, so the border of the field is invisible.
float SnowCoverHeight(float2 posLocalXZ) {
    float depth = PackParam(7);
    float footprintDepth = PackParam(8);

    float4 f = PackFieldSample(0, posLocalXZ);
    float coverage = PackFieldCoverage(0, posLocalXZ);
    float carved = saturate(f.x) * coverage;   // x: the carved amount (the snow removed), y: the rim
    float rim = f.y * coverage;

    // Low-frequency wind drifts + subtle surface undulation
    float drift = sin(posLocalXZ.x * 0.045 + posLocalXZ.y * 0.035) * 0.055 +
                  cos(posLocalXZ.x * 0.028 - posLocalXZ.y * 0.058) * 0.042;
    float micro = (sin(posLocalXZ.x * 0.75 + posLocalXZ.y * 0.50) +
                   cos(posLocalXZ.x * 0.40 - posLocalXZ.y * 0.90)) * 0.007;

    // The snow height carved down by the footsteps, with a raised rim and the natural drifts
    return depth * (1.0 - carved * footprintDepth) + rim + drift + micro;
}

// The cover fades out where the ground is too steep to hold snow (the ground's normal below ~0.75).
float SnowSlopeFade(float3 groundNormal) {
    return saturate(groundNormal.y / 0.75);
}

// ============================================================================
// 1. Snow Cover Field Update (ping-pong persistent field, rate = 20 Hz)
// ============================================================================
float4 UpdateCover(PackFieldInput f) {
    float4 val = f.prev; // x: carved amount (snow removed), y: compaction / rim

    float depth = PackParam(7);
    float footprintDepth = PackParam(8);
    float footprintWidth = PackParam(9);
    float refillRate = PackParam(10);

    // Carve footprints where FOOT colliders touch or penetrate the snow layer
    uint colCount = PackColliderCount();
    for (uint i = 0; i < colCount; ++i) {
        PackColliderData col = PackCollider(i);
        // Only foot colliders (flags & 1)
        if ((col.flags & 1u) == 0u) continue;

        float3 colLocal = PackWorldToLocal(col.pos);
        float effectiveRadius = max(0.40, col.radius * footprintWidth);
        float d = distance(f.posLocal, colLocal.xz);

        if (d < effectiveRadius * 1.5) {
            // The ground under the foot (the stage's height): the footprints also work on a raised stage.
            float groundY = PackGroundHeight(col.pos.xz);
            if (groundY > -1e8) {
                // The snow surface in world units: the ground plus the snow depth.
                float snowTop = groundY + depth;
                // Ankle ~1.0, toes ~0.2. Allow foot collider whose lower part touches snow
                if (col.pos.y - col.radius <= snowTop + 0.50 && col.pos.y >= groundY - 0.6) {
                    float carveTarget = footprintDepth * smoothstep(effectiveRadius, effectiveRadius * 0.25, d);
                    val.x = max(val.x, carveTarget);

                    float rimTarget = footprintDepth * 0.30 *
                        smoothstep(effectiveRadius * 0.50, effectiveRadius * 0.88, d) *
                        smoothstep(effectiveRadius * 1.50, effectiveRadius * 0.95, d);
                    val.y = max(val.y, rimTarget);
                }
            }
        }
    }

    val.x = max(0.0, val.x - refillRate * f.dt);
    val.y = max(0.0, val.y - refillRate * f.dt);

    return val;
}

// ============================================================================
// 2. Snow Surface Height & Shading (camera-centred grid around the camera)
// ============================================================================
float SnowHeight(PackSurfaceIn i) {
    // The height ABOVE the ground (the host adds the ground height and clips where there is no ground).
    float h = SnowCoverHeight(i.posLocal.xy);
    // No snow on steep surfaces: the cover thins out and vanishes where the ground is not nearly horizontal.
    return h * SnowSlopeFade(i.groundNormal);
}

PackSurfaceOut SnowShade(PackSurfaceShadeIn i) {
    PackSurfaceOut o;

    // The footprint carve (the field is performer-anchored: outside it the snow is untouched).
    float4 f = PackFieldSample(0, i.posLocal.xz);
    float coverage = PackFieldCoverage(0, i.posLocal.xz);
    float carved = saturate(f.x) * coverage;

    // Pristine snow color and customizable footprint shade
    float3 baseColor = PackParamColor(11);  // default [0.96, 0.97, 1.0]
    float3 printColor = PackParamColor(13); // default [0.70, 0.75, 0.82] (soft, slightly cool grey-blue)
    float printTint = PackParam(14);        // default 0.45 (modest tint)

    float printWeight = smoothstep(0.02, 0.45, carved);
    float3 targetCol = lerp(baseColor, printColor, printTint);
    float3 snowCol = lerp(baseColor, targetCol, printWeight);
    float2 microGrid = i.posLocal.xz * 45.0;
    uint2 cell = uint2((int2)floor(microGrid) + 32768);
    uint cellHash = PackHash(cell.x ^ PackHash(cell.y * 1999u + 17u));
    float grain = (float(cellHash & 0xffu) / 255.0 - 0.5) * 0.03;
    snowCol += grain;

    // No snow on steep surfaces: the cover fades out with the ground's slope (the host clips where there is no
    // ground at all).
    float slope = SnowSlopeFade(i.groundNormal);
    clip(slope - 0.02);

    o.albedo = snowCol;
    o.alpha = 1.0;
    o.roughness = 0.8;
    o.normal = i.normal;

    // View-dependent sparkle glints from deterministic hash of local position / texels
    float sparkleParam = PackParam(12);
    float3 emission = float3(0.0, 0.0, 0.0);
    if (sparkleParam > 0.001) {
        float glintChance = float(cellHash & 0xffffu) * (1.0 / 65535.0);
        if (glintChance > 0.80) { // ~20% of crystals sparkle
            float3 tilt = (PackRandom3(cellHash, 991u) - 0.5) * 0.75;
            float3 facetNormal = normalize(i.normal + tilt);

            float3 L = PackSunDir();
            float3 V = i.viewDir;
            float3 H = normalize(L + V);
            float NdotH = saturate(dot(facetNormal, H));
            float spec = pow(NdotH, 65.0);

            float shadow = PackSunShadow(i.posWorld);
            float3 sunColor = PackSunColor();
            emission = sunColor * (spec * sparkleParam * 2.8 * shadow);
        }
    }
    o.emission = emission;
    return o;
}

// ============================================================================
// 3. Falling Snow Particles (camera space, count = 24000)
// ============================================================================
void SimulateFlakes(inout PackParticle p, PackSimInput s) {
    float density = PackParam(0);
    float fallSpeed = PackParam(1);
    float flakeSize = PackParam(2);
    float3 wind = PackParamVec3(3);
    float turbulence = PackParam(4);
    float spawnHeight = PackParam(6);

    // The camera-space tile: its size is the range param ("wrapParam"); flakes fill the whole tile, so the wrapped box
    // in front of the camera has no empty bands.
    float halfTile = 0.5 * max(PackWrapSize(), 1.0);

    // Initial state setup for newly created particles
    if (p.age < 0.0) {
        float3 rnd = PackRandom3(s.id, 10101u);
        float2 tileXZ = (rnd.xy - 0.5) * 2.0 * halfTile;
        float localY = rnd.z * spawnHeight;
        p.pos = PackLocalToWorld(PackWrapTile(float3(tileXZ.x, localY, tileXZ.y)));
        p.age = rnd.z * 10.0;

        // Snowflake size distribution: typical flakes (82%), medium flakes (14%), near-camera flakes (4%)
        float rSize = PackRandom(s.id, 777u);
        float sizeScale = 1.0;
        if (rSize < 0.82) {
            sizeScale = lerp(0.65, 1.05, rSize / 0.82);
        } else if (rSize < 0.96) {
            sizeScale = lerp(1.05, 1.65, (rSize - 0.82) / 0.14);
        } else {
            sizeScale = lerp(1.65, 2.50, (rSize - 0.96) / 0.04);
        }
        p.size = flakeSize * sizeScale;
        p.vel = wind + float3(0.0, -fallSpeed, 0.0);
        p.color = float4(1.0, 1.0, 1.0, 0.95);
        p.data = float4(rnd.x, rnd.y, sizeScale, 0.0);

        // Density < 1 hides a deterministic share of particles (size 0)
        if (PackRandom(s.id, 888u) >= density) {
            p.size = 0.0;
            p.color.a = 0.0;
        }
        return;
    }

    // Step physics
    p.age += s.dt;

    // Multi-frequency harmonic turbulence
    float phase = s.time * 1.8 + float(s.id & 63u) * 0.12;
    float3 turb = float3(
        sin(p.pos.y * 0.35 + phase),
        cos(p.pos.x * 0.25 + phase * 0.8) * 0.25,
        cos(p.pos.z * 0.35 + phase * 1.1)
    ) * turbulence;

    // Gentle organic swaying back and forth
    float swayFreq = 1.8 + float(s.id & 7u) * 0.25;
    float swayAmp = 0.35 + float((s.id >> 3) & 7u) * 0.08;
    float swayX = sin(s.time * swayFreq + float(s.id)) * swayAmp;
    float swayZ = cos(s.time * swayFreq * 0.9 + float(s.id * 3u)) * swayAmp;

    // Individual flake speed variation (larger flakes fall slightly faster)
    float indSpeed = fallSpeed * lerp(0.80, 1.25, PackRandom(s.id, 333u)) * (0.85 + p.data.z * 0.15);
    p.vel = wind + turb + float3(swayX, -indSpeed, swayZ);
    p.pos += p.vel * s.dt;

    // The landing test: the flake lands on the ground under the position it is DRAWN at (the camera-space wrap
    // moves it around the camera), not at the local y = 0.
    float3 pLoc = PackWorldToLocal(p.pos);
    float3 drawn = PackParticleWorld(pLoc);
    float groundY = PackGroundHeight(drawn.xz);
    bool landed;
    if (groundY > -1e8) {
        // The ground plus the snow that lies on it
        float snowTop = groundY + SnowCoverHeight(PackWorldToLocal(drawn).xz);
        landed = (p.pos.y <= snowTop);
    } else {
        // No ground under it (a void): the flake falls to the bottom of the tile.
        landed = (pLoc.y <= -1.0);
    }

    if (landed || pLoc.y > spawnHeight + 5.0) {
        // Respawn at the top, inside the tile
        uint stepSeed = (uint)(s.time * 20.0);
        float3 rSpawn = PackRandom3(s.id, stepSeed ^ 54321u);
        float2 tileXZ = (rSpawn.xy - 0.5) * 2.0 * halfTile;
        float3 newLoc = float3(tileXZ.x, spawnHeight + rSpawn.z * 1.5, tileXZ.y);
        p.pos = PackLocalToWorld(PackWrapTile(newLoc));
        p.age = 0.0;

        float rSize = PackRandom(s.id, 777u);
        float sizeScale = 1.0;
        if (rSize < 0.82) {
            sizeScale = lerp(0.65, 1.05, rSize / 0.82);
        } else if (rSize < 0.96) {
            sizeScale = lerp(1.05, 1.65, (rSize - 0.82) / 0.14);
        } else {
            sizeScale = lerp(1.65, 2.50, (rSize - 0.96) / 0.04);
        }
        p.size = flakeSize * sizeScale;
        p.vel = wind + float3(0.0, -fallSpeed, 0.0);
        p.color = float4(1.0, 1.0, 1.0, 0.95);
        p.data = float4(rSpawn.x, rSpawn.y, sizeScale, 0.0);

        if (PackRandom(s.id, 888u) >= density) {
            p.size = 0.0;
            p.color.a = 0.0;
        }
    } else {
        // Density modulation
        if (PackRandom(s.id, 888u) >= density) {
            p.size = 0.0;
            p.color.a = 0.0;
        } else if (p.size <= 0.0) {
            p.size = flakeSize * p.data.z;
            p.color.a = 0.95;
        }
    }
}

float4 ShadeFlakes(PackParticleShadeIn i) {
    float d = length(i.uv - 0.5);
    if (d > 0.5) return 0.0;

    // Firm core with soft feathered edge: alpha ~0.75-0.85 in core
    float edge = smoothstep(0.5, 0.20, d);
    float alpha = edge * 0.85 * i.particle.color.a;

    // Light with sun (forward scatter) + hemisphere ambient (scene sky / ground)
    float3 sunColor = PackSunColor();
    float3 hemi = PackHemiAmbient(float3(0.0, 1.0, 0.0));
    float3 L = PackSunDir();
    float3 V = i.viewDir;

    // Translucent crystalline forward scattering
    float forward = saturate(dot(L, -V) * 0.5 + 0.5);
    // As bright as the snow cover facing the sky (albedo ~0.95, sun + hemisphere), plus a little forward scatter, so
    // flakes read as snow against a bright sky and still follow a dark stage's lighting.
    // Flakes are drawn after the scene composite, so they also take the sky light they scatter (gSkyHorizon): against a
    // bright sky they stay white instead of reading as dark specks.
    float3 lit = 0.95 * (sunColor * (0.75 + 0.35 * forward) + hemi) + gSkyHorizon * 0.9;
    return float4(lit, alpha);
}

// ============================================================================
// 4. Kicked-up Snow Particles (count = 6000, local space: they spawn at the feet)
// ============================================================================
void SimulateKick(inout PackParticle p, PackSimInput s) {
    float kickAmount = PackParam(15);
    float kickHeight = PackParam(16);
    float minFootSpeed = PackParam(17);

    // Active particle simulation
    if (p.age >= 0.0) {
        p.age += s.dt;

        bool isClump = (p.data.y > 0.5);
        float lifeSpan = isClump ? 1.5 : 2.0;

        // Clumps fall faster with less drag; powder drifts with high drag
        float drag = isClump ? 1.8 : 3.0;
        float gravity = isClump ? -14.0 : -7.5;

        p.vel *= exp(-drag * s.dt);
        p.vel.y += gravity * s.dt;
        p.pos += p.vel * s.dt;

        // Puff expands and fades over its lifetime
        float lifeProgress = saturate(p.age / lifeSpan);
        float baseSize = p.data.x;
        float expansion = isClump ? 1.3 : 2.5;
        p.size = baseSize * (1.0 + lifeProgress * expansion);
        p.color.a = saturate(1.0 - lifeProgress * 1.05) * 0.9;

        // Check landing on the snow surface (the ground height under the particle)
        float3 pLoc = PackWorldToLocal(p.pos);
        float groundY = PackGroundHeight(p.pos.xz);
        bool landed;
        if (groundY > -1e8) {
            float snowTop = groundY + SnowCoverHeight(pLoc.xz);
            landed = (p.pos.y <= snowTop);
        } else {
            landed = (pLoc.y <= -1.0);
        }

        if (landed || p.age >= lifeSpan) {
            p.age = -1.0;
            p.size = 0.0;
            p.color.a = 0.0;
        }
        return;
    }

    // Inactive particle: attempt spawn from foot colliders
    p.size = 0.0;
    p.color.a = 0.0;

    uint colCount = PackColliderCount();
    if (colCount == 0 || kickAmount <= 0.0) return;

    // Pick a collider by hash
    uint colIdx = PackHash(s.id ^ (uint)(s.time * 40.0) ^ 0xa351u) % colCount;
    PackColliderData col = PackCollider(colIdx);

    // Only foot colliders kick snow
    if ((col.flags & 1u) == 0u) return;

    float speed = length(col.vel);
    if (speed < minFootSpeed) return;

    // Check if the foot is near the snow top (covers ankles ~1.0 and toes ~0.2)
    float3 colLoc = PackWorldToLocal(col.pos);
    float groundY = PackGroundHeight(col.pos.xz);
    if (groundY > -1e8) {
        float snowTop = groundY + SnowCoverHeight(colLoc.xz);
        if (col.pos.y - col.radius <= snowTop + 0.45 && col.pos.y >= snowTop - 0.7) {
            // Snow still present under the foot: fresh snow = 1.0, fully carved print = ~0.0
            float4 cov = PackFieldSample(0, colLoc.xz);
            float coverage = PackFieldCoverage(0, colLoc.xz);
            float snowAvailable = saturate(1.0 - saturate(cov.x) * coverage);
            float snowFactor = smoothstep(0.05, 0.85, snowAvailable);
            float effectiveKick = kickAmount * snowFactor;

            // Scale spawn probability on fast steps by remaining snow
            float prob = saturate((speed - minFootSpeed) * effectiveKick * s.dt * 6.0);
            float rndRoll = PackRandom(s.id, (uint)(s.time * 60.0) ^ 0x7b29u);
            if (rndRoll < prob) {
                // Clump (~35%) vs fine powder (~65%)
                float typeRoll = PackRandom(s.id, 0x5432u);
                bool isClump = (typeRoll < 0.35);

                float3 rndSpread = (PackRandom3(s.id, (uint)(s.time * 120.0) ^ 0x4f12u) - 0.5) * 2.0;
                p.pos = col.pos + rndSpread * (col.radius * 0.4) + float3(0.0, -col.radius * 0.25, 0.0);

                float3 upwardKick = float3(0.0, kickHeight * (isClump ? 3.0 : 2.2), 0.0);
                float3 randomSpread = rndSpread * (kickHeight * 1.2);
                p.vel = (col.vel * 0.6 + upwardKick + randomSpread) * lerp(0.5, 1.0, snowFactor);
                p.age = 0.0;

                float baseSize = isClump ? lerp(0.07, 0.15, PackRandom(s.id, 0x1234u))
                                         : lerp(0.04, 0.08, PackRandom(s.id, 0x1234u));
                p.size = baseSize * lerp(0.4, 1.0, snowFactor);
                p.color = float4(1.0, 1.0, 1.0, 0.9 * lerp(0.3, 1.0, snowFactor));
                p.data = float4(baseSize, isClump ? 1.0 : 0.0, 0.0, 0.0);
            }
        }
    }
}

float4 ShadeKick(PackParticleShadeIn i) {
    float d = length(i.uv - 0.5);
    bool isClump = (i.particle.data.y > 0.5);

    // Clumps have firmer cores; powder is softer
    float feather = isClump ? 0.08 : 0.03;
    float alpha = smoothstep(0.5, feather, d) * i.particle.color.a;

    float3 sunColor = PackSunColor();
    float3 hemi = PackHemiAmbient(float3(0.0, 1.0, 0.0));
    float3 lit = (sunColor * 0.95 + hemi * 1.1) * 1.1;
    return float4(lit, alpha);
}
