// Smooth dielectric interfaces (S2b): Fresnel reflectance, the reflection / Snell-refraction choice, and the
// Beer-Lambert absorption of the volume behind the surface. Shared by the analytic benchmark glass cube
// (offline_glass.hlsli / offline_gi.hlsl GlassScatter), the mesh dielectrics of the ray paths (pathtrace.hlsl,
// offline_gi.hlsl, KHR_materials_transmission / ior / volume) and the raster / RT camera view (mmd.hlsl MmdShadePbr).
// Include common.hlsli first. No bindings, so every shader stage can include it.
#ifndef MMDX_DIELECTRIC_HLSLI
#define MMDX_DIELECTRIC_HLSLI

// Fresnel reflectance of a smooth dielectric interface (unpolarised, average of the s / p terms).
// `eta` = n(transmitted side) / n(incident side).
float FresnelDielectric(float cosI, float eta) {
    float sinT2 = (1.0 - cosI * cosI) / (eta * eta);
    if (sinT2 >= 1.0) return 1.0;
    float cosT = sqrt(1.0 - sinT2);
    float rs = (cosI - eta * cosT) / (cosI + eta * cosT);
    float rp = (eta * cosI - cosT) / (eta * cosI + cosT);
    return 0.5 * (rs * rs + rp * rp);
}

// Snell refraction at a crossing; `n` faces the incoming ray, `inside` = the ray travels inside the body (the
// crossing is an exit, so the ior is inverted). Total internal reflection falls back to a mirror (`refracted` false).
float3 DielectricRefract(float3 d, float3 n, bool inside, float ior, out bool refracted) {
    refracted = false;
    float eta = inside ? 1.0 / max(ior, 1.0) : max(ior, 1.0);
    float3 t = refract(d, n, 1.0 / eta);
    if (dot(t, t) < 1e-8) return reflect(d, n);   // total internal reflection
    refracted = true;
    return normalize(t);
}

// Fresnel-weighted choice between mirror reflection and refraction at a crossing (roulette, `u` in [0,1) is the
// draw). `thin` = thin-walled material: the refracted ray goes straight through without bending (Fresnel still
// applies). Returns the next direction, sets `refracted`.
float3 DielectricScatter(float3 d, float3 n, bool inside, float ior, bool thin, float u, out bool refracted) {
    refracted = false;
    bool inBody = inside && !thin;
    float eta = inBody ? 1.0 / max(ior, 1.0) : max(ior, 1.0);
    float cosI = saturate(dot(-d, n));
    if (u < FresnelDielectric(cosI, eta)) return reflect(d, n);
    if (thin) {
        refracted = true;
        return d;
    }
    float3 t = refract(d, n, 1.0 / eta);
    if (dot(t, t) < 1e-8) return reflect(d, n);   // total internal reflection
    refracted = true;
    return normalize(t);
}

// KHR_materials_volume Beer-Lambert transmittance over `segment` units travelled inside the medium:
// exp(-sigma * segment) with sigma = -ln(attenuationColour) / attenuationDistance. `distance` 0 = no absorption.
// Thin-walled materials pass `segment` 0 (or are gated by the caller): they absorb nothing.
float3 DielectricAbsorb(float3 attenuation, float distance, float segment) {
    if (distance <= 0.0 || segment <= 0.0) return 1.0;
    float3 sigma = -log(max(attenuation, 1e-4)) / distance;
    return exp(-sigma * segment);
}

#endif
