// shaders/packs/snow/world.hlsl
// MMDX12 Built-in Snow World Shader Pack v1.0.0
// Implements falling snow, ground snow cover with footprints, and kicked-up snow puffs.

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
            // Ankle ~1.0, toes ~0.2. Allow foot collider whose lower part touches snow
            if (colLocal.y - col.radius <= depth + 0.50 && colLocal.y >= -0.6) {
                float carveTarget = footprintDepth * smoothstep(effectiveRadius, effectiveRadius * 0.25, d);
                val.x = max(val.x, carveTarget);

                float rimTarget = footprintDepth * 0.30 *
                    smoothstep(effectiveRadius * 0.50, effectiveRadius * 0.88, d) *
                    smoothstep(effectiveRadius * 1.50, effectiveRadius * 0.95, d);
                val.y = max(val.y, rimTarget);
            }
        }
    }

    val.x = max(0.0, val.x - refillRate * f.dt);
    val.y = max(0.0, val.y - refillRate * f.dt);

    return val;
}

// ============================================================================
// 2. Snow Surface Height & Shading (grid = 256)
// ============================================================================
float SnowHeight(PackSurfaceIn i) {
    float depth = PackParam(7);
    float footprintDepth = PackParam(8);
    float4 f = PackFieldSample(0, i.posLocal);
    float carved = saturate(f.x);
    float rim = f.y;

    // Low-frequency wind drifts + subtle surface undulation
    float drift = sin(i.posLocal.x * 0.16 + i.posLocal.y * 0.11) * 0.045 +
                  cos(i.posLocal.x * 0.09 - i.posLocal.y * 0.21) * 0.035;
    float micro = (sin(i.posLocal.x * 0.75 + i.posLocal.y * 0.50) +
                   cos(i.posLocal.x * 0.40 - i.posLocal.y * 0.90)) * 0.007;

    // Base snow height carved down by footsteps with raised rim and natural drifts
    float baseH = depth * (1.0 - carved * footprintDepth) + rim + drift + micro;

    // Irregular organic perimeter (never a straight square silhouette)
    float angle = atan2(i.posLocal.y, i.posLocal.x);
    float radius = length(i.posLocal);
    float maxRadius = 24.5 + sin(angle * 5.0) * 2.2 + cos(angle * 8.0) * 1.6 + sin(angle * 13.0) * 0.8;
    float edgeFade = smoothstep(maxRadius, maxRadius - 5.5, radius);

    // Thin out height towards the edge and strictly keep above y = 0
    float h = lerp(0.002, max(0.002, baseH), edgeFade);
    return h;
}

PackSurfaceOut SnowShade(PackSurfaceShadeIn i) {
    PackSurfaceOut o;
    float4 f = PackFieldSample(0, i.posLocal.xz);
    float carved = saturate(f.x);
    // Pristine snow color and deep compressed footprint color
    float3 baseColor = PackParamColor(11); // default [0.96, 0.97, 1.0]
    float3 deepPrintColor = float3(0.35, 0.50, 0.72);

    // Dark compacted edge at the steep walls of the footprint
    float darkEdge = smoothstep(0.015, 0.12, carved) * smoothstep(0.70, 0.20, carved) * 0.45;
    float printWeight = smoothstep(0.015, 0.40, carved);
    float3 snowCol = lerp(baseColor, deepPrintColor, printWeight * 0.9) * (1.0 - darkEdge);
    float2 microGrid = i.posLocal.xz * 45.0;
    uint2 cell = uint2((int2)floor(microGrid) + 32768);
    uint cellHash = PackHash(cell.x ^ PackHash(cell.y * 1999u + 17u));
    float grain = (float(cellHash & 0xffu) / 255.0 - 0.5) * 0.03;
    snowCol += grain;

    // Irregular perimeter: blend color towards dark ground floor, clip outside
    float angle = atan2(i.posLocal.z, i.posLocal.x);
    float radius = length(i.posLocal.xz);
    float maxRadius = 24.5 + sin(angle * 5.0) * 2.2 + cos(angle * 8.0) * 1.6 + sin(angle * 13.0) * 0.8;
    float edgeFade = smoothstep(maxRadius, maxRadius - 5.5, radius);
    clip(edgeFade - 0.001);
    float3 groundColor = float3(0.08, 0.09, 0.11);
    o.albedo = lerp(groundColor, snowCol, edgeFade);

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
            emission = sunColor * (spec * sparkleParam * 2.8 * shadow * edgeFade);
        }
    }
    o.emission = emission;
    return o;
}

// ============================================================================
// 3. Falling Snow Particles (count = 24000)
// ============================================================================
void SimulateFlakes(inout PackParticle p, PackSimInput s) {
    float density = PackParam(0);
    float fallSpeed = PackParam(1);
    float flakeSize = PackParam(2);
    float3 wind = PackParamVec3(3);
    float turbulence = PackParam(4);
    float areaRadius = PackParam(5);
    float spawnHeight = PackParam(6);
    float depth = PackParam(7);

    // Initial state setup for newly created particles
    if (p.age < 0.0) {
        float3 rnd = PackRandom3(s.id, 10101u);
        float r = sqrt(rnd.x) * areaRadius;
        float theta = rnd.y * 6.2831853;
        float localY = rnd.z * spawnHeight;
        p.pos = PackLocalToWorld(float3(r * cos(theta), localY, r * sin(theta)));
        p.age = rnd.z * 10.0;

        // Rich depth variation: small flakes, fluffy flakes, and occasional large hero flakes
        float rSize = PackRandom(s.id, 777u);
        float sizeScale = 1.0;
        if (rSize < 0.78) {
            sizeScale = lerp(0.45, 1.0, rSize / 0.78);
        } else if (rSize < 0.94) {
            sizeScale = lerp(1.1, 1.8, (rSize - 0.78) / 0.16);
        } else {
            sizeScale = lerp(2.2, 3.5, (rSize - 0.94) / 0.06);
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

    // Check bounds: respawn if reaching snow surface, leaving cylinder, or falling below
    float3 pLoc = PackWorldToLocal(p.pos);
    float distXZ = length(pLoc.xz);
    float4 cov = PackFieldSample(0, pLoc.xz);
    float snowH = max(0.002, depth * (1.0 - saturate(cov.x)) + cov.y);

    if (pLoc.y <= snowH || distXZ > areaRadius || pLoc.y > spawnHeight + 5.0 || pLoc.y < -1.0) {
        // Respawn at the top
        uint stepSeed = (uint)(s.time * 20.0);
        float3 rSpawn = PackRandom3(s.id, stepSeed ^ 54321u);
        float r = sqrt(rSpawn.x) * areaRadius;
        float theta = rSpawn.y * 6.2831853;
        float3 newLoc = float3(r * cos(theta), spawnHeight + rSpawn.z * 1.5, r * sin(theta));
        p.pos = PackLocalToWorld(newLoc);
        p.age = 0.0;

        float rSize = PackRandom(s.id, 777u);
        float sizeScale = 1.0;
        if (rSize < 0.78) {
            sizeScale = lerp(0.45, 1.0, rSize / 0.78);
        } else if (rSize < 0.94) {
            sizeScale = lerp(1.1, 1.8, (rSize - 0.78) / 0.16);
        } else {
            sizeScale = lerp(2.2, 3.5, (rSize - 0.94) / 0.06);
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

    // Soft feathered edge, with extra bokeh softness for larger flakes
    float feather = lerp(0.06, 0.35, saturate((i.particle.size - 0.1) / 0.25));
    float alpha = smoothstep(0.5, feather, d) * i.particle.color.a;

    // Lighting with sun and ambient
    float3 sunColor = PackSunColor();
    float3 ambient = PackAmbient();
    float3 L = PackSunDir();
    float3 V = i.viewDir;

    // Forward scattering for translucent ice crystals
    float forward = saturate(dot(L, -V) * 0.5 + 0.5);
    float3 lit = lerp(ambient * 1.15, sunColor * 0.95 + ambient, forward * 0.6 + 0.4);
    return float4(lit, alpha);
}

// ============================================================================
// 4. Kicked-up Snow Particles (count = 6000)
// ============================================================================
void SimulateKick(inout PackParticle p, PackSimInput s) {
    float depth = PackParam(7);
    float kickAmount = PackParam(13);
    float kickHeight = PackParam(14);
    float minFootSpeed = PackParam(15);

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

        // Check landing on snow surface
        float3 pLoc = PackWorldToLocal(p.pos);
        float4 cov = PackFieldSample(0, pLoc.xz);
        float snowTop = max(0.002, depth * (1.0 - saturate(cov.x)) + cov.y);

        if (pLoc.y <= snowTop || p.age >= lifeSpan) {
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
    float4 cov = PackFieldSample(0, colLoc.xz);
    float snowTop = max(0.002, depth * (1.0 - saturate(cov.x)) + cov.y);

    if (colLoc.y - col.radius <= snowTop + 0.45 && colLoc.y >= snowTop - 0.7) {
        // High spawn probability on fast steps
        float prob = saturate((speed - minFootSpeed) * kickAmount * s.dt * 6.0);
        float rndRoll = PackRandom(s.id, (uint)(s.time * 60.0) ^ 0x7b29u);
        if (rndRoll < prob) {
            // Clump (~35%) vs fine powder (~65%)
            float typeRoll = PackRandom(s.id, 0x5432u);
            bool isClump = (typeRoll < 0.35);

            float3 rndSpread = (PackRandom3(s.id, (uint)(s.time * 120.0) ^ 0x4f12u) - 0.5) * 2.0;
            p.pos = col.pos + rndSpread * (col.radius * 0.4) + float3(0.0, -col.radius * 0.25, 0.0);

            float3 upwardKick = float3(0.0, kickHeight * (isClump ? 3.0 : 2.2), 0.0);
            float3 randomSpread = rndSpread * (kickHeight * 1.2);
            p.vel = col.vel * 0.6 + upwardKick + randomSpread;
            p.age = 0.0;

            float baseSize = isClump ? lerp(0.07, 0.15, PackRandom(s.id, 0x1234u))
                                     : lerp(0.04, 0.08, PackRandom(s.id, 0x1234u));
            p.size = baseSize;
            p.color = float4(1.0, 1.0, 1.0, 0.9);
            p.data = float4(baseSize, isClump ? 1.0 : 0.0, 0.0, 0.0);
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
    float3 ambient = PackAmbient();
    float3 lit = (sunColor * 0.95 + ambient * 1.1) * 1.2; // Bright glowing puffs
    return float4(lit, alpha);
}
