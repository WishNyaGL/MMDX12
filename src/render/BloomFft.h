#pragma once
// FFT convolution bloom shared by BloomPass (real time) and the offline renderer (bloom_fft.hlsl): the thresholded
// image goes into a kSize^2 grid (content centred, aspect kept, a border against wrap-around), is transformed, multiplied
// by the spectrum of a procedural per-channel kernel (BloomKernel) and transformed back. RGB and the luminance-weighted
// depth in alpha are convolved as two slots of two channels each. The kernel spectrum is rebuilt when the kernel changes.
#include "render/RenderPass.h"
#include "render/RenderTypes.h"
#include <filesystem>

namespace mmdx {

class FftBloom {
public:
    static constexpr uint32_t kSize = 512;   // grid size (power of two; bloom_fft.hlsl FFT_N)

    // Compiles the compute pipelines and creates the grids; false (and everything released) when unavailable.
    bool Create(Dx12Context& ctx, const std::filesystem::path& shaderDir, const wchar_t* name);
    void Release(Dx12Context& ctx);
    explicit operator bool() const { return grid_[0] && rows_; }

    // Convolves src (rgb bright image, a = luminance x linear depth; any state, left in kSrvAll) into dst (an
    // RGBA UAV texture of any size, left in kUav), scaled by gain. aspect = width / height of the image.
    void Convolve(PassContext& pc, Texture& src, Texture& dst, float aspect, const BloomKernel& kernel, float gain);

private:
    void BuildKernel(PassContext& pc, float cw, const BloomKernel& kernel);
    void Transform(PassContext& pc, Texture& in, Texture& out, int slot, float ox, float oy, float cw, float ch, Texture& src);

    ComputePipeline input_, rows_, cols_, kernel_, output_;
    Texture grid_[3];      // kSize^2 RGBA32F: two scratch grids + the slot 0 result
    Texture spec_[2];      // kernel spectrum per slot
    BloomKernel built_;    // kernel of spec_
    float builtCw_ = 0.0f; // content width it was built for
    bool ready_ = false;
};

} // namespace mmdx
