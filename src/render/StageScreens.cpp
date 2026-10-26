// S5 stage screens: one texture per LED surface, its source (image / video / camera feed), and the per-material
// binding. See StageScreens.h. The camera feed itself is rendered by the Renderer (the callback): it owns the scene
// passes and the constant-buffer slots.
#include "render/StageScreens.h"
#include "render/GpuModel.h"
#include "render/VideoTexture.h"
#include "asset/ImageLoader.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mmdx {

namespace {

constexpr uint32_t kScreenCap = 2048;          // a screen texture is at most this wide / tall
constexpr uint32_t kFeedCapW = 960;            // a camera feed renders at most this large
constexpr uint32_t kFeedCapH = 540;

uint64_t HashMix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}
uint64_t HashText(uint64_t h, const std::string& s) {
    for (unsigned char c : s) h = HashMix(h, c);
    return HashMix(h, s.size());
}
uint64_t HashPath(uint64_t h, const std::filesystem::path& p) {
    return HashText(h, PathToUtf8(p));
}

// One frame's binding of a model: the signature of what was bound, so a change restores the model's own materials.
struct ModelBinding {
    uint64_t sig = 0;
    std::vector<uint32_t> materials;
};

} // namespace

struct StageScreens::Impl {
    struct Slot {
        std::string key;
        uint64_t modelUid = 0;
        std::vector<uint32_t> materials;
        StageScreenSource source = StageScreenSource::None;
        std::filesystem::path path;
        uint32_t width = 0, height = 0;   // texture size (capped)
        float brightness = 1.0f;
        bool enabled = true;
        uint64_t generation = 0;          // bumped when the bound texture changes
        Texture tex;                      // image / camera feed texture (owned)
        ComPtr<ID3D12Resource> imageRes;  // UploadBatch image texture (no RTV; the SRV source)
        VideoTexture video;               // video source (one decoder per video screen)
        std::filesystem::path failedPath; // a file whose load failed: warn once, show black

        Slot() = default;
        Slot(Slot&&) = default;
        Slot& operator=(Slot&&) = default;
    };

    std::vector<Slot> slots;
    ComPtr<ID3D12Resource> black;          // 1x1 black texture for source none / program / off
    std::unordered_map<uint64_t, ModelBinding> bound;
    uint32_t feedCursor = 0;               // round robin over the camera feeds

    bool warnedProgram = false;
};

StageScreens::StageScreens() : impl_(std::make_unique<Impl>()) {}
StageScreens::~StageScreens() = default;

void StageScreens::ForgetModel(uint64_t uid) {
    if (impl_) impl_->bound.erase(uid);
}

namespace {

// A 1x1 black texture for a screen with no source (none / program / off): the surface stays black instead of
// showing the material's own texture. A texture cannot live in an UPLOAD heap, so the value is copied in once through
// a tiny upload buffer on the frame's command list.
bool EnsureBlack(ComPtr<ID3D12Resource>& black, Dx12Context& ctx, ID3D12GraphicsCommandList* cmd) {
    if (black) return true;
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 1, 0,
                                                              D3D12_RESOURCE_FLAG_NONE);
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
    if (!CheckHr(ctx.Device()->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&black)),
                 "stage screen: black texture"))
        return false;
    black->SetName(L"screen.black");
    const D3D12_RESOURCE_DESC td = black->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    ctx.Device()->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
    const CD3DX12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(total);
    ComPtr<ID3D12Resource> staging;
    if (!CheckHr(ctx.Device()->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bd,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&staging)),
                 "stage screen: black staging"))
        { black.Reset(); return false; }
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, nullptr, &mapped))) {
        black.Reset();
        return false;
    }
    std::memset(mapped, 0, (size_t)total);
    staging->Unmap(0, nullptr);
    if (cmd) {
        CD3DX12_TEXTURE_COPY_LOCATION dst(black.Get(), 0);
        CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(), fp);
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            black.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &barrier);
    }
    ctx.DeferRelease(staging);   // freed once the frame that copies from it is done
    return true;
}

// Loads `path` into one texture, capped at kScreenCap (a larger image is downscaled, aspect kept).
bool LoadScreenImage(Dx12Context& ctx, const std::filesystem::path& path, ComPtr<ID3D12Resource>& out,
                     std::string& error) {
    ImageRGBA8 image;
    if (!LoadImageRGBA8(path, image, &error) || image.Empty()) {
        if (error.empty()) error = "cannot decode the image";
        return false;
    }
    uint32_t w = image.Width(), h = image.Height();
    if (w > kScreenCap || h > kScreenCap) {
        // DownscaleRgba8 keeps the aspect and only takes a width: pick the width that also caps the height.
        const uint32_t maxW = w >= h ? kScreenCap : std::max(1u, (uint32_t)((uint64_t)w * kScreenCap / h));
        uint32_t nw = 0, nh = 0;
        std::vector<uint8_t> px = DownscaleRgba8(w, h, image.mips[0].pixels.data(), maxW, nw, nh);
        if (nw == 0 || nh == 0 || px.empty()) {
            error = "downscale failed";
            return false;
        }
        ImageRGBA8 resized;
        resized.mips.push_back({nw, nh, std::move(px)});
        resized.hasAlpha = image.hasAlpha;
        image = std::move(resized);
    }
    UploadBatch batch(ctx);
    out = batch.CreateTexture(image, L"stage.screen");
    batch.Submit();
    if (!out) {
        error = "texture upload failed";
        return false;
    }
    return true;
}

} // namespace

void StageScreens::Clear(Dx12Context& ctx) {
    Impl& impl = *impl_;
    if (!ctx.Device()) return;
    for (Impl::Slot& s : impl.slots) {
        s.video.Release(ctx);
        s.tex.Release(ctx);
        s.imageRes.Reset();
    }
    impl.slots.clear();
    impl.black.Reset();
    impl.bound.clear();
    impl.feedCursor = 0;
}

void StageScreens::Render(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, const FrameView& view,
                          const FeedRender& renderFeed) {
    Impl& impl = *impl_;
    if (!ctx.Device() || !cmd) return;

    // 1. Reconcile the slot list with this frame's screens (a screen keeps its texture by key).
    {
        std::vector<Impl::Slot> next;
        next.reserve(view.screens.size());
        for (const ScreenView& sv : view.screens) {
            auto it = std::find_if(impl.slots.begin(), impl.slots.end(),
                                   [&](const Impl::Slot& s) { return s.key == sv.key; });
            if (it != impl.slots.end()) {
                Impl::Slot slot = std::move(*it);
                impl.slots.erase(it);
                next.push_back(std::move(slot));
            } else {
                Impl::Slot slot;
                slot.key = sv.key;
                next.push_back(std::move(slot));
            }
        }
        // what is left belongs to no screen anymore: free it
        for (Impl::Slot& gone : impl.slots) {
            gone.video.Release(ctx);
            gone.tex.Release(ctx);
        }
        impl.slots = std::move(next);
    }

    // 2. Sources: create / free the textures the screens need this frame.
    bool needWait = false;
    for (size_t i = 0; i < impl.slots.size(); ++i) {
        Impl::Slot& s = impl.slots[i];
        const ScreenView& sv = view.screens[i];
        s.modelUid = sv.modelUid;
        s.materials = sv.materials;
        s.brightness = sv.brightness;
        s.enabled = sv.enabled;
        s.width = std::clamp(sv.width, 1u, kScreenCap);
        s.height = std::clamp(sv.height, 1u, kScreenCap);
        const StageScreenSource source = sv.enabled ? sv.source : StageScreenSource::None;
        if (source != s.source) {
            // switching source drops the previous one (the video decoder, the image / feed texture)
            if (s.source == StageScreenSource::Video) s.video.Release(ctx);
            if (s.tex) {
                needWait = true;
                s.tex.Release(ctx);
            }
            s.imageRes.Reset();
            s.source = source;
            s.path.clear();
            ++s.generation;
        }
        if (source == StageScreenSource::Image || source == StageScreenSource::Video) {
            if (s.path != sv.path) {
                if (s.video.IsOpen() || s.tex || s.imageRes) {
                    s.video.Release(ctx);
                    if (s.tex) {
                        needWait = true;
                        s.tex.Release(ctx);
                    }
                    s.imageRes.Reset();
                }
                s.path = sv.path;
                s.failedPath.clear();
                ++s.generation;
            }
        } else {
            s.path = sv.path;   // recorded for the log only
        }
        switch (source) {
        case StageScreenSource::Image:
            if (!s.imageRes && !s.path.empty() && s.failedPath != s.path) {
                std::string error;
                if (LoadScreenImage(ctx, s.path, s.imageRes, error)) {
                    LOG_INFO("stage screen '%s': image %s", s.key.c_str(), PathToUtf8(s.path.filename()).c_str());
                    ++s.generation;
                } else {
                    LOG_WARN("stage screen '%s': image %s: %s", s.key.c_str(), PathToUtf8(s.path).c_str(),
                             error.c_str());
                    s.failedPath = s.path;
                    ++s.generation;
                }
            }
            if (!s.imageRes) EnsureBlack(impl.black, ctx, cmd);
            break;
        case StageScreenSource::Video:
            if (!s.video.IsOpen() && !s.path.empty() && s.failedPath != s.path) {
                if (s.video.Open(ctx, s.path)) {
                    LOG_INFO("stage screen '%s': video %s %ux%u", s.key.c_str(),
                             PathToUtf8(s.path.filename()).c_str(), s.video.Width(), s.video.Height());
                    ++s.generation;
                } else {
                    s.failedPath = s.path;   // Open logged the reason; one warning per file
                    ++s.generation;
                }
            }
            if (!s.video.IsOpen()) EnsureBlack(impl.black, ctx, cmd);
            break;
        case StageScreenSource::Camera:
            if (!s.tex) {
                const uint32_t w = std::min(s.width, kFeedCapW), h = std::min(s.height, kFeedCapH);
                const float clear[4] = {0, 0, 0, 1};
                if (s.tex.Create(ctx, std::max(1u, w), std::max(1u, h), DXGI_FORMAT_R8G8B8A8_UNORM,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                 D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, L"screen.feed", 1, 1, clear))
                {
                    LOG_INFO("stage screen '%s': camera feed %ux%u", s.key.c_str(), w, h);
                    ++s.generation;
                }
                else
                    LOG_WARN("stage screen '%s': feed texture creation failed", s.key.c_str());
            }
            break;
        case StageScreenSource::Program:
            if (!impl.warnedProgram) {
                impl.warnedProgram = true;
                LOG_WARN("stage screen '%s': the \"program\" source is not implemented yet (black)", s.key.c_str());
            }
            EnsureBlack(impl.black, ctx, cmd);
            break;
        case StageScreenSource::None:
        default:
            EnsureBlack(impl.black, ctx, cmd);
            break;
        }
    }
    if (needWait) ctx.WaitForGpu();

    // 3. Video decode / upload for this frame (loops by duration; exact for offline renders).
    for (Impl::Slot& s : impl.slots) {
        if (s.source != StageScreenSource::Video || !s.video.IsOpen()) continue;
        if (!s.video.Update(ctx, cmd, ctx.FrameSlot(), (double)view.sceneTime, view.videoExact, view.cameraCut))
            continue;   // no frame yet: the previous texture stays
    }

    // 4. Camera feeds: render at most kMaxFeedsPerFrame per frame, round robin over the rest.
    {
        std::vector<size_t> feeds;
        for (size_t i = 0; i < impl.slots.size(); ++i)
            if (impl.slots[i].source == StageScreenSource::Camera && impl.slots[i].tex && impl.slots[i].enabled)
                feeds.push_back(i);
        if (!feeds.empty()) {
            const uint32_t count = std::min<uint32_t>((uint32_t)feeds.size(), kMaxFeedsPerFrame);
            for (uint32_t f = 0; f < count; ++f) {
                const size_t idx = feeds[(impl.feedCursor + f) % feeds.size()];
                Impl::Slot& s = impl.slots[idx];
                const ScreenView& sv = view.screens[idx];
                if (renderFeed(f, sv, s.tex))
                    s.tex.state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            }
            impl.feedCursor = (impl.feedCursor + count) % (uint32_t)feeds.size();
        }
    }

    // 5. Bind: every screen's texture goes on the materials it claims (MatFlag_Screen = emission).
    // The descriptor rewrite and the constants flush wait for the GPU (frames in flight read both).
    bool bindChanged = false;
    std::unordered_map<uint64_t, ModelBinding> next;
    for (size_t i = 0; i < impl.slots.size(); ++i) {
        const Impl::Slot& s = impl.slots[i];
        uint64_t sig = 0x9e3779b97f4a7c15ull;
        sig = HashMix(sig, s.modelUid);
        sig = HashText(sig, s.key);
        sig = HashMix(sig, s.generation);
        uint32_t bits = 0;
        std::memcpy(&bits, &s.brightness, sizeof(bits));
        sig = HashMix(sig, bits);
        sig = HashMix(sig, s.materials.size());
        for (uint32_t m : s.materials) sig = HashMix(sig, m);
        auto it = next.find(s.modelUid);
        if (it == next.end()) {
            ModelBinding mb;
            mb.sig = sig;
            mb.materials = s.materials;
            next.emplace(s.modelUid, std::move(mb));
        } else {
            it->second.sig = HashMix(it->second.sig, sig);
            for (uint32_t m : s.materials)
                if (std::find(it->second.materials.begin(), it->second.materials.end(), m) ==
                    it->second.materials.end())
                    it->second.materials.push_back(m);
        }
    }
    for (const auto& [uid, mb] : next) {
        const auto old = impl.bound.find(uid);
        if (old != impl.bound.end() && old->second.sig == mb.sig) continue;
        bindChanged = true;
    }
    for (const auto& [uid, mb] : impl.bound) {
        if (!next.count(uid)) bindChanged = true;
    }
    if (bindChanged || (impl.bound.empty() && !next.empty())) {
        ctx.WaitForGpu();
        // look the models up in this frame's drawn set (never keep GpuModel pointers: a model can be freed)
        const auto modelOf = [&](uint64_t uid) -> GpuModel* {
            for (GpuModel* m : view.models)
                if (m && m->Uid() == uid) return m;
            return nullptr;
        };
        // restore the materials of models whose screens went away / changed
        for (const auto& [uid, mb] : impl.bound) {
            GpuModel* model = modelOf(uid);
            const auto nit = next.find(uid);
            if (!model) continue;
            if (nit != next.end() && nit->second.sig == mb.sig) continue;
            for (uint32_t m : mb.materials) {
                bool still = false;
                if (nit != next.end())
                    still = std::find(nit->second.materials.begin(), nit->second.materials.end(), m) !=
                            nit->second.materials.end();
                if (!still) {
                    model->SetMaterialScreen(m, false, 1.0f);
                    model->BindMaterialTexture(ctx, m, nullptr);
                }
            }
        }
        for (size_t i = 0; i < impl.slots.size(); ++i) {
            const Impl::Slot& s = impl.slots[i];
            GpuModel* model = modelOf(s.modelUid);
            if (!model) continue;
            ID3D12Resource* res = nullptr;
            if (s.source == StageScreenSource::Image)
                res = s.imageRes.Get();
            else if (s.source == StageScreenSource::Video)
                res = s.video.TextureResource();
            else if (s.source == StageScreenSource::Camera)
                res = s.tex.res.Get();
            if (!res) res = impl.black.Get();
            if (!res) continue;
            for (uint32_t m : s.materials) {
                model->SetMaterialScreen(m, true, s.brightness);
                model->BindMaterialTexture(ctx, m, res);
            }
        }
        for (const auto& [uid, mb] : next) {
            GpuModel* model = modelOf(uid);
            if (model) model->FlushMaterialConstants(ctx.FrameNumber());
        }
        for (const auto& [uid, mb] : impl.bound) {
            GpuModel* model = modelOf(uid);
            if (model) model->FlushMaterialConstants(ctx.FrameNumber());
        }
        impl.bound = std::move(next);
    }
}

} // namespace mmdx
