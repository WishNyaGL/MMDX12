#pragma once
#include "render/Dx12Context.h"
#include "render/GpuModel.h"
#include "render/OfflineRenderer.h"
#include "render/PackTextures.h"
#include "render/RayTracing.h"
#include "render/RenderPass.h"
#include "render/RenderTypes.h"
#include "render/ShaderInterop.h"
#include "render/Upscaler.h"
#include "render/DlssFrameGen.h"
#include "render/MmeRuntime.h"
#include "render/MmeSkin.h"
#include "render/ScreenVideo.h"
#include "render/StageScreens.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mmdx {

class MmeRuntime;
class MmeSkin;
class ScenePass;

struct PmxModel;
struct ImageRGBA8;
struct CompressedImage;
struct PbrModelData;

struct RenderInitProgress {
    std::atomic<float> fraction{0.0f};  // 0..1
    std::mutex mutex;
    std::string status;                 // guarded by mutex, UTF-8
    void SetStatus(const std::string& s) { std::lock_guard<std::mutex> l(mutex); status = s; }
    std::string Status() { std::lock_guard<std::mutex> l(mutex); return status; }
};

// Frame orchestration: see Passes.h for the pass list. Without models (menus) the renderer
// only clears the back buffer, so the UI can draw on a clean surface.
class Renderer {
public:
    static constexpr uint32_t kMaxPunctualLights = 16;
    static constexpr uint32_t kMaxFogVolumes = 16;   // S9: local fog volumes bound to the volumetric passes
    Renderer() = default;
    ~Renderer();

    bool Initialize(Dx12Context& ctx, const std::filesystem::path& shaderDir, RenderInitProgress* progress = nullptr);
    void Shutdown();  // waits for GPU

    std::unique_ptr<GpuModel> CreateModel(UploadBatch& batch, const PmxModel& pmx,
                                          const std::vector<ImageRGBA8>& textures,
                                          ModelRole role = ModelRole::Character,
                                          const PbrModelData* pbr = nullptr,
                                          const std::vector<CompressedImage>* compressed = nullptr);

    // M6b: hot-reload failure / success messages (the UI shows a toast). Set once at startup.
    void SetMmeMessage(std::function<void(const std::string&, bool /*error*/)> cb) {
        if (mme_) mme_->SetErrorCallback([cb](const std::string& m) { if (cb) cb(m, true); });
    }

    void SetSettings(const RenderSettings& s) { settings_ = s; }
    // Colour LUT for PostPass (strip layout in render/ColorLut.h); null or empty clears it.
    // Uploads and waits for the GPU: call outside of frame recording.
    void SetColorLut(const ImageRGBA8* strip);
    bool HasColorLut() const { return targets_.lut != nullptr; }
    const RenderSettings& Settings() const { return settings_; }

    // Records the frame into `cmd` (from ctx.BeginFrame()). The caller must already have
    // called model->UpdateSkinning/UpdateMorphs with ctx.FrameNumber(). On return the back
    // buffer holds the final image, is in RENDER_TARGET state, its RTV is bound, the
    // viewport/scissor cover the whole back buffer and ctx.SrvHeap() is the bound heap,
    // so the caller can draw UI (ImGui) directly afterwards.
    void Render(ID3D12GraphicsCommandList* cmd, const FrameView& view);
    // The menu surface Render() leaves without models: the back buffer cleared and bound for the UI. Used
    // after background work (a headless sample render) in the same frame; keeps temporal history.
    void ClearBackBuffer(ID3D12GraphicsCommandList* cmd);

    // Renders `view` into a w x h RGBA8 image (level 0 only) with a transparent background
    // (alpha = coverage, straight alpha). Uses its own command list and waits for the GPU.
    // Must be called outside of frame recording (before ctx.BeginFrame()).
    bool RenderToImage(const FrameView& view, uint32_t w, uint32_t h, ImageRGBA8& out);

    // The final (post-processed, sRGB) image of the last Render() call at output resolution, as an
    // opaque RGBA8 image. Waits for the GPU; call after the frame was submitted (ctx.EndFrame),
    // outside frame recording. Used to render videos with the real-time renderers.
    // preserveAlpha: keep the frame's coverage alpha (RenderSettings::transparentBackground) and un-premultiply the
    // colour, giving straight RGBA8 for a transparent export; false forces opaque (today's behaviour).
    bool ReadFinalImage(ImageRGBA8& out, bool preserveAlpha = false);

    // ---- asynchronous final-image readback (offline video) ----------------------------------
    // Records a copy of the last Render() result into a small ring of readback buffers. Call after Render()
    // and before ctx.EndFrame() (the command list is still open). The colour/alpha decode happens later, so a
    // video frame no longer waits for the GPU between rendering and encoding. False when no ring slot is free
    // (the caller should fall back to ReadFinalImage) or there is no frame.
    bool QueueFinalReadback(ID3D12GraphicsCommandList* cmd, bool preserveAlpha);
    // After ctx.EndFrame(): signals the readback fence (the queue is ordered after the frame's work).
    void EndFrameReadback();
    // Maps the oldest recorded readback once its GPU work has finished. `wait`: block until it is (drain at the
    // end of a render). Frames are returned in the order they were recorded; false when none is ready.
    // Call outside frame recording.
    bool PollFinalReadback(ImageRGBA8& out, bool wait);
    // Pending readbacks not collected yet.
    uint32_t PendingReadbacks() const { return (uint32_t)readbackQueue_.size(); }
    // Offline video preview: uploads the exact image handed to the encoder. Call outside frame recording;
    // returns an ImTextureID (0 when the image is empty).
    uint64_t SetOfflinePreviewImage(const ImageRGBA8& image);
    uint64_t OfflinePreviewTexture() const;

    // Blurred copy of the last presented frame for frosted UI panels (ImTextureID, 0 if none)
    // and the back-buffer rectangle the scene image occupies (letterbox), to map UVs.
    uint64_t UiBackdropTexture() const;
    void PresentRect(float& x, float& y, float& w, float& h) const;
    bool SceneVisible() const { return sceneVisible_; }

    const RenderStats& Stats() const { return stats_; }
    // M8b: releases the video layers (decode threads, staging buffers, textures). Call when the scene ends
    // (App::UnloadScene) so nothing is left running; the passes reload on the next frame with layers.
    void ReleaseVideoLayers();
    // DXR paths (RayTraced / PathTraced) need tier 1.1, shader model 6.5 and dxcompiler.dll;
    // otherwise Render falls back to Raster.
    bool RayTracingSupported() const { return rtSupported_; }
    // Probed once in Initialize (DLL present, GPU supported). None is always available.
    bool UpscalerAvailable(UpscalerKind kind) const;
    // DLSS Frame Generation (render/DlssFrameGen.h). Available on RTX 40+; MaxMultiplier() is 2 on RTX 40,
    // 4 on RTX 50. Off = no cost.
    bool FrameGenAvailable() const;
    int FrameGenMaxMultiplier() const;
    const char* FrameGenReason() const;
    // True when this frame recorded DLSS-G output (the caller presents it with Dx12Context::EndFrameFrameGen).
    bool FrameGenActive() const { return frameGenActive_; }
    int FrameGenGeneratedCount() const;
    ID3D12Resource* FrameGenGeneratedFrame(int i) const;
    ID3D12Resource* FrameGenRealFrame() const;
    // The hudless copy for this frame (Renderer::Render calls it before the UI); the caller signals the final
    // copy after ImGui via FrameGenEvaluate.
    void FrameGenCopyHudless(ID3D12GraphicsCommandList* cmd);
    bool FrameGenEvaluate(ID3D12GraphicsCommandList* cmd);
    // DLSS Ray Reconstruction (nvngx_dlssd.dll + GPU support): the UI enables its switch only then.
    bool RayReconstructionAvailable() const;

    // ---- offline GI renderer (render/OfflineRenderer.h) ------------------------------------
    // Available with ray tracing when the offline pipelines compiled.
    bool OfflineSupported() const { return offline_ != nullptr; }
    // Starts an offline image of `view` (the pose uploaded for ctx.FrameNumber()) and records its
    // first share of work into `cmd` (from ctx.BeginFrame()), then presents the preview: call it
    // instead of Render() for that frame (same back-buffer contract for the UI). Waits for the GPU
    // first. False when unsupported or the acceleration structures could not be built.
    bool BeginOffline(ID3D12GraphicsCommandList* cmd, const FrameView& view, const OfflineJobDesc& job);
    // Instead of Render() on the following frames: records the next share of offline work and
    // presents the current preview (or the final image once Done).
    void RenderOffline(ID3D12GraphicsCommandList* cmd);
    const OfflineProgress& OfflineStatus() const;
    // Final image of a Done job (RGBA8, opaque). Waits for the GPU; call outside frame recording.
    // preserveAlpha: keep the coverage alpha (OfflineJobDesc::exportAlpha) and un-premultiply the colour.
    bool ReadOfflineImage(ImageRGBA8& out, bool preserveAlpha = false);
    void CancelOffline();
    // false: BeginOffline / RenderOffline leave the back buffer alone (a sample render behind the UI).
    void SetOfflinePresent(bool present) { offlinePresent_ = present; }

private:
    void EnsureTargets(uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight, uint32_t msaa);
    void EnsureShadowMap(uint32_t size);
    void EnsurePointShadowMap(const LightParams& light, bool offscreen);
    void ReleaseTargets();
    void CreateBuiltinTextures();
    void ReadGpuTimer();
    void DrainMmeForget();   // frees MME resources of models destroyed kFramesInFlight+1 frames ago
    // Schedules the per-model MME resource release for a model that left the drawn set (uids only, never pointers:
    // a freed address may be reused). Deduped against the pending list.
    void ForgetModel(uint64_t modelUid);
    // S5: renders one stage screen's camera feed into `target` (raster, own constant-buffer / light slots). Runs
    // before the scene passes; the screens inside the feed keep their previous texture (no recursion). `target` is
    // the screen's own RGBA8 texture; `base` is the frame's SceneConstants (the feed overrides the camera). False when
    // nothing was drawn (the screen keeps its content).
    bool RenderScreenFeed(ID3D12GraphicsCommandList* cmd, const FrameView& view, const ScreenView& screen,
                          Texture& target, uint32_t feedIndex, const SceneConstants& base);    // S5: the HDR targets a camera feed draws into (one set per feed slot, recreated when the feed size changes).
    bool EnsureFeedTargets(uint32_t feedIndex, uint32_t width, uint32_t height);
    // S5: reconciles the stage screens of `view` / renders their camera feeds / binds the textures (before the
    // scene passes, so the ray-traced geometry sees the screen material flags). `baseSc` is the frame's
    // SceneConstants.
    void UpdateStageScreens(ID3D12GraphicsCommandList* cmd, const FrameView& view, const SceneConstants& baseSc);    void RecordScene(ID3D12GraphicsCommandList* cmd, const FrameView& view, uint32_t w, uint32_t h,
                     uint32_t cbSlot, uint64_t frame, bool offscreen);
    // Fills jitterPx_ (render pixels, FSR convention) for this frame and the constants.
    void FillSceneConstants(const FrameView& view, uint32_t w, uint32_t h, bool offscreen, SceneConstants& sc);
    RenderPath EffectivePath() const;          // settings_.renderPath, or Raster without DXR support
    IUpscaler* EffectiveUpscaler() const;      // null for None or an unavailable upscaler
    // Punctual lights -> GpuLight (premultiplied colour, normalized direction); returns the count
    // written (<= kMaxPunctualLights).
    // pointCap: point lights that get a shadow slice (raster: the map's capacity; ray-traced paths: no cap).
    static uint32_t FillGpuLights(const LightParams& light, GpuLight* out, uint32_t pointCap = kMaxPunctualLights);
    // S9: the same for the local fog volumes (<= kMaxFogVolumes); returns the count written.
    static uint32_t FillGpuFogVolumes(const LightParams& light, GpuFogVolume* out);

    Dx12Context* ctx_ = nullptr;
    std::filesystem::path shaderDir_;
    RenderSettings settings_;
    RenderStats stats_;
    RenderTargets targets_;
    Texture lut_;   // colour LUT strip (targets_.lut points here when set)
    BuiltinTextures builtin_;
    TransientDescriptors transient_;
    std::unique_ptr<IUpscaler> upscalers_[4];  // indexed by UpscalerKind; [0] unused
    bool upscalerAvailable_[4] = {true, false, false, false};
    std::unique_ptr<DlssFrameGen> frameGen_;
    bool frameGenActive_ = false;
    // Camera / jitter / depth data of the frame Render() last recorded, for FrameGenEvaluate (which runs after
    // the UI draw). Valid when frameGenPending_ is set.
    struct FrameGenFrameData {
        DirectX::XMFLOAT4X4 viewProjNoJitter{};
        DirectX::XMFLOAT4X4 prevViewProjNoJitter{};
        DirectX::XMFLOAT4X4 invView{};
        DirectX::XMFLOAT4X4 proj{};
        DirectX::XMFLOAT3 eye{};
        DirectX::XMFLOAT3 fwd{}, up{}, right{};
        float nearZ = 0.5f, farZ = 3000.0f, fov = 0.5f;
        float jitterX = 0, jitterY = 0;
        uint32_t width = 0, height = 0;
    };
    FrameGenFrameData frameGenData_{};
    bool frameGenPending_ = false;
    bool frameGenReset_ = false;
    bool frameGenWasPending_ = false;
    std::unique_ptr<PackTextures> packTextures_;
    std::unique_ptr<RtScene> rt_;
    // Models whose MME resources were released with ForgetModel; freed kFramesInFlight frames later so the
    // in-flight command lists that still reference their descriptors are done (same idea as GpuModel's ring).
    struct PendingMmeForget { uint64_t modelUid; uint64_t frame; };
    std::vector<PendingMmeForget> pendingMmeForget_;
    // The on-screen frame's model uids (RecordScene). A model that drops out of it (destroyed or replaced) has
    // its per-model MME resources released; see Renderer::ForgetModel.
    std::vector<uint64_t> frameModels_;
    // MME runtime + object skinning (M4b): created in Initialize, released with the renderer. Both passes read
    // them through PassContext::mme / mmeSkin.
    std::unique_ptr<MmeRuntime> mme_;
    std::unique_ptr<MmeSkin> mmeSkin_;
    std::unique_ptr<OfflineRenderer> offline_;   // null without ray tracing / offline pipelines
    std::unique_ptr<ScreenVideo> screenVideo_;   // background AVI (背景AVI) feeding screen.bmp materials
    std::unique_ptr<StageScreens> stageScreens_;  // S5: MMDX stage screens (image / video / camera feed)
    bool offlinePresent_ = true;
    bool rtSupported_ = false;
    float jitterPx_[2] = {};
    int64_t lastFrameQpc_ = 0;
    float frameTimeMs_ = 16.7f;
    std::vector<std::unique_ptr<IRenderPass>> passes_;
    uint32_t pipelineMsaa_ = 0;
    // per-frame scene constants + punctual lights (UPLOAD heap, persistently mapped):
    // kCbSlots slots (on-screen frames + RenderToImage), the extra views of a quad view, then the S5 camera feeds
    // (StageScreens::kMaxFeedsPerFrame x kCbSlots; see kFeedCbBase in Renderer.cpp).
    static constexpr uint32_t kFeedSlots = StageScreens::kMaxFeedsPerFrame;
    ComPtr<ID3D12Resource> sceneCb_, lightBuf_;
    uint8_t* sceneCbMapped_ = nullptr;
    uint8_t* lightBufMapped_ = nullptr;
    // S9: local fog volumes of the frame (StructuredBuffer<GpuFogVolume>, same slot layout as the lights)
    ComPtr<ID3D12Resource> fogBuf_;
    uint8_t* fogBufMapped_ = nullptr;
    // S5: a camera feed draws into its own HDR targets (colour / normal / velocity / depth) at the feed size; the
    // tonemapped copy goes into the screen's RGBA8 texture (shaders/screenfeed.hlsl). The feed's SceneConstants /
    // lights live in the kFeedCbBase slots, after every other slot.
    struct FeedTargets {
        Texture color, normal, velocity, depth;
        uint32_t width = 0, height = 0;
    };
    FeedTargets feeds_[StageScreens::kMaxFeedsPerFrame];
    // The feeds' own descriptor windows (the main window belongs to the scene passes; a feed runs before them and
    // must not consume their per-frame budget). kFramesInFlight x kMaxFeedsPerFrame windows.
    TransientDescriptors feedTransient_;
    FullscreenPipeline feedResolve_;
    bool feedResolveTried_ = false;
    ScenePass* scenePass_ = nullptr;   // the scene pass (owned by passes_): the S5 camera feeds draw with it
    // temporal state
    DirectX::XMFLOAT4X4 prevViewProj_{};
    DirectX::XMFLOAT3 prevEye_{};
    bool havePrev_ = false, prevTaa_ = false;
    uint32_t temporalIndex_ = 0;
    // UI backdrop SRV (persistent, for ImGui)
    uint32_t backdropSrv_ = DescriptorHeap::kInvalid;
    ID3D12Resource* backdropSrvRes_ = nullptr;
    float presentRect_[4] = {};
    bool sceneVisible_ = false;
    bool hasFinal_ = false;   // targets_.ldr holds a finished Render() image (ReadFinalImage)
    // asynchronous final-image readback ring (RecordFinalReadback / TryCollectFinalImage)
    struct ReadbackSlot {
        ComPtr<ID3D12Resource> res;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        uint32_t width = 0, height = 0;
        bool preserveAlpha = false;
        bool pending = false;
        uint64_t fence = 0;
    };
    static constexpr uint32_t kReadbackRing = 3;
    ReadbackSlot readback_[kReadbackRing];
    std::vector<uint32_t> readbackQueue_;   // ring indices, oldest first
    ComPtr<ID3D12Fence> readbackFence_;
    HANDLE readbackEvent_ = nullptr;
    uint64_t readbackFenceValue_ = 0;
    uint64_t readbackRecorded_ = 0;         // slots recorded since the last EndFrameReadback
    bool DecodeReadback(ReadbackSlot& slot, bool preserveAlpha, ImageRGBA8& out);
    // offline video preview texture (SetOfflinePreviewImage); two SRV slots so overwriting a descriptor never
    // races the GPU still drawing the previous preview
    ComPtr<ID3D12Resource> previewRes_[2];
    uint32_t previewSrv_[2] = {DescriptorHeap::kInvalid, DescriptorHeap::kInvalid};
    uint32_t previewSlot_ = 0;
    // offscreen rendering
    ComPtr<ID3D12CommandAllocator> offAlloc_;
    ComPtr<ID3D12GraphicsCommandList> offList_;
    // GPU timing
    ComPtr<ID3D12QueryHeap> timestampHeap_;
    ComPtr<ID3D12Resource> timestampReadback_;
    uint64_t timestampFrequency_ = 0;
};

} // namespace mmdx
