// anim_texture_test: M7 fix 4 self test for asset/AnimatedImage. Builds a synthetic 2-frame APNG and a 2-frame
// animated GIF in memory, decodes both, and checks the frames, delays and per-frame texels (plus FrameAt's
// time -> frame mapping). No window, no assets.
//
//   anim_texture_test [--verbose]
//
// Exit code 0 when every case passes, 1 otherwise.
#include "asset/AnimatedImage.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mmdx;

namespace {

int g_failures = 0;
bool g_verbose = false;

void Check(bool cond, const char* what) {
    if (!cond) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    } else if (g_verbose) {
        std::printf("ok  : %s\n", what);
    }
}

// ---- PNG / APNG writer (stored DEFLATE: no compression, only a zlib header + Adler-32) -----------------------------

void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v >> 24)); b.push_back((uint8_t)(v >> 16)); b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v);
}
void PutU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v >> 8)); b.push_back((uint8_t)v);
}
// little-endian (DEFLATE stored blocks, GIF logical screen / image descriptor).
void PutU16LE(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8));
}

uint32_t Crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void PngChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    PutU32(out, (uint32_t)data.size());
    const size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t crc = Crc32(out.data() + crcStart, 4 + data.size());
    PutU32(out, crc);
}

uint32_t Adler32(const std::vector<uint8_t>& data) {
    uint32_t a = 1, b = 0;
    for (uint8_t c : data) { a = (a + c) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

// Wraps raw DEFLATE "stored" blocks in the PNG zlib framing. `raw` is the filtered scanline stream.
std::vector<uint8_t> ZlibStore(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);   // zlib header
    size_t p = 0;
    while (p < raw.size()) {
        const size_t n = std::min<size_t>(65535, raw.size() - p);
        const bool last = (p + n >= raw.size());
        z.push_back(last ? 1 : 0);
        PutU16LE(z, (uint16_t)n);
        PutU16LE(z, (uint16_t)~n);
        z.insert(z.end(), raw.begin() + p, raw.begin() + p + n);
        p += n;
    }
    if (raw.empty()) z.push_back(1);
    PutU32(z, Adler32(raw));
    return z;
}

// One RGBA image (w x h) encoded as a PNG IDAT stream: filter type 0 rows, stored DEFLATE.
std::vector<uint8_t> PngRaw(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);   // filter none
        for (uint32_t x = 0; x < w; ++x) { raw.push_back(r); raw.push_back(g); raw.push_back(b); raw.push_back(a); }
    }
    return ZlibStore(raw);
}

void PngHeader(std::vector<uint8_t>& out, uint32_t w, uint32_t h) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), sig, sig + 8);
    std::vector<uint8_t> ihdr;
    PutU32(ihdr, w); PutU32(ihdr, h);
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // colour type RGBA
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);   // compression / filter / interlace
    PngChunk(out, "IHDR", ihdr);
}

// A 2-frame APNG: frame 0 red, frame 1 blue, each 100 ms, full-frame (blend source).
std::vector<uint8_t> MakeApng() {
    const uint32_t W = 4, H = 4;
    std::vector<uint8_t> out;
    PngHeader(out, W, H);
    std::vector<uint8_t> actl;
    PutU32(actl, 2);   // num frames
    PutU32(actl, 0);   // num plays (infinite)
    PngChunk(out, "acTL", actl);
    // frame 0 fcTL (seq 0) then its IDAT
    auto fctl = [&](uint32_t seq, uint16_t dn, uint16_t dd) {
        std::vector<uint8_t> f;
        PutU32(f, seq); PutU32(f, W); PutU32(f, H); PutU32(f, 0); PutU32(f, 0);
        PutU16(f, dn); PutU16(f, dd); f.push_back(0); f.push_back(0);   // dispose none, blend source
        PngChunk(out, "fcTL", f);
    };
    fctl(0, 1, 10);
    std::vector<uint8_t> idat0 = PngRaw(W, H, 255, 0, 0, 255);
    PngChunk(out, "IDAT", idat0);
    // frame 1 fcTL (seq 1) then fdAT (seq 2)
    fctl(1, 1, 10);
    std::vector<uint8_t> fdat;
    PutU32(fdat, 2);
    std::vector<uint8_t> idat1 = PngRaw(W, H, 0, 0, 255, 255);
    fdat.insert(fdat.end(), idat1.begin(), idat1.end());
    PngChunk(out, "fdAT", fdat);
    PngChunk(out, "IEND", {});
    return out;
}

// ---- GIF writer -----------------------------------------------------------------------------------------------------

void GifBlock(std::vector<uint8_t>& out, const std::vector<uint8_t>& data) {
    size_t p = 0;
    while (p < data.size()) {
        const size_t n = std::min<size_t>(255, data.size() - p);
        out.push_back((uint8_t)n);
        out.insert(out.end(), data.begin() + p, data.begin() + p + n);
        p += n;
    }
    out.push_back(0);
}

// A 2-frame GIF (2x2): frame 0 red, frame 1 green, each 10 centiseconds. Uses an uncompressed-ish LZW encoder with
// clear codes only (every pixel a fresh literal), which every decoder accepts.
std::vector<uint8_t> MakeGif() {
    std::vector<uint8_t> out;
    const uint8_t header[6] = {'G', 'I', 'F', '8', '9', 'a'};
    out.insert(out.end(), header, header + 6);
    PutU16LE(out, 2); PutU16LE(out, 2);
    out.push_back(0x00);   // no GCT
    out.push_back(0); out.push_back(0);

    auto image = [&](uint8_t colR, uint8_t colG, uint8_t colB) {
        // graphic control extension: dispose none, delay 10cs, no transparency
        out.push_back(0x21); out.push_back(0xF9); out.push_back(4);
        out.push_back(0x00); PutU16LE(out, 10); out.push_back(0); out.push_back(0);
        // image descriptor
        out.push_back(0x2C);
        PutU16LE(out, 0); PutU16LE(out, 0); PutU16LE(out, 2); PutU16LE(out, 2);
        out.push_back(0x80);   // local colour table, 2 colours
        out.push_back(colR); out.push_back(colG); out.push_back(colB);
        out.push_back(0); out.push_back(0); out.push_back(0);
        out.push_back(2);   // LZW min code size
        // LZW stream: clear, then four iterations of (literal 0), end. Indices all 0 -> colour 0.
        // Codes: CLEAR=4, END=5, first free=6, code size starts at 3.
        std::vector<uint8_t> lzw;
        uint32_t bitBuf = 0; int bits = 0;
        auto emit = [&](int code, int size) {
            bitBuf |= (uint32_t)code << bits; bits += size;
            while (bits >= 8) { lzw.push_back((uint8_t)(bitBuf & 0xFF)); bitBuf >>= 8; bits -= 8; }
        };
        emit(4, 3);   // clear
        for (int i = 0; i < 4; ++i) { emit(4, 3); emit(0, 3); }   // clear + literal: code size stays 3
        emit(5, 3);   // end
        if (bits > 0) lzw.push_back((uint8_t)(bitBuf & 0xFF));
        GifBlock(out, lzw);
    };
    image(255, 0, 0);
    image(0, 255, 0);
    out.push_back(0x3B);   // trailer
    return out;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--verbose") g_verbose = true;

    // ---- APNG ----
    {
        std::vector<uint8_t> png = MakeApng();
        AnimatedImage img;
        std::string err;
        const bool ok = img.LoadFromMemory(png.data(), png.size(), &err);
        Check(ok, "APNG decodes");
        if (ok) {
            Check(img.FrameCount() == 2, "APNG has 2 frames");
            Check(img.Width() == 4 && img.Height() == 4, "APNG canvas 4x4");
            if (img.FrameCount() == 2) {
                const AnimImageFrame& f0 = img.Frame(0);
                const AnimImageFrame& f1 = img.Frame(1);
                Check(f0.rgba[0] == 255 && f0.rgba[1] == 0 && f0.rgba[2] == 0, "APNG frame 0 is red");
                Check(f1.rgba[0] == 0 && f1.rgba[1] == 0 && f1.rgba[2] == 255, "APNG frame 1 is blue");
                Check(f0.delay > 0.0f && f1.delay > 0.0f, "APNG delays positive");
                Check(img.FrameAt(0.05f) == 0, "FrameAt(0.05) = 0");
                Check(img.FrameAt(0.15f) == 1, "FrameAt(0.15) = 1");
                Check(img.FrameAt(img.TotalDuration() + 0.15f) == 1, "FrameAt wraps");
            }
        } else {
            std::printf("  APNG error: %s\n", err.c_str());
        }
    }

    // ---- GIF ----
    {
        std::vector<uint8_t> gif = MakeGif();
        AnimatedImage img;
        std::string err;
        const bool ok = img.LoadFromMemory(gif.data(), gif.size(), &err);
        Check(ok, "GIF decodes");
        if (ok) {
            Check(img.FrameCount() == 2, "GIF has 2 frames");
            Check(img.Width() == 2 && img.Height() == 2, "GIF canvas 2x2");
            if (img.FrameCount() == 2) {
                Check(img.Frame(0).rgba[0] == 255 && img.Frame(0).rgba[1] == 0, "GIF frame 0 is red");
                Check(img.Frame(1).rgba[0] == 0 && img.Frame(1).rgba[1] == 255, "GIF frame 1 is green");
                Check(img.Frame(0).delay >= 0.09f && img.Frame(0).delay <= 0.11f, "GIF delay ~0.1 s");
            }
        } else {
            std::printf("  GIF error: %s\n", err.c_str());
        }
    }

    // ---- plain PNG (single frame) ----
    {
        std::vector<uint8_t> out;
        PngHeader(out, 2, 2);
        std::vector<uint8_t> idat = PngRaw(2, 2, 10, 20, 30, 255);
        PngChunk(out, "IDAT", idat);
        PngChunk(out, "IEND", {});
        AnimatedImage img;
        std::string err;
        const bool ok = img.LoadFromMemory(out.data(), out.size(), &err);
        Check(ok && img.FrameCount() == 1, "plain PNG = 1 frame");
        if (ok) Check(img.Frame(0).rgba[0] == 10 && img.Frame(0).rgba[1] == 20 && img.Frame(0).rgba[2] == 30,
                      "plain PNG texel");
    }

    // ---- TEXTUREVALUE packing (fix 5) ----
    {
        const uint8_t texels[2 * 2 * 4] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 255, 255, 255, 64};
        float f4[4 * 4] = {0};
        const uint32_t n = PackTexelsToFloat4(texels, 2, 2, f4, 4);
        Check(n == 4, "TEXTUREVALUE packs 4 float4s");
        Check(f4[0] == 1.0f && f4[1] == 0.0f && f4[3] == 1.0f, "texel 0 packed");
        Check(f4[5] == 1.0f && f4[7] > 0.49f && f4[7] < 0.51f, "texel 1 packed");
        const uint32_t m = PackTexelsToFloat4(texels, 2, 2, f4, 2);
        Check(m == 2, "TEXTUREVALUE clamps to maxTexels");
    }

    std::printf("anim_texture_test: %s (%d failure%s)\n", g_failures ? "FAIL" : "PASS", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
