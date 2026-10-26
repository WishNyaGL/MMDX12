// S9b-1: the Physical Sky light's shader branch (Preetham 1999, the Perez distribution in Yxy), included by
// common.hlsli after the SceneCB declaration. The constants come from Renderer::FillSceneConstants
// (render/PhysicalSky.{h,cpp} evaluates them on the CPU); the CPU twin is PhysicalSkyRadiance there.
//
// The branch is taken by SkyColor (common.hlsli) while gSkyParams.x > 0.5; with no Sky light every value here is 0 and
// no shader reads it, so the frame is exactly the one before S9b-1.

// Perez's five-term angular distribution: (1 + A e^(B / cos theta)) (1 + C e^(D gamma) + E cos^2 gamma).
float PhysicalSkyPerez(float4 abcd, float e, float cosTheta, float cosGamma) {
    return (1.0 + abcd.x * exp(abcd.y / max(cosTheta, 0.01))) *
           (1.0 + abcd.z * exp(abcd.w * acos(clamp(cosGamma, -1.0, 1.0))) + e * cosGamma * cosGamma);
}

// Yxy -> linear sRGB (the Preetham paper's XYZ -> RGB matrix).
float3 PhysicalSkyYxyToRgb(float y, float x, float yChroma) {
    float yy = max(yChroma, 1e-4);
    float X = x / yy * y;
    float Y = y;
    float Z = (1.0 - x - yChroma) / yy * y;
    return float3(3.2406 * X - 1.5372 * Y - 0.4986 * Z, -0.9689 * X + 1.8758 * Y + 0.0415 * Z,
                  0.0557 * X - 0.2040 * Y + 1.0570 * Z);
}

// The model's radiance for a direction, before the haze blend and the night floor (float3 physical_sky.h's
// ModelRadiance).
float3 PhysicalSkyModelRadiance(float3 dir) {
    float3 s = gSkySunToward.xyz;
    // 1e-4, not 0: a ray straight down would normalise a zero vector (the below-horizon branch drops this value anyway)
    float3 d = normalize(float3(dir.x, max(dir.y, 1e-4), dir.z));
    float cosTheta = max(d.y, 0.0);
    float cosGamma = clamp(dot(d, s), -1.0, 1.0);
    float Y = gSkyZenithOverF.x * PhysicalSkyPerez(gSkyPerezY, gSkyPerezE.x, cosTheta, cosGamma);
    float x = gSkyZenithOverF.y * PhysicalSkyPerez(gSkyPerezX, gSkyPerezE.y, cosTheta, cosGamma);
    // + 0.012 onto the daylight locus (Preetham's chromaticities sit slightly magenta: a mauve noon horizon);
    // mirrored in render/PhysicalSky.cpp
    float y = gSkyZenithOverF.z * PhysicalSkyPerez(gSkyPerezYy, gSkyPerezE.z, cosTheta, cosGamma) + 0.012;
    return max(PhysicalSkyYxyToRgb(Y, x, y), 0.0);
}

// The sky's linear radiance in a world direction (up is +y), the twin of mmdx::PhysicalSkyRadiance.
float3 PhysicalSkyRadiance(float3 dir) {
    float scale = gSkyParams.y * gSkyParams.w;
    float3 night = gSkyNight.xyz;
    float3 horiz = PhysicalSkyModelRadiance(normalize(float3(dir.x, 0.02, dir.z))) * scale + night;
    float3 sky = PhysicalSkyModelRadiance(dir) * scale + night;
    // horizon haze: the lower ~15 degrees lean toward the horizon colour
    sky = lerp(sky, horiz, gSkyParams.z * (1.0 - smoothstep(0.0, 0.26, dir.y)));
    if (dir.y >= 0.0) return sky;
    // below the horizon: today's blend from the horizon colour to the ground colour
    return lerp(horiz, gGroundColor, saturate(-dir.y * 6.0));
}

// S9b-4: the procedural cloud layer on the S9b-1 sky (app/Lighting.cpp fills gCloudParams / gCloudParams2). A layer of
// value-noise density at cloudHeightKm: the view ray meets it on the ground plane, the noise is sampled there (the wind
// offset already in km) and the ray's density is lit by the sun (a Henyey-Greenstein forward lobe plus a self-shadow
// step 0.3 km toward the sun) and by the sky / ground ambient colours.
float CloudHash(float2 p) { p = frac(p * float2(0.1031, 0.1030)); p += dot(p, p.yx + 33.33); return frac((p.x + p.y) * p.x); }
float CloudNoise(float2 p) {           // value noise, smooth
    float2 i = floor(p), f = frac(p); float2 w = f * f * (3.0 - 2.0 * f);
    float a = CloudHash(i), b = CloudHash(i + float2(1, 0)), c = CloudHash(i + float2(0, 1)), d = CloudHash(i + float2(1, 1));
    return lerp(lerp(a, b, w.x), lerp(c, d, w.x), w.y);
}
float CloudFbm(float2 p) {             // 5 octaves, rotated, in [0, 1]
    float sum = 0.0, amp = 0.5;
    const float2x2 rot = float2x2(0.8, -0.6, 0.6, 0.8);
    [unroll] for (int o = 0; o < 5; ++o) { sum += amp * CloudNoise(p); p = mul(rot, p) * 2.02 + 17.1; amp *= 0.5; }
    return sum / 0.96875;
}
float CloudDensity(float2 km) {        // km = ground-plane position of the layer point (wind already added)
    const float n = CloudFbm(km / max(gCloudParams2.x, 0.05));
    return saturate((n - (1.0 - gCloudParams.x)) / max(gCloudParams.z, 0.02));
}
// Composite the cloud layer over the sky colour `c` (linear, before * gSunIntensity) for a direction above the horizon.
float3 PhysicalSkyClouds(float3 dir, float3 c) {
    if (gCloudParams.x <= 0.0 || dir.y <= 0.0) return c;
    const float2 km = dir.xz / max(dir.y, 0.03) * gCloudParams.w + gCloudParams2.yz;
    const float d = CloudDensity(km);
    if (d <= 0.0) return c;
    const float3 s = gSkySunToward.xyz;
    // self-shadow: the density 0.3 km toward the sun along the layer
    // (at least 0.105 km even with a high sun, else the step re-samples the same point and every cloud core went black;
    // extinction 1, not 3: the layer is thin, a sunlit cloud keeps 37..100 % of the sun)
    const float2 toSun = s.xz / max(length(s.xz), 1e-3) * 0.3 * max(1.0 - s.y, 0.35);
    const float trans = exp(-1.0 * CloudDensity(km + toSun));
    const float g = 0.6, cosT = dot(dir, s);
    const float hg = (1.0 - g * g) / (4.0 * PI * pow(max(1.0 + g * g - 2.0 * g * cosT, 1e-4), 1.5));
    const float3 sunLin = SrgbToLinear(saturate(gLightColor));
    // calibrated so a sunlit noon cloud reads white over the sky (~1.0 linear with MMD's default sun) and a
    // self-shadowed core stays grey-blue (the ambient share); review 2026-10-27: 0.55 / 6 / zenith-only ambient drew dark, bluish clouds
    const float3 lit = sunLin * trans * (3.0 + 8.0 * hg) + (gSkyZenith + gSkyHorizon) * 0.45 + gGroundColor * 0.2;
    const float alpha = d * gCloudParams.y * smoothstep(0.0, 0.12, dir.y);
    return lerp(c, lit, alpha);
}
