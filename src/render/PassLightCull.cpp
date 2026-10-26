// S7c: the clustered light cull (shaders/light_cull.hlsl): the main camera's froxel grid (kClusterTilesX x
// kClusterTilesY x kClusterSlices over the view) gets one light list per cluster, written into the frame slot's raw
// buffer (Renderer::lightClusterBuf_, one block of kClusterBytes per slot). The scene pixel shaders read their
// fragment's list through the bindless buffer table instead of looping over every light.
//
// The pass is a no-op (byte-identical frame) unless the renderer turned clustering on for this frame
// (PassContext::lightCluster, SceneConstants::lightClusterParams.x >= 0): more than kClusterMinLights lights, the main
// camera view, and the compute path available. The buffer is transitioned to UNORDERED_ACCESS for the dispatch and back
// to ALL_SHADER_RESOURCE afterwards, the state the scene pass reads it in.
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "render/Renderer.h"
#include "core/Log.h"

namespace mmdx {

bool LightCullPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    if (!RtPipelinesSupported(ctx)) {
        // no compute shaders: clustering stays off (SceneConstants::lightClusterParams.x = -1)
        cull_ = {};
        return true;
    }
    if (!cull_.Create(ctx, shaderDir / L"light_cull.hlsl", "CSLightCull")) {
        LOG_WARN("LightCull: pipelines unavailable, the scene keeps the full light loop");
        cull_ = {};
    }
    return true;
}

void LightCullPass::Execute(PassContext& pc) {
    if (!cull_ || !pc.lightCluster) return;
    if (!pc.sceneConstantsCpu || pc.sceneConstantsCpu->lightClusterParams.x < 0.0f) return;
    // one raw-buffer UAV for the cull (u0 of the ComputePipeline's UAV table) + seven null UAVs; the descriptor is
    // allocated up front so a full window never leaves the buffer in UNORDERED_ACCESS
    const uint32_t first = pc.transient.Alloc(8);
    if (first == DescriptorHeap::kInvalid) return;

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    // the cluster block is a shader resource between frames (see the header)
    const D3D12_RESOURCE_BARRIER toUav = CD3DX12_RESOURCE_BARRIER::Transition(pc.lightCluster, kSrvAll, kUav);
    cmd->ResourceBarrier(1, &toUav);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = (UINT)((uint64_t)pc.lightClusterSlot * kClusterDwords);
    uav.Buffer.NumElements = kClusterDwords;
    uav.Buffer.StructureByteStride = 0;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    pc.ctx.Device()->CreateUnorderedAccessView(pc.lightCluster, nullptr, &uav, pc.ctx.SrvHeap().Cpu(first));
    D3D12_UNORDERED_ACCESS_VIEW_DESC null{};
    null.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    null.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    for (uint32_t i = 1; i < 8; ++i)
        pc.ctx.Device()->CreateUnorderedAccessView(nullptr, nullptr, &null, pc.ctx.SrvHeap().Cpu(first + i));

    const SceneConstants& sc = *pc.sceneConstantsCpu;
    const float constants[8] = {(float)kClusterTilesX, (float)kClusterTilesY, (float)kClusterSlices, sc.numLights,
                                sc.nearZ, std::min(sc.farZ, kClusterFarZ), sc.lightClusterParams.z,
                                sc.lightClusterParams.w};
    // one group of 64 threads (light_cull.hlsl numthreads) per cluster: the tiles flattened into X, the slice in Y
    cull_.Dispatch(pc, {}, pc.ctx.SrvHeap().Gpu(first), constants, 8,
                   kClusterTilesX * kClusterTilesY, kClusterSlices);

    const D3D12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(pc.lightCluster, kUav, kSrvAll);
    cmd->ResourceBarrier(1, &toSrv);
}

} // namespace mmdx
