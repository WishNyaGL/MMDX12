// Offline GI renderer, outline layer: MMD inverted-hull edges rasterised at the offline image
// size (4x MSAA), occluded by a depth pre-pass of the same skinned meshes. Drawn once per render
// iteration with that iteration's shutter time and lens position (the tracer uses the same camera,
// offline_common.hlsli), so the averaged layer carries the same motion blur and depth of field.
// The resolved layers (premultiplied linear colour, a = coverage) are accumulated and composited
// over the path-traced image by offline_post.hlsl CSFinalize.
// Root signature (OfflineRenderer.cpp): b0 SceneConstants, b1 MaterialConstants, b2 EdgeCB (root
// constants), t0 bones, t4 previous bones (root SRVs), t1..t3 material table, s0 anisotropic wrap.
// Packs that draw their own outlines (PACK_HAS_EDGE) use offline_edge_pack.hlsl for the edge draw.
#include "common.hlsli"
#include "offline_common.hlsli"
#include "skinning.hlsli"

cbuffer MaterialCB : register(b1) {
    float4 gDiffuse; float3 gSpecular; float gSpecularPower; float3 gAmbient; float gEdgeSize;
    float4 gEdgeColor; uint gFlags; float gReflectivity; uint gPackClass; uint gPackHeadBone;
    // material morph factors (ApplyTexFactor): texture, sphere, toon
    float4 gTexMul; float4 gTexAdd; float4 gSphereMul; float4 gSphereAdd; float4 gToonMul; float4 gToonAdd;
};
#define MAT_HAS_TEXTURE 1u

struct BoneMatrix { row_major float4x4 m; };
StructuredBuffer<BoneMatrix> gBones : register(t0);
StructuredBuffer<BoneMatrix> gPrevBones : register(t4);
Texture2D gTexture : register(t1);
SamplerState gWrap : register(s0);

struct VSIn {
    float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0;
    uint4 bones : BLENDINDICES; float4 weights : BLENDWEIGHT; float edge : TEXCOORD1;
    float3 morph : TEXCOORD2; float3 prevMorph : TEXCOORD3;
    float3 sdefC : TEXCOORD4; float3 sdefR0 : TEXCOORD5; float3 sdefR1 : TEXCOORD6; float sdef : TEXCOORD7;
    float4 uv1 : TEXCOORD8; float4 uv2 : TEXCOORD9;   // additional UV1/UV2 (zero without them)
};

#include "offline_edge_skin.hlsli"   // EdgeCB (b2), SkinAt, ToClip, OfflineEdgeClip

struct DepthOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
// Coverage test like the ray tracer's binary alpha (>= 0.5). Pack alpha key (pack.json "alphaKey"):
// a keyed material does not write depth for its cut pixels. (packClass bits 8..15 hold round(key*255).)
#define PackAlphaKey(gc) (float(((gc) >> 8) & 0xFFu) * (1.0 / 255.0))

DepthOut VSDepth(VSIn v) {
    float3 wp, wn;
    SkinAt(v, wp, wn);
    DepthOut o;
    o.pos = ToClip(wp);
    o.uv = v.uv;
    return o;
}

// Coverage test like the ray tracer's binary alpha (>= 0.5). Pack alpha key (pack.json "alphaKey"):
// a keyed material does not write depth for its cut pixels. (packClass bits 8..15 hold round(key*255).)
void PSDepth(DepthOut i) {
    float4 tex = (gFlags & MAT_HAS_TEXTURE) ? gTexture.Sample(gWrap, i.uv) : float4(1, 1, 1, 1);
    float a = gDiffuse.a * tex.a;
    clip(a - 0.5);
    const float alphaKey = PackAlphaKey(gPackClass);
    if (alphaKey > 0.0) clip(tex.r - alphaKey);
}

float4 VSEdge(VSIn v) : SV_Position {
    float3 wp, wn;
    SkinAt(v, wp, wn);
    float px = gEdgeSize * v.edge * gEdgeScale;   // outline width in pixels (scaled with height / 1080)
    return OfflineEdgeClip(wp, wn, px);
}

// Premultiplied output (blend ONE / INV_SRC_ALPHA). Same colour as mmd.hlsl PSEdge.
float4 PSEdge(float4 pos : SV_Position) : SV_Target {
    float a = gEdgeColor.a;
    return float4(SrgbToLinear(gEdgeColor.rgb) * gSunIntensity * 0.85 * a, a);
}
