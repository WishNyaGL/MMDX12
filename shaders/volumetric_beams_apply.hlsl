// Spot beams on top of the froxel result (volumetric_beams.hlsl): additive, so the transmittance blend's alpha is 1
// (lit = lit + L). Same render resolution as lit, so the point sample is exact.
//   PSApplyBeams: t0 beams (full res rgb), gOut = lit (Transmittance blend)
#include "fullscreen.hlsli"
Texture2D<float4> gBeams : register(t0);

float4 PSApplyBeams(FsOut i) : SV_Target {
    return float4(gBeams.SampleLevel(gPoint, i.uv, 0).rgb, 1.0);
}
