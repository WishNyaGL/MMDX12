#pragma once
// M4a: MME object-effect skinning. object / object_ss / edge techniques draw a model with the effect's own VS/PS,
// and MMD CPU-skins its vertices: the effect VS expects already-skinned world-space POSITION / NORMAL / TEXCOORD0
// (+ additional UVs on TEXCOORD1..4). This class runs shaders/skin.hlsl's CSSkin (the same kernel the ray tracer
// uses) into a per-model 48-byte RtVertex buffer (position @0, normal @12, uv @24, prevPosition @32, world space,
// current + previous pose) and hands the MME object input layout its vertex buffer view + a synthetic input layout.
// The buffer is created on first use, keyed by the model's uid (GpuModel::Uid()), skinned once per frame on first
// request. See docs/mme/mme_design.md §2.6.
#include "render/Dx12Context.h"
#include "render/MmeCompiler.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <vector>

namespace mmdx {

class GpuModel;
struct PassContext;

// Host vertex-buffer slots for an MME object draw (the input layout Part 2 consumers build from
// MmeObjectInputLayout maps semantics onto these).
enum MmeObjectSlot : uint32_t {
    MmeSlot_Skinned = 0,   // this class's buffer: POSITION/NORMAL/TEXCOORD0 (48-byte stride) + TEXCOORD12 (host prev)
    MmeSlot_AddUv = 1,     // GpuModel::AddUvBufferView(): TEXCOORD1 / TEXCOORD2 (stride 0 zero element when absent)
    MmeSlot_Zero = 2,      // static zeros: TEXCOORD3..4, COLOR0 (= white)
};

class MmeSkin {
public:
    MmeSkin() = default;
    ~MmeSkin();
    MmeSkin(const MmeSkin&) = delete;
    MmeSkin& operator=(const MmeSkin&) = delete;

    // Compiles skin.hlsl (CSSkin, cs_6_0) and builds the root signature (the layout RtScene uses for skin.hlsl).
    // Returns false and logs a Warning once when DXC is unavailable; the caller then reports MME object mode
    // unavailable. No FXC path.
    bool Init(Dx12Context& ctx, const std::filesystem::path& shaderDir);

    // Runs CSSkin for `model` when it has not been skinned yet for (pc.frame, generation), creating its buffer on
    // first use. Records barriers + a Dispatch into pc.cmd. Safe to call only between the model's pose upload and
    // its draw. The real-time layers keep the defaults (generation 0, shutter 1 = the current pose). The offline
    // renderer bumps `generation` every iteration and passes that iteration's `shutter` time (0 = previous pose,
    // 1 = current), so the buffer holds the iteration's pose (offline motion blur) instead of the frame's first.
    void Skin(PassContext& pc, GpuModel& model, uint64_t generation = 0, float shutter = 1.0f);

    // Vertex buffer view over the model's skinned buffer (stride 48). Zero when it has none yet.
    D3D12_VERTEX_BUFFER_VIEW Vb(const GpuModel& model) const;

    // Releases the per-model buffer when the model is destroyed (the GPU must be idle).
    void Forget(Dx12Context& ctx, uint64_t modelUid);

    bool Available() const { return pso_ != nullptr; }

private:
    struct Entry {
        ComPtr<ID3D12Resource> buffer;
        uint64_t bytes = 0;            // buffer capacity, to notice a too-small entry for a reused uid
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        uint64_t skinnedFrame = UINT64_MAX;
        uint64_t skinnedGeneration = UINT64_MAX;
    };

    Entry* FindOrCreate(Dx12Context& ctx, const GpuModel& model);

    Dx12Context* ctx_ = nullptr;
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_;
    std::map<uint64_t, Entry> entries_;
};

// The input layout for an MME object draw, matching every VS input semantic (`vs`) to the host slots above.
// POSITION/NORMAL/TEXCOORD0 and TEXCOORD12 (the host previous world position at offset 32 of the same 48-byte
// buffer) come from the skinned buffer (slot 0); TEXCOORD1/2 from the add-UV buffer (slot 1); TEXCOORD3/4 and
// COLOR0 from the static zero/white buffer (slot 2); unknown semantics map to slot 2 and are logged once.
std::vector<D3D12_INPUT_ELEMENT_DESC> MmeObjectInputLayout(const MmeStage& vs);

} // namespace mmdx
