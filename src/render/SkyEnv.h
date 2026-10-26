#pragma once
// S9b-2: the sky as a light. The Physical Sky's radiance is turned into what an environment light needs, in one raw
// buffer per renderer (shaders/sky_env.hlsl builds it, shaders/sky_env.hlsli reads it):
//   * a luminance CDF over an equirectangular 128x64 grid (row CDF + marginal CDF + total), so the path tracers can
//     importance-sample the sky the way S8a importance-samples the emissive triangles, and
//   * the order-2 SH coefficients of the sky's radiance, whose Lambertian irradiance SkyShIrradiance evaluates for the
//     raster / RT ambient (mmd.hlsl's Hemisphere).
// Four small compute passes, one command list, a UAV barrier between them; the buffer ends in ALL_SHADER_RESOURCE.
// SceneConstants::skyEnvParams.x is the buffer's SrvHeap raw-SRV index, -1 = no sky environment (no Physical Sky, the
// passes failed, or the offline background override): then nothing is built, bound or evaluated.
#include "render/RenderPass.h"
#include "render/ShaderInterop.h"
#include <filesystem>

namespace mmdx {

// One buffer, its SRV / UAV and the four build passes (compiled on the first build, like the S8b average pass). The
// real-time renderer and the offline renderer each own one, so a real-time frame never rewrites the buffer an offline
// image in flight is still reading.
class SkyEnvBuilder {
public:
    // Allocates the buffer (DEFAULT heap, UAV + raw SRV in the shared SrvHeap). No shader is compiled here.
    bool Create(Dx12Context& ctx, const std::filesystem::path& shaderDir);
    void Release(Dx12Context& ctx);
    // Builds the map from the SceneConstants at pc.sceneConstants and points `sc` at it. Does nothing (x = -1) unless
    // `sc.skyParams.x > 0.5` and the buffer exists.
    void Build(PassContext& pc, SceneConstants& sc);
    // This frame's / image's raw SRV index (-1 = no map built).
    int Srv() const { return srv_ == DescriptorHeap::kInvalid ? -1 : (int)srv_; }

private:
    Dx12Context* ctx_ = nullptr;
    std::filesystem::path shaderDir_;
    ComPtr<ID3D12Resource> buf_;
    uint32_t srv_ = DescriptorHeap::kInvalid;
    uint32_t uav_ = DescriptorHeap::kInvalid;
    D3D12_RESOURCE_STATES state_ = D3D12_RESOURCE_STATE_COMMON;
    ComputePipeline lum_, rows_, marg_, sh_;
    bool pipesTried_ = false;
    bool logged_ = false;   // one SKYENV log line per instance (not per frame)
};

} // namespace mmdx
