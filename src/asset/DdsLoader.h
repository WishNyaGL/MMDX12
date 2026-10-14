#pragma once
// M5a: a small DDS (DirectDraw Surface) decoder for MME textures and model textures. Decodes level 0 of a 2D DDS
// into an ImageRGBA8 (the mip chain is built by the image loader). See DdsLoader.cpp for the supported formats.
//
// Decision (spec M5a #2): the image struct (ImageRGBA8) is 8-bit RGBA only, so float / 16-bit formats are converted
// (clamped, rounded) to RGBA8 rather than reaching the GPU unclamped. This is the documented "otherwise convert to
// RGBA8 and note it" path.
#include "asset/ImageLoader.h"

#include <cstdint>
#include <string>

namespace mmdx {

// True when the buffer starts with the "DDS " magic.
bool IsDdsImage(const uint8_t* bytes, size_t size);

// Decodes level 0 of a 2D DDS into `out` (only level 0 is filled; `out.hasAlpha` is set). Returns false with a
// message in `error` for a malformed header, a cube / volume resource, or an unsupported format.
bool LoadDdsLevel0(const uint8_t* bytes, size_t size, ImageRGBA8& out, std::string* error = nullptr);

} // namespace mmdx
