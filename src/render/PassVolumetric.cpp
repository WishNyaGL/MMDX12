// VolumetricPass (volumetric.hlsl, volumetric_apply.hlsl; S9d volumetric_froxel.hlsl): the frame's participating medium.
//   mode 0 (RenderSettings::volumetricMode, the default): a froxel grid (kFroxelX x kFroxelY screen tiles x kFroxelZ
//         exponential view-depth slices) injected and lit from the S7c cluster lists, temporally reprojected, integrated
//         front to back and sampled per pixel -- volumetric_froxel.hlsl / volumetric_froxel_apply.hlsl
//   mode 1: the half-res in-scattering march (sun and spot light shadows), temporal accumulation, depth-aware separable
//         blur, depth-aware upsample into lit as lit * transmittance + in-scattered light (volumetric.hlsl)
// Both modes share the medium (volumetric_common.hlsli) and composite the same way: lit * T + L.
// In the froxel mode the spot beams are a per-pixel pass at full render resolution (volumetric_beams.hlsl).
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "render/Renderer.h"
#include "core/Log.h"
#include <cstring>

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
    // S9d: the froxel passes; the light pass has a raster (shadow maps) and a ray-traced (TLAS) variant, so a frame
    // picks one without a pipeline rebuild.
    const std::filesystem::path froxelFile = shaderDir / L"volumetric_froxel.hlsl";
    const ShaderDefines rtDefines = {{"VOL_RT_SHADOWS", "1"}};
    const bool froxelOk =
        froxelLight_[0].Create(ctx, froxelFile, "CSFroxelLight") &&
        froxelLight_[1].Create(ctx, froxelFile, "CSFroxelLight", rtDefines) &&
        froxelTemporal_.Create(ctx, froxelFile, "CSFroxelTemporal") &&
        froxelIntegrate_.Create(ctx, froxelFile, "CSFroxelIntegrate") &&
        froxelApply_.Create(ctx, shaderDir / L"volumetric_froxel_apply.hlsl", "PSApplyFroxel",
                            {RenderTargets::kColorFormat}, {}, FullscreenPipeline::Blend::Transmittance);
    if (!froxelOk) {
        LOG_WARN("Volumetric: froxel pipelines unavailable, the march stays in use");
        froxelLight_[0] = {};
        froxelLight_[1] = {};
        froxelTemporal_ = {};
        froxelIntegrate_ = {};
        froxelApply_ = {};
    }
    // the spot beams (full render res, on top of the froxel grid); without them the spots stay in the froxel grid
    const std::filesystem::path beamsFile = shaderDir / L"volumetric_beams.hlsl";
    const bool beamsOk =
        beams_.Create(ctx, beamsFile, "CSBeams") && beamTemporal_.Create(ctx, beamsFile, "CSBeamTemporal") &&
        beamApply_.Create(ctx, shaderDir / L"volumetric_beams_apply.hlsl", "PSApplyBeams",
                          {RenderTargets::kColorFormat}, {}, FullscreenPipeline::Blend::Transmittance);
    if (!beamsOk) {
        LOG_WARN("Volumetric: beam pipelines unavailable, spots stay in the froxel grid");
        beams_ = {};
        beamTemporal_ = {};
        beamApply_ = {};
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
    // S9d: the froxel grid does not follow the render resolution, so a resize keeps it (it is created on demand).
    // The beams do follow it (one texel per pixel): full render res RGBA16F.
    beamRaw_.Create(ctx, targets.width, targets.height, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrv,
                    L"volumetric.beamRaw");
    beamHistory_[0].Create(ctx, targets.width, targets.height, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrv,
                           L"volumetric.beamHistory0");
    beamHistory_[1].Create(ctx, targets.width, targets.height, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kSrv,
                           L"volumetric.beamHistory1");
    beamHistoryReady_ = false;
}

void VolumetricPass::ReleaseTargets(Dx12Context& ctx) {
    raw_.Release(ctx);
    temp_.Release(ctx);
    history_[0].Release(ctx);
    history_[1].Release(ctx);
    historyReady_ = false;
    froxelRaw_.Release(ctx);
    froxelHistory_[0].Release(ctx);
    froxelHistory_[1].Release(ctx);
    froxelLit_.Release(ctx);
    froxelReady_ = false;
    froxelHistoryValid_ = false;
    beamRaw_.Release(ctx);
    beamHistory_[0].Release(ctx);
    beamHistory_[1].Release(ctx);
    beamHistoryReady_ = false;
}

// S9d: the froxel grids (RGBA16F Texture3D, kFroxelX x kFroxelY x kFroxelZ = 5.6 MB each). Nothing is allocated until a
// frame actually runs the froxel mode, so volumetrics off (and the march) cost no memory.
bool VolumetricPass::EnsureFroxelTargets(Dx12Context& ctx) {
    if (froxelReady_) return true;
    const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool ok = froxelRaw_.Create3D(ctx, kFroxelX, kFroxelY, kFroxelZ, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, srv,
                                       L"volumetric.froxelRaw") &&
                    froxelHistory_[0].Create3D(ctx, kFroxelX, kFroxelY, kFroxelZ, DXGI_FORMAT_R16G16B16A16_FLOAT, uav,
                                               srv, L"volumetric.froxelHistory0") &&
                    froxelHistory_[1].Create3D(ctx, kFroxelX, kFroxelY, kFroxelZ, DXGI_FORMAT_R16G16B16A16_FLOAT, uav,
                                               srv, L"volumetric.froxelHistory1") &&
                    froxelLit_.Create3D(ctx, kFroxelX, kFroxelY, kFroxelZ, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, srv,
                                        L"volumetric.froxelLit");
    if (!ok) {
        LOG_WARN("Volumetric: froxel grid allocation failed, the march stays in use");
        froxelRaw_.Release(ctx);
        froxelHistory_[0].Release(ctx);
        froxelHistory_[1].Release(ctx);
        froxelLit_.Release(ctx);
        return false;
    }
    froxelReady_ = true;
    froxelHistoryIndex_ = 0;
    froxelHistoryValid_ = false;   // the fresh textures hold nothing: the first frame must not blend them
    const double mb = 4.0 * (double)kFroxelX * kFroxelY * kFroxelZ * 8.0 / (1024.0 * 1024.0);
    LOG_INFO("FROXEL grid=%ux%ux%u format=RGBA16F textures=4 MB=%.1f", kFroxelX, kFroxelY, kFroxelZ, mb);
    return true;
}

void VolumetricPass::Execute(PassContext& pc) {
    if (!pc.settings.volumetric || pc.offscreen || !apply_ || !raw_) {
        historyReady_ = false;
        beamHistoryReady_ = false;
        return;
    }
    // S9d: the froxel mode (default); the march is the ini / CLI fallback and keeps its own history chain. The light
    // pass of a non-raster frame needs the TLAS it traces, so a frame without one falls back to the raster variant's
    // shadow maps (the shadow pass renders those for RT / PT frames too) and only a device without either goes back to
    // the march.
    if (pc.settings.volumetricMode == 0 && froxelApply_ && froxelTemporal_ && froxelIntegrate_ &&
        froxelLight_[pc.path != RenderPath::Raster && pc.rt ? 1 : 0] && EnsureFroxelTargets(pc.ctx)) {
        historyReady_ = false;   // the march's history does not follow these frames: no blending after a mode switch
        ExecuteFroxel(pc);
        return;
    }
    // the beams only run in the froxel mode: no history to blend the next time it is on
    beamHistoryReady_ = false;
    if (!march_) {
        historyReady_ = false;
        return;
    }
    ExecuteMarch(pc);
}

void VolumetricPass::ExecuteMarch(PassContext& pc) {
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
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

// S9d: the froxel mode. Inject + light (one thread per froxel, jittered along z), temporal reprojection through the
// previous view projection, front-to-back integration per column, then the per-pixel apply of lit * T + L.
void VolumetricPass::ExecuteFroxel(PassContext& pc) {
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const float mediumRadius = VolumetricMediumRadius(pc.view.light);
    const float maxDistance = VolumetricMaxDistance(pc.view.camera.eye, mediumRadius);
    const float fogG = pc.view.light.volumetricAnisotropy > -0.99f ? pc.view.light.volumetricAnisotropy
                                                                 : pc.settings.volumetricAnisotropy;
    const float sunG = pc.view.light.sunVolumetricG > -0.99f
                           ? pc.view.light.sunVolumetricG
                           : (fogG > -0.99f ? fogG : 0.55f);
    const float punctualG = fogG > -0.99f ? fogG : 0.3f;
    const uint32_t fogCount = (uint32_t)std::min<size_t>(pc.view.light.fogVolumes.size(), Renderer::kMaxFogVolumes);
    const float sigma0 = (pc.view.light.volumetricSigma0 >= 0.0f ? pc.view.light.volumetricSigma0 : 0.005f) *
                         pc.settings.volumetricDensity;
    const float falloff = pc.view.light.volumetricFalloff >= 0.0f ? pc.view.light.volumetricFalloff : 0.012f;
    const DirectX::XMFLOAT3& tint = pc.view.light.volumetricColor;
    // the march's constants (ambient scale 0.25, the medium radius, the sun's haze share), so both modes share a look
    const float p0[4] = {sigma0, falloff, maxDistance, sunG};
    const float p1[4] = {punctualG, (float)fogCount, pc.view.light.sunVolumetric, mediumRadius};
    const float p2[4] = {tint.x, tint.y, tint.z, 0.25f};
    // bit 0: multiple scattering on, bit 1: the beam pass draws the spots, so the froxel grid skips them
    const bool beams = beams_ && beamTemporal_ && beamApply_ && beamRaw_;
    const float p3[4] = {(pc.settings.volumetricMultiScatter ? 1.0f : 0.0f) + (beams ? 2.0f : 0.0f),
                         (float)kFroxelX, (float)kFroxelY, (float)kFroxelZ};
    const uint32_t groupsX = (kFroxelX + 7) / 8;
    const uint32_t groupsY = ((kFroxelY * kFroxelZ) + 7) / 8;
    float params[16];
    std::memcpy(params, p0, sizeof(p0));
    std::memcpy(params + 4, p1, sizeof(p1));
    std::memcpy(params + 8, p2, sizeof(p2));
    std::memcpy(params + 12, p3, sizeof(p3));

    // 1. inject + light this frame's fresh grid
    froxelRaw_.Transition(cmd, kUav);
    // the raster variant's cascades are rendered for RT / PT frames too (ShadowPass), so it is also the fallback when a
    // ray-traced frame has no TLAS this frame
    if (pc.path == RenderPath::Raster || !pc.rt) {
        t.shadowMap.Transition(cmd, kSrvAll);
        t.spotShadowMap.Transition(cmd, kSrvAll);
        froxelLight_[0].Dispatch(pc, pc.transient.SrvTable(pc.ctx, {nullptr, nullptr, &t.shadowMap, &t.spotShadowMap}),
                                 pc.transient.UavTable(pc.ctx, {&froxelRaw_}), params, 16, groupsX, groupsY);
    } else {
        // RT / PT: the light pass traces its shadows against the TLAS
        froxelLight_[1].Dispatch(pc, pc.transient.SrvTable(pc.ctx, {}),
                                 pc.transient.UavTable(pc.ctx, {&froxelRaw_}), params, 16, groupsX, groupsY);
    }

    // 2. temporal: blend with the last frame's grid reprojected through prevViewProjNoJitter (ping-pong)
    const bool valid = froxelHistoryValid_ && pc.frame == froxelLastFrame_ + 1 && !pc.view.cameraCut;
    const uint32_t prev = froxelHistoryIndex_, next = froxelHistoryIndex_ ^ 1u;
    froxelRaw_.Transition(cmd, kSrvAll);
    froxelHistory_[prev].Transition(cmd, kSrvAll);
    froxelHistory_[next].Transition(cmd, kUav);
    const float tp0[4] = {valid ? 1.0f : 0.0f, 0.1f, froxelLastNearZ_, froxelLastMaxDistance_};
    float tparams[16];
    std::memcpy(tparams, tp0, sizeof(tp0));
    tparams[4] = maxDistance;   // gP1.x: this frame's grid (the froxel centre the reprojection starts from)
    tparams[5] = (float)fogCount;
    tparams[6] = pc.view.light.sunVolumetric;
    tparams[7] = mediumRadius;
    std::memcpy(tparams + 8, p2, sizeof(p2));
    std::memcpy(tparams + 12, p3, sizeof(p3));
    froxelTemporal_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&froxelRaw_, &froxelHistory_[prev]}),
                             pc.transient.UavTable(pc.ctx, {&froxelHistory_[next]}), tparams, 16, groupsX, groupsY);
    froxelHistoryIndex_ = next;

    // 3. integrate every column front to back into the grid the apply pass reads
    froxelHistory_[next].Transition(cmd, kSrvAll);
    froxelLit_.Transition(cmd, kUav);
    const float ip0[4] = {maxDistance, 0, 0, 0};
    float iparams[16];
    std::memcpy(iparams, ip0, sizeof(ip0));
    std::memcpy(iparams + 4, p1, sizeof(p1));
    std::memcpy(iparams + 8, p2, sizeof(p2));
    std::memcpy(iparams + 12, p3, sizeof(p3));
    froxelIntegrate_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&froxelHistory_[next]}),
                              pc.transient.UavTable(pc.ctx, {&froxelLit_}), iparams, 16,
                              (kFroxelX + 7) / 8, (kFroxelY + 7) / 8);

    // 4. lit = lit * T + L
    froxelLit_.Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    t.depth.Transition(cmd, kSrv);
    t.shadowMap.Transition(cmd, kSrv);
    t.spotShadowMap.Transition(cmd, kSrv);
    froxelRaw_.Transition(cmd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const float ca[4] = {(float)kFroxelX, (float)kFroxelY, (float)kFroxelZ, maxDistance};
    t.lit.Transition(cmd, kRt);
    froxelApply_.Draw(pc, {&t.lit}, pc.transient.SrvTable(pc.ctx, {&froxelLit_, &t.depth}), ca, 4);
    t.lit.Transition(cmd, kSrv);

    // 5. the spot beams at full render resolution, added on top (params in volumetric_beams.hlsl's gP layout)
    if (beams) {
        const float bp[16] = {sigma0, falloff, maxDistance, 0.0f,
                              punctualG, (float)fogCount, pc.settings.volumetricMultiScatter ? 1.0f : 0.0f,
                              mediumRadius,
                              tint.x, tint.y, tint.z, 0.0f,
                              0.0f, 0.0f, 0.0f, 0.0f};
        ExecuteBeams(pc, bp, maxDistance);
    } else {
        beamHistoryReady_ = false;
    }

    froxelLastFrame_ = pc.frame;
    froxelHistoryValid_ = true;
    froxelLastNearZ_ = std::max(pc.view.camera.nearZ, 0.01f);
    froxelLastMaxDistance_ = maxDistance;
}

// The spots at full render resolution: march (jittered per frame) -> raw, temporal blend through the previous view
// projection, then an additive full-resolution composite over lit (the froxel already applied its transmittance).
void VolumetricPass::ExecuteBeams(PassContext& pc, const float params[16], float maxDistance) {
    RenderTargets& t = pc.targets;
    ID3D12GraphicsCommandList* cmd = pc.cmd;

    // 1. march -> beamRaw_ (every path reads the shadow maps: the RT froxel branch may not have transitioned them)
    beamRaw_.Transition(cmd, kUav);
    t.depth.Transition(cmd, kSrvAll);
    t.shadowMap.Transition(cmd, kSrvAll);
    t.spotShadowMap.Transition(cmd, kSrvAll);
    beams_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&t.depth, nullptr, &t.shadowMap, &t.spotShadowMap}),
                    pc.transient.UavTable(pc.ctx, {&beamRaw_}), params, 16, Groups(beamRaw_.width),
                    Groups(beamRaw_.height));

    // 2. temporal blend -> beamHistory_[cur] (history only across consecutive frames, no cuts)
    const bool valid = beamHistoryReady_ && pc.frame == beamLastFrame_ + 1 && !pc.view.cameraCut;
    const uint32_t prev = beamHistoryIndex_, cur = prev ^ 1u;
    beamRaw_.Transition(cmd, kSrvAll);
    beamHistory_[prev].Transition(cmd, kSrvAll);
    beamHistory_[cur].Transition(cmd, kUav);
    const float ct[4] = {valid ? 1.0f : 0.0f, 0.15f, maxDistance, 0.0f};
    beamTemporal_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&beamRaw_, &beamHistory_[prev], &t.depth}),
                           pc.transient.UavTable(pc.ctx, {&beamHistory_[cur]}), ct, 4, Groups(beamRaw_.width),
                           Groups(beamRaw_.height));
    beamHistoryIndex_ = cur;
    beamHistoryReady_ = true;
    beamLastFrame_ = pc.frame;

    // 3. lit = lit + L (alpha 1: additive)
    beamHistory_[cur].Transition(cmd, kSrv);
    beamRaw_.Transition(cmd, kSrv);
    beamHistory_[prev].Transition(cmd, kSrv);
    t.depth.Transition(cmd, kSrv);
    t.shadowMap.Transition(cmd, kSrv);
    t.spotShadowMap.Transition(cmd, kSrv);
    t.lit.Transition(cmd, kRt);
    const float ca[4] = {0, 0, 0, 0};
    beamApply_.Draw(pc, {&t.lit}, pc.transient.SrvTable(pc.ctx, {&beamHistory_[cur]}), ca, 4);
    t.lit.Transition(cmd, kSrv);
}

} // namespace mmdx
