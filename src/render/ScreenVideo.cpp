// ScreenVideo: decode the background AVI (背景AVI) into one texture and bind it to every material whose diffuse
// texture file name is screen.bmp (render/GpuModel.h). See ScreenVideo.h.
#include "render/ScreenVideo.h"
#include "render/GpuModel.h"
#include "render/RenderPass.h"
#include "render/RenderTypes.h"
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
#include <vector>

namespace mmdx {

namespace {
constexpr uint32_t kCpuRing = 3;                                 // decoded CPU frames kept ahead of the requested time
constexpr size_t kMaxVideoBytes = 96ull * 1024 * 1024;           // guard: refuse absurd layer sizes
inline uint32_t StagingPitch(uint32_t w) { return (w * 4u + 255u) & ~255u; }
// Raster PS, RT ray-query PS and the PT / offline GI compute shaders all sample the texture.
constexpr D3D12_RESOURCE_STATES kScreenSrvState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
} // namespace

struct ScreenVideo::Impl {
    struct CpuFrame {
        std::vector<uint8_t> px;   // RGBA8, top-down
        double time = -1.0;
        uint64_t serial = 0;
        bool ready = false;
    };

    std::filesystem::path path;
    std::filesystem::path failedPath;   // a path whose open failed: warn once, do not retry every frame
    VideoReader reader;
    uint32_t w = 0, h = 0;
    double duration = 0.0, fps = 30.0;
    int screenMaterialCount = 0;

    // decode worker + CPU ring
    std::mutex mtx;
    std::condition_variable cv;
    CpuFrame cpu[kCpuRing];
    uint64_t serial = 0;
    double want = 0.0;
    uint64_t wantGen = 0;      // bumped on a seek: the worker flushes the ring
    double lastSceneTime = -1.0;
    std::thread worker;
    std::atomic<bool> quit{false};

    // GPU: one texture, per-frame-in-flight staging buffers + persistent mapped pointers
    Texture tex;
    ComPtr<ID3D12Resource> staging[Dx12Context::kFramesInFlight];
    void* stagingMapped[Dx12Context::kFramesInFlight] = {};
    uint32_t pitch = 0;
    uint32_t generation = 0;   // bumped when the texture is created / released

    // which models have been bound to the current generation (keyed by GpuModel::Uid)
    std::unordered_map<uint64_t, uint32_t> boundGen;

    // offline (synchronous) decode
    std::vector<uint8_t> offlinePx;
    VideoReader offlineReader;
    std::filesystem::path offlinePath;

    void StartWorker() {
        worker = std::thread([this] { Run(); });
    }
    void StopWorker() {
        if (!worker.joinable()) return;
        quit.store(true);
        { std::lock_guard<std::mutex> l(mtx); cv.notify_all(); }
        worker.join();
    }

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
            s.px = frame;
            s.time = t;
            s.serial = ++serial;
            s.ready = true;
            lastTime = t;
        }
    }

    // The ready frame nearest but not after `t`, uploaded into the texture when newer. True when a frame is shown.
    std::vector<uint8_t> uploadScratch;
    bool UploadFromRing(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, uint32_t slot, double t) {
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
        if (!have) return false;
        if (frameSerial == texSerial) return true;
        if (Upload(ctx, cmd, slot, uploadScratch)) { texSerial = frameSerial; return true; }
        return false;
    }
    uint64_t texSerial = UINT64_MAX;

    bool Upload(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, uint32_t slot, const std::vector<uint8_t>& px) {
        if (slot >= Dx12Context::kFramesInFlight || !staging[slot] || !tex) return false;
        if (px.size() != (size_t)w * h * 4) return false;
        void* mapped = stagingMapped[slot];
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy((uint8_t*)mapped + (size_t)y * pitch, px.data() + (size_t)y * w * 4, (size_t)w * 4);
        tex.Transition(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
        CD3DX12_TEXTURE_COPY_LOCATION dst(tex.res.Get(), 0);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{0, {DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, pitch}};
        CD3DX12_TEXTURE_COPY_LOCATION src(staging[slot].Get(), fp);
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        tex.Transition(cmd, kScreenSrvState);
        return true;
    }

    bool OpenGpu(Dx12Context& ctx) {
        if ((size_t)w * h * 4 > kMaxVideoBytes) return false;
        pitch = StagingPitch(w);
        if (!tex.Create(ctx, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                        kScreenSrvState, L"screen.video"))
            return false;
        const uint64_t stagingBytes = (uint64_t)pitch * h;
        for (uint32_t i = 0; i < Dx12Context::kFramesInFlight; ++i) {
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
            CD3DX12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(stagingBytes);
            if (FAILED(ctx.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                            IID_PPV_ARGS(&staging[i])))) return false;
            D3D12_RANGE read{0, 0};
            if (FAILED(staging[i]->Map(0, &read, &stagingMapped[i]))) return false;
        }
        texSerial = UINT64_MAX;
        ++generation;
        return true;
    }

    void ReleaseGpu(Dx12Context& ctx) {
        StopWorker();
        for (uint32_t i = 0; i < Dx12Context::kFramesInFlight; ++i) {
            if (staging[i] && stagingMapped[i]) staging[i]->Unmap(0, nullptr);
            stagingMapped[i] = nullptr;
            if (staging[i]) ctx.DeferRelease(staging[i]);
            staging[i].Reset();
        }
        tex.Release(ctx);
        reader.Close();
        path.clear();
        ++generation;
        texSerial = UINT64_MAX;
    }

    // Open / switch to `p`. Returns true when a texture is ready for the current generation. A file that cannot be
    // opened leaves the current video (texture and binding) untouched.
    bool Open(Dx12Context& ctx, const std::filesystem::path& p) {
        {   // header-only probe first: a broken file must not clobber the current video
            VideoReader probe;
            std::string err;
            if (!probe.Open(p, &err)) {
                LOG_WARN("screen video: cannot open %s: %s", PathToUtf8(p).c_str(), err.c_str());
                probe.Close();
                return false;
            }
            probe.Close();
        }
        ReleaseGpu(ctx);
        std::string err;
        if (!reader.Open(p, &err)) {
            LOG_WARN("screen video: cannot open %s: %s", PathToUtf8(p).c_str(), err.c_str());
            return false;
        }
        w = reader.Width();
        h = reader.Height();
        duration = reader.Duration();
        fps = reader.Fps();
        if (!OpenGpu(ctx)) {
            LOG_WARN("screen video: GPU upload resources failed for %s", PathToUtf8(p).c_str());
            reader.Close();
            return false;
        }
        path = p;
        quit.store(false);
        StartWorker();
        LOG_INFO("screen video: %s %ux%u %.2fs %.2f fps, %d screen materials", PathToUtf8(p.filename()).c_str(), w, h,
                 duration, fps, screenMaterialCount);
        return true;
    }
};

ScreenVideo::ScreenVideo() : impl_(std::make_unique<Impl>()) {}

ScreenVideo::~ScreenVideo() {
    if (impl_) impl_->StopWorker();
}

void ScreenVideo::ForgetModel(uint64_t uid) {
    if (impl_) impl_->boundGen.erase(uid);
}

void ScreenVideo::Clear(Dx12Context& ctx) {
    if (!impl_) return;
    impl_->ReleaseGpu(ctx);
    impl_->boundGen.clear();
    impl_->failedPath.clear();
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
            if (!impl.Open(ctx, layer->path)) {
                impl.failedPath = layer->path;   // one warning line (Open logs it); keep the previous binding
            } else {
                impl.failedPath.clear();
            }
        }
        if (impl.tex) {
            haveTex = true;
            // The video time loops by duration (same rule as the background pass).
            double t = layer->time;
            if (impl.duration > 0.0) {
                t = std::fmod(t, impl.duration);
                if (t < 0.0) t += impl.duration;
            }
            const uint32_t slot = ctx.FrameSlot();
            if (view.videoExact) {
                // Offline render: decode the exact frame for the rendered time synchronously.
                std::vector<uint8_t>& px = impl.offlinePx;
                if (impl.offlinePath != layer->path) {
                    std::string err;
                    if (!impl.offlineReader.Open(layer->path, &err)) {
                        LOG_WARN("screen video: cannot open %s: %s", PathToUtf8(layer->path).c_str(), err.c_str());
                        impl.offlinePath.clear();
                        haveTex = false;
                    } else {
                        impl.offlinePath = layer->path;
                    }
                }
                if (haveTex) {
                    if (!impl.offlineReader.ReadFrame(t, px) || px.empty()) haveTex = false;
                    else {
                        for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);   // BGRA -> RGBA
                        haveTex = impl.Upload(ctx, cmd, slot, px);
                        impl.texSerial = UINT64_MAX;   // the real-time ring re-uploads after an exact frame
                    }
                }
            } else {
                // Real time: push the wanted time into the worker and upload the ready frame.
                const bool seek = impl.lastSceneTime >= 0.0 && (double)view.sceneTime + 1e-6 < impl.lastSceneTime;
                impl.lastSceneTime = (double)view.sceneTime;
                {
                    std::lock_guard<std::mutex> l(impl.mtx);
                    impl.want = t;
                    if (seek) ++impl.wantGen;
                    impl.cv.notify_all();
                }
                impl.UploadFromRing(ctx, cmd, slot, t);   // the shown texture stays even when no frame is ready
            }
        }
    } else if (impl.tex || !impl.path.empty()) {
        ctx.WaitForGpu();
        impl.ReleaseGpu(ctx);   // bumps the generation: models rebind to their own texture below
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
        ID3D12Resource* bindRes = haveTex ? impl.tex.res.Get() : nullptr;
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
