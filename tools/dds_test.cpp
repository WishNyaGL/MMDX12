// dds_test: synthetic DDS decoding self test (M5a). Writes DDS files in memory (basic header + pixel data),
// decodes them through asset/DdsLoader (LoadDdsLevel0) and the image path (LoadImageRGBA8), and checks the decoded
// texel values. No window, no assets.
//
//   dds_test [--verbose]
//
// Exit code 0 when every case passes, 1 otherwise.
#include "asset/DdsLoader.h"
#include "asset/ImageLoader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
        Put32(b, 1);                   // mipMapCount
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

    // --- integration through the image path (mip chain built) -----------------------------------------------------
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

    std::printf("dds_test: %d/%d passed\n", g_pass, g_total);
    std::fflush(stdout);
    return g_pass == g_total ? 0 : 1;
}
