// ScreenVideo: decode the background AVI (背景AVI) into one texture and bind it to every material whose diffuse
// texture file name is screen.bmp (render/GpuModel.h). See ScreenVideo.h.
#include "render/ScreenVideo.h"
#include "render/GpuModel.h"
#include "render/RenderTypes.h"
#include "render/VideoTexture.h"
#include "core/Log.h"
#include <unordered_map>

namespace mmdx {

struct ScreenVideo::Impl {
    // One texture per file (render/VideoTexture.h): the decode worker, the rings and the GPU upload.
    VideoTexture video;
    std::filesystem::path path;          // the file the current texture was opened from
    std::filesystem::path failedPath;    // a path whose open failed: warn once, do not retry every frame
    int screenMaterialCount = 0;
    double lastSceneTime = -1.0;

    // which models have been bound to the current texture generation (keyed by GpuModel::Uid)
    std::unordered_map<uint64_t, uint32_t> boundGen;
    uint32_t generation = 0;   // bumped when the texture is created / released
};

ScreenVideo::ScreenVideo() : impl_(std::make_unique<Impl>()) {}

ScreenVideo::~ScreenVideo() = default;

void ScreenVideo::ForgetModel(uint64_t uid) {
    if (impl_) impl_->boundGen.erase(uid);
}

void ScreenVideo::Clear(Dx12Context& ctx) {
    if (!impl_) return;
    impl_->video.Release(ctx);
    impl_->path.clear();
    impl_->boundGen.clear();
    impl_->failedPath.clear();
    ++impl_->generation;
    impl_->lastSceneTime = -1.0;
}

void ScreenVideo::Update(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, const FrameView& view) {
    Impl& impl = *impl_;
    if (!ctx.Device() || !cmd) return;

    // The first screen layer of this frame (at most one background video).
    const VideoLayerDesc* layer = nullptr;
    int screenMaterials = 0;
    for (const GpuModel* m : view.models)
        if (m && m->HasScreenMaterials()) ++screenMaterials;
    for (const VideoLayerDesc& d : view.videoLayers) {
        if (d.screen && !d.path.empty()) { layer = &d; break; }
    }
    impl.screenMaterialCount = screenMaterials;

    bool haveTex = false;
    if (layer) {
        if (impl.path != layer->path && impl.failedPath != layer->path) {
            ctx.WaitForGpu();
            if (!impl.video.Open(ctx, layer->path)) {
                impl.failedPath = layer->path;   // one warning line (Open logs it); keep the previous binding
            } else {
                impl.failedPath.clear();
                impl.path = layer->path;
                ++impl.generation;
                LOG_INFO("screen video: %d screen materials", impl.screenMaterialCount);
            }
        }
        if (impl.video.HasTexture()) {
            haveTex = true;
            const bool seek = impl.lastSceneTime >= 0.0 && (double)view.sceneTime + 1e-6 < impl.lastSceneTime;
            impl.lastSceneTime = (double)view.sceneTime;
            if (!impl.video.Update(ctx, cmd, ctx.FrameSlot(), layer->time, view.videoExact, seek)) {
                // no frame decoded for this time yet: the shown texture stays
            }
        }
    } else if (impl.video.HasTexture() || !impl.path.empty()) {
        ctx.WaitForGpu();
        impl.video.Release(ctx);   // bumps the generation: models rebind to their own texture below
        impl.path.clear();
        ++impl.generation;
    }

    // Bind the current texture (or restore the model's own) on every model with screen materials whose bound
    // generation is stale. The descriptor rewrite waits for the GPU, so a shader-visible descriptor read by frames
    // in flight is never rewritten under them. A model that was not in view when the video was cleared rebinds here.
    bool needBind = false;
    for (GpuModel* m : view.models) {
        if (!m || !m->HasScreenMaterials()) continue;
        const auto it = impl.boundGen.find(m->Uid());
        if (it == impl.boundGen.end() || it->second != impl.generation) { needBind = true; break; }
    }
    if (needBind) {
        ctx.WaitForGpu();
        ID3D12Resource* bindRes = haveTex ? impl.video.TextureResource() : nullptr;
        for (GpuModel* m : view.models) {
            if (!m || !m->HasScreenMaterials()) continue;
            const auto it = impl.boundGen.find(m->Uid());
            if (it != impl.boundGen.end() && it->second == impl.generation) continue;
            m->BindScreenTexture(ctx, bindRes);
            impl.boundGen[m->Uid()] = impl.generation;
        }
    }
}

} // namespace mmdx
