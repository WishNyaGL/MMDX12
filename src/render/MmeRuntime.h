#pragma once
// M3a: MME runtime — GPU resources and pipelines for MikuMikuEffect (.fx) programs.
//
// One MmeEffectInstance owns one loaded effect: its M1 IR (MmeEffectDesc), its M2 compiled programs
// (vector<MmeProgram>) and the GPU objects they need (image textures, RENDER*/OFFSCREEN targets, a contiguous
// sampler range in the runtime's shared shader-visible sampler heap, a per-program vertex input layout and a
// lazy PSO cache). MmeRuntime owns the single shared root signature, the sampler heap, the per-frame constant
// ring and a per-target-size fullscreen-quad vertex buffer cache.
//
// Deviations / decisions from the spec (kept local so the header still matches the requested interface):
//  - ImageLoader has no `LoadImageFile`; the real entry point is `LoadImageRGBA8` (asset/ImageLoader.h).
//  - A NULL vertex shader is served by a passthrough VS compiled from an inline string (remote: the pass owner
//    may instead ship shaders/mme/quad.hlsl; no new shader file is added here to avoid a parallel-edit clash).
//  - Object- vs post-class routing (ScriptClass / ScriptOrder) is left to the calling pass; this layer loads any
//    requested file. Object/pre skipping is not implemented here.
//  - Render-target mip generation is deferred (mip 0 only) and logged once, as the spec permits for M3.
//  - Sampler ranges are the exact number of samplers each program stage uses (max register + 1) in the runtime's
//    shared shader-visible sampler heap.
//  - Extra public helpers beyond the requested interface: AllocSamplers (used by instance creation) and
//    SamplerTableGpu (used by StageSamplerTable); both are documented at their definitions.
#include "render/RenderPass.h"
#include "render/MmeCompiler.h"
#include "asset/MmeEffect.h"
#include "asset/AnimatedImage.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mmdx {

class MmeRuntime;
class GpuModel;

// One RENDERCOLORTARGET / RENDERDEPTHSTENCILTARGET / OFFSCREENRENDERTARGET declared by an effect parameter.
// `tex` is created on the render thread once the instance is ready and the output size is known; `ratioX/Y` and
// `fixedW/H` are the sizing annotations (a fixed size wins over the viewport ratio). An offscreen target also owns
// a companion D32_FLOAT depth texture (`depthTex`) and the parsed `DefaultEffect` assignment table (M5a).
struct MmeTarget {
    Texture tex;
    Texture depthTex;              // offscreen targets only: the draw depth (D32_FLOAT)
    std::string name;
    bool depth = false;
    bool offscreen = false;        // OFFSCREENRENDERTARGET: the M5a offscreen scene re-render target
    // fix 1: a RENDERDEPTHSTENCILTARGET requests D24S8 (or D32S8X24): the resource is the matching typeless
    // format, the DSV uses `dsvFormat` and an SRV uses `srvFormat`. Plain depth (UNKNOWN / D32F) stays R32.
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_D32_FLOAT;
    DXGI_FORMAT srvFormat = DXGI_FORMAT_R32_FLOAT;
    float ratioX = 1.0f, ratioY = 1.0f;
    uint32_t fixedW = 0, fixedH = 0;
    uint32_t mipLevels = 1;
    float clear[4] = {0, 0, 0, 1};
    float clearDepth = 1.0f;
    // DefaultEffect entries, in file order (first match wins): pattern = effect name. `effect` is one of
    // "hide" / "none" / "main_default" or a sub-effect file (.fx/.fxsub), case preserved.
    struct DefaultLine { std::string pattern; std::string effect; };
    std::vector<DefaultLine> defaultEffect;
};

// One loaded effect. Created by MmeRuntime::Update (async parse + compile on a worker thread); GPU objects are
// finished on the render thread the first frame the future is ready.
class MmeEffectInstance {
public:
    MmeEffectInstance(std::filesystem::path file, std::string key, MmeRuntime* rt, bool objectMode = false);
    ~MmeEffectInstance();

    bool ObjectMode() const { return objectMode_; }

    const std::filesystem::path& File() const { return file_; }
    bool Ready() const { return gpuBuilt_ && !failed_; }
    bool Failed() const { return failed_; }
    const MmeEffectDesc& Desc() const { return desc_; }              // M1 IR
    const std::vector<MmeProgram>& Programs() const { return programs_; }  // M2 results, index = program id
    int FindProgram(const std::string& technique, const std::string& pass) const;  // -1 if none / failed

    // M6b: diagnostics for the shader manager's MME tab.
    // One line per declared texture (`ResourceName`) that could not be decoded / found (white is used instead).
    const std::vector<std::string>& MissingTextures() const { return missingTextures_; }
    // Unsupported features: the first error of every failed pass plus any parser error diagnostic (asm blocks,
    // D3D9-only constructs, signature fix-up failures). Empty for a fully supported effect.
    const std::vector<std::string>& UnsupportedFeatures() const { return unsupportedFeatures_; }
    // M6b: a signature (file + #include mtimes) used by the hot-reload poll. Invalid before the load finished.
    const std::string& Signature() const { return signature_; }

    // RENDERCOLORTARGET / RENDERDEPTHSTENCILTARGET / OFFSCREEN param by (case-insensitive) parameter name.
    MmeTarget* FindTarget(const std::string& paramName);

    // M5a: every OFFSCREENRENDERTARGET of this instance (in declaration order). Empty when it has none.
    std::vector<MmeTarget*> OffscreenTargets();

    // Textures bound to a stage in register order: resolves sampler name -> `texture = <T>` -> file texture or
    // render target. Missing -> white. The table is persistent and cached (one range in the shader-visible SRV
    // heap per (program, stage)), rebuilt in place when an effect target it references is recreated; it holds
    // exactly the stage's texture count descriptors and consumes no transient ones. 0 on failure.
    D3D12_GPU_DESCRIPTOR_HANDLE StageSrvTable(PassContext& pc, int program, bool pixelStage);
    D3D12_GPU_DESCRIPTOR_HANDLE StageSamplerTable(int program, bool pixelStage) const;

    // M4b object draws: an extended SRV table where the material-semantic texture slots are overridden with the
    // material's own SRVs (GpuModel::Material::srvTable +0/+1/+2). `materialSrvTable` is the srvTable start.
    // Persistent and cached per (model, program, stage); freed with ForgetModel when the model is destroyed.
    D3D12_GPU_DESCRIPTOR_HANDLE StageSrvTableObject(PassContext& pc, int program, bool pixelStage,
                                                    uint32_t materialSrvTable, const GpuModel* model);

    // Frees the object-draw SRV tables built for `model` (called when a GpuModel is destroyed).
    void ForgetModel(Dx12Context& ctx, const GpuModel* model);

    // M5a: the MMD-style self-shadow map (R32_FLOAT) bound at register(s0) for `object_ss` draws. Owned by the
    // renderer (PassContext::mme), created on demand by the shadow-map pass. Pass nullptr to unbind.
    void SetObjectShadowMap(Texture* tex);
    // M5a: the MMD-style light camera (view/proj) the shadow map is rendered with; used for the object_ss compare.
    MmeRuntime* Runtime() const { return rt_; }

    // fix 1: D3D9 stencil reference (STENCILREF, default 0). D3D12 sets it with OMSetStencilRef per draw, so the
    // callers read it here and call cmd->OMSetStencilRef before the pass's draw.
    static uint32_t StencilRefOf(const MmePass& pass);

    // fix 4: chooses the ANIMATEDTEXTURE frame for the scene clock and re-uploads it when it changed (scene clock =
    // pc.view.sceneTime). Frame-stamped, so any draw of one frame uploads once. Called by the stage SRV table build
    // and directly by the object / offscreen draw paths.
    void UpdateAnimatedTextures(PassContext& pc);
    // fix 5: the packed float4 texels of a TEXTUREVALUE parameter (null when unknown / empty). `outCount` gets the
    // number of float4s. The returned buffer lives in the instance and is stable across the frame.
    const float* TexValueFor(const std::string& paramName, uint32_t& outCount);

    ID3D12PipelineState* Pso(Dx12Context& ctx, int program, const MmePass& pass,
                             const DXGI_FORMAT* rtFormats, uint32_t rtCount, DXGI_FORMAT dsFormat,
                             uint32_t sampleCount = 1, const std::vector<D3D12_INPUT_ELEMENT_DESC>* inputLayout = nullptr);

private:
    friend class MmeRuntime;

    struct LoadResult {
        MmeEffectDesc desc;
        std::vector<MmeProgram> programs;
        double ms = 0.0;
    };

    struct ImageTex {
        ComPtr<ID3D12Resource> res;
        DXGI_FORMAT srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    };

    struct ProgLayout {
        std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
        std::vector<std::string> names;   // backs elems[].SemanticName
    };

    bool BeginLoad();                                     // starts the worker; false when already started
    bool TryTakeResult(LoadResult& r);                    // consumes the future when ready (no GPU work)
    void FinishLoad(Dx12Context& ctx);                    // consumes the future, creates GPU objects (render thread)
    void ApplyLoad(Dx12Context& ctx, LoadResult& r);      // M6b: applies a finished LoadResult (fresh load or reload)
    void RefreshDiagnostics();                            // M6b: rebuild unsupportedFeatures_ / signature_
    void EnsureTargets(Dx12Context& ctx, uint32_t outW, uint32_t outH);
    void Release(Dx12Context& ctx);

    const MmeSampler* FindSampler(const std::string& name) const;
    const MmeParam* FindParam(const std::string& name) const;
    // Resolves a texture binding name (a DX9 sampler split by FXC) to a resource + its SRV format.
    bool ResolveTexture(const std::string& bindingName, ID3D12Resource** res, DXGI_FORMAT* srvFormat, bool* isTarget);
    const ProgLayout& Layout(int program);

    std::filesystem::path file_;
    std::string key_;
    MmeRuntime* rt_ = nullptr;
    bool objectMode_ = false;

    std::future<LoadResult> future_;
    bool loadStarted_ = false, loadDone_ = false, gpuBuilt_ = false, failed_ = false;

    MmeEffectDesc desc_;
    std::vector<MmeProgram> programs_;

    // M6b diagnostics (filled in FinishLoad)
    std::vector<std::string> missingTextures_;
    std::vector<std::string> unsupportedFeatures_;
    std::string signature_;   // M6b: file + include mtimes, polled by the hot-reload check

    std::unordered_map<std::string, MmeTarget> targets_;    // key: lower-cased parameter name
    std::unordered_map<std::string, ImageTex> images_;      // key: lower-cased texture parameter name

    // fix 4: ANIMATEDTEXTURE — one decoded animation per (lower-cased) parameter name. `res` holds the current
    // frame; `uploadedFrame` is the frame index currently in it; `uploadedStamp` guards against re-uploading more
    // than once per frame.
    struct AnimTex {
        AnimatedImage image;
        ComPtr<ID3D12Resource> res;        // animated.tex, DEFAULT heap, R8G8B8A8_UNORM
        ComPtr<ID3D12Resource> staging;    // persistent UPLOAD buffer for the reusable copy path
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
        uint32_t uploadedFrame = 0xFFFFFFFFu;
        uint64_t uploadedStamp = ~0ull;
        float offset = 0.0f, speed = 1.0f;
        std::string seekVar;   // SeekVariable annotation (named parameter); empty when absent
        std::filesystem::path file;
    };
    std::unordered_map<std::string, AnimTex> animated_;
    uint64_t animStamp_ = ~0ull;

    // fix 5: TEXTUREVALUE — a float4 array that mirrors a named texture's texels (CPU copy kept at load).
    struct TexValue {
        std::string texture;                 // the TextureName annotation target (lower case)
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;           // width * height * 4, mip 0
    };
    std::unordered_map<std::string, TexValue> texValues_;   // key: lower-cased param name
    std::unordered_map<std::string, std::vector<float>> texValueFloats_;   // key: lower-cased param name

    ComPtr<ID3D12Resource> white_;
    DXGI_FORMAT whiteSrvFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;

    std::vector<uint32_t> samplerBase_;                     // index = program*2 + (pixelStage?1:0)
    std::vector<uint32_t> samplerCount_;                    // exact descriptor count per stage (same indexing)
    std::vector<ProgLayout> layouts_;

    // One persistent stage SRV table (object or post). `first` is the base SRV-heap index, `count` its size
    // (the stage's texture count). The table is rewritten in place when `targetGen` no longer matches the
    // instance's target generation (an effect render target was recreated).
    struct StageSrvCache {
        uint32_t first = DescriptorHeap::kInvalid;
        uint32_t count = 0;
        uint32_t targetGen = 0;
    };

    // Key of a persistent stage SRV table: the owning model (null for post tables) + program + stage +
    // material SRV table index (object draws; kInvalid for post tables).
    struct SrvKey {
        const GpuModel* model = nullptr;
        uint32_t program = 0;
        bool pixelStage = false;
        uint32_t materialSrvTable = DescriptorHeap::kInvalid;
        bool operator==(const SrvKey& o) const {
            return model == o.model && program == o.program && pixelStage == o.pixelStage &&
                   materialSrvTable == o.materialSrvTable;
        }
    };
    struct SrvKeyHash {
        size_t operator()(const SrvKey& k) const {
            size_t h = (size_t)(uintptr_t)k.model;
            h = h * 1099511628211ull ^ (size_t)k.program;
            h = h * 1099511628211ull ^ (size_t)k.pixelStage;
            h = h * 1099511628211ull ^ (size_t)k.materialSrvTable;
            return h;
        }
    };

    // Builds / refreshes a persistent stage SRV table; `materialSrvTable` overrides the material-semantic slots
    // when not kInvalid. Returns the cached entry (with first == kInvalid on allocation failure).
    StageSrvCache& EnsureSrvTable(PassContext& pc, int program, bool pixelStage, uint32_t materialSrvTable,
                                  const GpuModel* model);
    void FreeSrvTable(Dx12Context& ctx, StageSrvCache& c);
    void FreeSrvTables(Dx12Context& ctx);

    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> psoCache_;
    std::unordered_map<SrvKey, StageSrvCache, SrvKeyHash> srvCache_;
    bool srvFailLogged_ = false;
    std::unordered_set<std::string> loggedStates_;
    std::unordered_set<std::string> loggedPasses_;
    uint32_t targetGen_ = 0;   // bumped when an effect render target / image is (re)created
    uint32_t targetW_ = 0, targetH_ = 0;
    // M5a: the MMD-style self-shadow map bound to a register(s0) sampler (no `texture=` state). Renderer-owned.
    Texture* shadowMap_ = nullptr;
    uint32_t missingFrames_ = 0;
};

// One wanted effect for MmeRuntime::Update: a file plus the compile mode (post effects = false, object effects =
// true; M4b). One instance per (file, objectMode).
struct MmeWanted {
    std::filesystem::path file;
    bool objectMode = false;
};

// The per-frame driver. Owns the shared root signature, sampler heap, constant ring and quad VB cache.
class MmeRuntime {
public:
    bool Init(Dx12Context& ctx, const std::filesystem::path& shaderDir);  // root signature, sampler heap, rings, passthrough VS
    void BeginFrame(Dx12Context& ctx, uint64_t frame);
    // Creates instances for new (file, mode) pairs, drops instances not listed for kFramesInFlight frames; finishes
    // async loads and (re)creates render targets at the given output size.
    void Update(Dx12Context& ctx, const std::vector<MmeWanted>& wanted, uint32_t outW, uint32_t outH);
    MmeEffectInstance* Instance(const std::filesystem::path& file, bool objectMode = false);
    // M5a: gets (creating if new) the instance for a file the renderer only discovers at draw time (an offscreen
    // owner's DefaultEffect sub-effect, the built-in standard effect). The instance stays alive as long as this is
    // called each frame; the caller must call Update() as usual so async loads finish and targets exist.
    MmeEffectInstance* EnsureInstance(const std::filesystem::path& file, bool objectMode = false);
    // M5a: all loaded instances (the offscreen pass iterates them for OFFSCREENRENDERTARGET owners).
    const std::vector<std::unique_ptr<MmeEffectInstance>>& Instances() const { return instances_; }
    // M5a: the MMD-style self-shadow map bound to register(s0) object_ss draws. Set by the shadow-map pass.
    void SetObjectShadowMap(Texture* tex);
    bool HasObjectShadowMap() const { return objectShadowMap_ != nullptr; }
    Texture* ObjectShadowMap() const { return objectShadowMap_; }
    // Frees the object-draw SRV tables every instance built for `model` (a GpuModel was destroyed).
    void ForgetModel(Dx12Context& ctx, const GpuModel* model);
    ID3D12RootSignature* RootSignature() const { return rootSig_.Get(); }
    ID3D12DescriptorHeap* SamplerHeap() const { return samplerHeap_.Heap(); }
    D3D12_GPU_VIRTUAL_ADDRESS AllocConstants(const void* data, uint32_t size);
    const D3D12_VERTEX_BUFFER_VIEW& QuadVb(Dx12Context& ctx, uint32_t w, uint32_t h);
    // M5a: generates the mip chain of an MME render target through a box-filter blit (mip 0 -> 1 -> ...). Called
    // after the pass / offscreen draw that wrote mip 0. A no-op when `target` needs no chain or the blit is
    // unavailable.
    void GenerateMips(PassContext& pc, Texture& target);
    void Release(Dx12Context& ctx);

    // M6b: hot reload (docs/mme/mme_design.md §2.12 hot reload / spec F). Call once per frame; at most ~1 Hz it
    // stats every loaded instance's file + #includes and, when one changed, starts a new parse + compile of that
    // instance. The old effect keeps drawing until the new one finishes; Update() swaps it in. A failure keeps
    // the old instance and reports an error string through the callback.
    void PollHotReload();
    // M6b: the callback a compile failure / reload reports through (set by the renderer; `message` is user-facing).
    void SetErrorCallback(std::function<void(const std::string&)> cb) { errorCb_ = std::move(cb); }

    // Allocates a contiguous range of `count` sampler descriptors; kInvalid when the heap is full.
    uint32_t AllocSamplers(uint32_t count) { return samplerHeap_.Allocate(count); }
    // GPU handle of a sampler range (used by MmeEffectInstance::StageSamplerTable).
    D3D12_GPU_DESCRIPTOR_HANDLE SamplerTableGpu(uint32_t base) const { return samplerHeap_.Gpu(base); }
    ID3DBlob* PassthroughVs() const { return passthroughVs_.Get(); }
    // The white 1x1 fallback SRV format used when a texture cannot be resolved.
    static constexpr DXGI_FORMAT kWhiteFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

private:
    friend class MmeEffectInstance;

    struct QuadEntry {
        ComPtr<ID3D12Resource> res;
        D3D12_VERTEX_BUFFER_VIEW view{};
    };
    struct ConstantRing {
        ComPtr<ID3D12Resource> res;
        uint8_t* mapped = nullptr;
        uint32_t used = 0;
    };

    static constexpr uint32_t kRingBytes = 4u * 1024u * 1024u;
    // D3D12 caps a shader-visible sampler heap at D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE (2048); larger requests
    // fail with E_INVALIDARG. The M3c spec asks for 16384, which D3D12 does not allow, so this stays at the maximum.
    static constexpr uint32_t kSamplerHeapSize = 2048;

    Dx12Context* ctx_ = nullptr;
    ComPtr<ID3D12RootSignature> rootSig_;
    DescriptorHeap samplerHeap_;
    ComPtr<ID3DBlob> passthroughVs_;
    ConstantRing rings_[Dx12Context::kFramesInFlight];
    uint32_t ringSlot_ = 0;
    bool ringOverflowLogged_ = false;

    std::vector<std::unique_ptr<MmeEffectInstance>> instances_;
    std::unordered_map<std::string, MmeEffectInstance*> byKey_;
    std::unordered_map<uint64_t, QuadEntry> quads_;
    Texture* objectShadowMap_ = nullptr;   // M5a: MMD-style self-shadow map (renderer-owned)

    // M6b hot reload state
    uint64_t nextReloadPollFrame_ = 0;
    std::function<void(const std::string&)> errorCb_;
    // A reload in flight: a detached instance being parsed + compiled for `key` (its file changed). When it is
    // ready Update() swaps it into the instances_ / byKey_ slot; a failure keeps the old (drawing) instance.
    struct Reload {
        std::string key;
        std::string sig;   // the changed file's signature (recorded on failure so it is not retried)
        std::unique_ptr<MmeEffectInstance> fresh;
    };
    std::vector<Reload> reloads_;
    // M6b: the signature of a file version whose reload failed: while it is unchanged, do not retry (one toast / [E]).
    std::map<std::string, std::string> failedReload_;

    // M5a: mip-chain generation for MME render targets (a box-filter blit down the chain). One pipeline per target
    // format, created lazily; the destination mip's RTV is allocated on demand.
    std::filesystem::path shaderDir_;
    ComPtr<ID3D12RootSignature> mipRoot_;
    std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> mipPsos_;   // key: DXGI_FORMAT
    uint32_t mipRtv_ = DescriptorHeap::kInvalid;                          // one reusable RTV slot for mip targets
    std::unordered_set<uintptr_t> mipLogged_;                             // once-per-target diagnostics
    // M5c fix: the per-level SRV tables GenerateMips binds (8 descriptors each) out of a persistent runtime-owned
    // range, rewound once per frame, instead of the per-frame transient window (many RENDERCOLORTARGETs with a full
    // mip chain exhausted it: AutoLuminous4's "TransientDescriptors: window exhausted"). kMipSrvTables tables per
    // frame slot, like the transient windows, so an in-flight frame's table is not overwritten.
    static constexpr uint32_t kMipSrvTables = 32;
    uint32_t mipSrvBase_ = DescriptorHeap::kInvalid;
    uint32_t mipSrvCursor_ = 0;
    bool mipOverflowLogged_ = false;
};

} // namespace mmdx
