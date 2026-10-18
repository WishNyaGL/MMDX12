// BloomPass: the mip-chain bloom (bloom.hlsl) and the FFT convolution bloom (BloomFft, bloom_fft.hlsl).
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "core/Log.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace mmdx {

namespace {
constexpr DXGI_FORMAT kBloomFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Weight of each coarser mip level relative to the finer one (BloomLook::radius 0..1; 1 = all levels equal).
float LevelFalloff(float radius) { return 0.3f + 0.7f * std::clamp(radius, 0.0f, 1.0f); }
} // namespace

// ---- BloomPass -------------------------------------------------------------------------------

bool BloomPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    const std::filesystem::path f = shaderDir / L"bloom.hlsl";
    shaderDir_ = shaderDir;
    fft_.Release(ctx);
    fftTried_ = false;
    return prefilter_.Create(ctx, f, "PSPrefilter", {kBloomFormat}) && down_.Create(ctx, f, "PSDown", {kBloomFormat}) &&
           up_.Create(ctx, f, "PSUp", {kBloomFormat}, {}, FullscreenPipeline::Blend::AdditiveAll);
}

void BloomPass::OnResize(Dx12Context& ctx, RenderTargets& targets) {
    uint32_t w = Half(targets.outWidth), h = Half(targets.outHeight);
    for (uint32_t i = 0; i < kMips; ++i) {
        // mip 0 is also the convolution bloom's output (compute)
        const D3D12_RESOURCE_FLAGS flags =
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | (i == 0 ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                              : D3D12_RESOURCE_FLAG_NONE);
        mips_[i].Create(ctx, w, h, kBloomFormat, flags, kSrv, L"bloom.mip");
        w = Half(w);
        h = Half(h);
    }
}

void BloomPass::ReleaseTargets(Dx12Context& ctx) {
    for (Texture& m : mips_) m.Release(ctx);
    fft_.Release(ctx);
    fftTried_ = false;
}

void BloomPass::Execute(PassContext& pc) {
    RenderTargets& t = pc.targets;
    t.bloom = nullptr;
    t.bloomScale = 1.0f;
    if (!pc.settings.bloom || !mips_[0] || !t.hdrFinal) return;
    ID3D12GraphicsCommandList* cmd = pc.cmd;
    const BloomLook& look = pc.settings.bloomLook;

    bool convolve = pc.settings.bloomConvolution && !pc.offscreen;
    if (convolve && !fftTried_) {
        fftTried_ = true;
        fft_.Create(pc.ctx, shaderDir_, L"bloom.fft");
    }
    convolve = convolve && fft_;

    // prefilter (threshold; alpha = luminance x linear depth for the depth mask)
    {
        const float c[4] = {1.0f / t.hdrFinal->width, 1.0f / t.hdrFinal->height, pc.settings.bloomThreshold,
                            t.depth ? 1.0f : 0.0f};
        mips_[0].Transition(cmd, kRt);
        prefilter_.Draw(pc, {&mips_[0]}, pc.transient.SrvTable(pc.ctx, {t.hdrFinal, t.depth ? &t.depth : nullptr}), c, 4);
        mips_[0].Transition(cmd, kSrv);
    }
    // the convolution takes the next level (quarter resolution); the mip chain goes all the way down
    const uint32_t levels = convolve ? 2 : kMips;
    for (uint32_t i = 1; i < levels; ++i) {
        const float c[4] = {1.0f / mips_[i - 1].width, 1.0f / mips_[i - 1].height, 0, 1};
        mips_[i].Transition(cmd, kRt);
        down_.Draw(pc, {&mips_[i]}, pc.transient.SrvTable(pc.ctx, {&mips_[i - 1]}), c, 4);
        mips_[i].Transition(cmd, kSrv);
    }

    if (convolve) {
        fft_.Convolve(pc, mips_[1], mips_[0], (float)t.outWidth / (float)t.outHeight, look.kernel, 1.0f);
        mips_[0].Transition(cmd, kSrv);
        t.bloom = &mips_[0];
        return;
    }

    // upsample: level i-1 += falloff x level i, so level j ends up weighted falloff^j
    const float falloff = LevelFalloff(look.radius);
    float sum = 0.0f, w = 1.0f;
    for (uint32_t i = 0; i < kMips; ++i, w *= falloff) sum += w;
    for (uint32_t i = kMips - 1; i > 0; --i) {
        const float c[4] = {1.0f / mips_[i].width, 1.0f / mips_[i].height, falloff, 1.0f};
        mips_[i - 1].Transition(cmd, kRt);
        up_.Draw(pc, {&mips_[i - 1]}, pc.transient.SrvTable(pc.ctx, {&mips_[i]}), c, 4);
        mips_[i - 1].Transition(cmd, kSrv);
    }
    t.bloom = &mips_[0];
    t.bloomScale = 1.0f / sum;
}

} // namespace mmdx
