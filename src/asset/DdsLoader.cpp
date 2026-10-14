// DdsLoader.cpp: a minimal DDS decoder (M5a). See DdsLoader.h.
//
// Supported: level 0 of a 2D DDS.
//   * legacy RGB(A) pixel formats by channel masks (A8R8G8B8/X8R8G8B8/A8B8G8R8/R8G8B8/R5G6B5/A1R5G5B5/A4R4G4B4),
//   * luminance / alpha formats (L8, A8L8, A8), ordered generically by their masks,
//   * FourCC DXT1 / DXT3 / DXT5 (software block decode to RGBA8),
//   * FourCC 36 (A16B16G16R16), 113 (A16B16G16R16F), 116 (A32B32G32R32F), 111 (R16F), 114 (R32F),
//   * a DX10 header: R8G8B8A8/B8G8R8A8 (+ _SRGB), R16G16B16A16_FLOAT/UNORM, R32G32B32A32_FLOAT, R32_FLOAT,
//     R16_FLOAT, R8_UNORM, A8_UNORM, R10G10B10A2_UNORM, BC1/2/3 (+ _SRGB); other DXGI formats fail with a message.
// Float / 16-bit formats are converted (clamped, rounded) to RGBA8: the ImageRGBA8 struct is 8-bit only (noted in
// the spec as the allowed fallback). Cube maps and volumes are rejected (the caller falls back to white).
#include "asset/DdsLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace mmdx {
namespace {

constexpr uint32_t kMagic = 0x20534444;   // "DDS "
constexpr uint32_t kHeaderSize = 124;
constexpr uint32_t kFourCCDxt1 = 0x31545844;   // "DXT1"
constexpr uint32_t kFourCCDxt3 = 0x33545844;   // "DXT3"
constexpr uint32_t kFourCCDxt5 = 0x35545844;   // "DXT5"
constexpr uint32_t kFourCCDx10 = 0x30315844;   // "DX10"
constexpr uint32_t kFourCC36 = 36;             // D3DFMT_A16B16G16R16
constexpr uint32_t kFourCC113 = 113;           // D3DFMT_A16B16G16R16F
constexpr uint32_t kFourCC116 = 116;           // D3DFMT_A32B32G32R32F
constexpr uint32_t kFourCC111 = 111;           // D3DFMT_R16F
constexpr uint32_t kFourCC114 = 114;           // D3DFMT_R32F

constexpr uint32_t kCaps2Cubemap = 0x200;
constexpr uint32_t kCaps2Volume = 0x200000;
constexpr uint32_t kDdpfFourCC = 0x4;

uint32_t Rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

uint8_t To8(float v) {
    if (!(v > 0.0f)) return 0;   // also catches NaN
    if (v >= 1.0f) return 255;
    return (uint8_t)std::lround(v * 255.0f);
}

uint8_t To8Unorm(uint32_t v, uint32_t maxv) { return maxv ? (uint8_t)((v * 255u + maxv / 2) / maxv) : 0; }

// Reads `bits` little-endian bits at byte offset `off`.
uint32_t ReadBits(const uint8_t* p, uint32_t bytes) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < bytes; ++i) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

uint32_t MaskShift(uint32_t mask) {
    uint32_t s = 0;
    if (!mask) return 0;
    while (!(mask & 1u)) { mask >>= 1; ++s; }
    return s;
}

uint32_t MaskWidth(uint32_t mask) {
    uint32_t w = 0;
    while (mask) { w += (mask & 1u); mask >>= 1; }
    return w;
}

float ExtractChannel(uint32_t pixel, uint32_t mask) {
    if (!mask) return 1.0f;   // missing channel defaults to 1 (D3D9 texture default)
    const uint32_t shift = MaskShift(mask);
    const uint32_t width = MaskWidth(mask);
    const uint32_t maxv = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
    const uint32_t raw = (pixel >> shift) & maxv;
    return maxv ? (float)raw / (float)maxv : 0.0f;
}

uint16_t Rd16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (uint32_t)(h >> 15) & 1u;
    const uint32_t exp = (uint32_t)(h >> 10) & 0x1Fu;
    const uint32_t mant = (uint32_t)h & 0x3FFu;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) return sign ? -0.0f : 0.0f;   // +-0
        // subnormal
        float m = (float)mant / 1024.0f;
        float val = std::ldexp(m, -14);
        return sign ? -val : val;
    }
    if (exp == 31) {
        f = (sign << 31) | 0x7F800000u | (mant << 13);
        float v;
        std::memcpy(&v, &f, 4);
        return v;
    }
    const uint32_t outExp = exp - 15 + 127;
    f = (sign << 31) | (outExp << 23) | (mant << 13);
    float v;
    std::memcpy(&v, &f, 4);
    return v;
}

// ---- DXT block decode --------------------------------------------------------------------------------------------

void DecodeDxt1Colors(uint16_t c0, uint16_t c1, uint32_t idx, bool opaque, uint8_t out[16][4]) {
    auto expand = [](uint16_t c, uint8_t* rgb) {
        const uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
        rgb[0] = (uint8_t)((r * 255 + 15) / 31);
        rgb[1] = (uint8_t)((g * 255 + 31) / 63);
        rgb[2] = (uint8_t)((b * 255 + 15) / 31);
    };
    uint8_t col[4][3];
    expand(c0, col[0]);
    expand(c1, col[1]);
    for (int i = 0; i < 3; ++i) col[2][i] = (uint8_t)((2 * col[0][i] + col[1][i] + 1) / 3);
    if (c0 > c1 || opaque) {
        for (int i = 0; i < 3; ++i) col[3][i] = (uint8_t)((col[0][i] + 2 * col[1][i] + 1) / 3);
    } else {
        col[3][0] = col[3][1] = col[3][2] = 0;
    }
    for (int i = 0; i < 16; ++i) {
        const uint32_t sel = (idx >> (2 * i)) & 3u;
        out[i][0] = col[sel][0];
        out[i][1] = col[sel][1];
        out[i][2] = col[sel][2];
        out[i][3] = (!opaque && sel == 3) ? 0 : 255;
    }
}

// Writes a 4x4 block of RGBA8 into `dst` (RGBA8, `w` wide) at block (bx,by), clamping to the image bounds.
void WriteBlock(uint8_t* dst, uint32_t w, uint32_t h, uint32_t bx, uint32_t by, const uint8_t block[16][4]) {
    for (uint32_t j = 0; j < 4; ++j) {
        const uint32_t y = by * 4 + j;
        if (y >= h) continue;
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t x = bx * 4 + i;
            if (x >= w) continue;
            uint8_t* p = dst + ((size_t)y * w + x) * 4;
            p[0] = block[j * 4 + i][0];
            p[1] = block[j * 4 + i][1];
            p[2] = block[j * 4 + i][2];
            p[3] = block[j * 4 + i][3];
        }
    }
}

bool DecodeDxt(uint32_t fourCC, const uint8_t* data, size_t size, uint32_t w, uint32_t h, std::vector<uint8_t>& out,
               std::string* error) {
    const uint32_t blockBytes = (fourCC == kFourCCDxt1) ? 8u : 16u;
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const size_t need = (size_t)bw * bh * blockBytes;
    if (size < need) {
        if (error) *error = "dds: truncated compressed data";
        return false;
    }
    out.assign((size_t)w * h * 4, 255);
    size_t off = 0;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = data + off;
            off += blockBytes;
            uint8_t colors[16][4];
            if (fourCC == kFourCCDxt1) {
                DecodeDxt1Colors(Rd16(blk), Rd16(blk + 2), Rd32(blk + 4), /*opaque=*/false, colors);
            } else {
                const bool dxt5 = fourCC == kFourCCDxt5;
                uint8_t alpha[16];
                if (dxt5) {
                    const uint8_t a0 = blk[0], a1 = blk[1];
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; ++i) bits |= (uint64_t)blk[2 + i] << (8 * i);
                    uint8_t ramp[8];
                    ramp[0] = a0;
                    ramp[1] = a1;
                    if (a0 > a1) {
                        for (int i = 1; i <= 6; ++i) ramp[1 + i] = (uint8_t)(((7 - i) * a0 + i * a1 + 3) / 7);
                    } else {
                        for (int i = 1; i <= 4; ++i) ramp[1 + i] = (uint8_t)(((5 - i) * a0 + i * a1 + 2) / 5);
                        ramp[6] = 0;
                        ramp[7] = 255;
                    }
                    for (int i = 0; i < 16; ++i) alpha[i] = ramp[(bits >> (3 * i)) & 7u];
                } else {   // DXT3: 4-bit explicit alpha
                    for (int i = 0; i < 16; ++i) {
                        const uint8_t nib = (uint8_t)((blk[i / 2] >> ((i % 2) * 4)) & 0xF);
                        alpha[i] = (uint8_t)(nib * 17);
                    }
                }
                DecodeDxt1Colors(Rd16(blk + 8), Rd16(blk + 10), Rd32(blk + 12), /*opaque=*/true, colors);
                for (int i = 0; i < 16; ++i) colors[i][3] = alpha[i];
            }
            WriteBlock(out.data(), w, h, bx, by, colors);
        }
    }
    return true;
}

// ---- DX10 format table ------------------------------------------------------------------------------------------

constexpr uint32_t kDxgiR8G8B8A8Unorm = 28;
constexpr uint32_t kDxgiR8G8B8A8UnormSrgb = 29;
constexpr uint32_t kDxgiB8G8R8A8Unorm = 87;
constexpr uint32_t kDxgiB8G8R8A8UnormSrgb = 91;
constexpr uint32_t kDxgiR8Unorm = 61;
constexpr uint32_t kDxgiR16Float = 54;
constexpr uint32_t kDxgiR32Float = 41;
constexpr uint32_t kDxgiR16G16B16A16Float = 10;
constexpr uint32_t kDxgiR16G16B16A16Unorm = 11;
constexpr uint32_t kDxgiR32G32B32A32Float = 2;
constexpr uint32_t kDxgiR10G10B10A2Unorm = 24;
constexpr uint32_t kDxgiA8Unorm = 65;
constexpr uint32_t kDxgiBc1Unorm = 71;
constexpr uint32_t kDxgiBc1UnormSrgb = 72;
constexpr uint32_t kDxgiBc2Unorm = 74;
constexpr uint32_t kDxgiBc2UnormSrgb = 75;
constexpr uint32_t kDxgiBc3Unorm = 77;
constexpr uint32_t kDxgiBc3UnormSrgb = 78;

bool DecodeDx10Format(uint32_t fmt, const uint8_t* data, size_t size, uint32_t w, uint32_t h,
                      std::vector<uint8_t>& out, std::string* error) {
    const size_t px = (size_t)w * h;
    switch (fmt) {
        case kDxgiR8G8B8A8Unorm:
        case kDxgiR8G8B8A8UnormSrgb:
            if (size < px * 4) break;
            out.assign(data, data + px * 4);
            return true;
        case kDxgiB8G8R8A8Unorm:
        case kDxgiB8G8R8A8UnormSrgb:
            if (size < px * 4) break;
            out.resize(px * 4);
            for (size_t i = 0; i < px; ++i) {
                out[i * 4 + 0] = data[i * 4 + 2];
                out[i * 4 + 1] = data[i * 4 + 1];
                out[i * 4 + 2] = data[i * 4 + 0];
                out[i * 4 + 3] = data[i * 4 + 3];
            }
            return true;
        case kDxgiR8Unorm:
        case kDxgiA8Unorm:
            if (size < px) break;
            out.resize(px * 4);
            for (size_t i = 0; i < px; ++i) {
                if (fmt == kDxgiA8Unorm) {
                    out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = 0;
                    out[i * 4 + 3] = data[i];
                } else {
                    out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = data[i];
                    out[i * 4 + 3] = 255;
                }
            }
            return true;
        case kDxgiR16Float:
        case kDxgiR32Float: {
            const size_t bpp = fmt == kDxgiR16Float ? 2u : 4u;
            if (size < px * bpp) break;
            out.resize(px * 4);
            for (size_t i = 0; i < px; ++i) {
                float v = fmt == kDxgiR16Float ? HalfToFloat(Rd16(data + i * 2)) : 0.0f;
                if (fmt == kDxgiR32Float) std::memcpy(&v, data + i * 4, 4);
                const uint8_t b = To8(v);
                out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = b;
                out[i * 4 + 3] = 255;
            }
            return true;
        }
        case kDxgiR16G16B16A16Float:
            if (size < px * 8) break;
            out.resize(px * 4);
            for (size_t i = 0; i < px; ++i) {
                const uint8_t* p = data + i * 8;
                for (int c = 0; c < 4; ++c) out[i * 4 + c] = To8(HalfToFloat(Rd16(p + c * 2)));
            }
            return true;
        case kDxgiR16G16B16A16Unorm:
            if (size < px * 8) break;
            out.resize(px * 4);
            for (size_t i = 0; i < px; ++i) {
                const uint8_t* p = data + i * 8;
                for (int c = 0; c < 4; ++c) out[i * 4 + c] = To8Unorm(Rd16(p + c * 2), 65535);
            }
            return true;
        case kDxgiR32G32B32A32Float:
            if (size < px * 16) break;
            out.resize(px * 4);
            for (size_t i = 0; i < px; ++i) {
                const float* f = (const float*)(data + i * 16);
                for (int c = 0; c < 4; ++c) out[i * 4 + c] = To8(f[c]);
            }
            return true;
        case kDxgiR10G10B10A2Unorm:
            if (size < px * 4) break;
            out.resize(px * 4);
            for (size_t i = 0; i < px; ++i) {
                const uint32_t v = Rd32(data + i * 4);
                out[i * 4 + 0] = To8Unorm(v & 0x3FF, 1023);
                out[i * 4 + 1] = To8Unorm((v >> 10) & 0x3FF, 1023);
                out[i * 4 + 2] = To8Unorm((v >> 20) & 0x3FF, 1023);
                out[i * 4 + 3] = To8Unorm((v >> 30) & 0x3, 3);
            }
            return true;
        case kDxgiBc1Unorm:
        case kDxgiBc1UnormSrgb:
            return DecodeDxt(kFourCCDxt1, data, size, w, h, out, error);
        case kDxgiBc2Unorm:
        case kDxgiBc2UnormSrgb:
            return DecodeDxt(kFourCCDxt3, data, size, w, h, out, error);
        case kDxgiBc3Unorm:
        case kDxgiBc3UnormSrgb:
            return DecodeDxt(kFourCCDxt5, data, size, w, h, out, error);
        default:
            if (error) *error = "dds: unsupported DXGI format " + std::to_string(fmt);
            return false;
    }
    if (error) *error = "dds: truncated DX10 data";
    return false;
}

} // namespace

bool IsDdsImage(const uint8_t* bytes, size_t size) {
    return bytes && size >= 128 && Rd32(bytes) == kMagic;
}

bool LoadDdsLevel0(const uint8_t* bytes, size_t size, ImageRGBA8& out, std::string* error) {
    out = ImageRGBA8{};
    if (!IsDdsImage(bytes, size)) {
        if (error) *error = "dds: bad magic";
        return false;
    }
    const uint32_t headerSize = Rd32(bytes + 4);
    if (headerSize != kHeaderSize) {
        if (error) *error = "dds: bad header size";
        return false;
    }
    const uint32_t h = Rd32(bytes + 12);
    const uint32_t w = Rd32(bytes + 16);
    const uint32_t caps2 = Rd32(bytes + 112);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        if (error) *error = "dds: bad dimensions";
        return false;
    }
    if (caps2 & kCaps2Cubemap) {
        if (error) *error = "dds: cube map unsupported";
        return false;
    }
    if (caps2 & kCaps2Volume) {
        if (error) *error = "dds: volume texture unsupported";
        return false;
    }

    const uint8_t* pf = bytes + 76;
    const uint32_t pfFlags = Rd32(pf + 4);
    const uint32_t fourCC = Rd32(pf + 8);
    const uint32_t bitCount = Rd32(pf + 12);
    const uint32_t rMask = Rd32(pf + 16);
    const uint32_t gMask = Rd32(pf + 20);
    const uint32_t bMask = Rd32(pf + 24);
    const uint32_t aMask = Rd32(pf + 28);

    size_t dataOff = 128;
    std::vector<uint8_t> pixels;

    if (fourCC == kFourCCDx10) {
        if (size < 148) {
            if (error) *error = "dds: truncated DX10 header";
            return false;
        }
        const uint32_t dxgi = Rd32(bytes + 128);
        const uint32_t arraySize = Rd32(bytes + 140);
        if (arraySize > 1) {
            // A 2D array: level 0 slice 0 is at the start; level 0 of each slice is contiguous but slice 0 is first.
        }
        dataOff = 148;
        if (!DecodeDx10Format(dxgi, bytes + dataOff, size - dataOff, w, h, pixels, error)) return false;
    } else if (fourCC == kFourCCDxt1 || fourCC == kFourCCDxt3 || fourCC == kFourCCDxt5) {
        if (!DecodeDxt(fourCC, bytes + dataOff, size - dataOff, w, h, pixels, error)) return false;
    } else if (fourCC == kFourCC36) {   // A16B16G16R16 (UNORM)
        const size_t px = (size_t)w * h;
        if (size - dataOff < px * 8) {
            if (error) *error = "dds: truncated A16B16G16R16 data";
            return false;
        }
        pixels.resize(px * 4);
        const uint8_t* p = bytes + dataOff;
        for (size_t i = 0; i < px; ++i) {
            for (int c = 0; c < 4; ++c) pixels[i * 4 + c] = To8Unorm(Rd16(p + i * 8 + c * 2), 65535);
        }
    } else if (fourCC == kFourCC113) {   // A16B16G16R16F
        const size_t px = (size_t)w * h;
        if (size - dataOff < px * 8) {
            if (error) *error = "dds: truncated A16B16G16R16F data";
            return false;
        }
        pixels.resize(px * 4);
        const uint8_t* p = bytes + dataOff;
        for (size_t i = 0; i < px; ++i)
            for (int c = 0; c < 4; ++c) pixels[i * 4 + c] = To8(HalfToFloat(Rd16(p + i * 8 + c * 2)));
    } else if (fourCC == kFourCC116) {   // A32B32G32R32F
        const size_t px = (size_t)w * h;
        if (size - dataOff < px * 16) {
            if (error) *error = "dds: truncated A32B32G32R32F data";
            return false;
        }
        pixels.resize(px * 4);
        const float* p = (const float*)(bytes + dataOff);
        for (size_t i = 0; i < px * 4; ++i) pixels[i] = To8(p[i]);
    } else if (fourCC == kFourCC111 || fourCC == kFourCC114) {   // R16F / R32F
        const size_t px = (size_t)w * h;
        const size_t bpp = fourCC == kFourCC111 ? 2u : 4u;
        if (size - dataOff < px * bpp) {
            if (error) *error = "dds: truncated R16F/R32F data";
            return false;
        }
        pixels.resize(px * 4);
        const uint8_t* p = bytes + dataOff;
        for (size_t i = 0; i < px; ++i) {
            float v = fourCC == kFourCC111 ? HalfToFloat(Rd16(p + i * 2)) : 0.0f;
            if (fourCC == kFourCC114) std::memcpy(&v, p + i * 4, 4);
            const uint8_t b = To8(v);   // single channel: treated as luminance
            pixels[i * 4 + 0] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = b;
            pixels[i * 4 + 3] = 255;
        }
    } else if (pfFlags & kDdpfFourCC) {
        if (error) *error = "dds: unsupported FourCC";
        return false;
    } else {
        // Legacy bit-mask format (RGB/RGBA/luminance/alpha). Generic per-channel extraction.
        const uint32_t bppBytes = (bitCount + 7) / 8;
        if (bppBytes == 0 || bppBytes > 4) {
            if (error) *error = "dds: unsupported bit count";
            return false;
        }
        const size_t px = (size_t)w * h;
        if (size - dataOff < px * bppBytes) {
            if (error) *error = "dds: truncated uncompressed data";
            return false;
        }
        pixels.resize(px * 4);
        // Luminance when a single colour channel (R) is present with no G/B.
        const bool luminance = (gMask == 0 && bMask == 0 && rMask != 0);
        // Alpha-only when no colour channel is described at all.
        const bool alphaOnly = (rMask == 0 && gMask == 0 && bMask == 0 && aMask != 0);
        const uint8_t* p = bytes + dataOff;
        for (size_t i = 0; i < px; ++i) {
            const uint32_t v = ReadBits(p + i * bppBytes, bppBytes);
            if (luminance) {
                const uint8_t l = To8(ExtractChannel(v, rMask));
                pixels[i * 4 + 0] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = l;
                pixels[i * 4 + 3] = aMask ? To8(ExtractChannel(v, aMask)) : 255;
            } else if (alphaOnly) {
                pixels[i * 4 + 0] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = 0;
                pixels[i * 4 + 3] = To8(ExtractChannel(v, aMask));
            } else {
                pixels[i * 4 + 0] = To8(ExtractChannel(v, rMask));
                pixels[i * 4 + 1] = To8(ExtractChannel(v, gMask));
                pixels[i * 4 + 2] = To8(ExtractChannel(v, bMask));
                pixels[i * 4 + 3] = aMask ? To8(ExtractChannel(v, aMask)) : 255;
            }
        }
    }

    if (pixels.size() != (size_t)w * h * 4) {
        if (error) *error = "dds: decoded size mismatch";
        return false;
    }

    ImageRGBA8::Level level0;
    level0.width = w;
    level0.height = h;
    level0.pixels = std::move(pixels);

    bool hasAlpha = false;
    for (size_t i = 3; i < level0.pixels.size(); i += 4)
        if (level0.pixels[i] < 255) { hasAlpha = true; break; }

    out.mips.push_back(std::move(level0));
    out.hasAlpha = hasAlpha;
    return true;
}

} // namespace mmdx
