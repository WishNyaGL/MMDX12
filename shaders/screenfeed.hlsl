// S5 stage screen camera feed: the scene rendered from a stage camera into the screen's RGBA8 texture.
//
// The feed is rendered by the ScenePass draw path (ScenePass::DrawFeed) into HDR targets, then tonemapped and
// sRGB-encoded into the LED texture (the screen then shows it as emission), because the screen texture is a
// display-referred image every render path samples.
//   t0 HDR colour.
#include "fullscreen.hlsli"

Texture2D<float4> gFeed : register(t0);

// Hue-preserving Reinhard (a uniform scale by the luminance, so the feed keeps its colours) then sRGB.
float4 PSFeed(FsOut i) : SV_Target {
    float3 c = max(gFeed.SampleLevel(gLinear, i.uv, 0).rgb, 0.0);
    c /= 1.0 + Luminance(c);
    return float4(LinearToSrgb(saturate(c)), 1.0);
}
