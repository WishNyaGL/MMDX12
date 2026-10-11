// hoyo_toon_v2_core.hlsli: shared shading math for hoyo_toon v2 (raster and PT/GI).
#ifndef HOYO_TOON_V2_CORE_HLSLI
#define HOYO_TOON_V2_CORE_HLSLI

#define T_BODY_LM   0u
#define T_HAIR_LM   1u
#define T_BODY_RAMP 2u
#define T_HAIR_RAMP 3u
#define T_FACE_SDF  4u
#define T_METAL     5u
// Star Rail maps (pack.json order): used when the Genshin map of the same part is missing
#define T_BODY_WARM 6u   // Body_Warm_Ramp (day)   256 x 16: 8 material rows, two pixel rows each
#define T_BODY_COOL 7u   // Body_Cool_Ramp (night)
#define T_HAIR_WARM 8u   // Hair_Warm_Ramp         256 x 2: one row
#define T_HAIR_COOL 9u
#define T_BODY_LM_SR 10u // Body_LightMap_L: R specular, G shadow bias (0.39 neutral), B specular size, A material id (8 steps)

// light map alpha (1.0 / 0.7 / 0.5 / 0.3 / 0.0) -> ramp row 0..4
uint MaterialRow(float a) {
    return a > 0.85 ? 0u : (a > 0.6 ? 1u : (a > 0.4 ? 2u : (a > 0.15 ? 3u : 4u)));
}

// Star Rail light map alpha: eight steps of 1/8 (0.02, 0.15, 0.27 ...) -> ramp row 0..7
uint SrMaterialRow(float a) {
    return (uint)min(a * 8.0, 7.0);
}

// Star Rail ramp V for a material row; `height` = ramp height in pixels (16 body, 2 hair: rows are 2 pixels tall)
float SrRampV(uint row, uint height) {
    uint rows = max(height / 2u, 1u);
    return (min(row, rows - 1u) * 2.0 + 1.0) / (rows * 2.0);
}

// Star Rail's painted shadow bias G is 0.39 where Genshin's is 0.5: rescale so the neutral value stays neutral
static const float kSrBiasScale = 0.5 / 0.392;

// Computes the horizontal face normal from the head frame.
// Projects the head-local horizontal offset into a cylindrical normal.
float3 HoyoFaceNormal(float3 worldPos, float3 headPos, float headScale, float3 headRight, float3 headForward, float faceWidth) {
    float3 p = worldPos - headPos;
    float w = max(faceWidth, 0.01);
    float s = max(headScale, 1e-4);
    float x = dot(p, headRight) / (w * s);
    float z = dot(p, headForward) / s;
    float2 n = float2(x, z);
    float len = length(n);
    n = len > 1e-4 ? n / len : float2(0.0, 1.0);
    return normalize(n.x * headRight + n.y * headForward);
}

// Projects the light direction into the head's horizontal plane.
float3 HoyoHeadHorizontalLight(float3 L, float3 headUp, float3 headForward) {
    float3 Lhoriz = L - headUp * dot(L, headUp);
    float len = length(Lhoriz);
    return len > 1e-4 ? Lhoriz / len : headForward;
}

// Geometric face light calculation: smoothstep of dot(Nface, Lhoriz)
// Under frontal light, ndl > 0 across the entire face front, leaving it cleanly lit.
// Under side light, the terminator smoothly sweeps across the center of the face.
float HoyoFaceGeometricLight(float3 worldPos, float3 headPos, float headScale, float3 headRight, float3 headUp, float3 headForward, float3 L, float faceWidth, float faceSoftness) {
    float3 Nface = HoyoFaceNormal(worldPos, headPos, headScale, headRight, headForward, faceWidth);
    float3 Lhoriz = HoyoHeadHorizontalLight(L, headUp, headForward);
    float ndl = dot(Nface, Lhoriz);
    return smoothstep(-faceSoftness, faceSoftness, ndl);
}

// Fallback shadow tint warm factor
float3 HoyoV2WarmFactor(float extraWarmth, float fbWarmth) {
    return lerp(float3(1.0, 1.0, 1.0), float3(1.0, 0.68, 0.70), saturate(fbWarmth + extraWarmth));
}

#endif // HOYO_TOON_V2_CORE_HLSLI
