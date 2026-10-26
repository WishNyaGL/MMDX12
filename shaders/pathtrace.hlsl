// Unidirectional path tracer (inline ray queries): explicit sun next-event estimation,
// punctual-light NEE, analytic studio floor, stochastic alpha-tested transparency.
// u0 light (divided by albedo on surfaces), u1/u2/u3/u4 primary G-buffer for denoising.
#include "rt_common.hlsli"   // includes common.hlsli
#include "light_pick.hlsli"   // S7a: the punctual-light CDF pick
#define PBR_SAMPLE_LEVEL0     // compute: no screen-space gradients
#include "pt_pbr.hlsli"
#include "emissive_nee.hlsli"   // S8a: emissive-triangle NEE + MIS (needs the PBR record decode)
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);
SamplerState gLinearWrap : register(s2);
#ifdef MMDX_PT_PACK
#include "pt_pack_api.hlsli"
#include "pt_pack_glue.hlsli"
#include MMDX_PT_PACK
#endif

RWTexture2D<float4> gLightOut : register(u0);
RWTexture2D<float4> gAlbedoOut : register(u1);
RWTexture2D<float4> gNormalOut : register(u2);
RWTexture2D<float2> gVelocityOut : register(u3);
RWTexture2D<float> gDepthOut : register(u4);
// DLSS Ray Reconstruction guides (only written when gP2.x > 0.5; bound to dummies otherwise).
RWTexture2D<float4> gDiffuseAlbedoOut : register(u5);
RWTexture2D<float4> gSpecularAlbedoOut : register(u6);
RWTexture2D<float4> gGuideNormalOut : register(u7);   // rgb = world normal, a = roughness (packed roughness mode)
RWTexture2D<uint> gIdOut : register(u8);

// Average specular reflectance for the RR guide (DLSS-RR guide appendix, EnvBRDFApprox2).
float3 RrSpecularAlbedo(float3 specColor, float alpha, float NoV) {
    NoV = abs(NoV);
    float4 X = float4(1.0, NoV, NoV * NoV, NoV * NoV * NoV);
    float4 Y = float4(1.0, alpha, alpha * alpha, alpha * alpha * alpha);
    float2x2 M1 = float2x2(0.99044, -1.28514, 1.29678, -0.755907);
    float3x3 M2 = float3x3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627);
    float2x2 M3 = float2x2(0.0365463, 3.32707, 9.0632, -9.04756);
    float3x3 M4 = float3x3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123);
    float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
    float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
    bias *= saturate(specColor.g * 50.0);
    return mad(specColor, max(0.0, scale), max(0.0, bias));
}

struct PtLight {
    float3 pos; float invRange;
    float3 color; float cosOuter;
    float3 dir; float cosInner;
    float shadowSlice; float shadowType; float shadowSoftness; float shadowDensity;
    float3 shadowColor; float falloff;
    float affectDiffuse; float affectSpecular; float pointShadowSlice; float pad;
    float areaWidth; float areaHeight; float isArea; float pad2;
};
StructuredBuffer<PtLight> gPtLights : register(t2, space1);

static const float kSunCosMax = 0.99996;          // ~0.5 degree sun radius
static const float kSkyScale = 1.0;               // sky radiance multiplier for indirect rays
static const float kFloorExtent = 900.0;          // same as mmd.hlsl
// MMD's ambient colour is a self-illumination term (raster: saturate(ambient + diffuse * light));
// part of it is emitted so path-traced scenes keep the familiar brightness.
static const float kAmbientEmission = 0.5;

float3 SunIrradiance() { return SrgbToLinear(gLightColor) * (PI * 3.0) * gSunIntensity; }

// Character materials keep the raster toon model (mmd.hlsl PSMain) for the sun: a physical
// Lambert term models faces into 3D shading, which MMD rigs are not authored for. The sun
// visibility `sh` comes from a traced shadow ray instead of the shadow map. Linear output.
float3 ToonSun(RtGeometry g, float4 tex, float2 uv, float3 n, float3 V, float rawSh) {
    float3 L = -gLightDir;
    float3 lit = saturate(g.ambient + g.diffuse.rgb * gLightColor) * tex.rgb;
    if (g.flags & (MAT_SPHERE_MUL | MAT_SPHERE_ADD)) {
        float3 nv = normalize(mul(n, (float3x3)gView));
        float2 suv = nv.xy * float2(0.5, -0.5) + 0.5;
        float3 sp = ApplyTexFactor3(gBindlessTex[NonUniformResourceIndex(g.sphereSrv)].SampleLevel(gLinear, suv, 0).rgb, g.sphereMul, g.sphereAdd, (g.flags & MAT_SPHERE_MUL) ? 1.0 : 0.0);
        if (g.flags & MAT_SPHERE_MUL) lit *= sp; else lit += sp;
    }
    float occ = (1.0 - rawSh) * gSunShadowParams.z;
    float sh = 1.0 - occ;
    float ndl = dot(n, L);
    float flat = (g.flags & MAT_FLAT) ? 1.0 : 0.0;
    float3 c;
    if (g.flags & MAT_TOON_MAP) {
        // Project Sekai layout: the "toon" is the painted shadow colour at the same UV.
        float3 shadowTex = ApplyTexFactor3(gBindlessTex[NonUniformResourceIndex(g.toonSrv)].SampleLevel(gRtWrap, uv, 0).rgb, g.toonMul, g.toonAdd, 1.0);
        float term = sh * lerp(smoothstep(-0.03, 0.06, ndl), 1.0, flat);
        c = lerp(saturate(g.ambient + g.diffuse.rgb * gLightColor) * shadowTex, lit, term);
    } else if (g.flags & MAT_HAS_TOON) {
        float v = lerp(1.0, saturate(0.5 - 0.5 * ndl), sh);
        c = lit * ApplyTexFactor3(gBindlessTex[NonUniformResourceIndex(g.toonSrv)].SampleLevel(gLinear, float2(0.5, v), 0).rgb, g.toonMul, g.toonAdd, 1.0);
    } else {
        c = lit;
    }
    if (!(g.flags & MAT_TOON_MAP)) {
        float term = lerp(sh * smoothstep(-0.12, 0.22, ndl), lerp(1.0, sh, 0.8), flat);
        float3 shade = c * lerp(1.0, saturate(c), 0.55) * float3(0.90, 0.90, 0.96);
        c = lerp(shade, c, term);
    }
    c += lit * gSunShadowColor.rgb * occ;
    if (g.specularPower > 0.0)
        c += pow(saturate(dot(normalize(L + V), n)), g.specularPower) * g.specular * gLightColor * sh;
    float3 color = SrgbToLinear(saturate(c)) * gSunIntensity;
    float rim = pow(1.0 - saturate(dot(n, V)), 4.0);
    float side = smoothstep(-0.2, 0.5, ndl + 0.25);
    color += gRimColor * rim * side * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
    return color;
}

// PBR direct at a hit (mirrors mmd.hlsl MmdShadePbr's sun structure; linear, traced visibility `sh`).
// CEL/TONEBASED keep the MMD shading with the PBR mapped normal; the MMD Blinn specular is replaced by GGX.
// Emissive is added by the caller (once per hit).
float3 PbrSunDirect(PtPbrSurface p, RtGeometry g, float4 tex, float2 uv, float3 V, float sh, float flat) {
    float3 L = -gLightDir;
    float3 n = p.N;
    float ndl = dot(n, L);
    float occ = (1.0 - sh) * gSunShadowParams.z;
    float3 mddLight = saturate(g.ambient + g.diffuse.rgb * gLightColor);
    float3 sunLin = SrgbToLinear(saturate(gLightColor)) * gSunIntensity;

    // TONEBASED (9): MMD shading replaced by a hardness toon toward the customB shadow colour.
    if (p.model == 9u) {
        float hard = saturate(p.customA);
        float litAmt = saturate(ndl * hard + 1.0 - hard) * sh;
        float3 c9 = SrgbToLinear(saturate(mddLight * tex.rgb)) * lerp(p.customB, 1.0, litAmt) * gSunIntensity;
        float3 rim9 = pow(1.0 - saturate(dot(n, V)), 4.0) * smoothstep(-0.2, 0.5, dot(n, L) + 0.25);
        c9 += gRimColor * rim9 * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
        return c9;
    }

    float3 cAlbedo = LinearToSrgb(saturate(p.diffuse));   // gamma, for the MMD diffuse term (metal removed)
    float3 c = mddLight * cAlbedo;
    if (p.model == 8u) {   // CEL: the toon ramp replaces the PBR diffuse colour
        float v = lerp(1.0, saturate(0.5 - 0.5 * ndl), sh);
        c = mddLight * tex.rgb * ApplyTexFactor3(gBindlessTex[NonUniformResourceIndex(g.toonSrv)].SampleLevel(gLinear, float2(0.5, v), 0).rgb, g.toonMul, g.toonAdd, 1.0);
    } else {
        float term = lerp(sh * smoothstep(-0.12, 0.22, ndl), lerp(1.0, sh, 0.8), flat);
        float3 shade = c * lerp(1.0, saturate(c), 0.55) * float3(0.90, 0.90, 0.96);
        c = lerp(shade, c, term);
    }
    c += mddLight * cAlbedo * gSunShadowColor.rgb * occ;
    float3 color = SrgbToLinear(saturate(c)) * gSunIntensity;

    if (p.model == 1u) {   // SKIN: soft SSS tint (customA curvature, customB transmittance)
        float ss = pow(1.0 - saturate(ndl), 2.0) * pow(1.0 - saturate(-ndl), 3.0 / (p.customA + 0.001)) *
                   saturate(p.customA - 0.04);
        color += p.customB * (ss * 0.7) * sunLin * sh;
    }
    if (p.specOn) {   // MMD Blinn replaced by GGX (per-model lobe), shadowed by the same sun visibility
        float3 spec = PtPbrEvalSpecular(p, V, L);
        color += spec * sh * sunLin * saturate(ndl) * p.specScale;
    }
    float rim = pow(1.0 - saturate(dot(n, V)), 4.0);
    float side = smoothstep(-0.2, 0.5, dot(n, L) + 0.25);
    float3 rimTint = lerp(float3(1, 1, 1), dot(p.f0, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)).xxx, p.metal);
    return color + gRimColor * rimTint * rim * side * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
}

[numthreads(8, 8, 1)]
void CSPathTrace(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    gLightOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 pix = id.xy;

    uint nl = max((uint)gNumLights, 1u);   // one light is always sampled when lights exist
    uint sampleCount = max((uint)gP0.x, 1u);
    uint bounces = (uint)gP0.y;
    float3 radiance = 0;
    float3 primAlbedo = 0, primNormal = 0, primPos = 0, primPrev = 0, primRd = 0;
    float primRefl = 0, primRough = 0.5, primCosV = 1.0;
    uint primId = 0;
    bool primHit = false;
    [loop] for (uint s = 0; s < sampleCount; ++s) {
        uint rng = RngSeed(pix, (uint)gP1.x, s);
        float2 sub = (s == 0) ? float2(0.5, 0.5) : float2(Rand(rng), Rand(rng));
        float2 uv = (pix + sub) * gInvViewportSize;
        float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
        float4 v = mul(float4(ndc, 1.0, 1.0), gInvProj);
        float3 viewDir = normalize(v.xyz / v.w);
        float3 rd = normalize(mul(viewDir, (float3x3)gInvView));
        float3 ro = gEyePos;
        // gInvProj is the jittered projection: the jitter is already inside, do not add any.

        float3 throughput = 1.0;
        float3 dir = rd, origin = ro;
        bool primarySky = false;
        // S8a: MIS across bounces -- the pdf of the direction the previous vertex's BSDF sampled (0 / invalid at the
        // camera and after a delta event: those emitters keep weight 1) and where it sampled it.
        float prevPdfBsdf = 0.0;
        float3 prevBsdfPos = 0;
        bool prevBsdfValid = false;
#ifdef MMDX_PT_PACK
        bool diffuseChain = false;
#endif

        [loop] for (uint bounce = 0; bounce <= bounces; ++bounce) {
            float tMin = (bounce == 0) ? gNearZ : 0.0;
            float tMax = 1e5;

            // analytic studio floor plane y = 0
            float tp = 0;
            bool floorHit = false;
            if (gP0.z > 0.5 && dir.y < -1e-5) {
                float t = -origin.y / dir.y;
                if (t > tMin && length((origin + dir * t).xz) < kFloorExtent) {
                    tp = t;
                    floorHit = true;
                    tMax = t;
                }
            }

            float alphaThr = max(Rand(rng), 0.004);   // stochastic transparency
            RtHit hit;
            bool hitMesh = TraceClosest(origin, dir, tMin, tMax, alphaThr, hit);
            if (!hitMesh && !floorHit) {
                radiance += throughput * SkyColor(dir) * ((bounce == 0) ? 1.0 : kSkyScale);
                if (bounce == 0 && s == 0) { primarySky = true; primRd = rd; }
                break;
            }

            float3 pos, prevPos, n, faceN, albedo;
            float refl, rough;
            bool receive;
            bool backFace = false;   // S2b: the ray hit the back face of a (transmissive) mesh
            bool toon = false;    // character material: raster toon sun, no Lambert NEE
            float flat = 0.0;     // MAT_FLAT (faces): even fill instead of traced indirect
            PtPbrSurface pbr;     // decoded PBR record (used when isPbrHit)
            bool isPbrHit = false;
            // S8a: the hit geometry / triangle (MIS looks the triangle's light pdf up), the hit's emission terms, and
            // whether the emission arrived through a BSDF-sampled direction (depth > 0, no delta bounce).
            uint hitGeom = 0xFFFFFFFFu, hitPrim = 0xFFFFFFFFu;
            float3 hitTexLin = 0.0;
            float3 emission = 0.0;   // the hit's own emission, weighted by MIS below
#ifdef PT_PACK_HAS_LIGHTING
            float flatIndirect = 0.0;   // pack's flat indirect requesting (falls back to flat)
#endif
            uint hitId = 0;
            if (hitMesh) {
                RtGeometry g = LoadGeometry(hit.instanceId, hit.geometryIndex);
                uint modelIdx = (g.flags >> 24) & 0xFFu;
                uint isChar = (g.flags & RTG_CHARACTER) ? 0x80u : 0u;
                uint packClass = 7u;
                uint tag = 0u;
                RtSurface sf = FetchSurface(g, hit.prim, hit.bary);
                hitGeom = hit.instanceId + hit.geometryIndex;   // index into the RtGeometry table
                hitPrim = hit.prim;
                backFace = dot(sf.faceNormal, dir) > 0.0;   // S2b: inside a transmissive body
                if (backFace) sf.faceNormal = -sf.faceNormal;
                if (dot(sf.normal, sf.faceNormal) < 0) sf.normal = -sf.normal;
                float4 tex = SampleBaseTexture(g, sf.uv, bounce == 0 ? 0.0 : 2.0);
                hitTexLin = SrgbToLinear(saturate(tex.rgb));
                albedo = SrgbToLinear(saturate(MaterialAlbedo(g, tex.rgb)));
                float r = g.reflectivity;
                if (g.flags & MAT_STAGE)
                    r = saturate(r + gFloorGloss * smoothstep(0.82, 0.97, sf.normal.y));
                refl = r;
                rough = clamp(sqrt(2.0 / (g.specularPower + 2.0)), 0.03, 0.6);
                receive = (g.flags & MAT_RECEIVE) != 0;
                pos = sf.pos; prevPos = sf.prevPos; n = sf.normal; faceN = sf.faceNormal;
                // S5: a stage screen surface shows its texture as emission, black behind it (no diffuse / specular / AO).
                const bool screenMat = (g.flags & MAT_SCREEN) != 0;
                if (screenMat) {
                    emission += EmissionAt(g, hitTexLin, 0.0, false);
                    albedo = 0.0;
                    refl = 0.0;
                }
                bool canPbr = !screenMat && (g.pbrIndex != 0xFFFFFFFFu);
#ifdef MMDX_PT_PACK
                // a PT pack wins over the record (raster rule): keep the plain surface the pack expects
                if (canPbr && (g.flags & RTG_PT_PACK) && LoadPtPackRecord(g.packSrv).materialClass != PACK_CLASS_OFF)
                    canPbr = false;
#endif
                if (canPbr) {   // PBR record: mapped normal, albedo, GGX, AO, emissive, reflectivity
                    pbr = PtPbrDecode(hit.prim, g, sf, sf.normal, g.diffuse.rgb);
                    n = pbr.N;
                    albedo = pbr.baseColor;
                    refl = saturate(Luminance(pbr.f0) * (1.0 - pbr.rough));
                    rough = pbr.rough;
                    isPbrHit = true;
                }
                if (g.flags & MAT_STAGE) {
                    if (!screenMat) {
                        radiance += throughput * SrgbToLinear(saturate(g.ambient * tex.rgb)) * gSunIntensity * kAmbientEmission;
                        emission += EmissionAt(g, hitTexLin, 0.0, false);
                    }
                    // S2a: a stage material's PBR emissive (KHR_materials_emissive_strength, HDR) is visible to a
                    // camera hit here too, like offline GI's CameraDirect; a stage's PBR sun term stays out (the
                    // stage path below is ambient-only).
                    if (isPbrHit) emission += EmissionAt(g, hitTexLin, pbr.emissive, true);
                } else {
#ifdef MMDX_PT_PACK
                    bool isPtPackHit = (g.flags & RTG_PT_PACK) != 0;
                    if (isPtPackHit) {
                        RtPtPackRecord rec = LoadPtPackRecord(g.packSrv);
                        if (rec.materialClass != PACK_CLASS_OFF) {
                            packClass = rec.materialClass & 0x7u;
                            isPbrHit = false;   // the pack wins the PBR record (raster rule)
                            PtPackBindRecord(rec);
                            PtPackBindGeometry(g);
                            PtPackIn packIn;
                            packIn.pos = pos;
                            packIn.normal = n;
                            packIn.V = -dir;
                            packIn.uv = sf.uv;
                            packIn.uv1 = sf.uv1;
                            packIn.uv2 = sf.uv2;
                            packIn.L = -gLightDir;
                            float sh = 1.0;
                            if (!diffuseChain) {
                                if (receive && gShadowParams.w < 0.5) {
                                    float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), -gLightDir, SunShadowConeCos(kSunCosMax));
                                    sh = Luminance(saturate(TraceShadowRayTinted(OffsetRayOrigin(pos, faceN), sd,
                                                                                  1e5, 0xFFu)));
                                }
                                packIn.sunVis = sh;
                            } else {
                                packIn.sunVis = 0.0;
                            }
                            packIn.baseColor = SrgbToLinear(saturate(tex.rgb * g.diffuse.rgb));
                            packIn.materialClass = rec.materialClass;
                            [unroll] for (int p = 0; p < 16; ++p)
                                packIn.params[p] = rec.params[p >> 2][p & 3];
                            packIn.headRight = rec.headRight.xyz;
                            packIn.headPos = rec.headPos.xyz;
                            packIn.headScale = rec.headPos.w;
                            packIn.headUp = rec.headUp.xyz;
                            packIn.headForward = rec.headForward.xyz;
                            packIn.headValid = (rec.headValid != 0);

                            PtPackOut ptPackOut = PackEvaluate(packIn);
                            albedo = ptPackOut.albedo;
#if defined(PACK_CAP_SURFACE_OUTPUT) && PACK_CAP_SURFACE_OUTPUT
                            tag = ptPackOut.tag & 0xFu;
                            n = normalize(ptPackOut.normal);
                            radiance += throughput * ptPackOut.emission;
#endif
                            // the pack decides: MAT_FLAT (a material without toon) would drop its terminator
                            flat = ptPackOut.flatFace ? 1.0 : 0.0;
#ifdef PT_PACK_HAS_LIGHTING
                            PtPackLighting ptLgt = PackEvaluateLighting(packIn, ptPackOut);
                            flatIndirect = (ptLgt.flatIndirect > 0.5) ? 1.0 : flat;   // keep the MAT_FLAT bounce stop for flat packs

                            if (!diffuseChain) {
                                toon = true;
                                radiance += throughput * PtPackComposeSunDirect(ptPackOut, ptLgt, n, sh, flat);
                                if (ptLgt.flatIndirect > 0.5) {
                                    // raster flat fill: no sky/ground gradient and no occlusion modelling the face
                                    float3 fill = lerp(gGroundColor, gSkyZenith, 0.65) * gSunIntensity * gHemiStrength;
                                    radiance += throughput * albedo * fill;
                                }
                            } else {
                                toon = false;
                            }
#else
                            if (!diffuseChain) {
                                toon = true;
                                radiance += throughput * PtPackComposeSunDirect(ptPackOut, n, sh, flat > 0.5);
                                if (flat > 0.5) {
                                    // raster flat fill: no sky/ground gradient and no occlusion modelling the face
                                    float3 fill = lerp(gGroundColor, gSkyZenith, 0.65) * gSunIntensity * gHemiStrength;
                                    radiance += throughput * albedo * fill;
                                }
                            } else {
                                toon = false;
                            }
#endif
                        } else {
                            isPtPackHit = false;
                        }
                    }
                    if (!isPtPackHit)
#endif
                    {
                        if (isPbrHit) {
                            // ---- PBR record: mirror MmdShadePbr's sun structure; GGX for punctual NEE ----
                            toon = true;
                            flat = (g.flags & MAT_FLAT) ? 1.0 : 0.0;
#ifdef PT_PACK_HAS_LIGHTING
                            flatIndirect = flat;
#endif
                            float sh = 1.0;
                            if (receive && gShadowParams.w < 0.5 && gSunShadowParams.x > 0.5) {
                                float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), -gLightDir, SunShadowConeCos(kSunCosMax));
                                sh = Luminance(saturate(TraceShadowRayTinted(OffsetRayOrigin(pos, faceN), sd, 1e5,
                                                                              0xFFu)));
                            }
                            radiance += throughput * PbrSunDirect(pbr, g, tex, sf.uv, -dir, sh, flat);
                            emission += EmissionAt(g, hitTexLin, pbr.emissive, true);
                            if (flat > 0.5) {
                                // raster flat fill: no sky/ground gradient and no occlusion modelling the face
                                float3 fill = lerp(gGroundColor, gSkyZenith, 0.65) * gSunIntensity * gHemiStrength;
                                radiance += throughput * albedo * fill;
                            }
                        } else {
                            toon = true;
                            flat = (g.flags & MAT_FLAT) ? 1.0 : 0.0;
#ifdef PT_PACK_HAS_LIGHTING
                            flatIndirect = flat;
#endif
                            float sh = 1.0;
                            if (receive && gShadowParams.w < 0.5 && gSunShadowParams.x > 0.5) {
                                float cosMax = SunShadowConeCos(kSunCosMax);
                                float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), -gLightDir, cosMax);
                                sh = Luminance(saturate(TraceShadowRayTinted(OffsetRayOrigin(pos, faceN), sd, 1e5,
                                                                              0xFFu)));
                            }
                            radiance += throughput * ToonSun(g, tex, sf.uv, n, -dir, sh);
                            // AutoLuminous (そぼろ): marked (non-PBR, non-pack) materials emit their colour.
                            emission += EmissionAt(g, hitTexLin, 0.0, false);
#ifdef PT_PACK_HAS_LIGHTING
                            if (flatIndirect > 0.5) {
#else
                            if (flat > 0.5) {
#endif
                                // raster flat fill: no sky/ground gradient and no occlusion modelling the face
                                float3 fill = lerp(gGroundColor, gSkyZenith, 0.65) * gSunIntensity * gHemiStrength;
                                radiance += throughput * albedo * fill;
                            }
                        }
                    }
                }
                hitId = (tag & 0xFu) | ((packClass & 0x7u) << 4) | isChar | (modelIdx << 8);
            } else {
                pos = origin + dir * tp;
                prevPos = pos;
                faceN = float3(0, 1, 0);
                n = faceN;
                float r = length(pos.xz);
                albedo = lerp(float3(0.80, 0.83, 0.86), gSkyHorizon * 0.95, smoothstep(40.0, 420.0, r));
                refl = 0.42 * (1.0 - smoothstep(120.0, 600.0, r));
                rough = 0.12;
                receive = true;
            }

            // S8a: the hit's own emission (a frame without emitters adds exactly 0: the value is unchanged).
            if (any(emission > 0.0)) {
                // MIS against the light sampling: the NEE estimate for this triangle was added at the previous vertex,
                // so an emission that arrived through a BSDF-sampled direction at depth >= 1 takes the BSDF weight;
                // camera hits (depth 0) and delta events (the transmissive branch below) keep weight 1.
                // A back-face hit of a double-sided emitter keeps weight 1: the NEE only samples the front side.
                if (EmissiveOn() && prevBsdfValid && hitMesh && !backFace && EmissiveBase(hitGeom) != 0xFFFFFFFFu) {
                    const float pdfLight = EmissiveHitPdf(hitGeom, hitPrim, pos, prevBsdfPos);
                    if (pdfLight > 0.0) emission *= MisWeightBsdf(prevPdfBsdf, pdfLight);
                }
                radiance += throughput * emission;
            }

            // S2b mesh dielectric: a transmissive PBR hit continues with the transmission event (probability =
            // the transmission factor) or the ordinary surface BSDF below. The event is the analytic glass cube's
            // dielectric step with the material's own ior: thin-walled goes straight through (Fresnel still applies),
            // a volume refracts and absorbs over the chord it travelled inside (Beer-Lambert). Choosing one event and
            // reporting its contribution is what the primary G-buffer sees, so the denoiser demodulates the glass and
            // not the surface behind it.
            // The roulette value is derived from the current state without advancing the path's stream, so a scene
            // without transmissive materials consumes exactly the random numbers it always did.
            if (isPbrHit && PtPbrIsTransmissive(pbr) && RandValue(rng) < saturate(pbr.transmission)) {
                PtPbrTransmission tx = PtPbrTransmit(pbr, dir, faceN, backFace, hit.t,
                                                     float2(Rand(rng), Rand(rng)));
                // the Fresnel split is the roulette inside the event: a reflection keeps the full weight, a
                // refraction carries the base colour tint (glTF) and the absorption of the volume it leaves
                throughput *= tx.refracted ? tx.tint * tx.absorb : tx.absorb;
                if (bounce == 0 && s == 0) {   // the denoiser demodulates the interface, not what is behind it
                    primAlbedo = max(pbr.baseColor, 0.02);
                    primNormal = n;
                    primRefl = saturate(Luminance(pbr.f0) + 0.04);
                    primRough = max(pbr.rough, 0.03);
                    primCosV = saturate(dot(n, -dir));
                    primPos = pos;
                    primPrev = prevPos;
                    primHit = true;
                    primId = hitId;
                }
                if (bounce == bounces) break;
                prevBsdfValid = false;   // S8a: a delta event: an emitter found through it keeps weight 1
                dir = tx.dir;
                origin = OffsetRayOrigin(pos, tx.refracted ? -faceN : faceN);
                if (bounce >= 2) {
                    float q = min(max(throughput.r, max(throughput.g, throughput.b)), 0.95);
                    if (Rand(rng) > q) break;
                    throughput /= q;
                }
                continue;
            }
            if (bounce == 0 && s == 0) {
                primAlbedo = albedo;
                primNormal = n;
                primRefl = refl;
                primRough = rough;
                primCosV = saturate(dot(n, -dir));
                primPos = pos;
                primPrev = prevPos;
                primHit = true;
                primId = hitId;
            }

            // Fresnel-weighted specular probability (PBR: Schlick F(N.V) luminance vs the diffuse weight)
            float cosV = saturate(dot(n, -dir));
            float pSpec = refl > 0.001 ? saturate(refl + (1.0 - refl) * pow(1.0 - cosV, 5.0) * refl) : 0.0;
            if (isPbrHit) pSpec = PtPbrSpecProb(pbr, -dir);

            // sun next-event estimation (diffuse lobe)
            float3 L = -gLightDir;
            float ndl = dot(n, L);
            if (!toon && ndl > 0.0 && dot(faceN, L) > 0.0) {
                float cosMax = SunShadowConeCos(kSunCosMax);
                float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), L, cosMax);
                // S2b: a transmissive mesh tints the sun where an opaque caster would stop it
                float3 rawVis = receive && gShadowParams.w < 0.5 && gSunShadowParams.x > 0.5
                                    ? TraceShadowRayTinted(OffsetRayOrigin(pos, faceN), sd, 1e5, 0xFFu)
                                    : float3(1, 1, 1);
                float occ = (1.0 - Luminance(saturate(rawVis))) * gSunShadowParams.z;
                float3 visFactor = (1.0 - occ) + gSunShadowColor.rgb * occ;
                radiance += throughput * (1.0 - pSpec) * (isPbrHit ? pbr.diffuse : albedo) / PI * SunIrradiance() * ndl * visFactor;
            }

            // punctual-light NEE (one light per bounce: S7a power CDF when the frame has one, else uniform)
            if (gNumLights >= 1.0) {
                float pickPdf = 0.0;
                uint li = 0u;
                bool cdfPick = (int)gLightCdf.x >= 0;
                if (cdfPick) {
                    li = LightCdfPick(Rand(rng), nl, pickPdf);
                    cdfPick = pickPdf > 0.0;
                }
                if (!cdfPick) {
                    li = min((uint)(Rand(rng) * (float)nl), nl - 1u);
                    pickPdf = 1.0 / (float)nl;
                }
                const float pickWeight = 1.0 / pickPdf;   // <-> the old (float)nl for the uniform pick
                PtLight l = gPtLights[li];
                float dist;
                float3 ld;
                float atten;
                if (l.isArea > 0.5) {
                    float3 norm = l.dir;
                    float3 c = cross(float3(0, 1, 0), norm);
                    float cLen = length(c);
                    float3 right = (cLen > 1e-4) ? (c / cLen) : float3(1, 0, 0);
                    float3 up = cross(norm, right);
                    float u = (Rand(rng) - 0.5) * l.areaWidth;
                    float v = (Rand(rng) - 0.5) * l.areaHeight;
                    float3 samplePos = l.pos + right * u + up * v;
                    float3 d = samplePos - pos;
                    dist = length(d);
                    ld = d / max(dist, 1e-4);
                    atten = PunctualFalloff(dist, l.invRange, l.falloff) * max(0.0, dot(norm, -ld));
                } else {
                    float3 d = l.pos - pos;
                    dist = length(d);
                    ld = d / max(dist, 1e-4);
                    atten = PunctualFalloff(dist, l.invRange, l.falloff);
                    if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
                }

                if (atten > 0.0) {
                    float ndlp = dot(n, ld);
                    float diff = toon ? lerp(smoothstep(-0.05, 0.25, ndlp), saturate(ndlp * 0.3 + 0.7), flat) : ndlp;
                    bool evalDiff = (l.affectDiffuse > 0.0) && (diff > 0.0);

                    float3 R = reflect(dir, n);
                    float cosHalfAngle = cos(rough * 0.5);
                    float omega = max(2.0 * PI * (1.0 - cosHalfAngle), 1e-6);
                    bool evalSpec;
                    if (isPbrHit)
                        evalSpec = (l.affectSpecular > 0.5) && (pSpec > 0.0) && (dot(faceN, ld) > 0.0) && (dot(pbr.N, ld) > 0.0);
                    else
                        evalSpec = (l.affectSpecular > 0.5) && (pSpec > 0.0) && (dot(faceN, ld) > 0.0) && (dot(R, ld) >= cosHalfAngle);

                    if (evalDiff || evalSpec) {
                        float rawVis = 1.0;
                        if (l.shadowType > 0.5) {
                            float3 rayDir = ld;
                            if (l.isArea <= 0.5 && l.shadowType > 1.5) {
                                float cosCone = PunctualShadowConeCos(0.9999, l.shadowSoftness);
                                rayDir = SampleCone(float2(Rand(rng), Rand(rng)), ld, cosCone);
                            }
                            // S2b: tint through transmissive stage / prop geometry, then the character mask rule
                            rawVis = Luminance(saturate(TraceShadowRayTinted(OffsetRayOrigin(pos, faceN), rayDir,
                                                                            max(dist - 0.05, 0.0),
                                                                            RT_MASK_CHARACTER)));
                        }
                        float3 shadowFactor = ShadowTransmission(rawVis, l.shadowDensity, l.shadowColor);
                        if (evalDiff) {
                            radiance += throughput * (1.0 - pSpec) * (isPbrHit ? pbr.diffuse : albedo) * l.color * shadowFactor * atten * diff * pickWeight;
                        }
                        if (evalSpec) {
                            if (isPbrHit)   // GGX (the lobe already carries N.L)
                                radiance += throughput * pSpec * l.color * shadowFactor * atten *
                                            PtPbrEvalSpecular(pbr, -dir, ld) * pickWeight;
                            else
                                radiance += throughput * pSpec * l.color * shadowFactor * atten * (pickWeight / omega);
                        }
                    }
                }
            }

            // S8a: emissive-triangle NEE (one sample per shading point, IN ADDITION to the punctual NEE above): LED
            // screens / AutoLuminous / PBR emissive light the surface directly instead of only where a path happens to
            // hit them. MIS: the weight of this estimate against the BSDF path that could have found the same triangle.
            if (EmissiveOn() && !(isPbrHit && PtPbrIsTransmissive(pbr))) {
                EmissiveSample es;
                if (EmissiveSamplePoint(Rand(rng), float2(Rand(rng), Rand(rng)), pos, es)) {
                    const float3 wi = (es.pos - pos) / es.dist;
                    const float cosS = dot(n, wi);
                    // one-sided: only the winding side emits (the hit path culls the back face)
                    if (cosS > 0.0 && dot(faceN, wi) > 0.0 && dot(es.nLight, -wi) > 0.0) {
                        const float3 Le = EmissiveSampleRadiance(es.tri, es.uv, bounce == 0 ? 0.0 : 2.0);
                        if (any(Le > 0.0)) {
                            // visibility as the path itself would find the emitter (see EmissiveVisible)
                            const float visFactor = EmissiveVisible(OffsetRayOrigin(pos, faceN), wi, es.dist,
                                                                    max(Rand(rng), 0.004));
                            // the last vertex (and a flat face, which ends its path here) has no BSDF continuation
                            // that could find this emitter: the light sample carries all of it
#ifdef PT_PACK_HAS_LIGHTING
                            const bool lastVertex = bounce == bounces || flatIndirect > 0.5;
#else
                            const bool lastVertex = bounce == bounces || flat > 0.5;
#endif
                            const float pdfBsdf = PathBsdfPdf(isPbrHit, pbr, rough, pSpec, n, -dir, wi);
                            const float wMis = lastVertex ? 1.0 : MisWeightLight(es.pdf, pdfBsdf);
                            // the BSDF the continuation below realises (MIS: both halves estimate one integral): a
                            // record's diffuse is the full lobe x AO and its VNDF sample the full GGX lobe; the plain
                            // surface's diffuse carries (1 - pSpec), its cone pSpec / omega
                            float3 f;
                            if (isPbrHit) {
                                f = pbr.diffuse * pbr.ao / PI * cosS;
                                if (pSpec > 0.0) f += PbrSpecCosine(pbr, -dir, wi);
                            } else {
                                f = (1.0 - pSpec) * albedo / PI * cosS;
                                if (pSpec > 0.0 && dot(reflect(dir, n), wi) >= cos(rough * 0.5))
                                    f += pSpec / ConeLobeOmega(rough);
                            }
                            radiance += throughput * wMis * f * Le * visFactor / es.pdf;
                        }
                    }
                }
            }

#ifdef PT_PACK_HAS_LIGHTING
            if (bounce == bounces || flatIndirect > 0.5) break;
#else
            if (bounce == bounces || flat > 0.5) break;
#endif

            // next direction: Fresnel-weighted specular vs cosine diffuse
            float3 nextDir;
            if (isPbrHit) {
                // PBR continuation: VNDF specular vs cosine diffuse, weighted by the same pSpec.
                if (Rand(rng) < pSpec) {
                    float3 w;
                    PtPbrSampleSpecular(pbr, -dir, float2(Rand(rng), Rand(rng)), nextDir, w);
                    throughput *= w / pSpec;
                } else {
                    nextDir = CosineSampleHemisphere(float2(Rand(rng), Rand(rng)), n);
                    throughput *= pbr.diffuse * pbr.ao / (1.0 - pSpec);
#ifdef MMDX_PT_PACK
                    diffuseChain = true;
#endif
                }
            } else if (Rand(rng) < pSpec) {
                nextDir = SampleCone(float2(Rand(rng), Rand(rng)), reflect(dir, n), cos(rough * 0.5));
            } else {
                nextDir = CosineSampleHemisphere(float2(Rand(rng), Rand(rng)), n);
                throughput *= albedo;
#ifdef MMDX_PT_PACK
                diffuseChain = true;
#endif
            }
            if (dot(nextDir, faceN) <= 0.0) break;
            // S8a: the pdf of this continuation, for the MIS of an emitter the next ray happens to hit.
            prevPdfBsdf = PathBsdfPdf(isPbrHit, pbr, rough, pSpec, n, -dir, nextDir);
            prevBsdfPos = pos;
            prevBsdfValid = prevPdfBsdf > 0.0;
            dir = nextDir;
            origin = OffsetRayOrigin(pos, faceN);

            // Russian roulette from bounce >= 2
            if (bounce >= 2) {
                float q = min(max(throughput.r, max(throughput.g, throughput.b)), 0.95);
                if (Rand(rng) > q) break;
                throughput /= q;
            }
        }
    }

    radiance /= sampleCount;
    radiance = min(radiance, 64.0);   // firefly guard

    if (!primHit) {
        // sky primary (sample 0 missed)
        if (gP2.x > 0.5) {   // RR guides: a neutral grey albedo for the sky (guide requirement)
            gDiffuseAlbedoOut[pix] = float4(0.5, 0.5, 0.5, 0.0);
            gSpecularAlbedoOut[pix] = float4(0, 0, 0, 0);
            gGuideNormalOut[pix] = 0;
        }
        gAlbedoOut[pix] = float4(1, 1, 1, gP0.w > 0.5 ? 0.0 : 1.0);
        gLightOut[pix] = float4(radiance, 0.0);
        gNormalOut[pix] = 0;
        gDepthOut[pix] = 1.0;
        float4 cur = mul(float4(primRd, 0.0), gViewProjNoJitter);
        float4 prev = mul(float4(primRd, 0.0), gPrevViewProjNoJitter);
        float2 vel = (cur.xy / cur.w - prev.xy / prev.w) * float2(0.5, -0.5);
        gVelocityOut[pix] = vel;
        if (gP2.z > 0.5) gIdOut[pix] = 0u;
        return;
    }

    float3 a = max(primAlbedo, 0.02);
    gAlbedoOut[pix] = float4(a, 1.0);
    gLightOut[pix] = float4(radiance / a, 1.0);
    if (gP2.x > 0.5) {
        // RR guides: world-space shading normal, roughness, diffuse and specular albedo. The primary
        // reflectance is split into its diffuse (albedo scaled by 1 - reflectance) and specular parts.
        gDiffuseAlbedoOut[pix] = float4(saturate(a * (1.0 - primRefl)), 0.0);
        gSpecularAlbedoOut[pix] = float4(RrSpecularAlbedo(primRefl.xxx, primRough * primRough, primCosV), 0.0);
        gGuideNormalOut[pix] = float4(normalize(primNormal), primRough);
    }
    float3 vn = normalize(mul(primNormal, (float3x3)gView));
    float2 enc = OctEncode(vn);
    gNormalOut[pix] = float4(enc.x, enc.y, primRefl, 1.0);
    float4 c = mul(float4(primPos, 1.0), gViewProj);
    gDepthOut[pix] = c.z / c.w;
    float4 cur = mul(float4(primPos, 1.0), gViewProjNoJitter);
    float4 prev = mul(float4(primPrev, 1.0), gPrevViewProjNoJitter);
    float2 vel = (cur.xy / cur.w - prev.xy / prev.w) * float2(0.5, -0.5);
    gVelocityOut[pix] = vel;
    if (gP2.z > 0.5) gIdOut[pix] = primId;
}
