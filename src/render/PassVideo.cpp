// VideoPass (docs/mme/mme_design.md §5b; video.hlsl): the song's background / overlay videos.
//   - Background ("Video(bg)"): replaces the sky / clear colour where the scene has no geometry (raw device depth
//     ~1), aspect-fill. Runs right after PostPass, over the LDR image.
//   - Overlay ("Video(overlay)"): composited over the final image (after the effect packs and the MME post chain),
//     aspect-fit, black key by default. Runs right before BackdropPass.
// In play mode each layer's frames are decoded ahead of the requested time on a worker thread into a small CPU ring
// and uploaded into a per-frame-in-flight persistent texture (a UPLOAD staging buffer per frame in flight); the frame
// is never stalled on decode (the last uploaded frame stays). A seek flushes the ring. The offline entry points
// decode synchronously, so the frame for the rendered time is exact.
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "asset/VideoReader.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace mmdx {

namespace {
// One persistent video texture + staging buffer per frame in flight (the frame's upload is complete before the
// slot is reused kFramesInFlight frames later). Keeps all video buffers well under the 64 MB budget.
constexpr uint32_t kRingTex = Dx12Context::kFramesInFlight;
constexpr uint32_t kCpuRing = 3;                                  // decoded CPU frames kept ahead of the requested time
constexpr size_t kMaxVideoBytes = 96ull * 1024 * 1024;            // guard: refuse absurd layer sizes
} // namespace

// ---- root constants + upload helper --------------------------------------------------------------------------

namespace {
// gP0 = (srcW, srcH, dstW, dstH), gP1 = (kind, blend, depthEncoding, 0). depthEncoding: 0 = play mode (raw device
// depth, sky = 1 in .x), 1 = offline G-buffer (view depth in .w, sky = 1e6).
void LayerConstants(uint32_t srcW, uint32_t srcH, VideoLayerKind kind, VideoBlend blend, uint32_t dstW, uint32_t dstH,
                    float depthEncoding, float out[8]) {
    out[0] = (float)srcW; out[1] = (float)srcH; out[2] = (float)dstW; out[3] = (float)dstH;
    out[4] = kind == VideoLayerKind::Background ? 0.0f : 1.0f;
    out[5] = (float)(uint8_t)blend;
    out[6] = depthEncoding;
    out[7] = 0.0f;
}

// 256-byte aligned staging row pitch (a D3D12 buffer -> texture copy requires it).
inline uint32_t StagingPitch(uint32_t w) { return (w * 4u + 255u) & ~255u; }

// Copies top-down RGBA8 into a fresh UPLOAD buffer laid out as `w x h` RGBA8 and issues the texture copy.
bool UploadFrame(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, Texture& tex, uint32_t w, uint32_t h,
                 const std::vector<uint8_t>& px) {
    if (px.size() != (size_t)w * h * 4) return false;
    const uint32_t pitch = StagingPitch(w);
    const uint64_t bytes = (uint64_t)pitch * h;
    ComPtr<ID3D12Resource> staging;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    CD3DX12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    if (FAILED(ctx.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&staging)))) return false;
    void* mapped = nullptr;
    D3D12_RANGE read{0, 0};
    if (FAILED(staging->Map(0, &read, &mapped))) return false;
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy((uint8_t*)mapped + (size_t)y * pitch, px.data() + (size_t)y * w * 4, (size_t)w * 4);
    staging->Unmap(0, nullptr);
    tex.Transition(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
    CD3DX12_TEXTURE_COPY_LOCATION dst(tex.res.Get(), 0);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{0, {DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, pitch}};
    CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(), fp);
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    tex.Transition(cmd, kSrv);
    ctx.DeferRelease(staging);
    return true;
}
} // namespace

struct VideoPass::Impl {
    // One decoded CPU frame in the ring.
    struct CpuFrame {
        std::vector<uint8_t> px;   // RGBA8, top-down
        double time = -1.0;
        uint64_t serial = 0;
        bool ready = false;
    };

    // One active video layer: decode worker + CPU ring + persistent upload textures.
    struct Layer {
        std::filesystem::path path;
        VideoLayerKind kind = VideoLayerKind::Background;
        VideoBlend blend = VideoBlend::BlackKey;
        VideoReader reader;
        uint32_t w = 0, h = 0;
        double duration = 0.0, fps = 30.0;

        // CPU ring (worker writes, render thread reads)
        std::mutex mtx;
        std::condition_variable cv;
        CpuFrame cpu[kCpuRing];
        uint64_t serial = 0;
        double want = 0.0;
        uint64_t wantGen = 0;      // bumped on a seek: the worker flushes the ring next tick

        // worker
        std::thread worker;
        std::atomic<bool> quit{false};

        // GPU (render thread)
        Texture tex[kRingTex];
        ComPtr<ID3D12Resource> staging[kRingTex];
        uint64_t texSerial[kRingTex] = {};
        bool texHave[kRingTex] = {};

        void StartWorker() {
            worker = std::thread([this] { Run(); });
        }
        void StopWorker() {
            if (!worker.joinable()) return;
            quit.store(true);
            { std::lock_guard<std::mutex> l(mtx); cv.notify_all(); }
            worker.join();
        }

        // Worker: keep a decoded frame for the latest `want` in the ring (never blocks the render thread).
        void Run() {
            uint64_t lastGen = 0;
            double lastTime = -1e30;
            std::vector<uint8_t> frame;
            while (!quit.load()) {
                double t;
                uint64_t gen;
                {
                    std::unique_lock<std::mutex> l(mtx);
                    if (quit.load()) break;
                    t = want;
                    gen = wantGen;
                    if (gen != lastGen) {
                        for (CpuFrame& s : cpu) s.ready = false;
                        lastGen = gen;
                        lastTime = -1e30;
                    } else if (lastTime > -1e29 && std::fabs(t - lastTime) <= 0.5 / std::max(1.0, fps)) {
                        cv.wait_for(l, std::chrono::milliseconds(2), [this] { return quit.load(); });
                        continue;
                    }
                }
                if (!reader.ReadFrame(t, frame) || frame.empty()) {
                    std::unique_lock<std::mutex> l(mtx);
                    cv.wait_for(l, std::chrono::milliseconds(4), [this] { return quit.load(); });
                    continue;
                }
                for (size_t i = 0; i + 3 < frame.size(); i += 4) std::swap(frame[i], frame[i + 2]);   // BGRA -> RGBA
                std::lock_guard<std::mutex> l(mtx);
                int pick = -1;
                for (int i = 0; i < (int)kCpuRing; ++i)
                    if (!cpu[i].ready) { pick = i; break; }
                if (pick < 0) {
                    double dist = -1.0;
                    for (int i = 0; i < (int)kCpuRing; ++i) {
                        const double d = std::fabs(cpu[i].time - t);
                        if (d > dist) { dist = d; pick = i; }
                    }
                }
                CpuFrame& s = cpu[pick];
                s.px = frame;             // reuse capacity
                s.time = t;
                s.serial = ++serial;
                s.ready = true;
                lastTime = t;
            }
        }

        // Render thread: the ready frame nearest but not after `t`; uploads it if newer. True when a frame is shown.
        // The chosen frame is copied under the lock so the worker cannot overwrite it mid-upload.
        std::vector<uint8_t> uploadScratch;
        bool Prepare(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, uint32_t slot, double t) {
            uint64_t frameSerial = 0;
            bool have = false;
            {
                std::lock_guard<std::mutex> l(mtx);
                int pick = -1;
                double best = -1e30;
                for (int i = 0; i < (int)kCpuRing; ++i) {
                    if (!cpu[i].ready) continue;
                    if (cpu[i].time <= t + 0.5 / std::max(1.0, fps) && cpu[i].time > best) { best = cpu[i].time; pick = i; }
                }
                if (pick >= 0) {
                    uploadScratch = cpu[pick].px;
                    frameSerial = cpu[pick].serial;
                    have = true;
                }
            }
            if (!have) return texHave[slot];
            if (!(texHave[slot] && texSerial[slot] == frameSerial)) {
                if (Upload(ctx, cmd, slot, uploadScratch)) texSerial[slot] = frameSerial, texHave[slot] = true;
            }
            return texHave[slot];
        }

        uint32_t pitch = 0;   // staging row pitch (256-byte aligned)
        bool Upload(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, uint32_t slot, const std::vector<uint8_t>& px) {
            const uint64_t bytes = (uint64_t)w * h * 4;
            if (px.size() != bytes || !staging[slot]) return false;
            void* mapped = nullptr;
            D3D12_RANGE read{0, 0};
            if (FAILED(staging[slot]->Map(0, &read, &mapped))) return false;
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy((uint8_t*)mapped + (size_t)y * pitch, px.data() + (size_t)y * w * 4, (size_t)w * 4);
            staging[slot]->Unmap(0, nullptr);
            tex[slot].Transition(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
            CD3DX12_TEXTURE_COPY_LOCATION dst(tex[slot].res.Get(), 0);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{0, {DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, pitch}};
            CD3DX12_TEXTURE_COPY_LOCATION src(staging[slot].Get(), fp);
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            tex[slot].Transition(cmd, kSrv);
            return true;
        }

        bool OpenGpu(Dx12Context& ctx) {
            if ((size_t)w * h * 4 > kMaxVideoBytes) return false;
            pitch = StagingPitch(w);
            const uint64_t stagingBytes = (uint64_t)pitch * h;
            for (uint32_t i = 0; i < kRingTex; ++i) {
                if (!tex[i].Create(ctx, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, kSrv,
                                   L"video.frame")) return false;
                D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
                CD3DX12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(stagingBytes);
                if (FAILED(ctx.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                IID_PPV_ARGS(&staging[i])))) return false;
            }
            return true;
        }
        void ReleaseGpu(Dx12Context& ctx) {
            for (uint32_t i = 0; i < kRingTex; ++i) {
                tex[i].Release(ctx);
                if (staging[i]) ctx.DeferRelease(staging[i]);
                staging[i].Reset();
                texHave[i] = false;
                texSerial[i] = 0;
            }
        }
    };

    std::vector<std::unique_ptr<Layer>> layers;
    double lastSceneTime = -1.0;
    FullscreenPipeline pipe_;       // bg: PSVideoBg; overlay: PSVideoOverlay
    FullscreenPipeline copyPipe_;   // overlay only: PSVideoCopy
    Texture scratch_;               // overlay only: ldr ping-pong (output resolution)

    // Offline (synchronous) decode: one reader cached by path, one texture.
    VideoReader offlineReader;
    std::filesystem::path offlinePath;
    Texture offlineTex;
    uint32_t offlineW = 0, offlineH = 0;

    void SyncLayers(PassContext& pc, bool wantBg);
    bool DecodeOffline(PassContext& pc, const VideoLayerDesc& d, bool wantBg, Texture& target, Texture& depthOrScratch);
    void Draw(PassContext& pc, Layer& l, uint32_t slot, Texture& target, Texture& sceneOrDepth, bool bg);

    // Seconds into the layer for this frame (the background loops).
    static double LayerTime(const Layer& l, bool bg, double sceneTime) {
        if (bg && l.duration > 0.0) {
            double t = std::fmod(sceneTime, l.duration);
            if (t < 0.0) t += l.duration;
            return t;
        }
        return sceneTime;
    }
};

void VideoPass::Impl::SyncLayers(PassContext& pc, bool wantBg) {
    std::vector<const VideoLayerDesc*> wanted;
    for (const VideoLayerDesc& d : pc.view.videoLayers)
        if ((d.kind == VideoLayerKind::Background) == wantBg && !d.path.empty()) wanted.push_back(&d);

    for (size_t i = layers.size(); i-- > 0;) {
        bool keep = false;
        for (const auto* d : wanted) if (d->path == layers[i]->path) { keep = true; break; }
        if (!keep) {
            layers[i]->StopWorker();
            layers[i]->ReleaseGpu(pc.ctx);
            layers.erase(layers.begin() + i);
        }
    }
    for (const auto* d : wanted) {
        bool have = false;
        for (auto& l : layers) if (l->path == d->path) { have = true; l->kind = d->kind; l->blend = d->blend; break; }
        if (have) continue;
        auto layer = std::make_unique<Layer>();
        layer->path = d->path;
        layer->kind = d->kind;
        layer->blend = d->blend;
        std::string err;
        if (!layer->reader.Open(d->path, &err)) {
            LOG_WARN("video layer: cannot open %s: %s", PathToUtf8(d->path).c_str(), err.c_str());
            continue;
        }
        layer->w = layer->reader.Width();
        layer->h = layer->reader.Height();
        layer->duration = layer->reader.Duration();
        layer->fps = layer->reader.Fps();
        if (!layer->OpenGpu(pc.ctx)) {
            LOG_WARN("video layer: GPU upload resources failed for %s", PathToUtf8(d->path).c_str());
            continue;
        }
        LOG_INFO("video layer (%s): %s %ux%u %.2fs", wantBg ? "background" : "overlay",
                 PathToUtf8(d->path.filename()).c_str(), layer->w, layer->h, layer->duration);
        layer->StartWorker();
        layers.push_back(std::move(layer));
    }
}

void VideoPass::Impl::Draw(PassContext& pc, Layer& l, uint32_t slot, Texture& target, Texture& sceneOrDepth, bool bg) {
    float c[8];
    LayerConstants(l.w, l.h, l.kind, l.blend, target.width, target.height, 0.0f, c);
    if (bg) {
        pipe_.Draw(pc, {&target}, pc.transient.SrvTable(pc.ctx, {nullptr, &l.tex[slot], &sceneOrDepth}), c, 8);
    } else {
        pipe_.Draw(pc, {&target}, pc.transient.SrvTable(pc.ctx, {&sceneOrDepth, &l.tex[slot]}), c, 8);
    }
}

void VideoPass::Execute(PassContext& pc) {
    Impl& impl = *impl_;
    const bool wantBg = stage_ == VideoStage::Background;
    RenderTargets& t = pc.targets;
    if (!t.ldr) return;
    // Layer list follows the frame's videoLayers (open / drop as the scene changes).
    impl.SyncLayers(pc, wantBg);
    if (impl.layers.empty()) return;

    const double sceneTime = (double)pc.view.sceneTime;
    const bool seek = impl.lastSceneTime >= 0.0 && sceneTime + 1e-6 < impl.lastSceneTime;
    impl.lastSceneTime = sceneTime;
    // Push the wanted time (and flush on a seek) into every layer's worker.
    for (auto& lp : impl.layers) {
        std::lock_guard<std::mutex> l(lp->mtx);
        lp->want = Impl::LayerTime(*lp, wantBg, sceneTime);
        if (seek) ++lp->wantGen;
        lp->cv.notify_all();
    }

    const uint32_t slot = (uint32_t)(pc.frame % kRingTex);
    ID3D12GraphicsCommandList* cmd = pc.cmd;

    if (wantBg) {
        // Background: straight over ldr where the camera's depth is far.
        t.ldr.Transition(cmd, kRt);
        t.depth.Transition(cmd, kSrv);
        for (auto& lp : impl.layers) {
            const double vt = Impl::LayerTime(*lp, true, sceneTime);
            if (!lp->Prepare(pc.ctx, cmd, slot, vt)) continue;
            impl.Draw(pc, *lp, slot, t.ldr, t.depth, true);
        }
        t.ldr.Transition(cmd, kSrv);
        return;
    }

    // Overlay: ldr -> scratch (copy), then composite over ldr (t0 = scratch = the scene under the overlay).
    if (!impl.scratch_) return;
    t.ldr.Transition(cmd, kSrv);
    impl.scratch_.Transition(cmd, kRt);
    impl.copyPipe_.Draw(pc, {&impl.scratch_}, pc.transient.SrvTable(pc.ctx, {&t.ldr}), nullptr, 0);
    impl.scratch_.Transition(cmd, kSrv);
    t.ldr.Transition(cmd, kRt);
    for (auto& lp : impl.layers) {
        if (!lp->Prepare(pc.ctx, cmd, slot, sceneTime)) continue;
        impl.Draw(pc, *lp, slot, t.ldr, impl.scratch_, false);
    }
    t.ldr.Transition(cmd, kSrv);
}

bool VideoPass::Impl::DecodeOffline(PassContext& pc, const VideoLayerDesc& d, bool wantBg, Texture& target,
                                    Texture& depthOrScratch) {
    if (offlinePath != d.path) {
        std::string err;
        if (!offlineReader.Open(d.path, &err)) {
            LOG_WARN("offline video: cannot open %s: %s", PathToUtf8(d.path).c_str(), err.c_str());
            offlinePath.clear();
            return false;
        }
        offlinePath = d.path;
    }
    const double dur = offlineReader.Duration();
    double vt = d.time;
    if (wantBg && dur > 0.0) {
        vt = std::fmod(vt, dur);
        if (vt < 0.0) vt += dur;
    }
    std::vector<uint8_t> px;
    if (!offlineReader.ReadFrame(vt, px) || px.empty()) return false;
    for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);   // BGRA -> RGBA

    const uint32_t w = offlineReader.Width(), h = offlineReader.Height();
    if (!offlineTex || offlineW != w || offlineH != h) {
        offlineTex.Release(pc.ctx);
        if (!offlineTex.Create(pc.ctx, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, kSrv,
                               L"video.offline"))
            return false;
        offlineW = w; offlineH = h;
    }
    if (!UploadFrame(pc.ctx, pc.cmd, offlineTex, w, h, px)) return false;

    float c[8];
    LayerConstants(w, h, d.kind, d.blend, target.width, target.height, 1.0f, c);
    if (wantBg) {
        target.Transition(pc.cmd, kRt);
        depthOrScratch.Transition(pc.cmd, kSrv);
        pipe_.Draw(pc, {&target}, pc.transient.SrvTable(pc.ctx, {nullptr, &offlineTex, &depthOrScratch}), c, 8);
        target.Transition(pc.cmd, kSrv);
    } else {
        depthOrScratch.Transition(pc.cmd, kRt);
        copyPipe_.Draw(pc, {&depthOrScratch}, pc.transient.SrvTable(pc.ctx, {&target}), nullptr, 0);
        depthOrScratch.Transition(pc.cmd, kSrv);
        target.Transition(pc.cmd, kRt);
        pipe_.Draw(pc, {&target}, pc.transient.SrvTable(pc.ctx, {&depthOrScratch, &offlineTex}), c, 8);
        target.Transition(pc.cmd, kSrv);
    }
    return true;
}

VideoPass::VideoPass(VideoStage stage) : impl_(std::make_unique<Impl>()), stage_(stage) {}

VideoPass::~VideoPass() {
    if (impl_)
        for (auto& l : impl_->layers) l->StopWorker();
}

bool VideoPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    const std::filesystem::path file = shaderDir / L"video.hlsl";
    if (stage_ == VideoStage::Background) return impl_->pipe_.Create(ctx, file, "PSVideoBg", {RenderTargets::kLdrFormat});
    return impl_->pipe_.Create(ctx, file, "PSVideoOverlay", {RenderTargets::kLdrFormat}) &&
           impl_->copyPipe_.Create(ctx, file, "PSVideoCopy", {RenderTargets::kLdrFormat});
}

void VideoPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    if (stage_ != VideoStage::Overlay) return;
    impl_->scratch_.Create(ctx, targets.outWidth, targets.outHeight, RenderTargets::kLdrFormat,
                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kSrv, L"video.scratch");
}

void VideoPass::ReleaseTargets(Dx12Context& ctx) {
    if (stage_ != VideoStage::Overlay) return;
    impl_->scratch_.Release(ctx);
}

void VideoPass::Clear(Dx12Context& ctx) {
    for (auto& l : impl_->layers) { l->StopWorker(); l->ReleaseGpu(ctx); }
    impl_->layers.clear();
    impl_->offlineReader.Close();
    impl_->offlinePath.clear();
    impl_->offlineTex.Release(ctx);
    impl_->offlineW = impl_->offlineH = 0;
    impl_->lastSceneTime = -1.0;
}

bool VideoPass::RunOffline(PassContext& pc, Texture& target, Texture& depthOrScratch) {
    const bool wantBg = stage_ == VideoStage::Background;
    bool drew = false;
    for (const VideoLayerDesc& d : pc.view.videoLayers) {
        if ((d.kind == VideoLayerKind::Background) != wantBg || d.path.empty()) continue;
        if (impl_->DecodeOffline(pc, d, wantBg, target, depthOrScratch)) drew = true;
    }
    return drew;
}

} // namespace mmdx
