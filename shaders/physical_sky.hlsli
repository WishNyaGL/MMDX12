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
