// dds_test: synthetic DDS decoding self test (M5a). Writes DDS files in memory (basic header + pixel data),
// decodes them through asset/DdsLoader (LoadDdsLevel0) and the image path (LoadImageRGBA8), and checks the decoded
// texel values. No window, no assets.
//
//   dds_test [--verbose]
//
// Exit code 0 when every case passes, 1 otherwise.
#include "asset/DdsLoader.h"
#include "asset/BinaryReader.h"
#include "asset/ImageLoader.h"
#include "asset/ImportScene.h"
#include "core/TextUtil.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace mmdx;

namespace {

struct DdsWriter {
    uint32_t w = 0, h = 0;
    uint32_t pfFlags = 0;
    uint32_t fourCC = 0;
    uint32_t bitCount = 0;
    uint32_t rMask = 0, gMask = 0, bMask = 0, aMask = 0;
    uint32_t caps2 = 0;
    uint32_t mipCount = 1;
    bool dx10 = false;
    uint32_t dx10Format = 0;

    static void Put32(std::vector<uint8_t>& b, uint32_t v) {
        b.push_back((uint8_t)(v & 0xFF));
        b.push_back((uint8_t)((v >> 8) & 0xFF));
        b.push_back((uint8_t)((v >> 16) & 0xFF));
        b.push_back((uint8_t)((v >> 24) & 0xFF));
    }

    std::vector<uint8_t> Build(const std::vector<uint8_t>& pixelData) const {
        std::vector<uint8_t> b;
        Put32(b, 0x20534444);          // "DDS "
        Put32(b, 124);                 // header size
        Put32(b, 0x1 | 0x2 | 0x4 | 0x1000 | 0x8);   // caps/ddsCaps: texture
        Put32(b, h);
        Put32(b, w);
        Put32(b, 0);                   // pitchOrLinearSize
        Put32(b, 0);                   // depth
        Put32(b, mipCount);            // mipMapCount
        for (int i = 0; i < 11; ++i) Put32(b, 0);   // reserved
        Put32(b, 32);                  // pixel format size
        Put32(b, pfFlags);
        Put32(b, fourCC);
        Put32(b, bitCount);
        Put32(b, rMask);
        Put32(b, gMask);
        Put32(b, bMask);
        Put32(b, aMask);
        Put32(b, 0x1000);              // dwCaps (DDSCAPS_TEXTURE)
        Put32(b, caps2);               // caps2
        Put32(b, 0);                   // caps3
        Put32(b, 0);                   // caps4
        Put32(b, 0);                   // reserved2
        if (dx10) {
            Put32(b, dx10Format);
            Put32(b, 3);               // resource dimension = TEXTURE2D
            Put32(b, 0);               // misc flags
            Put32(b, 1);               // array size
            Put32(b, 0);               // misc flags 2
        }
        b.insert(b.end(), pixelData.begin(), pixelData.end());
        return b;
    }
};

int g_pass = 0, g_total = 0;
bool g_verbose = false;

void Check(const char* name, bool ok, const char* detail = "") {
    ++g_total;
    if (ok) ++g_pass;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail[0] ? " -- " : "", detail);
    std::fflush(stdout);
}

bool PixelIs(const ImageRGBA8& img, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (img.Empty() || x >= img.mips[0].width || y >= img.mips[0].height) return false;
    const uint8_t* p = img.mips[0].pixels.data() + ((size_t)y * img.mips[0].width + x) * 4;
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

const char* FormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_BC1_UNORM: return "BC1";
        case DXGI_FORMAT_BC2_UNORM: return "BC2";
        case DXGI_FORMAT_BC3_UNORM: return "BC3";
        case DXGI_FORMAT_BC4_UNORM: return "BC4";
        case DXGI_FORMAT_BC5_UNORM: return "BC5";
        case DXGI_FORMAT_BC6H_UF16: return "BC6H";
        case DXGI_FORMAT_BC7_UNORM: return "BC7";
        default: return "?";
    }
}

// One BC1 block: c0 / c1 are RGB565 endpoints, `idx` the 2-bit texel indices.
void PutBc1Block(std::vector<uint8_t>& out, uint16_t c0, uint16_t c1, uint32_t idx) {
    out.push_back((uint8_t)(c0 & 0xFF));
    out.push_back((uint8_t)(c0 >> 8));
    out.push_back((uint8_t)(c1 & 0xFF));
    out.push_back((uint8_t)(c1 >> 8));
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)((idx >> (8 * i)) & 0xFF));
}

// One BC7 mode 6 block: 7-bit RGBA endpoints, one p-bit per endpoint (7-bit + p = 8-bit), 4-bit indices for texels
// 1..15 and 3 bits for the anchor (texel 0): 7 + 56 + 2 + 63 = 128 bits, packed LSB first.
void PutBc7Mode6Block(std::vector<uint8_t>& out, const uint8_t e0[4], const uint8_t e1[4], const uint8_t idx[16]) {
    uint64_t bits[2] = {0, 0};
    int pos = 0;
    auto put = [&](uint32_t value, int count) {
        for (int i = 0; i < count; ++i, ++pos)
            if (value & (1u << i)) bits[pos / 64] |= (uint64_t)1 << (pos % 64);
    };
    put(1u << 6, 7);   // mode 6: six zeros then a one
    for (int e = 0; e < 2; ++e) {
        const uint8_t* ep = e == 0 ? e0 : e1;
        for (int c = 0; c < 4; ++c) put(ep[c] >> 1, 7);   // the 7 MSBs; the p-bit below carries the LSB
    }
    put(e0[0] & 1u, 1);   // p-bit endpoint 0 (all channels share it: the test colours are uniform)
    put(e1[0] & 1u, 1);   // p-bit endpoint 1
    put(idx[0], 3);       // anchor index: 3 bits
    for (int i = 1; i < 16; ++i) put(idx[i], 4);
    for (int w = 0; w < 2; ++w)
        for (int i = 0; i < 8; ++i) out.push_back((uint8_t)((bits[w] >> (8 * i)) & 0xFF));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::wstring(argv[i]) == L"--verbose") g_verbose = true;

    // --- legacy mask formats -------------------------------------------------------------------------------------
    {
        // A8R8G8B8, 2x1: one red opaque, one green half-alpha.
        DdsWriter d;
        d.w = 2; d.h = 1; d.pfFlags = 0x40 | 0x1; d.bitCount = 32;
        d.rMask = 0x00FF0000; d.gMask = 0x0000FF00; d.bMask = 0x000000FF; d.aMask = 0xFF000000;
        std::vector<uint8_t> px = {0x00, 0x00, 0xFF, 0xFF,  // A=FF R=FF G=00 B=00 -> red
                                   0x00, 0xFF, 0x00, 0x80}; // A=80 R=00 G=FF B=00 -> half green
        ImageRGBA8 img;
        std::string err;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img, &err) && PixelIs(img, 0, 0, 255, 0, 0, 255) &&
                        PixelIs(img, 1, 0, 0, 255, 0, 128) && img.hasAlpha;
        Check("A8R8G8B8 by masks", ok, ok ? "" : err.c_str());
    }
    {
        // X8R8G8B8 (no alpha): alpha forced to 255. File bytes (B8G8R8A8 order): blue.
        DdsWriter d;
        d.w = 1; d.h = 1; d.pfFlags = 0x40; d.bitCount = 32;
        d.rMask = 0x00FF0000; d.gMask = 0x0000FF00; d.bMask = 0x000000FF;
        std::vector<uint8_t> px = {0xFF, 0x00, 0x00, 0x00};
        ImageRGBA8 img;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img) && PixelIs(img, 0, 0, 0, 0, 255, 255) && !img.hasAlpha;
        Check("X8R8G8B8 no alpha", ok);
    }
    {
        // R8G8B8 (24-bit).
        DdsWriter d;
        d.w = 1; d.h = 1; d.pfFlags = 0x40; d.bitCount = 24;
        d.rMask = 0xFF0000; d.gMask = 0x00FF00; d.bMask = 0x0000FF;
        std::vector<uint8_t> px = {0x30, 0x20, 0x10};   // B,G,R (24-bit is BGRx in file order)
        ImageRGBA8 img;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img) && PixelIs(img, 0, 0, 16, 32, 48, 255);
        Check("R8G8B8 24-bit", ok);
    }
    {
        // R5G6B5 (16-bit).
        DdsWriter d;
        d.w = 1; d.h = 1; d.pfFlags = 0x40; d.bitCount = 16;
        d.rMask = 0xF800; d.gMask = 0x07E0; d.bMask = 0x001F;
        // R=31, G=63, B=31 -> 0xFFFF -> white.
        std::vector<uint8_t> px = {0xFF, 0xFF};
        ImageRGBA8 img;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img) && PixelIs(img, 0, 0, 255, 255, 255, 255);
        Check("R5G6B5", ok);
    }
    {
        // A8L8 (luminance + alpha).
        DdsWriter d;
        d.w = 1; d.h = 1; d.pfFlags = 0x20000 | 0x1; d.bitCount = 16;
        d.rMask = 0xFF; d.aMask = 0xFF00;
        std::vector<uint8_t> px = {0x80, 0x40};   // L=0x80, A=0x40
        ImageRGBA8 img;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img) && PixelIs(img, 0, 0, 128, 128, 128, 64);
        Check("A8L8 luminance", ok);
    }
    {
        // A8 (alpha only).
        DdsWriter d;
        d.w = 1; d.h = 1; d.pfFlags = 0x1; d.bitCount = 8;
        d.aMask = 0xFF;
        std::vector<uint8_t> px = {0x7F};
        ImageRGBA8 img;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img) && PixelIs(img, 0, 0, 0, 0, 0, 127);
        Check("A8 alpha only", ok);
    }

    // --- FourCC numeric formats ----------------------------------------------------------------------------------
    {
        // FourCC 36 = A16B16G16R16 (UNORM): R=0xFFFF -> 255, G=0x8000 -> ~128, B=0, A=0xFFFF.
        DdsWriter d;
        d.w = 1; d.h = 1; d.pfFlags = 0x4; d.fourCC = 36;
        std::vector<uint8_t> px = {0xFF, 0xFF, 0x00, 0x80, 0x00, 0x00, 0xFF, 0xFF};   // R,A,G,B in A16B16G16R16 order
        ImageRGBA8 img;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img) && PixelIs(img, 0, 0, 255, 128, 0, 255);
        Check("FourCC 36 A16B16G16R16", ok);
    }
    {
        // FourCC 114 = R32F: 1.0 -> 255.
        DdsWriter d;
        d.w = 1; d.h = 1; d.pfFlags = 0x4; d.fourCC = 114;
        const float one = 1.0f;
        std::vector<uint8_t> px(4);
        std::memcpy(px.data(), &one, 4);
        ImageRGBA8 img;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img) && PixelIs(img, 0, 0, 255, 255, 255, 255);
        Check("FourCC 114 R32F", ok);
    }
    {
        // FourCC 116 = A32B32G32R32F: (0.5,0.25,0,1).
        DdsWriter d;
        d.w = 1; d.h = 1; d.pfFlags = 0x4; d.fourCC = 116;
        const float v[4] = {0.5f, 0.25f, 0.0f, 1.0f};
        std::vector<uint8_t> px(16);
        std::memcpy(px.data(), v, 16);
        ImageRGBA8 img;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img) && PixelIs(img, 0, 0, 128, 64, 0, 255);
        Check("FourCC 116 A32B32G32R32F", ok);
    }

    // --- DXT ------------------------------------------------------------------------------------------------------
    {
        // DXT1: c0 = white (0xFFFF), c1 = black; index 0 (c0) for the first texel, 1 (c1) for the second.
        DdsWriter d;
        d.w = 2; d.h = 1; d.pfFlags = 0x4; d.fourCC = 0x31545844;
        std::vector<uint8_t> px;
        px.push_back(0xFF); px.push_back(0xFF);   // c0 = 0xFFFF
        px.push_back(0x00); px.push_back(0x00);   // c1 = 0x0000
        // indices: texel0 = 0, texel1 = 1 -> bits 00 01 = 0b0100 = 0x04 for the first 4 texels (only 2 used)
        px.push_back(0x04); px.push_back(0x00); px.push_back(0x00); px.push_back(0x00);
        ImageRGBA8 img;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img) && PixelIs(img, 0, 0, 255, 255, 255, 255) &&
                        PixelIs(img, 1, 0, 0, 0, 0, 255);
        Check("DXT1", ok);
    }
    {
        // DXT5: two texels, color c0=white/c1=white, alpha a0=255 a1=0 with explicit indices 0 then 7 (a1=0).
        DdsWriter d;
        d.w = 2; d.h = 1; d.pfFlags = 0x4; d.fourCC = 0x35545844;
        std::vector<uint8_t> px;
        px.push_back(0xFF); px.push_back(0x00);                    // a0, a1
        for (int i = 0; i < 6; ++i) px.push_back(0x00);            // alpha index bits (all 0 -> a0)
        px.push_back(0xFF); px.push_back(0xFF);                    // c0
        px.push_back(0xFF); px.push_back(0xFF);                    // c1
        for (int i = 0; i < 4; ++i) px.push_back(0x00);            // color indices (all c0)
        ImageRGBA8 img;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img) && PixelIs(img, 0, 0, 255, 255, 255, 255);
        Check("DXT5", ok);
    }

    // --- DX10 header ----------------------------------------------------------------------------------------------
    {
        // DX10 + R8G8B8A8_UNORM (28).
        DdsWriter d;
        d.w = 1; d.h = 1; d.pfFlags = 0x4; d.fourCC = 0x30315844; d.dx10 = true; d.dx10Format = 28;
        std::vector<uint8_t> px = {10, 20, 30, 40};
        ImageRGBA8 img;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img) && PixelIs(img, 0, 0, 10, 20, 30, 40);
        Check("DX10 R8G8B8A8_UNORM", ok);
    }
    {
        // DX10 + B8G8R8A8_UNORM (87): swizzled to RGBA.
        DdsWriter d;
        d.w = 1; d.h = 1; d.pfFlags = 0x4; d.fourCC = 0x30315844; d.dx10 = true; d.dx10Format = 87;
        std::vector<uint8_t> px = {10, 20, 30, 40};   // B,G,R,A
        ImageRGBA8 img;
        const auto bytes = d.Build(px);
        const bool ok = LoadDdsLevel0(bytes.data(), bytes.size(), img) && PixelIs(img, 0, 0, 30, 20, 10, 40);
        Check("DX10 B8G8R8A8_UNORM swizzle", ok);
    }

    // --- rejects --------------------------------------------------------------------------------------------------
    {
        // Cube map must be rejected.
        DdsWriter d;
        d.w = 1; d.h = 1; d.pfFlags = 0x40 | 0x1; d.bitCount = 32;
        d.rMask = 0x00FF0000; d.gMask = 0x0000FF00; d.bMask = 0x000000FF; d.aMask = 0xFF000000;
        d.caps2 = 0x200;   // DDSCAPS2_CUBEMAP
        std::vector<uint8_t> px = {0, 0, 0, 0};
        ImageRGBA8 img;
        std::string err;
        const auto bytes = d.Build(px);
        const bool ok = !LoadDdsLevel0(bytes.data(), bytes.size(), img, &err) && !err.empty();
        Check("cube map rejected", ok, g_verbose ? err.c_str() : "");
    }

// --- S11: block-compressed files kept compressed (LoadDdsCompressed) -------------------------------------------


    // BC1 (DXT1 legacy FourCC): one 4x4 block, c0 = white, c1 = black, texel 0 = c0, texel 1 = c1.
    {
        std::vector<uint8_t> px;
        PutBc1Block(px, 0xFFFF, 0x0000, 0x00000004u);
        DdsWriter d;
        d.w = 4; d.h = 4; d.pfFlags = 0x4; d.fourCC = 0x31545844;   // "DXT1"
        const auto bytes = d.Build(px);
        CompressedImage c;
        std::string err;
        bool ok = LoadDdsCompressed(bytes.data(), bytes.size(), c, &err);
        ok = ok && c.format == DXGI_FORMAT_BC1_UNORM && c.width == 4 && c.height == 4 && c.faces == 1 &&
             c.mipCount == 1 && c.mipOffsets.size() == 1 && c.mipOffsets[0] == 0 && c.data.size() == 8;
        ImageRGBA8 img;
        ok = ok && LoadDdsLevel0(bytes.data(), bytes.size(), img, &err) && PixelIs(img, 0, 0, 255, 255, 255, 255) &&
             PixelIs(img, 1, 0, 0, 0, 0, 255);
        Check("BC1 compressed kept + decode", ok, ok ? "" : err.c_str());
    }
    // BC1 with a full mip chain (4x4 -> 2x2 -> 1x1): 8 + 8 + 8 block bytes; every offset is checked.
    {
        std::vector<uint8_t> px;
        for (int i = 0; i < 3; ++i) PutBc1Block(px, 0xFFFF, 0x0000, 0x00000000u);
        DdsWriter d;
        d.w = 4; d.h = 4; d.pfFlags = 0x4; d.fourCC = 0x31545844; d.mipCount = 3;
        const auto bytes = d.Build(px);
        CompressedImage c;
        std::string err;
        bool ok = LoadDdsCompressed(bytes.data(), bytes.size(), c, &err) && c.mipCount == 3 &&
                  c.mipOffsets.size() == 3 && c.mipOffsets[0] == 0 && c.mipOffsets[1] == 8 && c.mipOffsets[2] == 16 &&
                  c.data.size() == 24;
        Check("BC1 mip chain offset", ok, ok ? "" : err.c_str());
    }
    // A 2x2 file: one block only (block-rounded rows) and dimensions that are not multiples of 4.
    {
        std::vector<uint8_t> px;
        PutBc1Block(px, 0xFFFF, 0x0000, 0x00000000u);
        DdsWriter d;
        d.w = 2; d.h = 2; d.pfFlags = 0x4; d.fourCC = 0x31545844;
        const auto bytes = d.Build(px);
        CompressedImage c;
        std::string err;
        const bool ok = LoadDdsCompressed(bytes.data(), bytes.size(), c, &err) && c.mipCount == 1 &&
                        c.data.size() == 8 && c.width == 2 && c.height == 2;
        Check("BC1 2x2 block-rounded", ok, ok ? "" : err.c_str());
    }
    // BC3 (DXT5): the alpha ramp is the known pattern; hasAlpha must be set.
    {
        std::vector<uint8_t> px;
        px.push_back(0x00);   // a0
        px.push_back(0xFF);   // a1
        for (int i = 0; i < 6; ++i) px.push_back(0x00);   // index 0 = a0 = 0 (transparent)
        px.push_back(0xFF); px.push_back(0xFF);   // c0 white
        px.push_back(0xFF); px.push_back(0xFF);   // c1 white
        for (int i = 0; i < 4; ++i) px.push_back(0x00);
        DdsWriter d;
        d.w = 4; d.h = 4; d.pfFlags = 0x4; d.fourCC = 0x35545844;   // "DXT5"
        const auto bytes = d.Build(px);
        CompressedImage c;
        std::string err;
        bool ok = LoadDdsCompressed(bytes.data(), bytes.size(), c, &err) && c.format == DXGI_FORMAT_BC3_UNORM &&
                  c.data.size() == 16 && c.hasAlpha;
        ImageRGBA8 img;
        ok = ok && LoadDdsLevel0(bytes.data(), bytes.size(), img, &err) && PixelIs(img, 0, 0, 255, 255, 255, 0);
        Check("BC3 compressed kept + alpha", ok, ok ? "" : err.c_str());
    }
    // BC4 (DX10 80) and BC5 (DX10 83): kept compressed; there is no RGBA8 decoder for them here, so the RGBA8 entry
    // point must refuse them (the image path returns the blocks instead: LoadImageWithCompressed).
    {
        std::vector<uint8_t> px(8, 0x00);   // BC4: a0 = 0, a1 = 0, all indices 0 -> one flat block
        DdsWriter d;
        d.w = 4; d.h = 4; d.pfFlags = 0x4; d.fourCC = 0x30315844; d.dx10 = true; d.dx10Format = 80;
        const auto bytes = d.Build(px);
        CompressedImage c;
        std::string err;
        bool ok = LoadDdsCompressed(bytes.data(), bytes.size(), c, &err) && c.format == DXGI_FORMAT_BC4_UNORM &&
                  c.data.size() == 8 && !c.hasAlpha;
        ImageRGBA8 img;
        ImageRGBA8 out;
        CompressedImage comp;
        ok = ok && !LoadDdsLevel0(bytes.data(), bytes.size(), img, nullptr) &&
             LoadImageWithCompressedFromMemory(bytes.data(), bytes.size(), out, comp) && out.Empty() &&
             comp.format == DXGI_FORMAT_BC4_UNORM;
        Check("BC4 compressed kept", ok, ok ? "" : err.c_str());
    }
    {
        std::vector<uint8_t> px(32, 0x00);   // BC5: two 16-byte blocks (8x4 = two blocks wide), all zero
        DdsWriter d;
        d.w = 8; d.h = 4; d.pfFlags = 0x4; d.fourCC = 0x30315844; d.dx10 = true; d.dx10Format = 83;
        const auto bytes = d.Build(px);
        CompressedImage c;
        std::string err;
        bool ok = LoadDdsCompressed(bytes.data(), bytes.size(), c, &err) && c.format == DXGI_FORMAT_BC5_UNORM &&
                  c.data.size() == 32;
        Check("BC5 compressed kept", ok, ok ? "" : err.c_str());
    }
    // BC7 mode 6: endpoints white / black, texel 0 = endpoint 0, the rest endpoint 1.
    {
        std::vector<uint8_t> px;
        const uint8_t e0[4] = {255, 255, 255, 255}, e1[4] = {0, 0, 0, 0};
        uint8_t idx[16] = {0};
        for (int i = 1; i < 16; ++i) idx[i] = 15;
        PutBc7Mode6Block(px, e0, e1, idx);
        DdsWriter d;
        d.w = 4; d.h = 4; d.pfFlags = 0x4; d.fourCC = 0x30315844; d.dx10 = true; d.dx10Format = 98;
        const auto bytes = d.Build(px);
        CompressedImage c;
        std::string err;
        const bool ok = LoadDdsCompressed(bytes.data(), bytes.size(), c, &err) &&
                        c.format == DXGI_FORMAT_BC7_UNORM && c.data.size() == 16 && c.mipCount == 1 &&
                        c.mipOffsets.size() == 1;
        Check("BC7 mode 6 compressed kept", ok, ok ? "" : err.c_str());
    }
    // A truncated BCn file (half a mip) is refused with a message.
    {
        std::vector<uint8_t> px(4, 0x00);
        DdsWriter d;
        d.w = 4; d.h = 4; d.pfFlags = 0x4; d.fourCC = 0x31545844;
        const auto bytes = d.Build(px);
        CompressedImage c;
        std::string err;
        const bool ok = !LoadDdsCompressed(bytes.data(), bytes.size(), c, &err) && !err.empty();
        Check("BC1 truncated refused", ok, g_verbose ? err.c_str() : "");
    }
    // A non-block format must be refused by LoadDdsCompressed (the RGBA8 path keeps it).
    {
        DdsWriter d;
        d.w = 1; d.h = 1; d.pfFlags = 0x40 | 0x1; d.bitCount = 32;
        d.rMask = 0x00FF0000; d.gMask = 0x0000FF00; d.bMask = 0x000000FF; d.aMask = 0xFF000000;
        std::vector<uint8_t> px = {0, 0, 0xFF, 0xFF};
        const auto bytes = d.Build(px);
        CompressedImage c;
        std::string err;
        const bool ok = !LoadDdsCompressed(bytes.data(), bytes.size(), c, &err) && !err.empty();
        Check("A8R8G8B8 not compressed", ok, g_verbose ? err.c_str() : "");
    }

    // --- S11: cube maps (LoadDdsCube) -----------------------------------------------------------------------------
    {
        // Six DXT1 faces, each flat: face f = c0 white with index f? Keep it simple: face 0 red, face 1 green, ...
        std::vector<uint8_t> px;
        const uint16_t colors[6] = {0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x07FF};   // R G B Y M C
        for (int f = 0; f < 6; ++f) PutBc1Block(px, colors[f], colors[f], 0x00000000u);
        DdsWriter d;
        d.w = 4; d.h = 4; d.pfFlags = 0x4; d.fourCC = 0x31545844; d.caps2 = 0x200 | 0xFC00;   // cubemap + all 6 faces
        const auto bytes = d.Build(px);
        bool ok = IsDdsCube(bytes.data(), bytes.size());
        std::array<ImageRGBA8, 6> faces;
        std::string err;
        ok = ok && LoadDdsCube(bytes.data(), bytes.size(), faces, &err);
        ok = ok && PixelIs(faces[0], 0, 0, 255, 0, 0, 255) && PixelIs(faces[1], 0, 0, 0, 255, 0, 255) &&
             PixelIs(faces[2], 0, 0, 0, 0, 255, 255);
        CompressedImage cube;
        ok = ok && LoadDdsCube(bytes.data(), bytes.size(), cube, &err) && cube.faces == 6 && cube.mipCount == 1 &&
             cube.mipOffsets.size() == 6 && cube.mipOffsets[5] == 40 && cube.data.size() == 48;
        // A cube is refused by the 2D compressed path (LoadDdsCompressed) and by nothing else.
        CompressedImage flat;
        ok = ok && !LoadDdsCompressed(bytes.data(), bytes.size(), flat, nullptr);
        Check("cube map round trip", ok, ok ? "" : err.c_str());
    }

    // --- S11: library scan (read-only) ---------------------------------------------------------------------------
    {
        namespace fs = std::filesystem;
        const fs::path root = argc >= 2 && std::wstring(argv[1]) != L"--verbose" ? fs::path(argv[1]) : fs::path(L"library");
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            if (g_verbose) std::printf("SKIP library scan: %s is not a directory\n", root.string().c_str());
        } else {
            struct Tally { int files = 0; int bc2d = 0; int cube2d = 0; int cubeBc = 0; int other = 0; int volume = 0; };
            Tally t;
            std::map<std::string, int> perFormat;
            std::vector<std::string> failures;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const std::wstring ext = it->path().extension().wstring();
                if (ext.size() != 4) continue;
                std::wstring low = ext;
                for (wchar_t& ch : low) ch = (wchar_t)towlower(ch);
                if (low != L".dds") continue;
                ++t.files;
                std::vector<uint8_t> bytes;
                if (!mmdx::ReadWholeFile(it->path(), bytes, nullptr)) {
                    failures.push_back(mmdx::PathToUtf8(it->path()) + ": read failed");
                    continue;
                }
                CompressedImage c;
                std::string err;
                const bool cube = IsDdsCube(bytes.data(), bytes.size());
                if (LoadDdsCompressed(bytes.data(), bytes.size(), c, &err)) {
                    ++t.bc2d;
                    ++perFormat[std::string("2D ") + FormatName(c.format)];
                } else if (cube && LoadDdsCube(bytes.data(), bytes.size(), c, &err)) {
                    ++t.cubeBc;
                    ++perFormat[std::string("cube ") + FormatName(c.format)];
                } else if (cube) {
                    ++t.cube2d;   // a cube in a non-block format: the RGBA8 path handles it
                } else if (err.find("not a supported block-compressed format") != std::string::npos) {
                    ++t.other;
                } else if (err.find("volume texture") != std::string::npos) {
                    ++t.volume;   // a volume DDS: unsupported on purpose (S11 keeps 2D + cube only)
                } else {
                    failures.push_back(mmdx::PathToUtf8(it->path()) + ": " + err);
                }
            }
            if (!ec) {
                std::printf("library scan %s: %d .dds, %d BC 2D, %d BC cube, %d other cube, %d other, %d volume\n",
                            root.string().c_str(), t.files, t.bc2d, t.cubeBc, t.cube2d, t.other, t.volume);
                for (const auto& [name, count] : perFormat) std::printf("  %s: %d\n", name.c_str(), count);
                for (const std::string& f : failures) std::printf("  FAIL %s\n", f.c_str());
                Check("library DDS scan (every BCn file loads)", failures.empty(), failures.empty() ? "" : failures[0].c_str());
            } else {
                std::printf("SKIP library scan: %s\n", ec.message().c_str());
            }
        }
    }

    {
        DdsWriter d;
        d.w = 4; d.h = 4; d.pfFlags = 0x40 | 0x1; d.bitCount = 32;
        d.rMask = 0x00FF0000; d.gMask = 0x0000FF00; d.bMask = 0x000000FF; d.aMask = 0xFF000000;
        std::vector<uint8_t> px(4 * 4 * 4);
        for (size_t i = 0; i < 16; ++i) { px[i * 4 + 0] = 0; px[i * 4 + 1] = 0; px[i * 4 + 2] = 0xFF; px[i * 4 + 3] = 0xFF; }
        const auto bytes = d.Build(px);
        const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "dds_test_synthetic.dds";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        }
        ImageRGBA8 img;
        std::string err;
        const bool loaded = LoadImageRGBA8(tmp, img, &err);
        bool ok = loaded && img.mips.size() == 3 && img.mips[0].width == 4 && img.mips[1].width == 2 &&
                  img.mips[2].width == 1 && PixelIs(img, 0, 0, 255, 0, 0, 255);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        Check("LoadImageRGBA8 DDS + mip chain", ok, ok ? "" : err.c_str());
    }

    // --- S11: glTF MSFT_texture_dds (a texture whose image is replaced by a DDS file) ------------------------------
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "dds_test_msft";
        std::error_code ec;
        fs::create_directories(dir, ec);
        // The DDS the material's texture must resolve to: one BC1 block, flat red.
        std::vector<uint8_t> bc;
        PutBc1Block(bc, 0xF800, 0xF800, 0x00000000u);
        DdsWriter dd;
        dd.w = 4; dd.h = 4; dd.pfFlags = 0x4; dd.fourCC = 0x31545844;
        const std::vector<uint8_t> ddsBytes = dd.Build(bc);
        {
            std::ofstream f(dir / "albedo.dds", std::ios::binary | std::ios::trunc);
            f.write((const char*)ddsBytes.data(), (std::streamsize)ddsBytes.size());
        }
        // One triangle. textures[0] names a PNG fallback that does not exist and carries MSFT_texture_dds pointing at
        // images[1] (the DDS): only the extension path can resolve to the DDS.
        const char* gltf = R"({
  "asset": {"version": "2.0"},
  "extensionsUsed": ["MSFT_texture_dds"],
  "images": [{"uri": "missing_albedo.png"}, {"uri": "albedo.dds"}],
  "textures": [{"source": 0, "extensions": {"MSFT_texture_dds": {"source": 1}}}],
  "materials": [{"name": "DdsMat", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0, "mode": 4}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0]},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 6}
  ],
  "buffers": [{"byteLength": 42, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}],
  "nodes": [{"mesh": 0}],
  "scenes": [{"nodes": [0]}],
  "scene": 0
})";
        const fs::path gltfPath = dir / "dds_image.gltf";
        {
            std::ofstream f(gltfPath, std::ios::binary | std::ios::trunc);
            f << gltf;
        }
        ImpScene scene;
        std::string err;
        bool ok = ImportGltf(gltfPath, scene, &err) && scene.textures.size() == 1 &&
                  scene.textures[0].file.filename() == "albedo.dds";
        if (ok) {
            ImageRGBA8 img;
            CompressedImage comp;
            ok = LoadImageWithCompressed(scene.textures[0].file, img, comp, &err) &&
                 comp.format == DXGI_FORMAT_BC1_UNORM && PixelIs(img, 0, 0, 255, 0, 0, 255);
        }
        fs::remove_all(dir, ec);
        Check("glTF MSFT_texture_dds image", ok, ok ? "" : err.c_str());
    }

    std::printf("dds_test: %d/%d passed\n", g_pass, g_total);
    std::fflush(stdout);
    return g_pass == g_total ? 0 : 1;
}
