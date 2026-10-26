// Local fog volume shapes (S9 / S9c): the shape tests shared by the single-scatter march
// (volumetric_common.hlsli) and the offline GI in-path medium (medium_common.hlsli). No resource declarations here:
// each includer declares its own VolFog buffer (the same layout, GpuFogVolume in ShaderInterop.h).
//   density is added to the global height fog inside the shape (a multiple of the base sigma0), color tints the
//   medium there, edge is the soft border band, sphere != 0 selects the ellipsoid.
#ifndef MMDX_FOG_SHAPES_HLSLI
#define MMDX_FOG_SHAPES_HLSLI

struct VolFog {
    float3 center;  float density;
    float3 extent;  float edge;
    float3 color;   int sphere;
};

// Fraction of the local medium at wp (0 outside, 1 deep inside) with the soft border band `edge`: box = the distance to
// the faces in units of the half extents, ellipsoid = the radius in units of the radii.
float VolFogShape(const VolFog f, float3 wp) {
    float3 e = max(abs(f.extent), 1e-4);
    float m;
    if (f.sphere != 0) {
        m = length((wp - f.center) / e);
    } else {
        float3 q = abs(wp - f.center) / e;
        m = max(max(q.x, q.y), max(q.z, 0.0));
    }
    return saturate((1.0 - m) / max(f.edge, 1e-3));
}

// Ray / axis-aligned ellipsoid (radii e around c) in the forward direction, unclipped (the caller clips to the march
// range): the unit sphere in the scaled space, where t stays the world ray parameter.
float2 VolRayEllipsoidForward(float3 o, float3 d, float3 c, float3 e) {
    float3 oc = (o - c) / e;
    float3 ds = d / e;
    float a = dot(ds, ds);
    float b = dot(oc, ds);
    float h = b * b - a * (dot(oc, oc) - 1.0);
    if (h <= 0.0) return float2(1, 0);
    h = sqrt(h);
    return float2((-b - h) / a, (-b + h) / a);
}

// Ray / axis-aligned box (half extents e around c), forward and unclipped.
float2 VolRayBoxForward(float3 o, float3 d, float3 c, float3 e) {
    // an axis-parallel component stays inside its slab: a tiny nudge keeps the division finite
    float3 dd;
    dd.x = abs(d.x) < 1e-6 ? (d.x < 0.0 ? -1e-6 : 1e-6) : d.x;
    dd.y = abs(d.y) < 1e-6 ? (d.y < 0.0 ? -1e-6 : 1e-6) : d.y;
    dd.z = abs(d.z) < 1e-6 ? (d.z < 0.0 ? -1e-6 : 1e-6) : d.z;
    float3 ta = (c - e - o) / dd;
    float3 tb = (c + e - o) / dd;
    float3 tlo = min(ta, tb), thi = max(ta, tb);
    return float2(max(max(tlo.x, tlo.y), tlo.z), min(min(thi.x, thi.y), thi.z));
}

#endif
