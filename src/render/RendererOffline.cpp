// Renderer offline entry points (see render/OfflineRenderer.h for the data flow).
#include "render/Renderer.h"
#include "render/RayTracing.h"
#include "asset/ImageLoader.h"
#include <cmath>

namespace mmdx {

bool Renderer::BeginOffline(ID3D12GraphicsCommandList* cmd, const FrameView& view, const OfflineJobDesc& job) {
    if (!offline_ || !rt_ || !cmd || job.width == 0 || job.height == 0) return false;
    ctx_->WaitForGpu();
    ID3D12DescriptorHeap* heaps[] = {ctx_->SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, heaps);
    transient_.Begin(ctx_->FrameSlot());
    SceneConstants sc{};
    FillSceneConstants(view, job.width, job.height, /*offscreen*/ true, sc);  // no jitter
    sc.floorGloss = 0.35f;     // independent of the real-time settings
    sc.fog = 0.0f;
    sc.transparentBg = 0.0f;
    // transparent export: the primary camera-ray miss colour (black). No bloom (black adds nothing) and the
    // coverage alpha comes from the G-buffer (CSFinalize), giving a straight RGBA8 image.
    sc.bgOverride = job.exportAlpha ? DirectX::XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f}
                                    : DirectX::XMFLOAT4{0.0f, 0.0f, 0.0f, 0.0f};
    if (view.motionBlur)
        DirectX::XMStoreFloat4x4(&sc.prevInvView,
                                 DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&view.prevCamera.view)));
    else
        sc.prevInvView = sc.invView;
    {
        const OfflineSceneProps& p = view.offlineProps;
        const OfflineGlassBox& g = p.glass;
        sc.glassCenter = {g.center.x, g.center.y, g.center.z, g.enabled ? 1.0f : 0.0f};
        sc.glassHalf = {g.halfExtents.x, g.halfExtents.y, g.halfExtents.z, g.cornerRadius};
        sc.glassParams = {std::cos(g.yawRadians), std::sin(g.yawRadians), g.ior, g.dispersion};
        sc.glassAbsorb = {g.absorption.x, g.absorption.y, g.absorption.z, 0.0f};
        for (int i = 0; i < 2; ++i) {
            const OfflineSoftbox& b = p.softboxes[i];
            sc.softbox[i][0] = {b.center.x, b.center.y, b.center.z, b.enabled ? 1.0f : 0.0f};
            sc.softbox[i][1] = {b.halfU.x, b.halfU.y, b.halfU.z, 0.0f};
            sc.softbox[i][2] = {b.halfV.x, b.halfV.y, b.halfV.z, 0.0f};
            sc.softbox[i][3] = {b.radiance.x, b.radiance.y, b.radiance.z, 0.0f};
        }
        sc.floorParams = p.customFloor ? DirectX::XMFLOAT4{p.floorAlbedo.x, p.floorAlbedo.y, p.floorAlbedo.z,
                                                           p.floorReflectivity}
                                       : DirectX::XMFLOAT4{0.0f, 0.0f, 0.0f, -1.0f};
    }
    // S7a: 256 lights x 112 B does not belong on the stack
    static thread_local std::vector<GpuLight> lights(Renderer::kMaxPunctualLights);
    std::fill(lights.begin(), lights.end(), GpuLight{});
    const uint32_t count = FillGpuLights(view.light, lights.data());
    // S7a: the lights' pick CDF (offline GI's K = 4 light samples per shading point); the offline renderer uploads it
    // and points SceneConstants::lightCdfParams at its own raw SRV.
    static thread_local std::vector<float> lightCdf(Renderer::kMaxPunctualLights);
    const uint32_t cdfCount = FillLightCdf(view.light, lightCdf.data(), (uint32_t)lightCdf.size());
    // negative when the offline renderer has no CDF buffer (the shaders then keep the uniform pick)
    sc.lightCdfParams = {(float)(int32_t)offline_->LightCdfSrv(), (float)cdfCount, 0.0f, 0.0f};
    // S7b: the image's spot gobos (the offline renderer uploads its own table through the bindless buffer table)
    std::vector<GpuGobo> gobos(kMaxPunctualLights);
    const bool goboAny = FillGoboTable(*ctx_, view.light, gobos.data(), view.sceneTime);
    sc.goboParams = {goboAny ? (float)(int32_t)offline_->GoboSrv() : -1.0f, 0.0f, 0.0f, 0.0f};
    // S7b: a camera screen feed rendered inside this image draws with the raster slot's constants (RenderScreenFeed
    // copies the frame slot's light / gobo table), so that slot holds the same table
    goboActive_ = goboAny;
    if (goboAny)
        memcpy(goboBufMapped_ + (size_t)ctx_->FrameSlot() * kMaxPunctualLights * sizeof(GpuGobo), gobos.data(),
               kMaxPunctualLights * sizeof(GpuGobo));
    // S7c: the offline renderer's shaders are the path tracer / GI (light CDF sampling); no light clusters
    sc.lightClusterParams = {-1.0f, 0.0f, 0.0f, 0.0f};
    // S9: the frame's local fog volumes (GpuFogVolume, zeroed past the count)
    GpuFogVolume fogVolumes[kMaxFogVolumes] = {};
    const uint32_t fogCount = FillGpuFogVolumes(view.light, fogVolumes);
    // Background AVI (背景AVI): the exact frame for the rendered time (view.videoExact) before the TLAS / draws.
    if (screenVideo_) screenVideo_->Update(*ctx_, cmd, view);
    // S5: the stage screens of the offline frame (image / video exact frame / camera feeds), before the TLAS so the
    // ray paths see this frame's screen flags.
    UpdateStageScreens(cmd, view, sc);
    if (!rt_->Build(cmd, view.models, ctx_->FrameNumber(), ctx_->FrameSlot())) return false;
    // S8a: the image's emissive-triangle pick table (the LED walls / AutoLuminous / PBR emitters of the built scene),
    // uploaded by the offline renderer with the image's other constants.
    Renderer::EmissiveTable emissive;
    const uint32_t emissiveCount = BuildEmissiveTable(rt_->GeometryRefs(), emissive, emissiveWarned_);
    OfflineRenderer::EmissiveTable emissiveUpload;
    const OfflineRenderer::EmissiveSrvs emissiveSrv = offline_->EmissiveSrv();
    if (emissiveCount > 0) {
        sc.emissiveParams = {(float)(int32_t)emissiveSrv.tris, (float)(int32_t)emissiveSrv.cdf, (float)emissiveCount,
                             (float)(int32_t)emissiveSrv.base};
        emissiveUpload.tris = emissive.tris.data();
        emissiveUpload.triCount = emissiveCount;
        emissiveUpload.cdf = emissive.cdf.data();
        emissiveUpload.base = emissive.base.data();
        emissiveUpload.baseCount = (uint32_t)emissive.base.size();
    } else {
        sc.emissiveParams = {-1.0f, -1.0f, 0.0f, -1.0f};
    }
    memcpy(sceneCbMapped_ + (size_t)ctx_->FrameSlot() * kSceneCbSize, &sc, sizeof(sc));
    // M5b: the offline job's post-effect list is filled by the caller (app) from the same list; this is only a
    // fallback for callers that did not (the renderer's current MME list).
    OfflineJobDesc jobWithMme = job;
    if (jobWithMme.mmeEffects.empty()) jobWithMme.mmeEffects = settings_.mmeEffects;
    offline_->Begin(cmd, transient_, &builtin_, *rt_, view, sc, lights.data(), count, lightCdf.data(), cdfCount,
                    fogVolumes, fogCount, jobWithMme, ctx_->FrameNumber(), emissiveUpload, mme_.get(), mmeSkin_.get(),
                    goboAny ? gobos.data() : nullptr);
    float area[4];
    ViewportArea(settings_, (float)ctx_->Width(), (float)ctx_->Height(), area);
    if (offlinePresent_) offline_->Present(cmd, transient_, presentRect_, area);
    sceneVisible_ = false;
    havePrev_ = false;
    return true;
}

void Renderer::RenderOffline(ID3D12GraphicsCommandList* cmd) {
    if (!offline_ || !rt_ || !cmd) return;
    ID3D12DescriptorHeap* heaps[] = {ctx_->SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, heaps);
    transient_.Begin(ctx_->FrameSlot());
    offline_->Render(cmd, transient_, &builtin_, *rt_);
    float area[4];
    ViewportArea(settings_, (float)ctx_->Width(), (float)ctx_->Height(), area);
    if (offlinePresent_) offline_->Present(cmd, transient_, presentRect_, area);
    sceneVisible_ = false;
    havePrev_ = false;
}

const OfflineProgress& Renderer::OfflineStatus() const {
    static OfflineProgress idle;
    return offline_ ? offline_->Progress() : idle;
}

bool Renderer::ReadOfflineImage(ImageRGBA8& out, bool preserveAlpha) {
    return offline_ && offline_->ReadImage(out, preserveAlpha);
}

void Renderer::CancelOffline() {
    if (offline_) offline_->Cancel();
}

} // namespace mmdx
