// S9d: the froxel result into lit: lit * T + L (Blend::Transmittance). (L, T) comes from the integrated grid at the
// pixel's own view depth: texel k of a column holds the prefix (L, T) accumulated up to that slice's FAR boundary
// (view depth z0 r^(k+1)), so sampling the column at the boundary index of the pixel's depth interpolates the two
// neighbouring prefixes and gives the pixel's own lit haze and transmittance.
//   t0 gFroxel (integrated: rgb = accumulated in-scattering, a = transmittance), t1 depth (full res)
//   gP0 = (gridX, gridY, gridZ, maxDistance)
#include "fullscreen.hlsli"

Texture3D<float4> gFroxel : register(t0);
Texture2D<float> gDepthTex : register(t1);

float4 PSApplyFroxel(FsOut i) : SV_Target {
    const float d = gDepthTex.SampleLevel(gPoint, i.uv, 0);
    const float z0 = max(gNearZ, 0.01), z1 = max(gP0.w, z0 * 1.05);
    const float z = d >= 1.0 ? z1 : clamp(LinearZ(d), z0, z1);
    const float slices = max(gP0.z, 1.0);
    // boundary index of the depth (0 = z0, slices = z1); texel k sits on boundary k + 1, its centre at k + 0.5
    const float b = saturate(log(z / z0) / max(log(z1 / z0), 1e-4)) * slices;
    const float w = saturate((b - 0.5) / slices);
    const float4 v = gFroxel.SampleLevel(gLinear, float3(i.uv, w), 0);
    return float4(v.rgb, saturate(v.a));
}
