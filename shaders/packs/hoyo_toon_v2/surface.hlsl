// hoyo_toon_v2: Genshin-style shading from the game's own material maps, on the official MMD models.
//
// The MMD releases of the HoYoverse characters use the game's UV layout, so the game's light maps, shadow ramps and face
// SDF line up with them. Those maps belong to HoYoverse and are not part of this pack: set a texture folder for the
// character (library panel / studio inspector, "텍스처 폴더") that holds its maps under their usual names, e.g.
//   Avatar_Girl_Pole_Hutao_Tex_Body_Lightmap.png, ..._Hair_Lightmap.png, ..._Body_Shadow_Ramp.png,
//   ..._Hair_Shadow_Ramp.png, Avatar_Girl_Tex_FaceLightmap.png   (+ optional Avatar_Tex_MetalMap.png)
// The pack declares the name endings (Body_Lightmap.png, ...), which match those files. Any map that is missing falls
// back to the geometric shading of hoyo_toon v1 for that part, so the pack also works with no textures at all.
//
// Maps (pack.json "textures" order):
//   0 Body_Lightmap  1 Hair_Lightmap   linear. R specular / metal mask (> 0.9 = metal), G shadow bias (0.5 neutral,
//                                      0 always shadowed, 1 always lit), B specular size, A material id (ramp row)
//   2 Body_Shadow_Ramp  3 Hair_Shadow_Ramp   256 x 20: rows 0-9 day, 10-19 night, two rows per material id;
//                                            u = 0 deep shadow .. 1 the light side
//   4 FaceLightmap   linear SDF for a light from the character's right; mirrored for the other side
//   5 MetalMap       matcap for metal parts
//
// Star Rail (2.2.0): the same pack reads Star Rail's maps when a part has no Genshin one. 6 Body_Warm_Ramp / 7 Body_Cool_Ramp
// (day / night, 8 material rows picked by the light map alpha in steps of 1/8), 8 / 9 Hair_Warm_Ramp / Hair_Cool_Ramp (one
// row), 10 Body_LightMap_L (the body light map; the hair one is Hair_LightMap, as in Genshin). Star Rail's light map has G = 0.39
// neutral and no metal flag in R (R is plain specular strength), so there is no matcap on those parts. Its Face_Shadow is not
// a Genshin SDF, so the face keeps the geometric light with the body ramp's skin tone.

#include "hoyo_toon_v2_core.hlsli"

// ---- parameters (pack.json "params", same order) ---------------------------------------------------------------------
#define P_SOFTNESS        PackParam(0)    // shadow edge width
#define P_NIGHT           PackParam(1)    // 0 day ramps .. 1 night ramps
#define P_SPECULAR        PackParam(2)
#define P_SHININESS       PackParam(3)
#define P_METAL           PackParam(4)
#define P_FACE_SOFTNESS   PackParam(5)
#define P_FACE_CAST       PackParam(6)    // how much cast shadows (hair, hat) darken the face
#define P_FACE_FLIP       PackParam(7)    // mirror the face SDF (models whose face UVs are mirrored)
#define P_RIM_WIDTH       PackParam(8)
#define P_RIM_STRENGTH    PackParam(9)
#define P_AMBIENT         PackParam(10)
#define P_EYE_BRIGHTNESS  PackParam(11)
#define P_FB_DARKNESS     PackParam(12)   // fallback (no maps): shadow darkness
#define P_FB_WARMTH       PackParam(13)   // fallback: shadow warmth
#define P_FB_FACE_WIDTH   PackParam(14)   // fallback: face half width in model units

bool HasMap(uint i) { return PackTexSize(i).x > 1u; }   // a missing map is a 1 x 1 white texture

// ---- maps --------------------------------------------------------------------------------------------------------------

// Star Rail: warm = day, cool = night; one of the two alone is used as it is
float3 SrRampColour(uint warm, uint cool, uint row, float u) {
    const bool hasWarm = HasMap(warm);
    const bool hasCool = HasMap(cool);
    const uint a = hasWarm ? warm : cool;
    const uint b = hasCool ? cool : a;
    float v = SrRampV(row, PackTexSize(a).y);
    float3 ca = PackSampleTexLevel(a, float2(u, v), 0).rgb;
    float3 cb = PackSampleTexLevel(b, float2(u, v), 0).rgb;
    return lerp(ca, cb, hasWarm && hasCool ? P_NIGHT : 0.0);
}

bool HasGenshinRamp(bool hair) { return HasMap(hair ? T_HAIR_RAMP : T_BODY_RAMP); }
bool HasSrRamp(bool hair) { return HasMap(hair ? T_HAIR_WARM : T_BODY_WARM) || HasMap(hair ? T_HAIR_COOL : T_BODY_COOL); }
bool HasRamp(bool hair) { return HasGenshinRamp(hair) || HasSrRamp(hair); }

float3 RampColour(uint tex, uint row, float u) {
    float v = (row * 2.0 + 1.0) / 20.0;
    float3 day = PackSampleTexLevel(tex, float2(u, v), 0).rgb;
    float3 night = PackSampleTexLevel(tex, float2(u, v + 0.5), 0).rgb;
    return lerp(day, night, P_NIGHT);   // linear (sRGB ramp)
}

// Genshin row (light map alpha in 5 steps) or Star Rail row (8 steps) of the part's ramp
float3 PartRamp(bool hair, bool sr, float alpha, float u) {
    if (!sr) return RampColour(hair ? T_HAIR_RAMP : T_BODY_RAMP, MaterialRow(alpha), u);
    return SrRampColour(hair ? T_HAIR_WARM : T_BODY_WARM, hair ? T_HAIR_COOL : T_BODY_COOL, SrMaterialRow(alpha), u);
}

// Face light from the SDF: the light is projected into the head's horizontal plane; the SDF holds, per pixel, the light
// angle at which it turns lit, for a light from one side; the other side reads the map mirrored.
float FaceSdfLight(PackSurface s) {
    float3 F = s.headForward - s.headUp * dot(s.headForward, s.headUp);
    F = dot(F, F) > 1e-6 ? normalize(F) : float3(0, 0, -1);
    float3 L = s.L - s.headUp * dot(s.L, s.headUp);
    L = dot(L, L) > 1e-6 ? normalize(L) : F;
    float rdl = dot(s.headRight, L);
    bool mirror = (rdl > 0.0) != (P_FACE_FLIP > 0.5);
    float sdf = PackSampleTexLevel(T_FACE_SDF, mirror ? float2(1.0 - s.uv.x, s.uv.y) : s.uv, 0).r;
    float threshold = 0.5 - 0.5 * dot(F, L);   // 0 light in front .. 1 light behind
    return smoothstep(threshold - P_FACE_SOFTNESS, threshold + P_FACE_SOFTNESS, sdf);
}

// ---- fallback (hoyo_toon v1) -----------------------------------------------------------------------------------------

// Uses the head-relative cylindrical horizontal normal so frontal light leaves the face clean,
// and side light sweeps a clean terminator across the face without vertical planar artifacts.
float FaceGeometricLight(PackSurface s) {
    if (!s.hasHead) return 1.0;
    return HoyoFaceGeometricLight(s.worldPos, s.headPos, s.headScale, s.headRight, s.headUp, s.headForward, s.L, P_FB_FACE_WIDTH, P_FACE_SOFTNESS);
}

float3 FallbackShadowTint(float extraWarmth) {   // linear multiplier for the shadow side
    float3 toonDark = SrgbToLinear(PackSampleToon(1.0));
    float3 warm = HoyoV2WarmFactor(extraWarmth, P_FB_WARMTH);
    return lerp(float3(1, 1, 1), toonDark * warm, P_FB_DARKNESS);
}

// ---- shading -------------------------------------------------------------------------------------------------------------

PackResult PackShade(PackSurface s) {
    PackResult r;
    r.alpha = s.alpha;
    r.reflectivity = 0.0;
    r.noAo = false;

    // lit colour = the MMD colour model (material diffuse / ambient * light * texture), as the model's author tuned it
    float3 albedo = SrgbToLinear(saturate(s.tex.rgb));
    float3 base = SrgbToLinear(saturate(PackMmdLit(s)));
    const uint sphereMode = PackSphereMode();
    if (sphereMode == 1u) {
        float3 sp = SrgbToLinear(PackSampleSphere(s.N));
        albedo *= sp;
        base *= sp;
    }
    float3 light = SrgbToLinear(gLightColor);

    float lit;               // 0 shadow .. 1 lit
    float3 shadowTint;       // linear multiplier on the shadow side
    float3 extra = 0;        // specular / metal, linear
    float flatFill = 0.0;
    const bool face = s.materialClass == PACK_FACE;
    const bool eye = s.materialClass == PACK_EYE;

    if (face || eye) {
        r.noAo = true;
        flatFill = 1.0;
        float faceLit = HasMap(T_FACE_SDF) ? FaceSdfLight(s) : FaceGeometricLight(s);
        lit = faceLit * lerp(1.0, s.shadow, P_FACE_CAST);
        if (eye) lit = lerp(lit, 1.0, 0.6);   // eyes stay readable in shadow
        // the skin row of the body ramp is the face's shadow tone
        const bool bodySr = !HasGenshinRamp(false);
        shadowTint = HasRamp(false) ? PartRamp(false, bodySr, bodySr ? 0.0 : 1.0, 0.5) : FallbackShadowTint(0.15);
    } else {
        const bool hair = s.materialClass == PACK_HAIR;
        // Star Rail part: its ramp and (body) light map stand in for the Genshin ones that are missing
        const bool srLm = !hair && !HasMap(T_BODY_LM) && HasMap(T_BODY_LM_SR);
        const bool sr = srLm || (!HasGenshinRamp(hair) && HasSrRamp(hair));
        const uint lmTex = hair ? T_HAIR_LM : (srLm ? T_BODY_LM_SR : T_BODY_LM);
        float halfLambert = dot(s.N, s.L) * 0.5 + 0.5;
        if (HasMap(lmTex)) {
            float4 lm = PackSampleTex(lmTex, s.uv);
            // painted shadow bias: G = 0.5 leaves the half-Lambert alone, lower pulls toward shadow, 0 / 1 are fixed
            float factor = halfLambert * saturate(lm.g * 2.0 * (sr ? kSrBiasScale : 1.0));
            lit = smoothstep(0.5 - P_SOFTNESS, 0.5 + P_SOFTNESS, factor);
            if (lm.g < 0.05) lit = 0.0;
            if (lm.g > 0.95) lit = 1.0;
            lit *= s.shadow;
            float u = saturate(factor * 2.0) * 0.9;   // the ramp's shadow part: darker toward the core
            shadowTint = HasRamp(hair) ? PartRamp(hair, sr, lm.a, u) : FallbackShadowTint(0.0);
            float3 h = normalize(s.L + s.V);
            float ndh = saturate(dot(s.N, h));
            if (!sr && lm.r > 0.9 && HasMap(T_METAL)) {
                // metal: matcap from the view-space normal, tinted by the albedo
                float3 nv = normalize(mul(s.N, (float3x3)gView));
                float m = PackSampleTexLevel(T_METAL, nv.xy * float2(0.5, -0.5) + 0.5, 0).r;
                extra += albedo * light * lerp(0.0, 1.6, m) * P_METAL * lerp(0.5, 1.0, lit);
                extra += albedo * light * pow(ndh, P_SHININESS * 4.0) * P_SPECULAR;
            } else {
                extra += albedo * light * step(1.0 - lm.b, pow(ndh, P_SHININESS)) * lm.r * P_SPECULAR * lit;
            }
        } else {
            lit = smoothstep(0.5 - P_SOFTNESS, 0.5 + P_SOFTNESS, halfLambert) * s.shadow;
            shadowTint = FallbackShadowTint(s.materialClass == PACK_SKIN ? 0.2 : 0.0);
        }
    }

    float3 c = base * lerp(shadowTint, float3(1, 1, 1), lit) + extra;
    if (eye) c *= P_EYE_BRIGHTNESS;
    float3 color = c * gSunIntensity;
    // flat fill: one ambient colour (no sky / ground gradient), so it adds brightness, not shape
    color += albedo * lerp(gGroundColor, gSkyZenith, 0.65) * gSunIntensity * gHemiStrength * P_AMBIENT;
    color += albedo * PunctualDiffuse(s.worldPos, s.N, 1.0, flatFill);

    // rim light: a thin hard band on the silhouette, light side, not on the face
    if (!face && !eye) {
        float fres = 1.0 - saturate(dot(s.N, s.V));
        float rim = smoothstep(1.0 - P_RIM_WIDTH, 1.0 - P_RIM_WIDTH + 0.04, fres);
        float lightSide = smoothstep(-0.1, 0.3, dot(s.N, s.L) + 0.2);
        color += gRimColor * rim * lightSide * s.shadow * P_RIM_STRENGTH * gSunIntensity * albedo * 2.0;
    }

    r.color = color;
    r.reflectivity = r.noAo ? 0.0 : PackEdgeReflectivity();
    return r;
}
