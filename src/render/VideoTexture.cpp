// VideoTexture: one video file decoded into one texture (extracted from ScreenVideo.cpp). See VideoTexture.h.
#include "render/VideoTexture.h"
#include "render/RenderPass.h"
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

struct VideoTexture::Impl {
    struct CpuFrame {
        std::vector<uint8_t> px;   // RGBA8, top-down
        double time = -1.0;
        uint64_t serial = 0;
        bool ready = false;
    };

    std::filesystem::path path;
    VideoReader reader;
    uint32_t w = 0, h = 0;
    double duration = 0.0, fps = 30.0;

    // decode worker + CPU ring
    std::mutex mtx;
    std::condition_variable cv;
    CpuFrame cpu[kCpuRing];
    uint64_t serial = 0;
    double want = 0.0;
    uint64_t wantGen = 0;      // bumped on a seek: the worker flushes the ring
    std::thread worker;
    std::atomic<bool> quit{false};

    // GPU: one texture, per-frame-in-flight staging buffers + persistent mapped pointers
    Texture tex;
    ComPtr<ID3D12Resource> staging[Dx12Context::kFramesInFlight];
    void* stagingMapped[Dx12Context::kFramesInFlight] = {};
    uint32_t pitch = 0;

    // offline (synchronous) decode
    std::vector<uint8_t> offlinePx;
    VideoReader offlineReader;
    std::filesystem::path offlinePath;

    void StartWorker() { worker = std::thread([this] { Run(); }); }
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
    uint64_t texSerial = UINT64_MAX;
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
        return true;
    }
};

VideoTexture::VideoTexture() : impl_(std::make_unique<Impl>()) {}

VideoTexture::~VideoTexture() {
    if (impl_) impl_->StopWorker();
}

VideoTexture::VideoTexture(VideoTexture&& o) noexcept : impl_(std::move(o.impl_)) {}
VideoTexture& VideoTexture::operator=(VideoTexture&& o) noexcept {
    if (this != &o) {
        if (impl_) impl_->StopWorker();
        impl_ = std::move(o.impl_);
    }
    return *this;
}

bool VideoTexture::IsOpen() const { return impl_ && !impl_->path.empty(); }
bool VideoTexture::HasTexture() const { return impl_ && impl_->tex.res != nullptr; }
ID3D12Resource* VideoTexture::TextureResource() const { return impl_ && impl_->tex ? impl_->tex.res.Get() : nullptr; }
const std::filesystem::path& VideoTexture::Path() const {
    static const std::filesystem::path kEmpty;
    return impl_ ? impl_->path : kEmpty;
}
uint32_t VideoTexture::Width() const { return impl_ ? impl_->w : 0; }
uint32_t VideoTexture::Height() const { return impl_ ? impl_->h : 0; }
double VideoTexture::Duration() const { return impl_ ? impl_->duration : 0.0; }

bool VideoTexture::Open(Dx12Context& ctx, const std::filesystem::path& p) {
    Impl& impl = *impl_;
    if (impl.path == p && impl.tex) return true;
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
    Release(ctx);
    std::string err;
    if (!impl.reader.Open(p, &err)) {
        LOG_WARN("screen video: cannot open %s: %s", PathToUtf8(p).c_str(), err.c_str());
        return false;
    }
    impl.w = impl.reader.Width();
    impl.h = impl.reader.Height();
    impl.duration = impl.reader.Duration();
    impl.fps = impl.reader.Fps();
    if (!impl.OpenGpu(ctx)) {
        LOG_WARN("screen video: GPU upload resources failed for %s", PathToUtf8(p).c_str());
        impl.reader.Close();
        return false;
    }
    impl.path = p;
    impl.quit.store(false);
    impl.StartWorker();
    LOG_INFO("screen video: %s %ux%u %.2fs %.2f fps", PathToUtf8(p.filename()).c_str(), impl.w, impl.h,
             impl.duration, impl.fps);
    return true;
}

void VideoTexture::Release(Dx12Context& ctx) {
    Impl& impl = *impl_;
    impl.StopWorker();
    for (uint32_t i = 0; i < Dx12Context::kFramesInFlight; ++i) {
        if (impl.staging[i] && impl.stagingMapped[i]) impl.staging[i]->Unmap(0, nullptr);
        impl.stagingMapped[i] = nullptr;
        if (impl.staging[i]) ctx.DeferRelease(impl.staging[i]);
        impl.staging[i].Reset();
    }
    impl.tex.Release(ctx);
    impl.offlineReader.Close();
    impl.offlinePath.clear();
    impl.offlinePx.clear();
    impl.reader.Close();
    impl.path.clear();
    impl.texSerial = UINT64_MAX;
}

bool VideoTexture::Update(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, uint32_t slot, double t, bool exact,
                          bool seek) {
    Impl& impl = *impl_;
    if (!impl.tex) return false;
    // The video time loops by duration (same rule as the background pass).
    if (impl.duration > 0.0) {
        t = std::fmod(t, impl.duration);
        if (t < 0.0) t += impl.duration;
    }
    if (exact) {
        // Offline render: decode the exact frame for the rendered time synchronously.
        std::vector<uint8_t>& px = impl.offlinePx;
        if (impl.offlinePath != impl.path) {
            std::string err;
            if (!impl.offlineReader.Open(impl.path, &err)) {
                LOG_WARN("screen video: cannot open %s: %s", PathToUtf8(impl.path).c_str(), err.c_str());
                impl.offlinePath.clear();
                return false;
            }
            impl.offlinePath = impl.path;
        }
        if (!impl.offlineReader.ReadFrame(t, px) || px.empty()) return false;
        for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);   // BGRA -> RGBA
        if (!impl.Upload(ctx, cmd, slot, px)) return false;
        impl.texSerial = UINT64_MAX;   // the real-time ring re-uploads after an exact frame
        return true;
    }
    // Real time: push the wanted time into the worker and upload the ready frame.
    {
        std::lock_guard<std::mutex> l(impl.mtx);
        impl.want = t;
        if (seek) ++impl.wantGen;
        impl.cv.notify_all();
    }
    impl.UploadFromRing(ctx, cmd, slot, t);   // the shown texture stays even when no frame is ready
    return true;
}

} // namespace mmdx
