// pt_surface.hlsl: offline GI shader pack template (docs/shader_pt_api.md).
//
// Optional second file in a surface pack, written for the offline GI path tracer (CSRender).
// The full contract is in shaders/pt_pack_api.hlsli.
//
// Authors implement:
//     PtPackOut PackEvaluate(PtPackIn i);
//
// PtPackIn provides:
//   i.pos, i.normal, i.V, i.uv, i.L, i.sunVis, i.baseColor (linear), i.materialClass, i.params[16],
//   head frame (i.headPos, i.headScale, i.headRight, i.headUp, i.headForward, i.headValid).
//
// PtPackOut returns:
//   o.albedo       linear surface albedo used for GI diffuse bounces
//   o.shadowTint   linear multiplier on the shaded side of the terminator
//   o.shadowBias   shifts the terminator in N.L units (a smoothed face normal = dot(Nsmooth, L) - dot(N, L))
//   o.terminator   (lo, hi) N.L edges of the terminator smoothstep; (0, 0) = engine default (-0.12, 0.22)
//   o.specular     additive linear radiance (rim, matcap, highlights)
//   o.flatFace     use engine flat-face handling (no GI gradient)
//
// Optional lighting controls (API 1.9.1): to separate the flat indirect gather from the pack's own
// terminator, or to weaken the traced cast shadow, define PT_PACK_HAS_LIGHTING and implement
//
//   PtPackLighting PackEvaluateLighting(PtPackIn i, PtPackOut o)
//
// returning o.flatIndirect (flat gather), o.sunShadow (0..1 cast-shadow weight), o.lit (direct 0..1, -1 = unused).
// Guard the block with #ifdef PT_PACK_API_LIGHTING so the pack still compiles on older apps.
// #ifdef PT_PACK_API_LIGHTING
// #define PT_PACK_HAS_LIGHTING 1
// #endif
//
// #ifdef PT_PACK_HAS_LIGHTING
// PtPackLighting PackEvaluateLighting(PtPackIn i, PtPackOut o) {
//     PtPackLighting l;
//     l.flatIndirect = o.flatFace ? 1.0 : 0.0;
//     l.sunShadow = 1.0;
//     l.lit = -1.0;
//     return l;
// }
// #endif

PtPackOut PackEvaluate(PtPackIn i) {
    PtPackOut o;
    o.albedo = i.baseColor;
    o.shadowTint = float3(0.7, 0.7, 0.75); // slightly cool shaded side
    o.shadowBias = 0.0;
    o.terminator = float2(0.0, 0.0);       // engine default softness
    o.specular = float3(0.0, 0.0, 0.0);
    o.flatFace = (i.materialClass == PACK_FACE);

    if (i.materialClass == PACK_SKIN) {
        o.shadowTint = float3(0.85, 0.75, 0.75); // warmer skin shadow
    }

    return o;
}
