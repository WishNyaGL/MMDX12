// S9b-2: the sky environment buffer's host side (see render/SkyEnv.h for what it is and why there is one per renderer).
#include "render/SkyEnv.h"
#include "render/PassCommon.h"
#include "core/Log.h"
#include <directx/d3dx12.h>

namespace mmdx {

bool SkyEnvBuilder::Create(Dx12Context& ctx, const std::filesystem::path& shaderDir) {
    ctx_ = &ctx;
    shaderDir_ = shaderDir;
    const CD3DX12_RESOURCE_DESC desc =
        CD3DX12_RESOURCE_DESC::Buffer(kSkyEnvFloats * 4u, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CD3DX12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    if (!CheckHr(ctx.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buf_)),
                 "CreateCommittedResource(sky environment)"))
        return false;
    buf_->SetName(L"scene.skyenv");
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;   // raw ByteAddressBuffer (the shaders' bindless buffer table)
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = kSkyEnvFloats;
    srv.Buffer.StructureByteStride = 0;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    srv_ = ctx.SrvHeap().Allocate(1);
    if (srv_ == DescriptorHeap::kInvalid) return false;
    ctx.Device()->CreateShaderResourceView(buf_.Get(), &srv, ctx.SrvHeap().Cpu(srv_));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = kSkyEnvFloats;
    uav.Buffer.StructureByteStride = 0;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    uav_ = ctx.SrvHeap().Allocate(1);
    if (uav_ == DescriptorHeap::kInvalid) return false;
    ctx.Device()->CreateUnorderedAccessView(buf_.Get(), nullptr, &uav, ctx.SrvHeap().Cpu(uav_));
    return true;
}

void SkyEnvBuilder::Release(Dx12Context& ctx) {
    if (srv_ != DescriptorHeap::kInvalid) ctx.SrvHeap().Free(srv_, 1);
    if (uav_ != DescriptorHeap::kInvalid) ctx.SrvHeap().Free(uav_, 1);
    srv_ = uav_ = DescriptorHeap::kInvalid;
    buf_.Reset();
    state_ = D3D12_RESOURCE_STATE_COMMON;
    lum_ = ComputePipeline{};
    rows_ = ComputePipeline{};
    marg_ = ComputePipeline{};
    sh_ = ComputePipeline{};
    pipesTried_ = false;
    logged_ = false;
    ctx_ = nullptr;
}

void SkyEnvBuilder::Build(PassContext& pc, SceneConstants& sc) {
    sc.skyEnvParams = {(float)(int32_t)DescriptorHeap::kInvalid, (float)kSkyEnvW, (float)kSkyEnvH, 0.0f};
    if (!buf_ || !pc.cmd || pc.sceneConstants == 0 || sc.skyParams.x <= 0.5) return;
    // A transparent-export background override is not the sky, so its escaped radiance is not either.
    if (sc.bgOverride.w > 0.5) return;
    if (!pipesTried_) {
        pipesTried_ = true;
        const std::filesystem::path file = shaderDir_ / L"sky_env.hlsl";
        if (!lum_.Create(*ctx_, file, "CSSkyEnvLum") || !rows_.Create(*ctx_, file, "CSSkyEnvRows") ||
            !marg_.Create(*ctx_, file, "CSSkyEnvMarg") || !sh_.Create(*ctx_, file, "CSSkySh")) {
            LOG_WARN("sky env: sky_env.hlsl unavailable, the sky lights nothing");
            lum_ = ComputePipeline{};
            rows_ = ComputePipeline{};
            marg_ = ComputePipeline{};
            sh_ = ComputePipeline{};
            return;
        }
    }
    if (!lum_ || !rows_ || !marg_ || !sh_) return;

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const D3D12_RESOURCE_BARRIER toUav = CD3DX12_RESOURCE_BARRIER::Transition(buf_.Get(), state_, kUav);
    cmd->ResourceBarrier(1, &toUav);
    const D3D12_GPU_DESCRIPTOR_HANDLE uavTable = ctx_->SrvHeap().Gpu(uav_);
    // One group per row (128 threads = the grid's columns), then the 64 row sums, then the SH integration.
    lum_.Dispatch(pc, {}, uavTable, nullptr, 0, kSkyEnvH, 1);
    const D3D12_RESOURCE_BARRIER afterLum = CD3DX12_RESOURCE_BARRIER::UAV(buf_.Get());
    cmd->ResourceBarrier(1, &afterLum);
    rows_.Dispatch(pc, {}, uavTable, nullptr, 0, kSkyEnvH, 1);
    const D3D12_RESOURCE_BARRIER afterRows = CD3DX12_RESOURCE_BARRIER::UAV(buf_.Get());
    cmd->ResourceBarrier(1, &afterRows);
    marg_.Dispatch(pc, {}, uavTable, nullptr, 0, 1, 1);
    const D3D12_RESOURCE_BARRIER afterMarg = CD3DX12_RESOURCE_BARRIER::UAV(buf_.Get());
    cmd->ResourceBarrier(1, &afterMarg);
    sh_.Dispatch(pc, {}, uavTable, nullptr, 0, 1, 1);
    const D3D12_RESOURCE_BARRIER toSrv =
        CD3DX12_RESOURCE_BARRIER::Transition(buf_.Get(), kUav, kSrvAll);
    cmd->ResourceBarrier(1, &toSrv);
    state_ = kSrvAll;

    sc.skyEnvParams = {(float)srv_, (float)kSkyEnvW, (float)kSkyEnvH, 0.0f};
    if (!logged_) {
        logged_ = true;
        LOG_INFO("SKYENV built %ux%u", kSkyEnvW, kSkyEnvH);
    }
}

} // namespace mmdx
