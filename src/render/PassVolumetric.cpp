// VolumetricPass (volumetric.hlsl, volumetric_apply.hlsl): half-res in-scattering march (sun and
// spot light shadows), temporal accumulation, depth-aware separable blur, depth-aware upsample
// into lit as lit * transmittance + in-scattered light.
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "render/Renderer.h"
#include "core/Log.h"

namespace mmdx {

bool VolumetricPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    if (!RtPipelinesSupported(ctx)) {
        LOG_WARN("Volumetric: compute shaders unavailable");
        return true;
    }
    const bool ok = march_.Create(ctx, shaderDir / L"volumetric.hlsl", "CSMarch") &&
                    temporal_.Create(ctx, shaderDir / L"volumetric.hlsl", "CSTemporal") &&
                    blur_.Create(ctx, shaderDir / L"volumetric.hlsl", "CSBlur") &&
                    apply_.Create(ctx, shaderDir / L"volumetric_apply.hlsl", "PSApply",
                                  {RenderTargets::kColorFormat}, {}, FullscreenPipeline::Blend::Transmittance);
    if (!ok) {
        LOG_WARN("Volumetric: pipelines unavailable");  // never fails the renderer
        march_ = {};
        temporal_ = {};
        blur_ = {};
        apply_ = {};
    }
    return true;
}

void VolumetricPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    const uint32_t w = Half(targets.width), h = Half(targets.height);
    const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    raw_.Create(ctx, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrv, L"volumetric.raw");
    temp_.Create(ctx, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrv, L"volumetric.temp");
    history_[0].Create(ctx, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrv, L"volumetric.history0");
    history_[1].Create(ctx, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrv, L"volumetric.history1");
    historyReady_ = false;
}

void VolumetricPass::ReleaseTargets(Dx12Context& ctx) {
    raw_.Release(ctx);
    temp_.Release(ctx);
    history_[0].Release(ctx);
    history_[1].Release(ctx);
    historyReady_ = false;
}

void VolumetricPass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    if (!pc.settings.volumetric || pc.offscreen || !march_ || !apply_ || !raw_) {
        historyReady_ = false;
        return;
    }
    // S9: the haze sphere (a stage's radius, grown to hold its fog volumes) and a march long enough to cross it from
    // the eye (400 as before while the eye is within 220 of the origin)
    const float mediumRadius = VolumetricMediumRadius(pc.view.light);
    const float maxDistance = VolumetricMaxDistance(pc.view.camera.eye, mediumRadius);
    // S9: the fog's anisotropy (-1 = the built-in constants), the local volumes and the medium of the frame (a stage's
    // environment.fog fills LightParams::volumetric*; the defaults are the constants the pass always used).
    const float fogG = pc.view.light.volumetricAnisotropy > -0.99f ? pc.view.light.volumetricAnisotropy
                                                                 : pc.settings.volumetricAnisotropy;
    const float sunG = pc.view.light.sunVolumetricG > -0.99f
                           ? pc.view.light.sunVolumetricG
                           : (fogG > -0.99f ? fogG : 0.55f);
    const float punctualG = fogG > -0.99f ? fogG : 0.3f;
    const uint32_t fogCount = (uint32_t)std::min<size_t>(pc.view.light.fogVolumes.size(), Renderer::kMaxFogVolumes);

    // 1. march (jittered per frame) -> raw_
    raw_.Transition(cmd, kUav);
    t.depth.Transition(cmd, kSrvAll);
    t.shadowMap.Transition(cmd, kSrvAll);
    t.spotShadowMap.Transition(cmd, kSrvAll);
    // S9: a stage's environment.fog density replaces the built-in 0.005 base; the user's density slider still scales it.
    const float sigma0 = (pc.view.light.volumetricSigma0 >= 0.0f ? pc.view.light.volumetricSigma0 : 0.005f) *
                         pc.settings.volumetricDensity;
    const float falloff = pc.view.light.volumetricFalloff >= 0.0f ? pc.view.light.volumetricFalloff : 0.012f;
    const DirectX::XMFLOAT3& tint = pc.view.light.volumetricColor;
    const float c0[16] = {sigma0, falloff, maxDistance, sunG,
                          1.0f / raw_.width, 1.0f / raw_.height, 6.0f, punctualG,
                          0.25f, 0.45f, (float)fogCount, pc.view.light.sunVolumetric,
                          tint.x, tint.y, tint.z, mediumRadius};
    const uint32_t c0Size = 16;
    march_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&t.depth, &t.shadowMap, &t.spotShadowMap}),
                    pc.transient.UavTable(pc.ctx, {&raw_}), c0, c0Size, Groups(raw_.width), Groups(raw_.height));

    // 2. temporal accumulation -> history_[cur] (history only across consecutive frames, no cuts)
    const bool valid = historyReady_ && pc.frame == lastFrame_ + 1 && !pc.view.cameraCut;
    const uint32_t prev = historyIndex_, cur = historyIndex_ ^ 1u;
    raw_.Transition(cmd, kSrvAll);
    history_[prev].Transition(cmd, kSrvAll);
    history_[cur].Transition(cmd, kUav);
    const float ct[4] = {valid ? 1.0f : 0.0f, 0.12f, maxDistance, 0.0f};
    temporal_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&raw_, &history_[prev], &t.depth}),
                       pc.transient.UavTable(pc.ctx, {&history_[cur]}), ct, 4, Groups(raw_.width), Groups(raw_.height));
    historyIndex_ = cur;
    historyReady_ = true;
    lastFrame_ = pc.frame;

    // 3. light depth-aware blur: history_[cur] -> temp_ -> raw_
    history_[cur].Transition(cmd, kSrvAll);
    temp_.Transition(cmd, kUav);
    const float ch[4] = {1, 0, 0, 0};
    blur_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&history_[cur], &t.depth}),
                   pc.transient.UavTable(pc.ctx, {&temp_}), ch, 4, Groups(raw_.width), Groups(raw_.height));
    temp_.Transition(cmd, kSrvAll);
    raw_.Transition(cmd, kUav);
    const float cv[4] = {0, 1, 0, 0};
    blur_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&temp_, &t.depth}),
                   pc.transient.UavTable(pc.ctx, {&raw_}), cv, 4, Groups(raw_.width), Groups(raw_.height));

    // 4. lit = lit * T + L
    raw_.Transition(cmd, kSrv);
    temp_.Transition(cmd, kSrv);
    history_[cur].Transition(cmd, kSrv);
    history_[prev].Transition(cmd, kSrv);
    t.depth.Transition(cmd, kSrv);
    t.shadowMap.Transition(cmd, kSrv);
    t.spotShadowMap.Transition(cmd, kSrv);
    const float ca[4] = {1.0f / raw_.width, 1.0f / raw_.height, 0, 0};
    t.lit.Transition(cmd, kRt);
    apply_.Draw(pc, {&t.lit}, pc.transient.SrvTable(pc.ctx, {&raw_, &t.depth}), ca, 4);
    t.lit.Transition(cmd, kSrv);
}

} // namespace mmdx
