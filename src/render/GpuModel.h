#pragma once
// GPU-side representation of one PMX model: static vertex/index buffers, per-material
// constants and descriptor tables, plus a ring of dynamic buffers for skinning matrices and
// morph deltas. The ring holds kRing frames so the previous frame's pose stays readable while
// the GPU may still be working on it (motion vectors need it).
#include "render/Dx12Context.h"
#include "render/ShaderInterop.h"
#include "render/ShaderPack.h"
#include "asset/ModelTraits.h"
#include "asset/PbrMaterial.h"
#include "asset/PmxModel.h"
#include <DirectXMath.h>
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

struct ImageRGBA8;

// Textures shared by all models, owned by Renderer.
struct BuiltinTextures {
    ComPtr<ID3D12Resource> white;      // 1x1 (255,255,255,255)
    ComPtr<ID3D12Resource> toon[10];   // generated stand-ins for MMD's toon01..toon10.bmp
};

enum class ModelRole : uint8_t { Character = 0, Stage = 1 };

// Per-material PBR source (MaterialSource) and the PBR debug channel (PbrDebug) live in render/RenderTypes.h
// (with the rest of the render settings); GpuModel reads them through RenderSettings.

class GpuModel {
public:
    static constexpr uint32_t kRing = Dx12Context::kFramesInFlight + 1;

    struct Material {
        uint32_t indexStart = 0, indexCount = 0;
        uint32_t srvTable = DescriptorHeap::kInvalid;  // 3 consecutive SRVs in ctx.SrvHeap(): t1 texture, t2 sphere, t3 toon
        D3D12_GPU_VIRTUAL_ADDRESS constants = 0;       // MaterialConstants (256 B), the current ring entry
        bool doubleSided = false;
        bool drawEdge = false;       // edge flag, size > 0, edge alpha > 0, material visible (material morphs)
        bool castShadow = false;     // PMX self-shadow-map / ground-shadow flags, material visible
        bool alphaTested = false;    // texture has alpha: shadow pass samples it
        bool visible = true;         // diffuse alpha > 0 (MMD skips invisible materials, edges included)
        bool morphable = false;      // a material morph targets it (constants change at run time)
        bool packOff = false;        // shader pack switched off for this material (ShaderChoice::materials): default shading
        bool additive = false;       // the model's own .emd effect is additive: ScenePass uses the additive PSO
        bool noDepthWrite = false;   // the model's own .emd effect draws with ZWriteEnable = false (no depth write)
        uint8_t sphereMode = 0;      // PMX sphere mode (0 none, 1 multiply, 2 add, 3 subtexture): MME UseSphereMap
        float edgeSize = 0;
    };

    GpuModel();
    ~GpuModel();
    GpuModel(const GpuModel&) = delete;
    GpuModel& operator=(const GpuModel&) = delete;

    // `textures` is parallel to pmx.textures; an empty image means "missing" -> white.
    // Creates GPU textures only for indices actually referenced by materials.
    bool Create(Dx12Context& ctx, UploadBatch& batch, const PmxModel& pmx,
                const std::vector<ImageRGBA8>& textures, const BuiltinTextures& builtin, ModelRole role);
    // `pbr` is the model's decoded Ray PBR payload (PbrModelData, may be empty / all-invalid). Creates the
    // per-material StructuredBuffer<GpuPbrMaterial> and its SRV when at least one material has a record.
    bool Create(Dx12Context& ctx, UploadBatch& batch, const PmxModel& pmx,
                const std::vector<ImageRGBA8>& textures, const BuiltinTextures& builtin, ModelRole role,
                const PbrModelData& pbr);
    // Per-material .emd blend hints (asset/MmeHints::MmeBlendHint: 0 normal, 1 no depth write, 2 additive). Call
    // after Create; sets Material::additive / Material::noDepthWrite and the MatFlag_* bits in the constants.
    // `def` applies to materials past the end of `hints` that have no own entry. Never changes ordinary models.
    void SetMaterialBlendHints(const std::vector<uint8_t>& hints, uint8_t def);
    // Frees descriptors. Caller must ensure the GPU no longer uses the model (WaitForGpu).
    void Destroy();

    // `frame` is ctx.FrameNumber(); the ring entry is frame % kRing. The first call after
    // Create() fills every ring entry so the "previous" pose is valid immediately.
    // `skin.size()` must equal BoneCount().
    void UpdateSkinning(uint64_t frame, const std::vector<DirectX::XMFLOAT4X4>& skin);
    // Copies deltas only if `version` differs from the version last written to this entry.
    void UpdateMorphs(uint64_t frame, const std::vector<DirectX::XMFLOAT3>& deltas, uint64_t version);
    // Material morphs (ModelInstance::MaterialMul/MaterialAdd/MaterialVersion): final = base * mul + add.
    // Empty vectors mean identity. Rewrites ring entry frame % kRing when it holds another version and
    // points Materials()[i].constants at it, so call it every frame like UpdateSkinning.
    // `opacity` is a per-model multiplier on every material's diffuse alpha (studio accessories, 1 = unchanged);
    // it is folded into the version, so 1 stays bit-identical to the call without it.
    void UpdateMaterials(uint64_t frame, const std::vector<PmxMorph::MaterialOffset>& mul,
                         const std::vector<PmxMorph::MaterialOffset>& add, uint64_t version, float opacity = 1.0f);

    // Shader pack (render/ShaderPack.h): nullptr = the default shading. Writes the material class, head bone and
    // parameters into the material constants; a no-op when nothing changed, so call it every frame before
    // UpdateMaterials. The scene pass draws the model with the pack's PSOs while ShaderPackId() is not empty.
    // `textureFolder` is the model's resolved per-character texture folder (empty = the pack folder only); `rawParams`
    // (key -> value, typed components as key#0..2) feed the params.ext slots.
    void SetShaderPack(const ShaderPack* pack, const PackParamValues& params, const std::filesystem::path& textureFolder,
                       const MaterialClassOverrides& materials = {}, const std::map<std::string, float>& rawParams = {});
    void SetShaderPack(const ShaderPack* pack, const std::map<std::string, float>& rawParams,
                       const std::filesystem::path& textureFolder, const MaterialClassOverrides& materials = {}) {
        SetShaderPack(pack, pack ? pack->Resolve(rawParams) : PackParamValues{}, textureFolder, materials, rawParams);
    }
    const std::string& ShaderPackId() const { return packId_; }
    // the pack shades at least one material (false without a pack, or with every material switched off)
    bool PackActive() const;
    const std::filesystem::path& ShaderTextureFolder() const { return packTextureFolder_; }
    bool HasPackParamsExt() const { return hasPackParamsExt_; }
    D3D12_GPU_VIRTUAL_ADDRESS PackParamsExtBuffer(uint64_t frame) const;
    const std::vector<DirectX::XMFLOAT4>& PackParamSlots() const { return packParamsExtSlots_; }

    // Background AVI (背景AVI): materials whose diffuse texture file name is screen.bmp show the current video
    // frame. True when the model has any (render/ScreenVideo.h binds the texture; the renderer keys bind state by
    // Uid()). `tex` != null rewrites srvTable + 0 and sets MatFlag_HasTexture; null restores the model's own
    // texture. Waits for the GPU at the caller (descriptor rewrite).
    bool HasScreenMaterials() const { return !screenMaterials_.empty(); }
    void BindScreenTexture(Dx12Context& ctx, ID3D12Resource* tex);

    // The model's default pack texture folder: where its face SDF / light maps live (found through the model's own MME
    // files by asset/MmeHints), used when a ShaderChoice has no explicit textureFolder. Empty when none was found.
    void SetDefaultPackTextureFolder(const std::filesystem::path& folder) { defaultPackTextureFolder_ = folder; }
    const std::filesystem::path& DefaultPackTextureFolder() const { return defaultPackTextureFolder_; }

    // M6a: per-material MME object effects (docs/mme/mme_design.md §2.12 / §2.13). `perMaterial` is parallel to
    // Materials(); an empty path = native shading for that material, an empty vector = no MME at all. An MME object
    // effect wins over a shader pack for the materials it owns. Mirrors SetShaderPack.
    void SetMmeEffects(std::vector<std::filesystem::path> perMaterial);
    // The effect file owning `material` (empty = native shading). The reference is stable for the model's lifetime.
    const std::filesystem::path& MmeEffectOf(size_t material) const {
        static const std::filesystem::path kEmpty;
        return material < mmeEffects_.size() ? mmeEffects_[material] : kEmpty;
    }
    bool HasMmeEffects() const;
    // The model's effect files, unique, in material order.
    std::vector<std::filesystem::path> MmeEffectFiles() const;
    bool MmeMaterial(size_t i) const { return i < mmeEffects_.size() && !mmeEffects_[i].empty(); }
    // M5c: the mesh of an MME effect object (an accessory `.x` its effect owns: particles, floors, sprites). Only the
    // MME draws show it: it stays out of the ray-traced scene (TLAS) and the offline depth / edge passes.
    void SetMmeEffectMesh(bool on) { mmeEffectMesh_ = on; }
    bool MmeEffectMesh() const { return mmeEffectMesh_; }
    // M4b CLI wrapper: one effect for the listed materials (empty `materialMask` = every material).
    void SetMmeEffect(const std::filesystem::path& fx, std::vector<int> materialMask = {});
    // Per-material TOONCOLOR (spec §4.2): the bottom-left texel of the material's toon texture (PMX toon or
    // built-in toon01..10), white when none. Read at model load; parallel to Materials().
    const DirectX::XMFLOAT3& MmeToonColor(size_t i) const {
        static const DirectX::XMFLOAT3 kWhite{1, 1, 1};
        return i < mmeToonColor_.size() ? mmeToonColor_[i] : kWhite;
    }

    const std::string& Name() const { return name_; }
    // Per-process unique id (1, 2, ...), stable for the model's lifetime. MME caches key by this instead of the
    // address: a freed model's address may be reused by a new model, which would otherwise hit a stale cache.
    uint64_t Uid() const { return uid_; }
    ModelRole Role() const { return role_; }
    // M8a: an animated stage (a song-bundled stage with a stage motion) is not a static stage: its BLAS is rebuilt
    // when it moves and its own shaders shade it as geometry that can move. Set after creation (stages are built
    // static, then a motion may be bound).
    void SetDynamic(bool dynamic) { dynamic_ = dynamic; }
    bool Dynamic() const { return dynamic_; }
    // A prop (a studio accessory): drawn like a character (it follows bones), but the world's ground map includes it
    // (it is a real surface the effects may lie on) while the performers never are.
    void SetProp(bool prop) { prop_ = prop; }
    bool IsProp() const { return prop_; }
    uint32_t BoneCount() const { return boneCount_; }
    uint32_t VertexCount() const { return vertexCount_; }
    uint32_t IndexCount() const { return indexCount_; }
    const std::vector<Material>& Materials() const { return materials_; }
    const D3D12_VERTEX_BUFFER_VIEW& VertexBufferView() const { return vbv_; }
    const D3D12_VERTEX_BUFFER_VIEW& MorphBufferView(uint64_t frame) const { return morphVbv_[frame % kRing]; }
    const D3D12_VERTEX_BUFFER_VIEW& PrevMorphBufferView(uint64_t frame) const { return morphVbv_[(frame + kRing - 1) % kRing]; }
    const D3D12_INDEX_BUFFER_VIEW& IndexBufferView() const { return ibv_; }
    // Slot 3: GpuSdef per vertex (stride 0, one zero element, when the model has no SDEF vertex).
    const D3D12_VERTEX_BUFFER_VIEW& SdefBufferView() const { return sdefVbv_; }
    bool HasSdef() const { return hasSdef_; }
    ID3D12Resource* SdefBuffer() const { return sdef_.Get(); }
    // Slot 4: GpuAddUv per vertex (stride 0, one zero element, when the model has no additional UV).
    const D3D12_VERTEX_BUFFER_VIEW& AddUvBufferView() const { return addUvVbv_; }
    bool HasAdditionalUv() const { return hasAdditionalUv_; }
    // Raw (ByteAddress) SRV heap index over the add-UV buffer; 0xFFFFFFFF when the model has none.
    uint32_t AddUvSrv() const { return addUvSrv_; }
    // Per-material PBR record buffer (StructuredBuffer<GpuPbrMaterial>, absolute SRV heap index; 0xFFFFFFFF when
    // the model has none, e.g. with --material-source default). pbrIndexOfMaterial = material index or 0xFFFFFFFF.
    uint32_t PbrSrv() const { return pbrSrv_; }
    bool HasPbr() const { return pbrSrv_ != DescriptorHeap::kInvalid; }
    // The model was created from a PbrModelData with at least one valid Ray record (its PBR payload was not empty
    // when the model loaded). Per-model PBR gate for the scene pass (P5): a model renders PBR when it was loaded
    // with records, whatever the global setting. Set by Renderer::CreateModel.
    bool HasPbrRecords() const { return hasPbrRecords_; }
    void SetHasPbrRecords(bool v) { hasPbrRecords_ = v; }
    uint32_t PbrIndex(size_t material) const {
        return material < pbrIndex_.size() ? pbrIndex_[material] : DescriptorHeap::kInvalid;
    }
    // Generic data traits of the model (additional UVs, toon-map materials) for ShaderPack::Suggests.
    ModelTraits Traits() const;
    D3D12_GPU_VIRTUAL_ADDRESS BoneBuffer(uint64_t frame) const;      // StructuredBuffer<float4x4> (t0)
    D3D12_GPU_VIRTUAL_ADDRESS PrevBoneBuffer(uint64_t frame) const;  // previous frame (t4)
    const DirectX::XMFLOAT4X4* BoneMatricesCpu(uint64_t frame) const {
        return reinterpret_cast<const DirectX::XMFLOAT4X4*>(boneMapped_[frame % kRing]);
    }
    const DirectX::XMFLOAT4X4* PrevBoneMatricesCpu(uint64_t frame) const {
        return reinterpret_cast<const DirectX::XMFLOAT4X4*>(boneMapped_[(frame + kRing - 1) % kRing]);
    }
    // Bind-pose bounds (model space).
    DirectX::XMFLOAT3 BoundsMin() const { return boundsMin_; }
    DirectX::XMFLOAT3 BoundsMax() const { return boundsMax_; }

    // Static buffers. vb: GpuVertex[], state VERTEX_AND_CONSTANT_BUFFER | NON_PIXEL_SHADER_RESOURCE.
    // ib: uint32[], state INDEX_BUFFER | NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE (also read
    // by ray queries through a raw SRV).
    ID3D12Resource* VertexBuffer() const { return vb_.Get(); }
    ID3D12Resource* IndexBuffer() const { return ib_.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS MorphBuffer(uint64_t frame) const { return morphVbv_[frame % kRing].BufferLocation; }
    D3D12_GPU_VIRTUAL_ADDRESS PrevMorphBuffer(uint64_t frame) const { return morphVbv_[(frame + kRing - 1) % kRing].BufferLocation; }
    // CPU copy of the current per-material constants (parallel to Materials(); material morphs applied).
    const std::vector<MaterialConstants>& MaterialConstantsCpu() const { return materialConsts_; }

    // Ray-tracing resources, created and owned by RtScene (render/RayTracing.cpp) on first use
    // and released with the model.
    struct RtResources {
        ComPtr<ID3D12Resource> vertices;      // DEFAULT heap, RtVertex[VertexCount()], UAV-capable
        D3D12_RESOURCE_STATES verticesState = D3D12_RESOURCE_STATE_COMMON;
        // Two BLAS parts so closest-hit rays can cull back faces like the raster pass:
        // [0] single-sided materials (culling on), [1] double-sided (instance TRIANGLE_CULL_DISABLE).
        struct BlasPart {
            ComPtr<ID3D12Resource> blas, scratch; // result (RAYTRACING_ACCELERATION_STRUCTURE) + scratch (UAV)
            uint64_t blasBytes = 0, scratchBytes = 0;
        };
        BlasPart parts[2];
        uint32_t srv = DescriptorHeap::kInvalid;  // 2 SrvHeap descriptors: +0 raw RtVertex SRV, +1 raw index SRV
        // Material index of each BLAS geometry: part 0's, then part 1's starting at partSplit.
        std::vector<uint32_t> geometryMaterials;
        uint32_t partSplit = 0;
        bool blasBuilt = false;
    };
    RtResources& Rt() { return rt_; }

private:
    Dx12Context* ctx_ = nullptr;
    uint64_t uid_ = 0;   // assigned in the constructor from a process-wide atomic counter
    std::string name_;
    uint32_t boneCount_ = 0, vertexCount_ = 0, indexCount_ = 0;
    ComPtr<ID3D12Resource> vb_, ib_, sdef_, addUv_;
    // MaterialConstants ring (UPLOAD heap, persistently mapped): kRing entries of materialCount * 256 B.
    ComPtr<ID3D12Resource> materialCb_;
    uint8_t* materialMapped_ = nullptr;
    uint64_t materialEntryVersion_[kRing] = {};
    uint64_t materialVersion_ = 0;              // version of materialConsts_
    std::vector<MaterialConstants> baseConsts_; // without material morphs
    std::vector<char> autoLum_;                 // per material: AutoLuminous convention matched at load (packs clear it)
    struct MaterialBase { bool edgeFlag = false, castFlag = false; };
    std::vector<MaterialBase> materialBase_;
    std::vector<uint8_t> materialBlend_;        // per material: 0 normal, 1 no-depth-write, 2 additive (MmeBlendHint)
    std::vector<ComPtr<ID3D12Resource>> textures_;  // parallel to pmx.textures (null if unused)
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_INDEX_BUFFER_VIEW ibv_{};
    D3D12_VERTEX_BUFFER_VIEW sdefVbv_{};
    bool hasSdef_ = false;
    D3D12_VERTEX_BUFFER_VIEW addUvVbv_{};
    bool hasAdditionalUv_ = false;
    uint32_t addUvSrv_ = DescriptorHeap::kInvalid;
    int additionalUvCount_ = 0;   // PMX header count (0..4)
    // Per-material PBR records (work unit P2): one SRV heap descriptor over a raw GpuPbrMaterial buffer, plus a
    // map material -> record index (kInvalid when the material has no record). Empty when the model has none.
    ComPtr<ID3D12Resource> pbrBuf_;
    std::vector<ComPtr<ID3D12Resource>> pbrTextures_;   // decoded map textures referenced by the records
    std::vector<uint32_t> pbrTextureSrvs_;              // SRV heap index per pbrTextures_ entry
    uint32_t pbrSrv_ = DescriptorHeap::kInvalid;
    std::vector<uint32_t> pbrIndex_;
    bool hasPbrRecords_ = false;
    ModelRole role_ = ModelRole::Character;
    bool dynamic_ = false;   // M8a: an animated stage rebuilds its BLAS
    bool prop_ = false;      // a studio prop (the world's ground map includes it)
    DirectX::XMFLOAT3 boundsMin_{}, boundsMax_{};
    ComPtr<ID3D12Resource> boneBuf_[kRing];
    void* boneMapped_[kRing] = {};
    ComPtr<ID3D12Resource> morphBuf_[kRing];
    void* morphMapped_[kRing] = {};
    uint64_t morphVersion_[kRing] = {};
    D3D12_VERTEX_BUFFER_VIEW morphVbv_[kRing]{};
    bool skinInitialized_ = false, morphInitialized_ = false;
    std::vector<Material> materials_;
    std::vector<MaterialConstants> materialConsts_;
    std::vector<uint32_t> srvAllocations_;  // srvTable starts to free
    RtResources rt_;
    // shader pack state
    std::vector<std::pair<std::string, std::string>> materialNames_;  // (name, nameEn) per material
    std::vector<std::string> materialTextures_;                       // diffuse texture path per material (pack classes)
    int32_t headBone_ = -1;
    DirectX::XMFLOAT3 headPos_{};
    std::string packId_;
    uint32_t packGeneration_ = 0;
    std::filesystem::path packTextureFolder_;
    std::filesystem::path defaultPackTextureFolder_;   // model's own MME-derived pack texture folder (fallback)
    PackParamValues packParams_{};
    MaterialClassOverrides packOverrides_{};
    // MME object effect (M4b/M6a)
    bool mmeEffectMesh_ = false;
    std::vector<std::filesystem::path> mmeEffects_;   // per material (empty path = native shading)
    std::vector<DirectX::XMFLOAT3> mmeToonColor_;   // per material: bottom-left toon texel (white when none)
    // Background AVI: per-material original diffuse SRV resource (video feeds these), and the material indices
    // whose diffuse texture file name is screen.bmp (case-insensitive).
    std::vector<ComPtr<ID3D12Resource>> screenOrigTex_;
    std::vector<char> screenOrigHasTex_;   // the material's MatFlag_HasTexture before a video was bound
    std::vector<uint32_t> screenMaterials_;
    ComPtr<ID3D12Resource> packParamsExtCb_;
    uint8_t* packParamsExtMapped_ = nullptr;
    std::vector<DirectX::XMFLOAT4> packParamsExtSlots_;
    bool hasPackParamsExt_ = false;
    static constexpr size_t kPackExtCbBytes = 128 * sizeof(DirectX::XMFLOAT4);   // pack_api.hlsli gPackParamSlots
    uint64_t packParamsExtVersion_ = 0;
    uint64_t packExtEntryVersion_[kRing] = {};
};

} // namespace mmdx
