#pragma once
// S9b-3: the Sky light's IMAGE (studio::SceneLight::skyMap / LightParams::skyMap). CPU only: decodes the file into one
// linear-radiance equirectangular panorama of RGBA16F halves, which Renderer::EnsureSkyMap uploads once and
// shaders/sky_map.hlsli samples through SkyColor's `gSkyParams.x > 1.5` branch.
//
// Sources (docs/stage_format.md "sky"): a Radiance .hdr, a .png / .jpg / .jpeg (8-bit), or a DDS: a cubemap (+X -X +Y
// -Y +Z -Z) is resampled to an equirect panorama, a 2D DDS is taken as an equirect directly. EXR has no vendored
// decoder and BC6H / BC7 DDS have no RGBA8 decode: both fail with a message the renderer logs once.
//
// Encoding (LightParams::skyMapEncoding): 0 auto (HDR linear, 8-bit images sRGB, an RGBA8 DDS with alpha Ray-MMD RGBT),
// 1 linear, 2 sRGB, 3 RGBT (Ray-MMD's range-1024 dome: a = A / 255, t = a / (1 + 1/1024 - a), rgb = RGB * t).
#include <DirectXMath.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

// Decodes `path` into an equirectangular linear-radiance image, wider than 4096 texels box-downsampled to <= 4096:
//   halfRgba  width * height * 4 halves (rgb = linear radiance, a = 1)
//   w, h      the panorama's size
//   avg       [0] zenith (v < 0.10), [1] horizon (0.45 < v < 0.55), [2] ground (v > 0.70): the mean linear rgb of
//             those rows, BEFORE any intensity (the caller scales them, so the hemisphere fallback and the fog follow
//             the image)
// Returns false with a message in `error` when the file cannot be read or decoded.
bool LoadSkyMap(const std::filesystem::path& path, int encoding, std::vector<uint16_t>& halfRgba, uint32_t& w,
                uint32_t& h, DirectX::XMFLOAT3 avg[3], std::string* error = nullptr);

} // namespace mmdx
