// MmeSkin.cpp: M4a's object-effect skinning (see MmeSkin.h and docs/mme/mme_design.md §2.6). Runs
// shaders/skin.hlsl's CSSkin (cs_6_0 via DXC) into a per-model 48-byte world-space RtVertex buffer with the same
// root-signature layout the ray tracer uses. DXC unavailable -> log a Warning once and report not available.
#include "render/MmeSkin.h"

#include "render/GpuModel.h"
#include "render/RenderPass.h"
#include "render/ShaderInterop.h"
#include "core/Log.h"

#include <directx/d3dx12.h>

#include <cstring>
#include <set>
#include <string>

namespace mmdx {

namespace {

std::string UpperAscii(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

ComPtr<ID3D12Resource> CreateMmeBuffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE type,
                                       D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                       const wchar_t* name) {
    if (bytes == 0) bytes = 4;
    D3D12_HEAP_PROPERTIES heap{type};
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
    ComPtr<ID3D12Resource> res;
    if (!CheckHr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                 IID_PPV_ARGS(&res)),
                 "MmeSkin: CreateCommittedResource"))
        return {};
    res->SetName(name);
    return res;
}

} // namespace

MmeSkin::~MmeSkin() = default;

bool MmeSkin::Init(Dx12Context& ctx, const std::filesystem::path& shaderDir) {
    ctx_ = &ctx;
    ID3D12Device* device = ctx.Device();

    // skin.hlsl root signature: 0: 4 root constants b0, 1..5: root SRVs t0..t4, 6: root UAV u0, 7: root SRV t5
    // (SDEF parameters). Identical to RtScene's (src/render/RayTracing.cpp).
    CD3DX12_ROOT_PARAMETER p[8];
    p[0].InitAsConstants(4, 0);
    for (uint32_t i = 0; i < 5; ++i) p[1 + i].InitAsShaderResourceView(i);
    p[6].InitAsUnorderedAccessView(0);
    p[7].InitAsShaderResourceView(5);
    CD3DX12_ROOT_SIGNATURE_DESC rs;
    rs.Init(8, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
        LOG_ERROR("MmeSkin: root signature serialization failed");
        ctx_ = nullptr;
        return false;
    }
    if (!CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&rootSig_)),
                 "MmeSkin: CreateRootSignature")) {
        ctx_ = nullptr;
        return false;
    }

    // No FXC path: without DXC the object-effect skinning is simply unavailable.
    std::string errors;
    ComPtr<ID3DBlob> cs = CompileShaderDxc(shaderDir / L"skin.hlsl", "CSSkin", "cs_6_0", {}, &errors);
    if (!cs) {
        LOG_WARN("MmeSkin unavailable: DXC could not compile skin.hlsl (cs_6_0)%s",
                 errors.empty() ? "" : (": " + errors).c_str());
        rootSig_.Reset();
        ctx_ = nullptr;
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSig_.Get();
    pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    if (!CheckHr(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pso_)), "MmeSkin: skin PSO")) {
        rootSig_.Reset();
        ctx_ = nullptr;
        return false;
    }

    LOG_INFO("MmeSkin: object-effect skinning enabled (skin.hlsl cs_6_0)");
    return true;
}

MmeSkin::Entry* MmeSkin::FindOrCreate(Dx12Context& ctx, const GpuModel& model) {
    const uint64_t uid = model.Uid();
    const uint64_t bytes = (uint64_t)model.VertexCount() * sizeof(RtVertex);
    auto it = entries_.find(uid);
    if (it != entries_.end()) {
        // Defensive: an entry under a reused uid (or a vertex count change) may be too small. Replace it — the old
        // buffer is deferred because in-flight frames may still read it.
        if (it->second.buffer && it->second.bytes >= bytes) return &it->second;
        if (it->second.buffer) ctx.DeferRelease(it->second.buffer);
        entries_.erase(it);
    }
    if (model.VertexCount() == 0) return nullptr;
    Entry e;
    e.bytes = bytes;
    e.buffer = CreateMmeBuffer(ctx.Device(), bytes,
                               D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"mme.skin");
    if (!e.buffer) return nullptr;
    e.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    e.skinnedFrame = UINT64_MAX;
    e.skinnedGeneration = UINT64_MAX;
    auto [ins, ok] = entries_.emplace(uid, std::move(e));
    return ok ? &ins->second : nullptr;
}

void MmeSkin::Skin(PassContext& pc, GpuModel& model, uint64_t generation, float shutter) {
    if (!pso_ || !ctx_) return;
    Entry* e = FindOrCreate(pc.ctx, model);
    if (!e) return;
    // Idempotent within a frame; the offline renderer bumps `generation` per iteration so its motion-blurred
    // iterations re-skin at their own shutter time instead of keeping the first iteration's pose.
    if (e->skinnedFrame == pc.frame && e->skinnedGeneration == generation) return;

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    if (e->state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        const CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
            e->buffer.Get(), e->state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &b);
    }
    e->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    cmd->SetComputeRootSignature(rootSig_.Get());
    cmd->SetPipelineState(pso_.Get());
    uint32_t c[4] = {model.VertexCount(), 0, model.HasSdef() ? 1u : 0u, 0};
    // `shutter` blends the previous pose toward the current one (offline motion blur; 1 = current pose only).
    std::memcpy(&c[1], &shutter, sizeof(float));
    cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
    cmd->SetComputeRootShaderResourceView(1, model.VertexBuffer()->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(2, model.BoneBuffer(pc.frame));
    cmd->SetComputeRootShaderResourceView(3, model.PrevBoneBuffer(pc.frame));
    cmd->SetComputeRootShaderResourceView(4, model.MorphBuffer(pc.frame));
    cmd->SetComputeRootShaderResourceView(5, model.PrevMorphBuffer(pc.frame));
    cmd->SetComputeRootUnorderedAccessView(6, e->buffer->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(7, model.SdefBuffer()->GetGPUVirtualAddress());
    cmd->Dispatch((model.VertexCount() + 63) / 64, 1, 1);

    const CD3DX12_RESOURCE_BARRIER back = CD3DX12_RESOURCE_BARRIER::Transition(
        e->buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    cmd->ResourceBarrier(1, &back);
    e->state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    e->skinnedFrame = pc.frame;
    e->skinnedGeneration = generation;
}

D3D12_VERTEX_BUFFER_VIEW MmeSkin::Vb(const GpuModel& model) const {
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    auto it = entries_.find(model.Uid());
    if (it == entries_.end() || !it->second.buffer) return vbv;
    vbv.BufferLocation = it->second.buffer->GetGPUVirtualAddress();
    vbv.SizeInBytes = (UINT)((uint64_t)model.VertexCount() * sizeof(RtVertex));
    vbv.StrideInBytes = (UINT)sizeof(RtVertex);
    return vbv;
}

void MmeSkin::Forget(Dx12Context& ctx, uint64_t modelUid) {
    auto it = entries_.find(modelUid);
    if (it == entries_.end()) return;
    if (it->second.buffer) ctx.DeferRelease(it->second.buffer);
    entries_.erase(it);
}

// ---- input layout ------------------------------------------------------------------------------------------------

namespace {

void AddElement(std::vector<D3D12_INPUT_ELEMENT_DESC>& out, const char* semantic, uint32_t index, uint32_t slot,
                DXGI_FORMAT fmt, uint32_t offset) {
    D3D12_INPUT_ELEMENT_DESC e{};
    e.SemanticName = semantic;
    e.SemanticIndex = index;
    e.Format = fmt;
    e.InputSlot = slot;
    e.AlignedByteOffset = offset;
    e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    e.InstanceDataStepRate = 0;
    out.push_back(e);
}

} // namespace

std::vector<D3D12_INPUT_ELEMENT_DESC> MmeObjectInputLayout(const MmeStage& vs) {
    std::vector<D3D12_INPUT_ELEMENT_DESC> out;
    static std::set<std::string> warned;
    for (const MmeSigElement& el : vs.inputs) {
        const std::string base = UpperAscii(el.semantic);
        const uint32_t ix = el.index;
        if (base == "POSITION") {
            AddElement(out, "POSITION", ix, MmeSlot_Skinned, DXGI_FORMAT_R32G32B32_FLOAT, 0);
        } else if (base == "NORMAL") {
            AddElement(out, "NORMAL", ix, MmeSlot_Skinned, DXGI_FORMAT_R32G32B32_FLOAT, 12);
        } else if (base == "TEXCOORD" && ix == 0) {
            AddElement(out, "TEXCOORD", 0, MmeSlot_Skinned, DXGI_FORMAT_R32G32_FLOAT, 24);
        } else if (base == "TEXCOORD" && ix == 12) {
            // host previous world position, same 48-byte buffer (stride 48 when bound), offset 32
            AddElement(out, "TEXCOORD", 12, MmeSlot_Skinned, DXGI_FORMAT_R32G32B32_FLOAT, 32);
        } else if (base == "TEXCOORD" && ix == 1) {
            AddElement(out, "TEXCOORD", 1, MmeSlot_AddUv, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
        } else if (base == "TEXCOORD" && ix == 2) {
            AddElement(out, "TEXCOORD", 2, MmeSlot_AddUv, DXGI_FORMAT_R32G32B32A32_FLOAT, 16);
        } else if (base == "COLOR") {
            // white at offset 16 of the static buffer (zeros at offset 0)
            AddElement(out, "COLOR", ix, MmeSlot_Zero, DXGI_FORMAT_R32G32B32A32_FLOAT, 16);
        } else if (base == "TEXCOORD") {
            AddElement(out, "TEXCOORD", ix, MmeSlot_Zero, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
        } else {
            const std::string key = base + std::to_string(ix);
            if (warned.insert(key).second)
                LOG_WARN("MmeSkin: unknown VS input semantic %s%d -> zero slot", base.c_str(), (int)ix);
            AddElement(out, el.semantic.empty() ? "TEXCOORD" : el.semantic.c_str(), ix, MmeSlot_Zero,
                       DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
        }
    }
    return out;
}

} // namespace mmdx
