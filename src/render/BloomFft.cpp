// FftBloom: the FFT convolution bloom (bloom_fft.hlsl), shared by BloomPass and the offline renderer.
#include "render/BloomFft.h"
#include "render/PassCommon.h"
#include "core/Log.h"
#include <cmath>
#include <string>

namespace mmdx {

bool FftBloom::Create(Dx12Context& ctx, const std::filesystem::path& shaderDir, const wchar_t* name) {
    Release(ctx);
    if (!RtPipelinesSupported(ctx)) return false;
    const ShaderDefines defs = {{"FFT_N", std::to_string(kSize)}, {"FFT_LOG2", "9"}};
    const std::filesystem::path f = shaderDir / L"bloom_fft.hlsl";
    bool ok = input_.Create(ctx, f, "CSInput", defs) && rows_.Create(ctx, f, "CSFftRows", defs) &&
              cols_.Create(ctx, f, "CSFftCols", defs) && kernel_.Create(ctx, f, "CSKernel", defs) &&
              output_.Create(ctx, f, "CSFftOutput", defs);
    const std::wstring base = name ? name : L"bloom.fft";
    for (int i = 0; ok && i < 3; ++i)
        ok = grid_[i].Create(ctx, kSize, kSize, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             kSrvAll, (base + L".grid").c_str());
    for (int i = 0; ok && i < 2; ++i)
        ok = spec_[i].Create(ctx, kSize, kSize, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             kSrvAll, (base + L".kernel").c_str());
    if (!ok) {
        LOG_WARN("Bloom: convolution bloom unavailable");
        Release(ctx);
        return false;
    }
    return true;
}

void FftBloom::Release(Dx12Context& ctx) {
    for (Texture& t : grid_) t.Release(ctx);
    for (Texture& t : spec_) t.Release(ctx);
    input_ = {};
    rows_ = {};
    cols_ = {};
    kernel_ = {};
    output_ = {};
    ready_ = false;
}

// Kernel grid of each slot, then its forward FFT (rows into grid_[1], columns into spec_[slot]).
void FftBloom::BuildKernel(PassContext& pc, float cw, const BloomKernel& k) {
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const uint32_t n = kSize;
    const float zero[4] = {0, 0, 0, 0};
    for (int slot = 0; slot < 2; ++slot) {
        const float c[12] = {cw, (float)k.rays, k.rotation * 3.14159265f / 180.0f, (float)slot,
                             k.spikeLength, k.spikeIntensity, k.glowRadius, k.glowIntensity,
                             k.dispersion, k.anamorphic, 0.0f, 0.0f};
        grid_[0].Transition(cmd, kUav);
        kernel_.Dispatch(pc, {}, pc.transient.UavTable(pc.ctx, {&grid_[0]}), c, 12, Groups(n), Groups(n));
        grid_[0].Transition(cmd, kSrvAll);
        grid_[1].Transition(cmd, kUav);
        rows_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&grid_[0]}), pc.transient.UavTable(pc.ctx, {&grid_[1]}), zero,
                       4, 1, n);
        grid_[1].Transition(cmd, kSrvAll);
        spec_[slot].Transition(cmd, kUav);
        cols_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&grid_[1]}), pc.transient.UavTable(pc.ctx, {&spec_[slot]}),
                       zero, 4, 1, n);
        spec_[slot].Transition(cmd, kSrvAll);
    }
    built_ = k;
    builtCw_ = cw;
    ready_ = true;
}

void FftBloom::Convolve(PassContext& pc, Texture& src, Texture& dst, float aspect, const BloomKernel& kernel,
                        float gain) {
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const uint32_t n = kSize;

    // content rect: 75% of the grid, centred, keeping the image's aspect ratio (the rest stops wrap-around)
    float cw, ch;
    if (aspect >= 1.0f) {
        cw = 0.75f * n;
        ch = cw / aspect;
    } else {
        ch = 0.75f * n;
        cw = ch * aspect;
    }
    const float ox = (n - cw) * 0.5f, oy = (n - ch) * 0.5f;
    // the kernel is laid out in grid pixels relative to the content width
    if (!ready_ || !(built_ == kernel) || std::fabs(builtCw_ - cw) > 0.5f) BuildKernel(pc, cw, kernel);

    const float zero[4] = {0, 0, 0, 0};
    const float inverse[4] = {1, 0, 0, 0};
    src.Transition(cmd, kSrvAll);
    for (int slot = 0; slot < 2; ++slot) {
        Texture& result = slot == 0 ? grid_[2] : grid_[1];
        const float inC[8] = {ox, oy, cw, ch, (float)slot, 0, 0, 0};
        grid_[0].Transition(cmd, kUav);
        input_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&src}), pc.transient.UavTable(pc.ctx, {&grid_[0]}), inC, 8,
                        Groups(n), Groups(n));
        // rows forward
        grid_[0].Transition(cmd, kSrvAll);
        grid_[1].Transition(cmd, kUav);
        rows_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&grid_[0]}), pc.transient.UavTable(pc.ctx, {&grid_[1]}), zero,
                       4, 1, n);
        // columns: forward, times the kernel spectrum, inverse
        grid_[1].Transition(cmd, kSrvAll);
        grid_[0].Transition(cmd, kUav);
        cols_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&grid_[1], &spec_[slot]}),
                       pc.transient.UavTable(pc.ctx, {&grid_[0]}), inverse, 4, 1, n);
        // rows inverse
        grid_[0].Transition(cmd, kSrvAll);
        result.Transition(cmd, kUav);
        rows_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&grid_[0]}), pc.transient.UavTable(pc.ctx, {&result}),
                       inverse, 4, 1, n);
        result.Transition(cmd, kSrvAll);
    }

    dst.Transition(cmd, kUav);
    const float outC[8] = {ox / n, oy / n, cw / n, ch / n, gain, (float)dst.width, (float)dst.height, 0.0f};
    output_.Dispatch(pc, pc.transient.SrvTable(pc.ctx, {&grid_[2], &grid_[1]}), pc.transient.UavTable(pc.ctx, {&dst}),
                     outC, 8, Groups(dst.width), Groups(dst.height));
}

} // namespace mmdx
