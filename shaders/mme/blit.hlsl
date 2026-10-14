// MME post effects (MmePostPass::ScriptExternal): a fullscreen copy of the chain input into the currently bound
// colour target, accepting a format change (one PSO per destination format, created lazily). Root layout =
// FullscreenPipeline (render/RenderPass.h): the point-clamp static sampler s0 and the t0..t7 SRV table.
Texture2D gBlitSrc : register(t0);
SamplerState gBlitPoint : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSFullscreen(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
float4 PSBlit(VSOut i) : SV_Target { return gBlitSrc.Sample(gBlitPoint, i.uv); }
// MMD's back buffer is effectively XRGB: a copy back into the scene's LDR image must be opaque, otherwise the
// effect's garbage alpha (MMD never uses back-buffer alpha) leaks into the present / capture composite.
float4 PSBlitOpaque(VSOut i) : SV_Target { return float4(gBlitSrc.Sample(gBlitPoint, i.uv).rgb, 1.0); }
