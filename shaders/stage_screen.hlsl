// The stage screen "program" pass's shader (StageScreens): compiled per screen pack with the pack's screen.hlsl
// appended (MMDX_PACK is the quoted include path, as effect.hlsl does for effect packs). The pack implements
// PackScreen(PackScreenInput) (contract: screen_api.hlsli / docs/shader_screen_api.md).
//
// The pack returns LINEAR RGB; the screen texture is an RGBA8 UNORM display image (the material shaders read it through
// SrgbToLinear, exactly like an image / video / camera-feed screen), so the result is sRGB-encoded here.
#include "fullscreen.hlsli"
#include "screen_api.hlsli"
#include MMDX_PACK

float4 PSPackScreen(FsOut i) : SV_Target {
    PackScreenInput In;
    In.uv = i.uv;
    In.size = gP0.xy;
    In.time = gP0.z;
    In.frameIndex = gP0.w;
    return float4(LinearToSrgb(saturate(PackScreen(In).rgb)), 1.0);
}
