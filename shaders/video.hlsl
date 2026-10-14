// Video layers of the MV packages (docs/mme/mme_design.md §5b; render/PassVideo.cpp).
//   PSVideoBg      background video: written where the scene has no geometry (depth = far), aspect-fill (cover)
//   PSVideoOverlay overlay video: composited over the final image, aspect-fit, black key / screen / add / normal
// Bindings (FullscreenPipeline t0..t7 table): t0 = the scene image (overlay), t1 = the video frame (BGRA8 as
// RGBA, display space; both), t2 = the scene depth (background, R32_FLOAT).
// Root constants: gP0 = (srcW, srcH, dstW, dstH), gP1 = (kind, blend, 0, 0).
#include "fullscreen.hlsli"

Texture2D<float4> gVideoScene : register(t0);
Texture2D<float4> gVideoCol   : register(t1);
// t2 = the scene depth. gP1.z selects the encoding: 0 = play mode (raw device depth, sky = 1 in .x),
// 1 = offline G-buffer (view depth in .w, sky = 1e6).
Texture2D<float4> gVideoDepth : register(t2);

// Maps a destination uv to a video uv. `cover` = aspect-fill (true) or aspect-fit (false).
// Returns false when the destination pixel falls outside an aspect-fit image (letterbox).
bool VideoUv(float2 uv, bool cover, out float2 outUv) {
    const float srcAspect = gP0.x / max(gP0.y, 1.0);
    const float dstAspect = gP0.z / max(gP0.w, 1.0);
    outUv = uv;
    if (cover) {
        if (dstAspect >= srcAspect) {
            const float f = dstAspect / srcAspect;        // visible fraction of the image height
            outUv.y = (uv.y - 0.5) * f + 0.5;
        } else {
            const float f = srcAspect / dstAspect;
            outUv.x = (uv.x - 0.5) * f + 0.5;
        }
        return true;
    }
    if (dstAspect >= srcAspect) {
        const float f = srcAspect / dstAspect;            // image width as a fraction of the target width
        outUv.x = (uv.x - 0.5) / f + 0.5;
        return outUv.x >= 0.0 && outUv.x <= 1.0;
    }
    const float f = dstAspect / srcAspect;                // image height as a fraction of the target height
    outUv.y = (uv.y - 0.5) / f + 0.5;
    return outUv.y >= 0.0 && outUv.y <= 1.0;
}

// Background: only where the scene did not draw (no geometry).
float4 PSVideoBg(FsOut i) : SV_Target {
    const float4 d = gVideoDepth.SampleLevel(gPoint, i.uv, 0);
    const bool far_ = gP1.z > 0.5 ? (d.w >= 1e5) : (d.x >= 0.9999);
    if (!far_) discard;
    float2 uv;
    VideoUv(i.uv, true, uv);
    float3 c = gVideoCol.SampleLevel(gLinear, saturate(uv), 0).rgb;
    return float4(c, 1.0);
}

// Overlay: composited over the final image (t0), aspect-fit. Black key by default.
float4 PSVideoOverlay(FsOut i) : SV_Target {
    float4 scene = gVideoScene.SampleLevel(gPoint, i.uv, 0);
    float2 uv;
    if (!VideoUv(i.uv, false, uv))
        return scene;
    float3 v = gVideoCol.SampleLevel(gLinear, saturate(uv), 0).rgb;
    const uint blend = (uint)(gP1.y + 0.5);
    if (blend == 3u)   // normal: straight alpha over
        return float4(lerp(scene.rgb, v, scene.a), scene.a);
    if (blend == 2u)   // add
        return float4(scene.rgb + v, scene.a);
    if (blend == 1u)   // screen
        return float4(1.0 - (1.0 - scene.rgb) * (1.0 - v), scene.a);
    // 0 (default): black key
    const float a = smoothstep(0.02, 0.08, max(v.r, max(v.g, v.b)));
    return float4(lerp(scene.rgb, v, a), scene.a);
}

// Straight copy (t0 -> target): grabs the LDR image into the overlay ping-pong scratch.
float4 PSVideoCopy(FsOut i) : SV_Target {
    return gVideoScene.SampleLevel(gPoint, i.uv, 0);
}
