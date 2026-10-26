#pragma once
// M5a: a small DDS (DirectDraw Surface) decoder for MME textures and model textures. Decodes level 0 of a 2D DDS
// into an ImageRGBA8 (the mip chain is built by the image loader). See DdsLoader.cpp for the supported formats.
//
// Decision (spec M5a #2): the image struct (ImageRGBA8) is 8-bit RGBA only, so float / 16-bit formats are converted
// (clamped, rounded) to RGBA8 rather than reaching the GPU unclamped. This is the documented "otherwise convert to
// RGBA8 and note it" path.
//
// S11: block-compressed DDS files (BC1..BC7) can instead stay compressed: LoadDdsCompressed keeps the file's blocks
// and ALL its mips in a CompressedImage, which the render layer uploads as-is (Dx12Context::UploadBatch::
// CreateTextureCompressed). The RGBA8 path above stays for every other format and for the CPU copies that need the
// pixels (alpha test / alpha-key detection, toon ramps, thumbnails).
#include "asset/ImageLoader.h"

#include <array>
#include <cstdint>
#include <dxgiformat.h>
#include <string>
#include <vector>

namespace mmdx {

// S11: a DDS whose BCn blocks are kept as they are on disk. `format` is the UNORM DXGI_FORMAT (BC1..BC7); an upload
// may choose the _SRGB variant (only BC1/2/3/7 have one; the model path stays UNORM because the shaders apply
// SrgbToLinear themselves — see GpuModel::Create). `mipOffsets[i]` is the byte offset of mip i inside `data`; for a
// cube map (`faces == 6`) the faces follow each other, so face f's mip i lives at `mipOffsets[f * mipCount + i]`. A
// file with a partial mip chain keeps the mips it has (the sampler clamps).
struct CompressedImage {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    uint32_t mipCount = 0;
    uint32_t faces = 1;              // 1 for a 2D texture, 6 for a cube map
    std::vector<size_t> mipOffsets;  // mipCount * faces entries
    std::vector<uint8_t> data;       // the blocks, mip after mip (face after face for a cube)
    bool hasAlpha = false;           // BC1/2/3: an alpha < 255 texel exists in level 0 (block formats we can decode)

    bool Empty() const { return data.empty() || mipCount == 0; }
};

// True when the buffer starts with the "DDS " magic.
bool IsDdsImage(const uint8_t* bytes, size_t size);

// Decodes level 0 of a 2D DDS into `out` (only level 0 is filled; `out.hasAlpha` is set). Returns false with a
// message in `error` for a malformed header, a cube / volume resource, or an unsupported format.
bool LoadDdsLevel0(const uint8_t* bytes, size_t size, ImageRGBA8& out, std::string* error = nullptr);

// True when `bytes` is a DDS carrying a cube map (DDSCAPS2_CUBEMAP).
bool IsDdsCube(const uint8_t* bytes, size_t size);

// S11: keeps a 2D BC1/BC2/BC3/BC4/BC5/BC6H/BC7 DDS compressed, with every mip the file holds. Returns false (with a
// message) for anything else, so the caller falls back to the RGBA8 path above. A cube map is refused here
// (LoadDdsCube).
bool LoadDdsCompressed(const uint8_t* bytes, size_t size, CompressedImage& out, std::string* error = nullptr);

// S11: a DDSCAPS2_CUBEMAP file, +X -X +Y -Y +Z -Z order. The ImageRGBA8 form decodes level 0 of each face (the
// callers build their own mips); the CompressedImage form keeps every face's blocks and mips (`faces == 6`).
bool LoadDdsCube(const uint8_t* bytes, size_t size, std::array<ImageRGBA8, 6>& faces, std::string* error = nullptr);
bool LoadDdsCube(const uint8_t* bytes, size_t size, CompressedImage& cube, std::string* error = nullptr);

} // namespace mmdx
