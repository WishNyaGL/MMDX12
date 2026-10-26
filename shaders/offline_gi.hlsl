// Offline GI renderer, part 1: irradiance cache + path tracer (see src/render/OfflineRenderer.h for
// the dispatch order). Indirect diffuse at camera hits comes from the irradiance cache built by the
// prepass (Cinema 4D style, see the cache section); everything else is path traced:
//  * Hits the cache does not cover, and every vertex after a mirror bounce, are brute force.
//  * Every vertex takes next-event estimation (soft sun through a cone sample, one punctual light
//    picked by its unshadowed contribution) and continues with a Fresnel-weighted mirror or cosine
//    diffuse bounce, up to kMaxDepth bounces with Russian roulette.
//  * Characters keep the MMD toon look: the camera-facing key light goes through the toon ramp
//    (ToonSun, as in pathtrace.hlsl) and GI adds coloured fill on top. Faces (MAT_FLAT) take an even
//    fill gathered around the view direction that ignores nearby geometry (nose, fringe).
//  * Skin (character texels with a skin tone, SkinWeight) gets subsurface scattering: sun
//    visibility diffused over per-channel scatter radii (soft, red-fringed shadow edges), light
//    leaking past the toon terminator, and translucency of thin backlit parts (ears, fingers)
//    from a traced thickness with per-channel extinction.
//  * Camera: thin lens focused on gP2.y (per-pixel lens samples) and a shutter time gP2.x shared by
//    the whole dispatch: the host rebuilds the character geometry at that time (RtScene::Build) and
//    the camera is interpolated from the shutter-open camera (offline_common.hlsli).
//  * Adaptive sampling: a pixel stops once the standard error of its perceptual luminance drops
//    below a threshold (after a minimum sample count).
//  * S9c in-path medium (full haze): every path segment travels through the frame's medium
//    (medium_common.hlsli: the height fog + the local fog volumes of the single-scatter pass), sampled by delta
//    tracking, with next-event estimation at each scattering event (sun + one punctual light, phase weighted, times
//    the medium transmittance) and the path continuing in a Henyey-Greenstein direction; the surface shadow rays are
//    dimmed by the medium too. GpuMedium::mode 0 (t5 space1) = today's code, no tracking loop entered.
//    Defining MMDX_MEDIUM_SINGLE_EVENT limits the medium to ONE event per path with sun + punctual NEE only (the S9c
//    correctness check against the single-scatter pass; a temporary build define, never set by the app).
//
// UAV table (identical for every kernel in this file):
//   u0 accum         RGBA32F image: radiance sum rgb, a = samples
//   u1 albedo accum  RGBA32F image: primary albedo sum rgb (1 for sky), a = primary hit count
//   u2 moments       RG32F image: sum of t, sum of t^2 (t = perceptual luminance of a sample)
//   u3 gbuffer       RGBA32F image: world normal of sample 0, w = view depth (1e6 = sky)
//   u4 counter       R32_UINT 1x1: pixels that traced in this dispatch (CSRender adds)
//   u5 outline accum RGBA32F image: sum of the per-iteration outline layers (only cleared here)
// Root constants:
//   gP0 = (seed, studioFloor, sampleIndex, minSamples)
//   gP1 = (imageWidth, imageHeight, 0, error threshold)
//   gP2 = (shutter time 0..1, focus distance (view z, <= 0: pinhole), lens radius, 0)
#include "rt_common.hlsli"   // includes common.hlsli
#include "offline_common.hlsli"
#include "dielectric.hlsli"
#include "offline_glass.hlsli"
#include "medium_common.hlsli"   // S9c: the in-path medium (fog_shapes.hlsli + the GpuMedium buffer t5 space1)
#define PBR_SAMPLE_LEVEL0     // compute: no screen-space gradients
#include "pt_pbr.hlsli"
cbuffer PassCB : register(b1) { float4 gP0; float4 gP1; float4 gP2; float4 gP3; };
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);
SamplerState gLinearWrap : register(s2);
#ifdef MMDX_PT_PACK
#include "pt_pack_api.hlsli"
#include "pt_pack_glue.hlsli"
#include MMDX_PT_PACK
#endif

RWTexture2D<float4> gAccum : register(u0);
RWTexture2D<float4> gAlbedoAccum : register(u1);
RWTexture2D<float2> gMoments : register(u2);
RWTexture2D<float4> gGbuffer : register(u3);
RWTexture2D<uint> gCounter : register(u4);
RWTexture2D<float4> gEdgeAccum : register(u5);
RWTexture2D<uint> gGbufId : register(u6);

struct PtLight {
    float3 pos; float invRange;
    float3 color; float cosOuter;
    float3 dir; float cosInner;
    float shadowSlice; float shadowType; float shadowSoftness; float shadowDensity;
    float3 shadowColor; float falloff;
    float affectDiffuse; float affectSpecular; float pointShadowSlice;
    float volumetric;   // S9: the light's share of the haze (GpuLight::_pad, 1 = physical)
    float areaWidth; float areaHeight; float isArea;
    float aniso;        // S9: this light's scattering anisotropy (GpuLight::_pad2); -1 = the fog's own
};
StructuredBuffer<PtLight> gPtLights : register(t2, space1);

static const float kSunCosMax = 0.99978;      // ~1.2 degree sun: soft, contact-hardening shadows
static const float kFloorExtent = 900.0;      // same as mmd.hlsl
static const float kAmbientEmission = 0.7;    // stage ambient emitted as light (raster unlit stage ~0.62 of lit)
static const float kToonIndirect = 0.6;       // GI fill on characters, on top of the toon key light
static const float kFlatGatherSkip = 1.5;     // faces: fill rays ignore geometry closer than this
static const uint kMaxDepth = 12u;
static const float kFirefly = 24.0;           // per-sample luminance clamp
static const float kHaze = 0.35;
// S9c: the medium NEE weights of the single-scatter pass (offline_volumetric.hlsl CSVolMarch: spotBoost / pointBoost)
static const float kMediumSpotBoost = 3.0;
static const float kMediumPointBoost = 0.45;
// Skin SSS (MMD units: 1 ~ 8 cm). Scatter radii diffuse the incident sun (red travels furthest);
// transmission depths drive the translucency of thin parts.
static const float3 kSkinRadius = float3(0.16, 0.07, 0.04);
static const float3 kSkinDepth = float3(0.18, 0.06, 0.035);
static const float3 kSkinScatterTint = float3(1.0, 0.36, 0.22);  // light leaking past the terminator
static const float kSkinThicknessMax = 0.8;     // thicker than this transmits nothing
static const float kSkinTranslucency = 0.6;              // aerial perspective, the raster default fog (composite.hlsl)

float3 SunIrradiance() { return SrgbToLinear(gLightColor) * (PI * 3.0) * gSunIntensity; }

// Radiance of rays that leave the scene. With the studio floor the world below the horizon is
// the faded-out cyclorama, i.e. the horizon colour (mmd.hlsl PSFloor fades to it).
float3 Background(float3 d) {
    if (gP0.y > 0.5 && d.y < 0.0) return SkyColor(normalize(float3(d.x, 0.0, d.z) + float3(0, 1e-4, 0)));
    return SkyColor(d);
}

float Perceptual(float3 c) {
    float l = Luminance(max(c, 0.0));
    return pow(l / (1.0 + l), 1.0 / 2.2);
}

// Uniform unit-disk sample (Shirley-Chiu concentric map).
float2 ConcentricDisk(float2 u) {
    float2 o = u * 2.0 - 1.0;
    if (o.x == 0.0 && o.y == 0.0) return 0.0;
    float r, phi;
    if (abs(o.x) > abs(o.y)) {
        r = o.x;
        phi = (PI / 4.0) * (o.y / o.x);
    } else {
        r = o.y;
        phi = (PI / 2.0) - (PI / 4.0) * (o.x / o.y);
    }
    return r * float2(cos(phi), sin(phi));
}

// ---- analytic props: glass box and softboxes (render benchmark scene) ------------------------
// The glass box (geometry in offline_glass.hlsli) is a smooth dielectric with dispersion (one hero
// colour channel per path once it refracts) and Beer-Lambert absorption inside.

// FresnelDielectric / the refraction step live in dielectric.hlsli (shared with the S2b mesh dielectrics).

// Index of refraction for the path's hero channel (-1: none chosen yet, green).
float GlassIor(int ch) { return gGlassParams.z + gGlassParams.w * 0.5 * (float)((ch < 0 ? 1 : ch) - 1); }

// Smooth dielectric scattering at a glass crossing (n faces the incoming ray): Fresnel-weighted
// choice between mirror reflection and refraction, through the same dielectric step a transmissive
// mesh uses with its own ior (dielectric.hlsli). The first refraction of a path picks its hero
// colour channel (dispersion): the throughput keeps that channel only, x3.
float3 GlassScatter(float3 d, float3 n, bool inside, inout int ch, inout float3 T, inout uint rng, out bool refracted) {
    refracted = false;
    const float u = Rand(rng);
    const float ior = GlassIor(ch);
    float eta = inside ? 1.0 / ior : ior;
    if (u < FresnelDielectric(saturate(dot(-d, n)), eta)) return reflect(d, n);
    if (ch < 0 && gGlassParams.w > 0.0) {
        ch = min((int)(Rand(rng) * 3.0), 2);
        T *= float3(ch == 0, ch == 1, ch == 2) * 3.0;
    }
    return DielectricRefract(d, n, inside, GlassIor(ch), refracted);
}

// Transparent shadow of the glass along a shadow ray: interface transmission at both crossings and
// Beer-Lambert absorption over the chord (the light is attenuated, not bent).
float3 GlassShadow(float3 o, float3 d, float tMax) {
    if (!GlassOn()) return 1.0;
    float tE, tX;
    float3 nE, nX;
    bool inE, inX;
    float3 tr = 1.0;
    if (!GlassIntersect(o, d, 0.0, tMax, tE, nE, inE)) return 1.0;
    float ior = gGlassParams.z;
    if (inE) {   // starts inside: only the exit
        tr *= 1.0 - FresnelDielectric(saturate(dot(d, nE)), 1.0 / ior);
        return tr * exp(-gGlassAbsorb.xyz * tE);
    }
    tr *= 1.0 - FresnelDielectric(saturate(-dot(d, nE)), ior);
    float3 p = o + d * (tE + kGlassEps * 4.0);
    if (GlassIntersect(p, d, 0.0, max(tMax - tE, 0.0), tX, nX, inX)) {
        tr *= 1.0 - FresnelDielectric(saturate(dot(d, nX)), 1.0 / ior);
        tr *= exp(-gGlassAbsorb.xyz * tX);
    } else {
        tr *= exp(-gGlassAbsorb.xyz * max(tMax - tE, 0.0));
    }
    return tr;
}

// Closest softbox (one-sided emissive rectangle, emitting on the side of cross(U, V)) in (tMin, tMax).
bool SoftboxIntersect(float3 o, float3 d, float tMin, float tMax, out float tHit, out float3 L) {
    tHit = tMax;
    L = 0;
    bool hit = false;
    [unroll] for (int i = 0; i < 2; ++i) {
        float4 c = gSoftbox[i * 4];
        if (c.w < 0.5) continue;
        float3 U = gSoftbox[i * 4 + 1].xyz, V = gSoftbox[i * 4 + 2].xyz;
        float3 n = cross(U, V);
        float dn = dot(d, n);
        if (dn >= 0.0) continue;
        float th = dot(c.xyz - o, n) / dn;
        if (th <= tMin || th >= tHit) continue;
        float3 p = o + d * th - c.xyz;
        if (abs(dot(p, U)) > dot(U, U) || abs(dot(p, V)) > dot(V, V)) continue;
        tHit = th;
        L = gSoftbox[i * 4 + 3].xyz;
        hit = true;
    }
    return hit;
}

// ---- scene ------------------------------------------------------------------------------

struct Surf {
    float3 pos, n, faceN;   // normals face the incoming ray
    float3 albedo;          // linear
    float3 emission;        // linear radiance (stage ambient, softboxes)
    float refl, rough;
    bool character, flat, receive;
    bool glass, inside;     // glass crossing; inside = leaving the glass body
    bool backFace;          // S2b: the ray hit the back face of the mesh (it is inside a transmissive body)
    bool emitter;           // softbox: the path ends here with `emission`
    bool pbr;               // the mesh hit has a PBR record (pt_pbr decoded into `pb`)
    RtGeometry g;           // undefined for the studio floor and the props
    float4 tex;
    float2 uv;
    float4 uv1, uv2;        // additional UV1/UV2 (zero for the floor / props)
    PtPbrSurface pb;        // PBR record (valid when `pbr`)
};

// Closest surface along a ray: meshes (stochastic alpha), the analytic studio floor, the glass box
// (unless skipGlass: the prepass sees straight through it) and the softboxes.
bool TraceScene(float3 o, float3 d, float tMin, inout uint rng, float lod, out Surf s, out float t,
                bool skipGlass = false) {
    s = (Surf)0;
    t = 1e5;
    float tMax = 1e5;
    bool floorHit = false;
    float tProp;
    float3 propN, propL;
    bool propInside;
    int prop = 0;   // 1 glass, 2 softbox
    if (!skipGlass && GlassIntersect(o, d, tMin, tMax, tProp, propN, propInside)) {
        tMax = tProp;
        prop = 1;
    }
    float tBox;
    if (SoftboxIntersect(o, d, tMin, tMax, tBox, propL)) {
        tMax = tBox;
        tProp = tBox;
        prop = 2;
    }
    if (gP0.y > 0.5 && d.y < -1e-5) {
        float tf = -o.y / d.y;
        // The floor dissolves into the sky with distance like the raster cyclorama (mmd.hlsl
        // PSFloor): stochastic coverage, so camera rays and GI see the same seamless fade.
        float r = length((o + d * tf).xz);
        if (tf > tMin && tf < tMax && r < kFloorExtent && Rand(rng) >= smoothstep(260.0, 820.0, r)) {
            tMax = tf;
            floorHit = true;
        }
    }
    RtHit hit;
    bool mesh = TraceClosest(o, d, tMin, tMax, max(Rand(rng), 0.004), hit);
    if (!mesh && !floorHit && prop == 0) return false;
    if (!mesh && !floorHit) {
        t = tProp;
        s.pos = o + d * t;
        if (prop == 2) {
            s.emitter = true;
            s.emission = propL;
            s.n = -d;
            s.faceN = -d;
            return true;
        }
        s.glass = true;
        s.inside = propInside;
        s.n = dot(propN, d) > 0.0 ? -propN : propN;   // toward the incoming ray
        s.faceN = s.n;
        s.albedo = 1.0;
        s.refl = 1.0;
        return true;
    }
    if (mesh) {
        t = hit.t;
        RtGeometry g = LoadGeometry(hit.instanceId, hit.geometryIndex);
        RtSurface sf = FetchSurface(g, hit.prim, hit.bary);
        s.backFace = dot(sf.faceNormal, d) > 0.0;   // S2b: the geometric normal faces away from the ray
        if (s.backFace) sf.faceNormal = -sf.faceNormal;
        if (dot(sf.normal, sf.faceNormal) < 0) sf.normal = -sf.normal;
        float4 tex = SampleBaseTexture(g, sf.uv, lod);
        s.g = g;
        s.tex = tex;
        s.uv = sf.uv;
        s.uv1 = sf.uv1;
        s.uv2 = sf.uv2;
        s.pos = sf.pos;
        s.n = sf.normal;
        s.faceN = sf.faceNormal;
        s.albedo = SrgbToLinear(saturate(MaterialAlbedo(g, tex.rgb)));
        s.character = (g.flags & MAT_STAGE) == 0;
        s.flat = (g.flags & MAT_FLAT) != 0;
        s.receive = (g.flags & MAT_RECEIVE) != 0;
        float r = g.reflectivity;
        if (!s.character) r = saturate(r + gFloorGloss * smoothstep(0.82, 0.97, sf.normal.y));
        s.refl = r;
        s.rough = clamp(sqrt(2.0 / (g.specularPower + 2.0)), 0.03, 0.6);
        s.emission = s.character ? 0.0 : SrgbToLinear(saturate(g.ambient * tex.rgb)) * gSunIntensity * kAmbientEmission;
        // AutoLuminous (そぼろ): a marked material emits its own colour (added once per hit, like the PBR emissive).
        s.emission += AutoLumEmission(g.flags, g.diffuse, g.specularPower, SrgbToLinear(saturate(tex.rgb)));
        // S5: a stage screen surface emits its texture and nothing else (black behind it, no ambient / bounce).
        if (g.flags & MAT_SCREEN) {
            s.emission = ScreenEmission(g.flags, SrgbToLinear(saturate(tex.rgb)), g.reflectivity);
            s.albedo = 0.0;
            s.refl = 0.0;
        }
        bool canPbr = !(g.flags & MAT_SCREEN) && (g.pbrIndex != 0xFFFFFFFFu);
#ifdef MMDX_PT_PACK
        // a PT pack wins over the record (raster rule): keep the plain surface the pack expects
        if (canPbr && (g.flags & RTG_PT_PACK) && LoadPtPackRecord(g.packSrv).materialClass != PACK_CLASS_OFF)
            canPbr = false;
#endif
        if (canPbr) {   // PBR record: mapped normal, albedo, GGX, AO, emissive
            s.pb = PtPbrDecode(hit.prim, g, sf, sf.normal, g.diffuse.rgb);
            s.n = s.pb.N;
            s.albedo = s.pb.baseColor;
            s.refl = saturate(Luminance(s.pb.f0) * (1.0 - s.pb.rough));
            s.rough = s.pb.rough;
            s.pbr = true;
        }
    } else {
        t = tMax;
        s.pos = o + d * t;
        s.n = float3(0, 1, 0);
        s.faceN = s.n;
        float r = length(s.pos.xz);
        const bool custom = gFloorParams.w >= 0.0;
        s.albedo = lerp(custom ? gFloorParams.xyz : float3(0.80, 0.83, 0.86), gSkyHorizon * 0.95, smoothstep(40.0, 420.0, r));
        s.refl = (custom ? gFloorParams.w : 0.42) * (1.0 - smoothstep(120.0, 600.0, r));
        s.rough = 0.12;
        s.receive = true;
    }
    return true;
}

// Camera ray through `pix` (pixel units) at this dispatch's shutter time, from a point on the lens.
void CameraRay(float2 pix, float2 lensU, out float3 o, out float3 d) {
    float2 uv = pix * gInvViewportSize;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 v = mul(float4(ndc, 1.0, 1.0), gInvProj);
    float3 vd = normalize(v.xyz / v.w);
    float3x3 basis;
    float3 eye;
    OfflineCameraAt(gP2.x, basis, eye);
    float3 lens = 0;
    if (gP2.y > 0.0 && gP2.z > 0.0) {
        lens = float3(ConcentricDisk(lensU) * gP2.z, 0.0);
        vd = normalize(vd * (gP2.y / vd.z) - lens);   // toward the in-focus point
    }
    o = eye + mul(lens, basis);
    d = normalize(mul(vd, basis));
}

// ---- lighting ---------------------------------------------------------------------------

// ---- S9c: the in-path medium (medium_common.hlsli) -------------------------------------------
// The GI paths travel through the same haze the single-scatter pass marches (level of detail: every segment of every
// path). Cheap helpers so mode 0 (the default when the haze is off) runs the old code and consumes no random numbers:
// every call below returns 1 / does nothing while gMedium[0].mode == 0.

// Transmittance of the medium between o and a light / the sky (shadow rays).
float MediumShadowT(float3 o, float3 d, float tMax, inout uint rng) {
#ifdef MMDX_MEDIUM_SINGLE_EVENT
    return 1.0;   // validation: the single-scatter pass only dims by the camera path (lit * T + L)
#endif
    return MediumOn() ? MediumTransmittance(o, d, tMax, rng) : 1.0;
}

// In-scattered radiance at a medium collision p looking along d (the path's direction): next-event estimation to the
// sun and (S9c-2: only in the validation build) to one punctual light. The visibility is the tinted mesh shadow ray
// times the medium transmittance along it.
float3 MediumNee(float3 p, float3 d, inout uint rng) {
    float3 L = 0;
    // sun: the same soft cone the surfaces sample, the single pass's sun colour (offline_volumetric.hlsl CSVolMarch)
    const float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), -gLightDir, SunShadowConeCos(kSunCosMax));
    float3 visSun = 1.0;
    if (gSunShadowParams.x >= 0.5) {
        float3 mesh = TraceShadowRayTinted(p, sd, 1e5, 0xFFu);
        float v = Luminance(mesh);
        visSun = (v > 0.0 ? ShadowTransmission(v, gSunShadowParams.z, gSunShadowColor.rgb) * mesh
                          : ShadowTransmission(0.0, gSunShadowParams.z, gSunShadowColor.rgb));
    }
    visSun *= MediumShadowT(p, sd, 1e5, rng);   // the medium between the event and the sun
    const float3 sunCol = SrgbToLinear(gLightColor) / SrgbToLinear(float3(0.6, 0.6, 0.6)) * gSunIntensity;
    L += sunCol * (gMedium[0].sunVolumetric * HgEval(dot(d, sd), gMedium[0].sunG)) * visSun;
    // S9c-2: the punctual / spot lights are equiangular light samples per segment (MediumPunctualSegment), not events:
    // their in-scattering is not a density along the path, so the deltas-tracking estimator does not own it. Only the
    // S9c validation build (which compares against the single pass's march) keeps the event's punctual NEE.
#ifdef MMDX_MEDIUM_SINGLE_EVENT

    // punctual: one light by its unshadowed contribution (weights = the single pass's in-scattering integrand)
    const uint nl = (uint)gNumLights;
    float total = 0.0;
    [loop] for (uint i = 0; i < nl; ++i) {
        PtLight l = gPtLights[i];
        if (l.volumetric <= 0.0) continue;
        float3 dv = l.pos - p;
        float dL = length(dv);
        float3 ld = dv / max(dL, 1e-4);
        float atten = PunctualFalloff(dL, l.invRange, (int)l.falloff);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
        if (atten <= 0.0) continue;
        const float g = l.aniso > -0.99 ? l.aniso : gMedium[0].g;
        total += Luminance(l.color * (l.volumetric * atten * HgEval(dot(d, ld), g)));
    }
    if (total > 0.0) {
        const float u = Rand(rng) * total;
        int picked = -1;
        float w = 0.0;
        float acc = 0.0;   // walked weight before this light
        [loop] for (uint i = 0; i < nl; ++i) {
            PtLight l = gPtLights[i];
            if (l.volumetric <= 0.0) continue;
            float3 dv = l.pos - p;
            float dL = length(dv);
            float3 ld = dv / max(dL, 1e-4);
            float atten = PunctualFalloff(dL, l.invRange, (int)l.falloff);
            if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
            else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
            if (atten <= 0.0) continue;
            const float g = l.aniso > -0.99 ? l.aniso : gMedium[0].g;
            w = Luminance(l.color * (l.volumetric * atten * HgEval(dot(d, ld), g)));
            picked = (int)i;
            if (w > 0.0) {
                if (u <= acc + w) break;
                acc += w;
            }
        }
        if (picked >= 0 && w > 0.0) {
            PtLight l = gPtLights[(uint)picked];
            float3 dv = l.pos - p;
            float dL = length(dv);
            float3 ld = dv / max(dL, 1e-4);
            float atten = PunctualFalloff(dL, l.invRange, (int)l.falloff);
            if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
            else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
            const float g = l.aniso > -0.99 ? l.aniso : gMedium[0].g;
            const bool spot = l.cosOuter > -1.0;
            const float boost = spot ? kMediumSpotBoost : kMediumPointBoost;
            float3 vis = 1.0;
            if (l.shadowType != 0.0) {
                float3 mesh = TraceShadowRayTinted(p, ld, max(dL - 0.05, 0.0), RT_MASK_CHARACTER);
                float v = Luminance(mesh);
                vis = (v > 0.0 ? ShadowTransmission(v, l.shadowDensity, l.shadowColor) * mesh
                               : ShadowTransmission(0.0, l.shadowDensity, l.shadowColor));
            }
            vis *= MediumShadowT(p, ld, max(dL - 0.05, 0.0), rng);
            L += l.color * (boost * l.volumetric) * (atten * HgEval(dot(d, ld), g)) * (total / w) * vis;
        }
    }
#endif
    return L;
}

// ---- S9c-2: equiangular punctual / spot light in-scattering on one path segment ----------------
// The punctual lights are delta lights: a path can never hit them, so their in-scattered radiance along a segment is the
// whole radiance they add to the medium there (only glass crossings of the segment clip it, as for any light). It is
// estimated per segment, independent of the medium's scattering events:
//
//   L = sum_lights int_a^b sigma_s(p(t)) * T(o->p(t)) * phase(d, ld) * Li(p(t)) * T(p(t)->light) dt
//
// with [a, b] the segment [0, tEnd] clipped to the haze sphere (MediumRange), the light's range sphere and, for a spot,
// its cone -- so a beam that only clips the segment still gets samples inside it. t is drawn with the equiangular
// (Kulla & Fajardo 2012) distribution around the light (MediumEquiangular), which for a point light makes
// 1 / pdf * T(o->p) ~ constant, so the estimate does not spike where the delta-tracking distance sampler rarely puts
// its events (a narrow cone seen along a long segment). Every light takes one sample (gNumLights <=
// kMediumAllLights), else one light is picked with the weights MediumNee used (total / w compensates, so the mean is
// the all-light sum).
//
// The scattering albedo is part of sigma_s here (unlike the collision event's NEE, which the path's albedo roulette
// weights), so the single-scatter energy matches the single pass's march.
static const uint kMediumAllLights = 8u;

// The part of the segment [0, tEnd] where light `l` can reach the medium: the haze sphere (MediumRange), the light's
// range sphere and, for a spot, its cone. Empty when x >= y.
float2 MediumLightRange(PtLight l, float3 o, float3 d, float tEnd) {
    const float2 seg = MediumRange(o, d, tEnd);
    if (seg.x >= seg.y) return float2(1, 0);
    float2 s = MediumRaySphere(o, d, l.pos, 1.0 / max(l.invRange, 1e-6), seg.y);
    s.x = max(s.x, seg.x);
    if (l.cosOuter > -1.0 && s.x < s.y) s = MediumRayCone(o, d, l.pos, l.dir, l.cosOuter, s.x, s.y);
    return s;
}

// The light's haze radiance at `p` (the pick's weight): falloff * phase * the light's haze share, with the spot cone /
// area window exactly as MediumNee's pick computed it.
float MediumPickWeight(PtLight l, float3 p, float3 d) {
    float3 dv = l.pos - p;
    float dL = length(dv);
    float atten = PunctualFalloff(dL, l.invRange, (int)l.falloff);
    if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-dv / max(dL, 1e-4), l.dir));
    else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -dv / max(dL, 1e-4)));
    const float g = l.aniso > -0.99 ? l.aniso : gMedium[0].g;
    return Luminance(l.color * (l.volumetric * atten * HgEval(dot(d, dv / max(dL, 1e-4)), g)));
}

// The pick's score: the weight at the middle of the light's own clipped interval (the stretch of the segment it actually
// lights, so a beam that clips the segment scores by its in-beam radiance, not by a window test at one fixed point),
// floored by a tiny share of the light's haze so the pick covers every light with a nonempty interval. Any pick is
// unbiased (total / w compensates); the floor only removes a support hole (a phase lobe facing away from the segment).
float MediumPickScore(PtLight l, float3 o, float3 d, float tEnd) {
    const float2 s = MediumLightRange(l, o, d, tEnd);
    if (s.x >= s.y) return 0.0;
    return MediumPickWeight(l, o + d * (0.5 * (s.x + s.y)), d) + 1e-4 * Luminance(l.color * l.volumetric);
}

// One equiangular sample of light `l` on the segment [0, tEnd] (0 when the segment misses it): see the block comment.
float3 MediumPunctualLight(PtLight l, float3 o, float3 d, float tEnd, float scale, inout uint rng) {
    const float2 s = MediumLightRange(l, o, d, tEnd);
    if (s.x >= s.y) return 0;
    float pdf;
    const float t = MediumEquiangular(o, d, l.pos, s.x, s.y, Rand(rng), pdf);
    if (pdf <= 0.0) return 0;
    float3 p = o + d * t;
    float3 dv = l.pos - p;
    float dL = length(dv);
    float3 ld = dv / max(dL, 1e-4);
    float atten = PunctualFalloff(dL, l.invRange, (int)l.falloff);
    if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
    else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
    if (atten <= 0.0) return 0;
    const float g = l.aniso > -0.99 ? l.aniso : gMedium[0].g;
    const float boost = l.cosOuter > -1.0 ? kMediumSpotBoost : kMediumPointBoost;
    // the tinted mesh shadow ray and the medium between the sample and the light (as MediumNee's punctual part)
    float3 vis = 1.0;
    if (l.shadowType != 0.0) {
        float3 mesh = TraceShadowRayTinted(p, ld, max(dL - 0.05, 0.0), RT_MASK_CHARACTER);
        float v = Luminance(mesh);
        vis = (v > 0.0 ? ShadowTransmission(v, l.shadowDensity, l.shadowColor) * mesh
                       : ShadowTransmission(0.0, l.shadowDensity, l.shadowColor));
    }
    vis *= MediumShadowT(p, ld, max(dL - 0.05, 0.0), rng);
    const float3 sigmaS = MediumAlbedo(p) * MediumSigma(p);
    const float Tview = MediumTransmittance(o, d, t, rng);
    const float3 Li = l.color * (atten * (boost * l.volumetric)) * HgEval(dot(d, ld), g);
    return sigmaS * Li * (Tview * scale * vis / pdf);
}

// The punctual / spot lights' in-scattered radiance on this segment (see the block comment above).
float3 MediumPunctualSegment(float3 o, float3 d, float tEnd, inout uint rng) {
#ifdef MMDX_MEDIUM_SINGLE_EVENT
    return 0;   // validation: the event's punctual NEE below is the whole punctual contribution (matches the single pass)
#endif
    const uint nl = (uint)gNumLights;
    if (nl == 0u) return 0;
    if (nl <= kMediumAllLights) {
        float3 L = 0;
        [loop] for (uint i = 0; i < nl; ++i) {
            PtLight l = gPtLights[i];
            if (l.volumetric <= 0.0) continue;
            L += MediumPunctualLight(l, o, d, tEnd, 1.0, rng);
        }
        return L;
    }
    // one light by its unshadowed contribution (the pick MediumNee's punctual part used): each light is scored at the
    // middle of the stretch of this segment it lights, and total / w makes the one-sample mean the all-light sum.
    if (MediumRange(o, d, tEnd).x >= MediumRange(o, d, tEnd).y) return 0;
    float total = 0.0;
    [loop] for (uint i = 0; i < nl; ++i) {
        PtLight l = gPtLights[i];
        if (l.volumetric <= 0.0) continue;
        total += MediumPickScore(l, o, d, tEnd);
    }
    if (total <= 0.0) return 0;
    const float u = Rand(rng) * total;
    PtLight picked = gPtLights[0];
    float w = 0.0;
    float acc = 0.0;
    [loop] for (uint i = 0; i < nl; ++i) {
        PtLight l = gPtLights[i];
        if (l.volumetric <= 0.0) continue;
        w = MediumPickScore(l, o, d, tEnd);
        if (w <= 0.0) continue;
        picked = l;
        if (u <= acc + w) break;
        acc += w;
    }
    if (w <= 0.0) return 0;
    return MediumPunctualLight(picked, o, d, tEnd, total / w, rng);
}

// One step through the medium along [0, tEnd] from o: a real collision consumes one event and reports the event point
// (the returned distance) and the path's new direction (HG around the current one); the caller then traces the scene
// again from there. When the event budget is spent the segment is handled by transmittance only.
//   0 = the segment is done (no medium change), 1 = the ray bent (trace the scene again), 2 = the path ends here
//   (the albedo roulette killed it, or the S9c validation build after its one event).
int MediumStep(inout float3 o, inout float3 d, float tEnd, inout float3 T, inout float3 radiance, inout uint events,
               inout uint rng, bool primary) {
    if (!MediumOn()) return 0;
    // S9c-2: the punctual / spot lights in the medium are one equiangular sample over the whole segment, taken before the
    // events (they are delta lights: no path can hit them, so this is the whole in-scattering they add here). Outside
    // the event budget: it is not a scattering event, so a spent budget (transmittance only) still keeps it.
    radiance += T * MediumPunctualSegment(o, d, tEnd, rng);
#ifdef MMDX_MEDIUM_SINGLE_EVENT
    // Validation build (the S9c correctness check): ONE event per path, sun + punctual NEE only. The event adds its NEE
    // (and the sky's isotropic fill the fast pass adds as ambient * (1 - T), on the camera segment) and the path then
    // continues unchanged, so this estimates exactly the single-scatter integral the fast pass marches -- only the
    // sampling differs. Never set by the app.
    if (events >= 1u) return 0;
    if (primary) {
        const float3 amb = (gSkyZenith + gSkyHorizon + gGroundColor) * (1.0 / 3.0) * gSunIntensity * 0.25;
        radiance += T * amb * (1.0 - MediumTransmittance(o, d, tEnd, rng));
    }
#endif
    if (events >= MediumMaxEvents()) {
        T *= MediumTransmittance(o, d, tEnd, rng);
        return 0;
    }
    float t;
    float3 albedo;
    if (!MediumSampleDistance(o, d, tEnd, rng, t, albedo)) return 0;
    float3 p = o + d * t;
    radiance += T * MediumNee(p, d, rng);
    // scattering albedo: Russian roulette on its luminance (throughput *= albedo / the survival probability)
    const float q = min(max(albedo.r, max(albedo.g, albedo.b)), 0.95);
    if (q <= 0.0 || Rand(rng) > q) return 2;
    T *= albedo / q;
    ++events;
#ifdef MMDX_MEDIUM_SINGLE_EVENT
    return 0;   // validation: the event's NEE is the whole medium contribution; the ray continues unchanged
#endif
    d = HgSample(d, gMedium[0].g, float2(Rand(rng), Rand(rng)));
    o = p;
    return 1;
}

// Unshadowed irradiance from punctual light `li` (pi * colour * attenuation * N.L, so that Lambert
// albedo / pi * E matches the raster albedo * colour * attenuation * N.L). Characters use the raster
// soft toon terminator (faces: almost no N.L).
float3 PunctualUnshadowed(Surf s, uint li, bool toon, out float3 ld, out float dist) {
    PtLight l = gPtLights[li];
    float3 d = l.pos - s.pos;
    dist = length(d);
    ld = d / max(dist, 1e-4);
    float atten = PunctualFalloff(dist, l.invRange, (int)l.falloff);
    if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
    else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
    float ndl = dot(s.n, ld);
    float flat = s.flat ? 1.0 : 0.0;
    float diff = toon ? lerp(smoothstep(-0.05, 0.25, ndl), saturate(ndl * 0.3 + 0.7), flat) : ndl;
    if (atten <= 0.0 || diff <= 0.0 || l.affectDiffuse <= 0.5) return 0.0;
    return PI * l.color * atten * diff;
}

// Punctual light direct lighting: diffuse irradiance with a single picked shadow ray, plus specular NEE
// for the specular lobe around reflect(-V, s.n) with half-angle rough * 0.5. Shadow rays are reused
// when diffuse and specular evaluate the same light.
void PunctualLighting(Surf s, float3 V, bool toon, float pSpec, inout uint rng, out float3 diffE, out float3 specR) {
    diffE = 0;
    specR = 0;
    uint nl = (uint)gNumLights;
    if (nl == 0u) return;

    float3 R = reflect(-V, s.n);
    float cosHalfAngle = cos(s.rough * 0.5);
    float omega = max(2.0 * PI * (1.0 - cosHalfAngle), 1e-6);
    bool canSpec = (pSpec > 0.0);

    float total = 0.0;
    for (uint i = 0; i < nl; ++i) {
        float3 ld;
        float dist;
        total += Luminance(PunctualUnshadowed(s, i, toon, ld, dist));
    }

    int pickedJ = -1;
    float pickedW = 0.0;
    if (total > 0.0) {
        float u = Rand(rng) * total;
        for (uint j = 0; j < nl; ++j) {
            float3 ld;
            float dist;
            float3 e = PunctualUnshadowed(s, j, toon, ld, dist);
            float w = Luminance(e);
            if (w <= 0.0) continue;
            u -= w;
            if (u <= 0.0 || j == nl - 1u) {
                pickedJ = (int)j;
                pickedW = w;
                break;
            }
        }
    }

    if (pickedJ >= 0) {
        PtLight l = gPtLights[(uint)pickedJ];
        int shadowType = (int)l.shadowType;
        float3 ld;
        float dist;
        float atten;
        float diff;
        if (l.isArea > 0.5) {
            float3 norm = l.dir;
            float3 c = cross(float3(0, 1, 0), norm);
            float cLen = length(c);
            float3 right = (cLen > 1e-4) ? (c / cLen) : float3(1, 0, 0);
            float3 up = cross(norm, right);
            float uA = (Rand(rng) - 0.5) * l.areaWidth;
            float vA = (Rand(rng) - 0.5) * l.areaHeight;
            float3 samplePos = l.pos + right * uA + up * vA;
            float3 d = samplePos - s.pos;
            dist = length(d);
            ld = d / max(dist, 1e-4);
            atten = PunctualFalloff(dist, l.invRange, (int)l.falloff) * max(0.0, dot(norm, -ld));
            float ndl = dot(s.n, ld);
            float flat = s.flat ? 1.0 : 0.0;
            diff = toon ? lerp(smoothstep(-0.05, 0.25, ndl), saturate(ndl * 0.3 + 0.7), flat) : ndl;
        } else {
            float3 d = l.pos - s.pos;
            dist = length(d);
            ld = d / max(dist, 1e-4);
            atten = PunctualFalloff(dist, l.invRange, (int)l.falloff);
            if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
            float ndl = dot(s.n, ld);
            float flat = s.flat ? 1.0 : 0.0;
            diff = toon ? lerp(smoothstep(-0.05, 0.25, ndl), saturate(ndl * 0.3 + 0.7), flat) : ndl;
        }

        bool evalDiff = (atten > 0.0) && (diff > 0.0) && (l.affectDiffuse > 0.5);
        bool evalSpecForPicked = canSpec && (l.affectSpecular > 0.5) && (atten > 0.0) &&
                                 ((s.pbr ? dot(s.pb.N, ld) > 0.0 : dot(R, ld) >= cosHalfAngle)) && (dot(s.faceN, ld) > 0.0);

        if (evalDiff || evalSpecForPicked) {
            float3 vis = 1.0;
            if (l.isArea > 0.5) {
                if (shadowType != 0) {
                    float3 po = OffsetRayOrigin(s.pos, s.faceN);
                    float3 meshTrans = TraceShadowRayTinted(po, ld, max(dist - 0.05, 0.0), RT_MASK_CHARACTER);
                    float hitVis = Luminance(meshTrans);
                    float3 shadowTrans = ShadowTransmission(hitVis, l.shadowDensity, l.shadowColor);
                    vis = shadowTrans * (hitVis > 0.0 ? meshTrans * GlassShadow(po, ld, max(dist - 0.05, 0.0))
                                                      : float3(1, 1, 1));
                }
            } else if (shadowType != 0) {
                float3 po = OffsetRayOrigin(s.pos, s.faceN);
                float3 dir = ld;
                if (shadowType == 2 && l.shadowSoftness > 0.0) {
                    float cosTheta = PunctualShadowConeCos(1.0, l.shadowSoftness);
                    dir = SampleCone(float2(Rand(rng), Rand(rng)), ld, cosTheta);
                }
                float3 meshTrans = TraceShadowRayTinted(po, dir, max(dist - 0.05, 0.0), RT_MASK_CHARACTER);
                float hitVis = Luminance(meshTrans);
                float3 shadowTrans = ShadowTransmission(hitVis, l.shadowDensity, l.shadowColor);
                vis = shadowTrans * (hitVis > 0.0 ? meshTrans * GlassShadow(po, dir, max(dist - 0.05, 0.0))
                                                  : float3(1, 1, 1));
            }

            // S9c: the medium between the surface and the light dims / tints it as well (mode 0: 1, no random numbers)
            if (MediumOn())
                vis *= MediumShadowT(OffsetRayOrigin(s.pos, s.faceN), ld, max(dist - 0.05, 0.0), rng);

            if (evalDiff) {
                float3 e = PI * l.color * atten * diff;
                diffE = e * (total / pickedW) * vis;
            }
            if (evalSpecForPicked) {
                if (s.pbr)
                    specR += pSpec * l.color * atten * vis * PtPbrEvalSpecular(s.pb, V, ld);
                else
                    specR += pSpec * l.color * atten * vis / omega;
            }
        }
    }

    if (canSpec) {
        for (uint k = 0; k < nl; ++k) {
            if ((int)k == pickedJ) continue;
            PtLight lk = gPtLights[k];
            if (lk.affectSpecular <= 0.5) continue;
            float3 ld;
            float dist;
            float atten;
            if (lk.isArea > 0.5) {
                float3 norm = lk.dir;
                float3 c = cross(float3(0, 1, 0), norm);
                float cLen = length(c);
                float3 right = (cLen > 1e-4) ? (c / cLen) : float3(1, 0, 0);
                float3 up = cross(norm, right);
                float uA = (Rand(rng) - 0.5) * lk.areaWidth;
                float vA = (Rand(rng) - 0.5) * lk.areaHeight;
                float3 samplePos = lk.pos + right * uA + up * vA;
                float3 d = samplePos - s.pos;
                dist = length(d);
                ld = d / max(dist, 1e-4);
                atten = PunctualFalloff(dist, lk.invRange, (int)lk.falloff) * max(0.0, dot(norm, -ld));
            } else {
                float3 d = lk.pos - s.pos;
                dist = length(d);
                ld = d / max(dist, 1e-4);
                atten = PunctualFalloff(dist, lk.invRange, (int)lk.falloff);
                if (lk.cosOuter > -1.0) atten *= smoothstep(lk.cosOuter, lk.cosInner, dot(-ld, lk.dir));
            }

            if (atten > 0.0 && (s.pbr ? dot(s.pb.N, ld) > 0.0 : dot(R, ld) >= cosHalfAngle) && dot(s.faceN, ld) > 0.0) {
                int shadowType = (int)lk.shadowType;
                float3 vis = 1.0;
                if (lk.isArea > 0.5) {
                    if (shadowType != 0) {
                        float3 po = OffsetRayOrigin(s.pos, s.faceN);
                        float3 meshTrans = TraceShadowRayTinted(po, ld, max(dist - 0.05, 0.0), RT_MASK_CHARACTER);
                        float hitVis = Luminance(meshTrans);
                        float3 shadowTrans = ShadowTransmission(hitVis, lk.shadowDensity, lk.shadowColor);
                        vis = shadowTrans * (hitVis > 0.0 ? meshTrans * GlassShadow(po, ld, max(dist - 0.05, 0.0))
                                                          : float3(1, 1, 1));
                    }
                } else if (shadowType != 0) {
                    float3 po = OffsetRayOrigin(s.pos, s.faceN);
                    float3 dir = ld;
                    if (shadowType == 2 && lk.shadowSoftness > 0.0) {
                        float cosTheta = PunctualShadowConeCos(1.0, lk.shadowSoftness);
                        dir = SampleCone(float2(Rand(rng), Rand(rng)), ld, cosTheta);
                    }
                    float3 meshTrans = TraceShadowRayTinted(po, dir, max(dist - 0.05, 0.0), RT_MASK_CHARACTER);
                    float hitVis = Luminance(meshTrans);
                    float3 shadowTrans = ShadowTransmission(hitVis, lk.shadowDensity, lk.shadowColor);
                    vis = shadowTrans * (hitVis > 0.0 ? meshTrans * GlassShadow(po, dir, max(dist - 0.05, 0.0))
                                                      : float3(1, 1, 1));
                }
                const float medT = MediumShadowT(OffsetRayOrigin(s.pos, s.faceN), ld, max(dist - 0.05, 0.0), rng);
                specR += pSpec * lk.color * atten * vis * medT * (s.pbr ? PtPbrEvalSpecular(s.pb, V, ld) : (1.0 / omega));
            }
        }
    }
}

float3 PunctualIrradiance(Surf s, bool toon, inout uint rng) {
    float3 diffE = 0, specR = 0;
    PunctualLighting(s, float3(0, 1, 0), toon, 0.0, rng, diffE, specR);
    return diffE;
}

// Sun visibility (soft cone sample), tinted by the glass it passes through.
float3 SunVisibility(Surf s, inout uint rng) {
    if (!s.receive) return 1.0;
    if (gSunShadowParams.x < 0.5)   // NoCast: the rays are clear, the medium still dims the sun
        return MediumShadowT(s.pos, -gLightDir, 1e5, rng);
    float cosMax = SunShadowConeCos(kSunCosMax);
    float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), -gLightDir, cosMax);
    float3 po = OffsetRayOrigin(s.pos, s.faceN);
    float3 meshTrans = TraceShadowRayTinted(po, sd, 1e5, 0xFFu);
    float v = Luminance(meshTrans);
    float3 trans = ShadowTransmission(v, gSunShadowParams.z, gSunShadowColor.rgb);
    // S9c: the medium between the surface and the sun dims / tints the light reaching it
    return trans * (v > 0.0 ? meshTrans * GlassShadow(po, sd, 1e5) : float3(1, 1, 1)) *
           MediumShadowT(po, sd, 1e5, rng);
}

// Lambert irradiance from the sun and the punctual lights (one shadow ray each).
float3 DirectIrradiance(Surf s, inout uint rng) {
    float3 E = 0;
    float3 L = -gLightDir;
    float ndl = dot(s.n, L);
    if (ndl > 0.0 && dot(s.faceN, L) > 0.0) E += SunIrradiance() * ndl * SunVisibility(s, rng);
    if (gNumLights >= 1.0) E += PunctualIrradiance(s, false, rng);
    return E;
}

// ---- skin subsurface scattering -----------------------------------------------------------

// 0..1 skin likelihood of a character texel from its fully lit colour (gamma space): warm
// low-saturation tones with r >= g >= b (peach to pale pink). Eyes, brows, hair and clothes in the
// same texture atlas fall outside the range.
float SkinWeight(Surf s) {
    if (!s.character) return 0.0;
    float3 c = saturate(s.g.ambient + s.g.diffuse.rgb) * s.tex.rgb;
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    if (mx < 0.3 || c.r < c.g || c.g < c.b - 0.02) return 0.0;
    float sat = (mx - mn) / mx;
    float hue = 60.0 * (c.g - c.b) / max(mx - mn, 1e-4);   // degrees within the red sector
    float w = smoothstep(0.04, 0.10, sat) * (1.0 - smoothstep(0.45, 0.60, sat));
    w *= smoothstep(-3.0, 5.0, hue) * (1.0 - smoothstep(30.0, 40.0, hue));
    return w * smoothstep(0.30, 0.50, mx);
}

// Sun visibility per colour channel for skin: each channel's shadow ray starts from a point
// jittered on the surface within that channel's scatter radius, so the averaged image shows the
// incident light diffused under the skin (soft, red-fringed shadow edges).
float3 SkinSunVisibility(Surf s, float skin, inout uint rng) {
    if (!s.receive) return 1.0;
    if (gSunShadowParams.x < 0.5)   // NoCast: the rays are clear, the medium still dims the sun
        return MediumShadowT(s.pos, -gLightDir, 1e5, rng).xxx;
    float cosMax = SunShadowConeCos(kSunCosMax);
    float3 t, b;
    BuildBasis(s.faceN, t, b);
    float3 vis;
    [unroll] for (int c = 0; c < 3; ++c) {
        float r = kSkinRadius[c] * skin * sqrt(Rand(rng));
        float phi = 6.2831853 * Rand(rng);
        float3 p = s.pos + (t * cos(phi) + b * sin(phi)) * r;
        float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), -gLightDir, cosMax);
        float3 po = OffsetRayOrigin(p, s.faceN);
        float3 meshTrans = TraceShadowRayTinted(po, sd, 1e5, 0xFFu);
        float v = Luminance(meshTrans);
        float3 trans = ShadowTransmission(v, gSunShadowParams.z, gSunShadowColor.rgb);
        vis[c] = (v > 0.0 ? trans * meshTrans * GlassShadow(po, sd, 1e5) : trans)[c] *
                 MediumShadowT(po, sd, 1e5, rng);
    }
    return vis;
}

// Closest character surface along a ray from inside the mesh (back faces included): the exit
// distance of light passing through a thin part. Returns -1 when nothing is hit within tMax.
float ExitDistance(float3 origin, float3 dir, float tMax) {
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.0;
    r.TMax = tMax;
    q.TraceRayInline(gTlas, RAY_FLAG_NONE, RT_MASK_CHARACTER, r);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            RtGeometry g = LoadGeometry(q.CandidateInstanceID(), q.CandidateGeometryIndex());
            if (CandidateAlpha(g, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()) >= 0.5)
                q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
}

// Light arriving from `dir` (unit, toward the light) through the skin behind the shaded point:
// per-channel transmittance exp(-thickness / depth) times the light's visibility at the exit point.
float3 SkinTransmittance(Surf s, float3 dir, float lightDist, inout uint rng) {
    float3 inside = s.pos - s.faceN * 0.004;   // just beneath the surface
    float d = ExitDistance(inside, dir, kSkinThicknessMax);
    if (d < 0.0) return 0.0;
    float3 trans = exp(-d / kSkinDepth);
    if (Luminance(trans) < 1e-3) return 0.0;
    float3 exitP = inside + dir * (d + 0.01);
    float3 meshVis = TraceShadowRayTinted(exitP, dir, max(lightDist - d, 0.0), 0xFFu);
    float vis = Luminance(meshVis);
    const float medT = MediumShadowT(exitP, dir, max(lightDist - d, 0.0), rng);
    return vis > 0.0 ? trans * meshVis * GlassShadow(exitP, dir, max(lightDist - d, 0.0)) * medT : 0.0;
}

// Translucency of thin skin (ears, fingers) lit from behind by the sun and one punctual light
// (picked uniformly), as radiance toward the camera.
float3 SkinTranslucency(Surf s, float skin, inout uint rng) {
    float3 c = 0;
    float3 L = -gLightDir;
    if (dot(s.faceN, L) < 0.2) {
        float3 sd = SampleCone(float2(Rand(rng), Rand(rng)), L, kSunCosMax);
        c += SunIrradiance() * SkinTransmittance(s, sd, 1e5, rng);
    }
    if (gNumLights >= 1.0) {
        uint nl = (uint)gNumLights;
        uint li = min((uint)(Rand(rng) * (float)nl), nl - 1u);
        PtLight l = gPtLights[li];
        float3 d = l.pos - s.pos;
        float dist = length(d);
        float3 ld = d / max(dist, 1e-4);
        float atten = PunctualFalloff(dist, l.invRange, (int)l.falloff);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
        if (atten > 0.0 && dot(s.faceN, ld) < 0.2 && l.affectDiffuse > 0.5)
            c += PI * l.color * atten * (float)nl * SkinTransmittance(s, ld, dist, rng);
    }
    return s.albedo / PI * c * kSkinTranslucency * skin;
}

// Character materials keep the raster toon model (mmd.hlsl PSMain) for the sun, with the traced
// soft-shadow visibility `sh`. Same as pathtrace.hlsl ToonSun. Linear output.
float3 ToonSun(Surf s, float3 V, float3 sh, float skin) {
    RtGeometry g = s.g;
    float3 n = s.n;
    float3 L = -gLightDir;
    float3 lit = saturate(g.ambient + g.diffuse.rgb * gLightColor) * s.tex.rgb;
    if (g.flags & (MAT_SPHERE_MUL | MAT_SPHERE_ADD)) {
        float3 nv = normalize(mul(n, (float3x3)gView));
        float2 suv = nv.xy * float2(0.5, -0.5) + 0.5;
        float3 sp = ApplyTexFactor3(gBindlessTex[NonUniformResourceIndex(g.sphereSrv)].SampleLevel(gLinear, suv, 0).rgb, g.sphereMul, g.sphereAdd, (g.flags & MAT_SPHERE_MUL) ? 1.0 : 0.0);
        if (g.flags & MAT_SPHERE_MUL) lit *= sp; else lit += sp;
    }
    float ndl = dot(n, L);
    float flat = s.flat ? 1.0 : 0.0;
    float3 c;
    if (g.flags & MAT_TOON_MAP) {
        float3 shadowTex = ApplyTexFactor3(gBindlessTex[NonUniformResourceIndex(g.toonSrv)].SampleLevel(gRtWrap, s.uv, 0).rgb, g.toonMul, g.toonAdd, 1.0);
        float3 term = sh * lerp(smoothstep(-0.03, 0.06, ndl), 1.0, flat);
        c = lerp(saturate(g.ambient + g.diffuse.rgb * gLightColor) * shadowTex, lit, term);
    } else if (g.flags & MAT_HAS_TOON) {
        // cast shadows push the lookup to the dark end of the ramp (per channel for skin)
        Texture2D toon = gBindlessTex[NonUniformResourceIndex(g.toonSrv)];
        float3 rampLit = ApplyTexFactor3(toon.SampleLevel(gLinear, float2(0.5, saturate(0.5 - 0.5 * ndl)), 0).rgb, g.toonMul, g.toonAdd, 1.0);
        float3 rampShadow = ApplyTexFactor3(toon.SampleLevel(gLinear, float2(0.5, 1.0), 0).rgb, g.toonMul, g.toonAdd, 1.0);
        c = lit * lerp(rampShadow, rampLit, sh);
    } else {
        c = lit;
    }
    if (!(g.flags & MAT_TOON_MAP)) {
        float3 term = lerp(sh * smoothstep(-0.12, 0.22, ndl), lerp(1.0, sh, 0.8), flat);
        // the shaded side of skin stays warm (light scattered through it), other materials cool
        float3 tint = lerp(float3(0.90, 0.90, 0.96), float3(1.0, 0.88, 0.84), skin);
        float3 shade = c * lerp(1.0, saturate(c), 0.55) * tint;
        c = lerp(shade, c, term);
        // light leaking past the terminator into the shadow side
        float band = smoothstep(-0.35, -0.02, ndl) * (1.0 - smoothstep(-0.02, 0.25, ndl)) * (1.0 - flat);
        c += lit * kSkinScatterTint * band * skin * 0.3 * sh;
    }
    if (g.specularPower > 0.0)
        c += pow(saturate(dot(normalize(L + V), n)), g.specularPower) * g.specular * gLightColor * sh;
    float3 color = SrgbToLinear(saturate(c)) * gSunIntensity;
    float rim = pow(1.0 - saturate(dot(n, V)), 4.0);
    float side = smoothstep(-0.2, 0.5, ndl + 0.25);
    color += gRimColor * rim * side * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
    return color;
}

// PBR sun direct at a camera/mirror hit (mirrors mmd.hlsl MmdShadePbr; linear). CEL/TONEBASED keep the toon
// structure with the PBR mapped normal; the MMD Blinn specular is replaced by GGX. Emissive is added by the caller.
float3 PbrSunDirect(Surf s, float3 V, float3 sh) {
    PtPbrSurface p = s.pb;
    RtGeometry g = s.g;
    float3 L = -gLightDir;
    float3 n = p.N;
    float ndl = dot(n, L);
    float flat = s.flat ? 1.0 : 0.0;
    float3 mddLight = saturate(g.ambient + g.diffuse.rgb * gLightColor);

    if (p.model == 9u) {   // TONEBASED: hardness toon toward the customB shadow colour
        float hard = saturate(p.customA);
        float litAmt = saturate(ndl * hard + 1.0 - hard) * max(sh.r, max(sh.g, sh.b));
        float3 c9 = SrgbToLinear(saturate(mddLight * s.tex.rgb)) * lerp(p.customB, 1.0, litAmt) * gSunIntensity;
        float3 rim9 = pow(1.0 - saturate(dot(n, V)), 4.0) * smoothstep(-0.2, 0.5, dot(n, L) + 0.25);
        c9 += gRimColor * rim9 * max(sh.r, max(sh.g, sh.b)) * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
        return c9;
    }

    float3 cAlbedo = LinearToSrgb(saturate(p.diffuse));
    float3 c = mddLight * cAlbedo;
    if (p.model == 8u) {   // CEL: toon ramp replaces the PBR diffuse colour
        float v = lerp(1.0, saturate(0.5 - 0.5 * ndl), max(sh.r, max(sh.g, sh.b)));
        c = mddLight * s.tex.rgb * ApplyTexFactor3(gBindlessTex[NonUniformResourceIndex(g.toonSrv)].SampleLevel(gLinear, float2(0.5, v), 0).rgb, g.toonMul, g.toonAdd, 1.0);
    } else {
        float3 term = sh * lerp(smoothstep(-0.12, 0.22, ndl), 1.0, flat);
        float3 shade = c * lerp(1.0, saturate(c), 0.55) * float3(0.90, 0.90, 0.96);
        c = lerp(shade, c, term);
    }
    float3 color = SrgbToLinear(saturate(c)) * gSunIntensity;

    if (p.model == 1u) {   // SKIN: soft SSS tint, the raster / PT term (customA curvature, customB transmittance)
        float ss = pow(1.0 - saturate(ndl), 2.0) * pow(1.0 - saturate(-ndl), 3.0 / (p.customA + 0.001)) *
                   saturate(p.customA - 0.04);
        color += p.customB * (ss * 0.7) * SrgbToLinear(saturate(gLightColor)) * gSunIntensity * sh;
    }
    if (p.specOn)   // MMD Blinn replaced by GGX (per-model lobe), scaled by the sun visibility
        color += PtPbrEvalSpecular(p, V, L) * sh * SrgbToLinear(saturate(gLightColor)) * gSunIntensity * saturate(ndl) * p.specScale;
    float rim = pow(1.0 - saturate(dot(n, V)), 4.0);
    float side = smoothstep(-0.2, 0.5, dot(n, L) + 0.25);
    float3 rimTint = lerp(float3(1, 1, 1), dot(p.f0, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)).xxx, p.metal);
    return color + gRimColor * rimTint * rim * side * sh * gRimStrength * gSunIntensity * (1.0 - 0.6 * flat);
}

// Glass crossing seen by the camera (or a mirror): delta lights have no mirror image in a smooth
// dielectric, so the sun and the spots add a tight normalized Blinn-Phong glint weighted by Fresnel.
float3 GlassGlint(Surf s, float3 V, inout uint rng) {
    const float e = 1200.0, norm = (e + 8.0) / (8.0 * PI);
    float3 c = 0;
    float3 L = -gLightDir;
    float3 h = normalize(L + V);
    float ndl = dot(s.n, L);
    if (ndl > 0.0) {
        float spec = norm * pow(saturate(dot(s.n, h)), e) * FresnelDielectric(saturate(dot(h, V)), gGlassParams.z);
        if (spec > 1e-3) {
            float3 po = OffsetRayOrigin(s.pos, s.faceN);
            float vis = 1.0;
            if (gSunShadowParams.x > 0.5)
                vis = Luminance(saturate(TraceShadowRayTinted(po, L, 1e5, 0xFFu)));
            vis *= MediumShadowT(po, L, 1e5, rng);   // S9c: the medium dims the glint too
            c += spec * SunIrradiance() * ndl * vis;
        }
    }
    uint nl = (uint)gNumLights;
    for (uint i = 0; i < nl; ++i) {
        PtLight l = gPtLights[i];
        if (l.affectSpecular <= 0.5) continue;
        float3 dl = l.pos - s.pos;
        float dist = length(dl);
        float3 ld = dl / max(dist, 1e-4);
        float atten = PunctualFalloff(dist, l.invRange, (int)l.falloff);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
        float nl2 = dot(s.n, ld);
        if (atten <= 0.0 || nl2 <= 0.0) continue;
        float3 hh = normalize(ld + V);
        c += norm * pow(saturate(dot(s.n, hh)), e) * FresnelDielectric(saturate(dot(hh, V)), gGlassParams.z) *
             PI * l.color * atten * nl2 * MediumShadowT(s.pos, ld, max(dist - 0.05, 0.0), rng);
    }
    return c;
}

// S2b: the same tight glint for a transmissive MESH surface (a smooth dielectric reflects the delta lights, which a
// sampled reflection direction would almost never find). Weighted by Fresnel at the material's ior.
float3 FresnelGlint(Surf s, float3 V, inout uint rng) {
    const float e = 1200.0, norm = (e + 8.0) / (8.0 * PI);
    const float ior = max(s.pb.ior, 1.0);
    float3 c = 0;
    float3 L = -gLightDir;
    float3 h = normalize(L + V);
    float ndl = dot(s.n, L);
    if (ndl > 0.0) {
        float spec = norm * pow(saturate(dot(s.n, h)), e) * FresnelDielectric(saturate(dot(h, V)), ior);
        if (spec > 1e-3) {
            float3 po = OffsetRayOrigin(s.pos, s.faceN);
            float3 vis = gSunShadowParams.x > 0.5 ? TraceShadowRayTinted(po, L, 1e5, 0xFFu) : float3(1, 1, 1);
            vis *= MediumShadowT(po, L, 1e5, rng);   // S9c: the medium dims the glint too
            c += spec * SunIrradiance() * ndl * vis;
        }
    }
    uint nl = (uint)gNumLights;
    for (uint i = 0; i < nl; ++i) {
        PtLight l = gPtLights[i];
        if (l.affectSpecular <= 0.5) continue;
        float3 dl = l.pos - s.pos;
        float dist = length(dl);
        float3 ld = dl / max(dist, 1e-4);
        float atten = PunctualFalloff(dist, l.invRange, (int)l.falloff);
        if (l.cosOuter > -1.0) atten *= smoothstep(l.cosOuter, l.cosInner, dot(-ld, l.dir));
        else if (l.isArea > 0.5) atten *= max(0.0, dot(l.dir, -ld));
        float nl2 = dot(s.n, ld);
        if (atten <= 0.0 || nl2 <= 0.0) continue;
        float3 hh = normalize(ld + V);
        c += norm * pow(saturate(dot(s.n, hh)), e) * FresnelDielectric(saturate(dot(hh, V)), ior) * PI * l.color *
             atten * nl2 * MediumShadowT(s.pos, ld, max(dist - 0.05, 0.0), rng);
    }
    return c;
}

// Direct light toward the camera (or a mirror): toon key for characters, Lambert sun for the
// stage, the punctual lights. `pSpec` is the probability of the specular continuation.
float3 CameraDirect(Surf s, float3 V, float pSpec, inout uint rng) {
    float3 c = s.emission;
    if (s.pbr) {
        // PBR record: MMD sun structure with GGX, like the raster / PT. No texture-hue skin heuristic here
        // (SkinWeight thresholds the mip-filtered base texture: blocky translucency patches on mapped skin); the
        // record's SKIN model carries its own SSS term.
        c += PbrSunDirect(s, V, SunVisibility(s, rng)) + s.pb.emissive;
    } else if (s.character) {
        float skin = SkinWeight(s);
        if (skin > 0.01) {
            c += ToonSun(s, V, SkinSunVisibility(s, skin, rng), skin);
            c += SkinTranslucency(s, skin, rng);
        } else {
            c += ToonSun(s, V, SunVisibility(s, rng), 0.0);
        }
    } else {
        float3 L = -gLightDir;
        float ndl = dot(s.n, L);
        if (ndl > 0.0 && dot(s.faceN, L) > 0.0)
            c += (1.0 - pSpec) * (s.pbr ? s.pb.diffuse : s.albedo) / PI * SunIrradiance() * ndl * SunVisibility(s, rng);
    }
    if (gNumLights >= 1.0) {
        float3 diffE = 0, specR = 0;
        PunctualLighting(s, V, s.character || s.pbr, pSpec, rng, diffE, specR);
        c += (1.0 - pSpec) * (s.pbr ? s.pb.diffuse : s.albedo) / PI * diffE + specR;
    }
    return c;
}

// Faces (MAT_FLAT): even environment fill gathered around the view direction, ignoring nearby
// geometry (nose, fringe), like the raster flat fill but lit by the scene.

// ---- irradiance cache (Cinema 4D-style IC prepass, used by the render) --------------------------
// Six passes place irradiance samples on screen grids from coarse to fine (stride halves per pass).
// A point at a finer level gets its own sample only where the geometry changes (normal / depth
// against the parent level) or the parent irradiance varies; elsewhere it inherits the parent's
// interpolated value. Every sample is the INDIRECT irradiance of its surface point, gathered with
// multi-bounce diffuse paths (faces: the even fill around the view direction). An edge-aware smoothing
// of the finest level gives the cache CSRender reads: indirect diffuse at camera hits comes from it
// (looked up by projecting the hit into the frame's camera, so it serves the lens / shutter samples
// too); direct light, reflections, depth of field and motion blur stay per-pixel ray traced, and hits
// the cache does not cover fall back to brute-force paths.
//   CSPrepassLevel   gP0 = (seed, studioFloor, level, stride), gP1 = (w, h, parent stride, rays)
//                    t0..t5 level indirect irradiance (a = 1 where sampled), t6 parent level geometry
//                    u6 this level's irradiance, u7 this level's geometry (oct view normal, view z, hit)
//   CSPrepassDisplay gP0 = (seed, studioFloor, level, stride), gP1 = (w, h, 0, 0), gP3.x = dot radius
//                    t0..t5 level irradiance, t6 this level's geometry; writes u0 (accum, 1 sample):
//                    the scene lit by direct light + the cache, sample points as white dots
//   CSPrepassSmooth  t5 finest level irradiance, t6 its geometry; u6 = the final cache
//   CSRender         t6 finest level geometry, t7 final cache, gP2.w = 1 when the cache is valid,
//                    gP3.x = max path depth (0: kMaxDepth), gP3.y = finest level stride
Texture2D<float4> gLevelE[6] : register(t0);
Texture2D<float4> gLevelG : register(t6);
Texture2D<float4> gIcFinal : register(t7);
RWTexture2D<float4> gPreE : register(u6);
RWTexture2D<float4> gPreG : register(u7);

static const uint kGatherDepth = 4u;   // diffuse bounces of a cache sample's gather paths

// Radiance arriving along a ray from diffusely lit surroundings: a brute-force diffuse path with
// next-event estimation at every vertex (Russian roulette from the second bounce).
float3 GatherPath(float3 o, float3 d, float tMin, inout uint rng) {
    float3 L = 0, T = 1;
    int ch = -1;
    uint glassEvents = 0u;
    uint medEvents = 0;   // S9c: medium events of the whole gather path (MediumMaxEvents, then transmittance)
    [loop] for (uint depth = 0; depth < kGatherDepth; ++depth) {
        Surf y;
        float t;
        bool hit = TraceScene(o, d, depth == 0u ? tMin : 0.0, rng, 1.0, y, t);
        // S9c: the gather paths run the same medium (the cache then holds fogged irradiance): a scattering event adds its
        // NEE and bends the ray, so the segment is traced again from the event point.
        bool ended = false;
        if (MediumOn()) {
            [loop] for (uint guard = 0; guard <= kMediumMaxEvents; ++guard) {
                const int step = MediumStep(o, d, hit ? t : 1e30, T, L, medEvents, rng, false);
                if (step == 0) break;
                if (step == 2) {
                    ended = true;
                    break;
                }
                hit = TraceScene(o, d, 0.0, rng, 1.0, y, t);
            }
        }
        if (ended) break;
        if (!hit) {
            L += T * Background(d);
            break;
        }
        if (y.emitter) {
            L += T * y.emission;
            break;
        }
        if (y.glass) {   // crossings do not count as diffuse bounces (at most 8 per path)
            if (y.inside) T *= exp(-gGlassAbsorb.xyz * t);
            bool refr;
            d = GlassScatter(d, y.n, y.inside, ch, T, rng, refr);
            o = OffsetRayOrigin(y.pos, refr ? -y.faceN : y.faceN);
            if (++glassEvents < 8u) --depth;
            continue;
        }
        L += T * (y.emission + (y.pbr ? y.pb.diffuse : y.albedo) / PI * DirectIrradiance(y, rng));
        float3 nd = CosineSampleHemisphere(float2(Rand(rng), Rand(rng)), y.n);
        if (dot(nd, y.faceN) <= 0.0) break;
        T *= (y.pbr ? y.pb.diffuse : y.albedo);
        o = OffsetRayOrigin(y.pos, y.faceN);
        d = nd;
        if (depth >= 1u) {
            float q = min(max(T.r, max(T.g, T.b)), 0.95);
            if (Rand(rng) > q) break;
            T /= q;
        }
    }
    return L;
}

// Irradiance at `q` (pixel units) interpolated from a grid (stride s) with geometry-aware bilinear
// weights; `cover` = bilinear weight on samples of the same surface (0..1).
float3 InterpolateGrid(Texture2D<float4> etex, float s, float2 q, float3 n, float z, float normalMin,
                       float depthTol, out float cover, out float lmin, out float lmax, out bool discontinuity) {
    uint lw, lh;
    gLevelG.GetDimensions(lw, lh);
    float2 pc = q / s - 0.5;
    int2 p0 = (int2)floor(pc);
    float2 f = pc - (float2)p0;
    float3 E = 0;
    lmin = 1e9;
    lmax = 0;
    discontinuity = false;
    cover = 0;
    [unroll] for (int c = 0; c < 4; ++c) {
        int2 off = int2(c & 1, c >> 1);
        int2 pp = clamp(p0 + off, int2(0, 0), int2(lw - 1, lh - 1));
        float4 g = gLevelG[pp];
        float wb = (off.x ? f.x : 1.0 - f.x) * (off.y ? f.y : 1.0 - f.y) + 1e-4;
        bool same = g.w > 0.5 && dot(OctDecode(g.xy), n) > normalMin && abs(g.z - z) < depthTol * z;
        if (!same) {
            discontinuity = true;
            continue;
        }
        float3 e = etex[pp].rgb;
        float l = Luminance(e);
        lmin = min(lmin, l);
        lmax = max(lmax, l);
        E += e * wb;
        cover += wb;
    }
    return cover > 1e-3 ? E / cover : 0.0;
}

// Indirect irradiance of a camera hit from the final cache (false when not covered). S2b: never across a
// transmissive interface -- the cache holds the irradiance of the opaque surfaces it sampled, so a dielectric vertex
// is always brute force.
bool IcLookup(Surf s, out float3 E) {
    E = 0;
    if (s.pbr && PtPbrIsTransmissive(s.pb)) return false;
    float4 c = mul(float4(s.pos, 1.0), gViewProjNoJitter);
    if (c.w <= 0.0) return false;
    float2 ndc = c.xy / c.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0)) return false;
    float3 vn = normalize(mul(s.n, (float3x3)gView));
    float z = mul(float4(s.pos, 1.0), gView).z;
    float cover, lmin, lmax;
    bool disc;
    E = InterpolateGrid(gIcFinal, gP3.y, uv * gViewportSize, vn, z, 0.85, 0.03, cover, lmin, lmax, disc);
    return cover > 0.5;
}

// One thread group per cache sample: the gather paths are spread over the group's lanes (a thread per sample
// left the GPU idle on the coarse levels, a few hundred samples each running every path serially). All lanes
// trace the same camera ray with the same seed, so they agree on the hit and on `need` (uniform branches).
static const uint kPrepassLanes = 32u;
groupshared float3 gsPrepassInd[kPrepassLanes];

[numthreads(32, 1, 1)]
void CSPrepassLevel(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex) {
    uint W = (uint)gP1.x, H = (uint)gP1.y;
    uint level = (uint)gP0.z;
    float s = gP0.w, ps = gP1.z;
    uint2 dims = (uint2(W, H) + (uint)s - 1u) / (uint)s;
    if (gid.x >= dims.x || gid.y >= dims.y) return;
    float2 q = min((float2)gid.xy * s + s * 0.5, float2(W, H) - 0.5);
    uint rng = RngSeed(gid.xy, (uint)gP0.x, 17u);
    float3 o, d;
    CameraRay(q, float2(0.5, 0.5), o, d);
    Surf sf;
    float t;
    if (!TraceScene(o, d, gNearZ, rng, 0.0, sf, t, true) || sf.emitter) {
        if (lane == 0u) {
            gPreG[gid.xy] = float4(0, 0, 1e6, 0);
            gPreE[gid.xy] = 0;
        }
        return;
    }
    float3 vn = normalize(mul(sf.n, (float3x3)gView));
    float z = mul(float4(sf.pos, 1.0), gView).z;
    if (lane == 0u) gPreG[gid.xy] = float4(OctEncode(vn), z, 1.0);

    bool need = level == 0u;
    float3 Ei = 0;
    if (!need) {
        float cover, lmin, lmax;
        bool disc;
        Ei = InterpolateGrid(gLevelE[level - 1u], ps, q, vn, z, 0.75, 0.06, cover, lmin, lmax, disc);
        if (cover <= 1e-3 || disc || lmax > lmin * 1.8 + 0.05) need = true;
    }
    if (!need) {
        if (lane == 0u) gPreE[gid.xy] = float4(Ei, 0.0);
        return;
    }
    // indirect irradiance: multi-bounce diffuse gather (faces: around the view direction, nearby
    // geometry skipped, like the render's face fill)
    // PT pack surfaces: the pack's flatFace decides at render time (not known here), so gather around the normal
    bool face = sf.character && sf.flat && (sf.g.flags & RTG_PT_PACK) == 0;
    float3 axis = face ? -d : sf.n;
    float tMin = face ? kFlatGatherSkip : 0.0;
    uint rays = max((uint)gP1.w, 1u);
    uint prng = RngSeed(gid.xy, (uint)gP0.x, 29u + lane);
    float3 ind = 0;
    [loop] for (uint i = lane; i < rays; i += kPrepassLanes) {
        float3 dir = CosineSampleHemisphere(float2(Rand(prng), Rand(prng)), axis);
        if (!face && dot(dir, sf.faceN) <= 0.0) continue;
        ind += GatherPath(OffsetRayOrigin(sf.pos, sf.faceN), dir, tMin, prng);
    }
    gsPrepassInd[lane] = ind;
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u) {
        float3 sum = 0;
        [loop] for (uint k = 0u; k < kPrepassLanes; ++k) sum += gsPrepassInd[k];
        gPreE[gid.xy] = float4(PI * sum / (float)rays, 1.0);
    }
}

[numthreads(8, 8, 1)]
void CSPrepassDisplay(uint3 id : SV_DispatchThreadID) {
    uint W = (uint)gP1.x, H = (uint)gP1.y;
    if (id.x >= W || id.y >= H) return;
    uint level = (uint)gP0.z;
    float s = gP0.w;
    float2 q = (float2)id.xy + 0.5;
    uint rng = RngSeed(id.xy, (uint)gP0.x, 19u);
    float3 o, d;
    CameraRay(q, float2(0.5, 0.5), o, d);
    Surf sf;
    float t;
    float3 color;
    if (!TraceScene(o, d, gNearZ, rng, 0.0, sf, t, true)) {
        color = Background(d);
    } else if (sf.emitter) {
        color = sf.emission;
    } else {
        float3 vn = normalize(mul(sf.n, (float3x3)gView));
        float z = mul(float4(sf.pos, 1.0), gView).z;
        float cover, lmin, lmax;
        bool disc;
        float3 E = InterpolateGrid(gLevelE[level], s, q, vn, z, 0.75, 0.06, cover, lmin, lmax, disc);
        if (cover <= 1e-3) {
            uint lw, lh;
            gLevelG.GetDimensions(lw, lh);
            E = gLevelE[level][min((uint2)(q / s), uint2(lw - 1, lh - 1))].rgb;   // nearest sample
        }
        color = sf.emission + sf.albedo / PI * (DirectIrradiance(sf, rng) + E);
    }
    // sample points of every finished level as white dots
    [loop] for (uint l = 0; l <= level; ++l) {
        float sl = s * (float)(1u << (level - l));
        uint lw, lh;
        gLevelE[l].GetDimensions(lw, lh);
        uint2 cell = min((uint2)(q / sl), uint2(lw - 1, lh - 1));
        float2 c = min((float2)cell * sl + sl * 0.5, float2(W, H) - 0.5);
        float radius = clamp(0.22 * sl, 0.75, gP3.x);   // finer passes leave smaller specks
        if (length(q - c) < radius && gLevelE[l][cell].a > 0.5) color = float3(1.4, 1.4, 1.4);
    }
    gAccum[id.xy] = float4(color, 1.0);
}

// Edge-aware smoothing of the finest level (the cache's interpolation noise), into the final cache.
[numthreads(8, 8, 1)]
void CSPrepassSmooth(uint3 id : SV_DispatchThreadID) {
    uint lw, lh;
    gLevelG.GetDimensions(lw, lh);
    if (id.x >= lw || id.y >= lh) return;
    float4 g0 = gLevelG[id.xy];
    float4 e0 = gLevelE[5][id.xy];
    if (g0.w < 0.5) {
        gPreE[id.xy] = e0;
        return;
    }
    float3 n0 = OctDecode(g0.xy);
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int dy = -2; dy <= 2; ++dy) {
        [unroll] for (int dx = -2; dx <= 2; ++dx) {
            int2 p = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(lw - 1, lh - 1));
            float4 g = gLevelG[p];
            if (g.w < 0.5) continue;
            float w = exp(-(float)(dx * dx + dy * dy) / 4.5) * pow(saturate(dot(n0, OctDecode(g.xy))), 8.0) *
                      exp(-abs(g.z - g0.z) / (0.02 * g0.z + 1e-3));
            sum += gLevelE[5][p].rgb * w;
            wsum += w;
        }
    }
    gPreE[id.xy] = float4(sum / max(wsum, 1e-6), e0.a);
}

// ---- kernels ----------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CSClearImage(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)gP1.x || id.y >= (uint)gP1.y) return;
    gAccum[id.xy] = 0.0;
    gAlbedoAccum[id.xy] = 0.0;
    gMoments[id.xy] = 0.0;
    gGbuffer[id.xy] = float4(0, 0, 0, 1e6);
    gEdgeAccum[id.xy] = 0.0;
}

[numthreads(1, 1, 1)]
void CSClearCounter(uint3 id : SV_DispatchThreadID) {
    gCounter[uint2(0, 0)] = 0u;
}

// ---- sampling tail: active tile compaction --------------------------------------------------------
// Once few pixels still sample, a whole-image dispatch keeps a full wave busy for every scattered active
// pixel. The tile list (R32_UINT, tilesX x (tilesY + 1)) holds the 8x8 tiles with a pixel still sampling
// (x | y << 16); texel (0, tilesY) is their count.
//   CSClearTiles   u7 list: count = 0
//   CSActiveTiles  gP0 = (seed, studioFloor, 0, minSamples), gP1 = (w, h, 0, error threshold); a group per tile
//   CSRender       gP3.z = 1: groups walk the list (t5) instead of the whole image
RWTexture2D<uint> gTileListW : register(u7);
Texture2D<uint> gTileList : register(t5);

bool PixelConverged(uint2 pix) {
    float4 acc = gAccum[pix];
    if (acc.a < gP0.w) return false;
    float2 m = gMoments[pix] / acc.a;
    return sqrt(max(m.y - m.x * m.x, 0.0) / acc.a) < gP1.w;
}

[numthreads(1, 1, 1)]
void CSClearTiles(uint3 id : SV_DispatchThreadID) {
    uint tw, th;
    gTileListW.GetDimensions(tw, th);
    gTileListW[uint2(0, th - 1u)] = 0u;
}

groupshared uint gsTileActive;

[numthreads(8, 8, 1)]
void CSActiveTiles(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0u) gsTileActive = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint2 pix = gid.xy * 8u + gtid.xy;
    if (pix.x < (uint)gP1.x && pix.y < (uint)gP1.y && !PixelConverged(pix)) InterlockedOr(gsTileActive, 1u);
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u && gsTileActive != 0u) {
        uint tw, th;
        gTileListW.GetDimensions(tw, th);
        uint idx;
        InterlockedAdd(gTileListW[uint2(0, th - 1u)], 1u, idx);
        gTileListW[uint2(idx % tw, idx / tw)] = gid.x | (gid.y << 16);
    }
}

// One camera path per pixel (pixels that already converged return early).
[numthreads(8, 8, 1)]
void CSRender(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    uint2 pix = id.xy;
    if (gP3.z > 0.5) {   // tile list mode (the sampling tail)
        uint tw, th;
        gTileList.GetDimensions(tw, th);
        uint idx = gid.y * tw + gid.x;
        if (idx >= gTileList[uint2(0, th - 1u)]) return;
        uint tile = gTileList[uint2(idx % tw, idx / tw)];
        pix = uint2(tile & 0xFFFFu, tile >> 16) * 8u + gtid.xy;
    }
    if (pix.x >= (uint)gP1.x || pix.y >= (uint)gP1.y) return;
    uint sampleIndex = (uint)gP0.z;
    float4 acc = gAccum[pix];
    if (sampleIndex > 0u && acc.a >= gP0.w) {
        float2 m = gMoments[pix] / acc.a;
        float err = sqrt(max(m.y - m.x * m.x, 0.0) / acc.a);
        if (err < gP1.w) return;
    }
    uint active = WaveActiveCountBits(true);
    if (WaveIsFirstLane()) InterlockedAdd(gCounter[uint2(0, 0)], active);

    uint rng = RngSeed(pix, (uint)gP0.x, 3u);
    float2 sub = sampleIndex == 0u ? float2(0.5, 0.5) : float2(Rand(rng), Rand(rng));
    float2 lensU = float2(Rand(rng), Rand(rng));
    float3 o, d;
    CameraRay(pix + sub, lensU, o, d);
    const float3 d0 = d;

    float3 radiance = 0, T = 1;
    bool diffuseChain = false;   // a diffuse bounce happened: plain Lambert NEE from here on
    bool specChain = true;       // only glass crossings so far: the next opaque hit is the "primary" one
    int ch = -1;                 // hero colour channel once the path refracted (dispersion)
    bool viaMesh = false;        // S2b: the path went through a mesh dielectric (sample mips, like PT after bounce 0)
    float tMinNext = 0.0;
    bool primHit = false;
    bool primLocked = false;     // S9c: a medium event already wrote the guides (item 7)
    float3 primAlbedo = 1, primN = 0;
    float primZ = 1e6, primT = 0.0, chainT = 0.0;
    uint primId = 0;
    uint medEvents = 0;          // S9c: medium scattering events this path (MediumMaxEvents, then transmittance)
    const uint maxDepth = gP3.x >= 1.0 ? min((uint)gP3.x, kMaxDepth) : kMaxDepth;
    [loop] for (uint depth = 0; depth <= maxDepth; ++depth) {
        Surf s;
        float t;
        bool ended = false;      // S9c: the medium ended the path (albedo roulette / the validation build)
        // mip 0 only for the camera / mirror chain; a mesh dielectric's refracted ray reads mips, so the surface seen
        // through a glass block is filtered (pathtrace.hlsl samples mip 2 after bounce 0 the same way)
        const float lod = viaMesh ? 2.0 : (specChain ? 0.0 : 1.0);
        bool hit = TraceScene(o, d, depth == 0u ? gNearZ : tMinNext, rng, lod, s, t);
        // S9c: every path segment travels through the medium: delta tracking up to the hit (or to the haze sphere's exit
        // for a miss); a scattering event adds its NEE and bends the ray, so the scene is traced again from it.
        if (MediumOn()) {
            [loop] for (uint guard = 0; guard <= kMediumMaxEvents; ++guard) {
                const int step = MediumStep(o, d, hit ? t : 1e30, T, radiance, medEvents, rng,
                                            depth == 0u && specChain && !primLocked);
                if (step == 0) break;
                if (step == 2) {
                    ended = true;
                    break;
                }
                if (specChain && !primLocked) {   // the first interaction of a camera ray is in the medium: the guides
                    primHit = true;
                    primLocked = true;
                    primAlbedo = MediumAlbedo(o);
                    primN = normalize(-d0);       // the view direction as the normal guide
                    primZ = max(mul(float4(o, 1.0), gView).z, gNearZ);
                    primT = primZ / max(dot(d0, gInvView[2].xyz), 1e-3);   // (depth = distance x the view axis)
                }
                hit = TraceScene(o, d, 0.0, rng, lod, s, t);
            }
        }
        if (ended) break;
        if (!hit) {
            // transparent export: a primary camera ray (still before its first opaque hit, glass included) that
            // leaves the scene outputs the background override; indirect / bounce misses keep the real sky.
            radiance += T * ((gBgOverride.w > 0.5 && specChain) ? gBgOverride.xyz : Background(d));
            break;
        }
        if (s.emitter) {   // softboxes are seen by rays only (no light sampling), so every hit counts
            radiance += T * s.emission;
            break;
        }
        if (specChain) chainT += t;
        if (s.glass) {
            if (s.inside) T *= exp(-gGlassAbsorb.xyz * t);
            else if (!diffuseChain) radiance += T * GlassGlint(s, -d, rng);
            if (depth == maxDepth) break;
            bool refr;
            d = GlassScatter(d, s.n, s.inside, ch, T, rng, refr);
            o = OffsetRayOrigin(s.pos, refr ? -s.faceN : s.faceN);
            tMinNext = 0.0;
            continue;
        }
        const bool viaGlass = specChain && depth > 0u;
#ifdef MMDX_PT_PACK
        PtPackOut ptPackOut = (PtPackOut)0;
#ifdef PT_PACK_HAS_LIGHTING
        PtPackLighting ptPackLgt;
        ptPackLgt.flatIndirect = 0.0;    // non-pack surfaces: follow s.flat
        ptPackLgt.sunShadow = 1.0;
        ptPackLgt.lit = -1.0;
#endif
        bool isPtPackHit = (s.g.flags & RTG_PT_PACK) != 0;
        float ptSunVis = 0.0;
        if (isPtPackHit) {
            RtPtPackRecord rec = LoadPtPackRecord(s.g.packSrv);
            if (rec.materialClass != PACK_CLASS_OFF) {
                PtPackBindRecord(rec);
                PtPackBindGeometry(s.g);
                PtPackIn packIn;
                packIn.pos = s.pos;
                packIn.normal = s.n;
                packIn.V = -d;
                packIn.uv = s.uv;
                packIn.uv1 = s.uv1;
                packIn.uv2 = s.uv2;
                packIn.L = -gLightDir;
                if (!diffuseChain) {
                    float3 sv = SunVisibility(s, rng);
                    ptSunVis = Luminance(sv);
                }
                packIn.sunVis = ptSunVis;
                packIn.baseColor = SrgbToLinear(saturate(s.tex.rgb * s.g.diffuse.rgb));
                packIn.materialClass = rec.materialClass;
                [unroll] for (int p = 0; p < 16; ++p)
                    packIn.params[p] = rec.params[p >> 2][p & 3];
                packIn.headRight = rec.headRight.xyz;
                packIn.headPos = rec.headPos.xyz;
                packIn.headScale = rec.headPos.w;
                packIn.headUp = rec.headUp.xyz;
                packIn.headForward = rec.headForward.xyz;
                packIn.headValid = (rec.headValid != 0);

                ptPackOut = PackEvaluate(packIn);
                s.albedo = ptPackOut.albedo;
                s.flat = ptPackOut.flatFace;   // the pack decides, not MAT_FLAT (as in pathtrace.hlsl)
#if defined(PACK_CAP_SURFACE_OUTPUT) && PACK_CAP_SURFACE_OUTPUT
                s.n = normalize(ptPackOut.normal);
                s.emission += ptPackOut.emission;
#endif
#ifdef PT_PACK_HAS_LIGHTING
                ptPackLgt = PackEvaluateLighting(packIn, ptPackOut);
#endif
            } else {
                isPtPackHit = false;
            }
        }
#endif
        // PBR record is used unless a PT pack class wins the hit (raster rule).
        bool isPbrHit = s.pbr;
#ifdef MMDX_PT_PACK
        if (isPtPackHit) { isPbrHit = false; s.pbr = false; }
#endif
        if (specChain) {
            if (!primLocked) {   // S9c: a medium event before the surface kept the guides (item 7)
                primHit = true;
                primAlbedo = s.albedo;
                primN = s.n;
                // virtual depth along the glass chain (a reflection may hit something behind the camera)
                primZ = max(chainT * dot(d0, gInvView[2].xyz), gNearZ);
                primT = chainT;
                uint modelIdx = (s.g.flags >> 24) & 0xFFu;
                uint isChar = (s.g.flags & RTG_CHARACTER) ? 0x80u : 0u;
                uint packClass = 7u;
                uint tag = 0u;
#ifdef MMDX_PT_PACK
                if (isPtPackHit) {
                    RtPtPackRecord rec = LoadPtPackRecord(s.g.packSrv);
                    packClass = rec.materialClass & 0x7u;
#if defined(PACK_CAP_SURFACE_OUTPUT) && PACK_CAP_SURFACE_OUTPUT
                    tag = ptPackOut.tag & 0xFu;
#endif
                }
#endif
                primId = (tag & 0xFu) | ((packClass & 0x7u) << 4) | isChar | (modelIdx << 8);
            }
            specChain = false;
        }

        // S2b mesh dielectric: a transmissive PBR hit continues with the transmission event (probability = the
        // transmission factor) or the ordinary surface below -- the same policy as pathtrace.hlsl. The transmission
        // event keeps the surface out of the irradiance cache (it is not a diffuse vertex, and the cache must never
        // be read across the interface), so that path is brute force.
        // The roulette value is derived from the current state without advancing the path's stream, so a scene
        // without transmissive materials consumes exactly the random numbers it always did.
        if (isPbrHit && PtPbrIsTransmissive(s.pb) &&
            RandValue(rng) < saturate(s.pb.transmission)) {
            PtPbrTransmission tx = PtPbrTransmit(s.pb, d, s.faceN, s.backFace, t, float2(Rand(rng), Rand(rng)));
            if (!s.backFace || s.pb.thick <= 0.0) radiance += T * FresnelGlint(s, -d, rng);
            T *= tx.refracted ? tx.tint * tx.absorb : tx.absorb;
            viaMesh = true;
            if (depth == maxDepth) break;
            d = tx.dir;
            o = OffsetRayOrigin(s.pos, tx.refracted ? -s.faceN : s.faceN);
            tMinNext = 0.0;
            continue;
        }
        float3 V = -d;
        float cosV = saturate(dot(s.n, V));
        float pSpec = s.refl > 0.001 ? saturate(s.refl + (1.0 - s.refl) * pow(1.0 - cosV, 5.0) * s.refl) : 0.0;
        if (isPbrHit) pSpec = PtPbrSpecProb(s.pb, V);
        if (!diffuseChain) {
#ifdef MMDX_PT_PACK
            if (isPtPackHit) {
#ifdef PT_PACK_HAS_LIGHTING
                float3 sunDirect = PtPackComposeSunDirect(ptPackOut, ptPackLgt, s.n, ptSunVis, s.flat);
#else
                float3 sunDirect = PtPackComposeSunDirect(ptPackOut, s.n, ptSunVis, s.flat);
#endif
                if (gNumLights >= 1.0)
                    sunDirect += (1.0 - pSpec) * s.albedo / PI * PunctualIrradiance(s, s.character, rng);
                radiance += T * (s.emission + sunDirect);
            } else
#endif
            {
                if (isPbrHit)
                    radiance += T * (CameraDirect(s, V, pSpec, rng) + s.pb.emissive);
                else
                    radiance += T * CameraDirect(s, V, pSpec, rng);
            }
        } else {
            float3 sunE = 0;
            float3 L = -gLightDir;
            float ndl = dot(s.n, L);
            if (ndl > 0.0 && dot(s.faceN, L) > 0.0) sunE = SunIrradiance() * ndl * SunVisibility(s, rng);
            float3 diffE = 0, specR = 0;
            if (gNumLights >= 1.0)
                PunctualLighting(s, V, false, pSpec, rng, diffE, specR);
            radiance += T * (s.emission + (isPbrHit ? s.pb.emissive : 0.0) + (1.0 - pSpec) * (isPbrHit ? s.pb.diffuse : s.albedo) / PI * (sunE + diffE) + specR);
        }
        if (depth == maxDepth) break;

        // indirect diffuse from the irradiance cache (the prepass); specular lobe still traced
        if ((depth == 0u || viaGlass) && gP2.w > 0.5) {
            float3 Eic;
            if (IcLookup(s, Eic)) {
                float flatIndirect = s.flat;
#ifdef PT_PACK_HAS_LIGHTING
                if (isPtPackHit) flatIndirect = ptPackLgt.flatIndirect;
#endif
                if (s.character && flatIndirect) {
                    radiance += T * s.albedo * kToonIndirect * Eic / PI;
                    break;
                }
                float3 indAlbedo = isPbrHit ? s.pb.diffuse : s.albedo;
                float indScale = (isPbrHit ? s.pb.ao : 1.0) * (s.character ? kToonIndirect : 1.0);
                radiance += T * (1.0 - pSpec) * indAlbedo * indScale * Eic / PI;
                if (Rand(rng) >= pSpec) break;
                float3 sd;
                if (isPbrHit) {
                    float3 w;
                    PtPbrSampleSpecular(s.pb, V, float2(Rand(rng), Rand(rng)), sd, w);
                    T *= w / pSpec;
                } else {
                    sd = SampleCone(float2(Rand(rng), Rand(rng)), reflect(d, s.n), cos(s.rough * 0.5));
                }
                if (dot(sd, s.faceN) <= 0.0) break;
                o = OffsetRayOrigin(s.pos, s.faceN);
                d = sd;
                tMinNext = 0.0;
                continue;
            }
        }

        float3 nd;
        tMinNext = 0.0;
        float flatIndirect = s.flat;
#ifdef PT_PACK_HAS_LIGHTING
        if (isPtPackHit) flatIndirect = ptPackLgt.flatIndirect;
#endif
        if (s.character && flatIndirect && !diffuseChain) {
            // faces: even fill around the view direction, skipping nearby geometry (nose, fringe)
            nd = CosineSampleHemisphere(float2(Rand(rng), Rand(rng)), V);
            T *= s.albedo * kToonIndirect;
            diffuseChain = true;
            tMinNext = kFlatGatherSkip;
        } else {
            if (isPbrHit) {
                // PBR continuation: VNDF specular vs cosine diffuse (same pSpec).
                if (Rand(rng) < pSpec) {
                    float3 w;
                    PtPbrSampleSpecular(s.pb, V, float2(Rand(rng), Rand(rng)), nd, w);
                    T *= w / pSpec;
                } else {
                    nd = CosineSampleHemisphere(float2(Rand(rng), Rand(rng)), s.n);
                    T *= s.pb.diffuse * s.pb.ao * ((s.character && !diffuseChain) ? kToonIndirect : 1.0) / (1.0 - pSpec);
                    diffuseChain = true;
                }
            } else if (Rand(rng) < pSpec) {
                nd = SampleCone(float2(Rand(rng), Rand(rng)), reflect(d, s.n), cos(s.rough * 0.5));
            } else {
                nd = CosineSampleHemisphere(float2(Rand(rng), Rand(rng)), s.n);
                T *= s.albedo * ((s.character && !diffuseChain) ? kToonIndirect : 1.0);
                diffuseChain = true;
            }
            if (dot(nd, s.faceN) <= 0.0) break;
        }
        o = OffsetRayOrigin(s.pos, s.faceN);
        d = nd;
        if (depth >= 3u) {
            float q = min(max(T.r, max(T.g, T.b)), 0.95);
            if (Rand(rng) > q) break;
            T /= q;
        }
    }

    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = 0;   // also guards the firefly clamp (inf * 0)
    if (primHit) {
        // aerial perspective toward the horizon colour, same curve as the raster haze
        float f = (1.0 - exp(-primT * kHaze * 0.0011)) * 0.85;
        radiance = lerp(radiance, SkyColor(float3(d0.x, max(d0.y, 0.0) * 0.3, d0.z)), f);
    }
    float lum = Luminance(radiance);
    if (lum > kFirefly) radiance *= kFirefly / lum;
    gAccum[pix] = acc + float4(radiance, 1.0);
    gAlbedoAccum[pix] = gAlbedoAccum[pix] + float4(primAlbedo, primHit ? 1.0 : 0.0);
    float tl = Perceptual(radiance);
    gMoments[pix] = gMoments[pix] + float2(tl, tl * tl);
    if (sampleIndex == 0u) {
        gGbuffer[pix] = primHit ? float4(primN, primZ) : float4(0, 0, 0, 1e6);
        gGbufId[pix] = primHit ? primId : 0u;
    }
}

