// M7 fix 4: AnimatedImage (see AnimatedImage.h). A self-contained animated GIF + APNG decoder.
//
// Why self-contained: WIC can enumerate GIF frames but the MME path must run on a load worker with no COM apartment
// assumptions and must also read APNG (which WIC does not expose frame-by-frame), so both formats share one small,
// testable decoder. The PNG path reverses the PNG filters and inflates with a compact built-in DEFLATE decoder; the
// GIF path is a plain LZW decoder with disposal / blend handling. Only what MME's ANIMATEDTEXTURE needs is supported
// (8-bit palettes, 32-bit RGBA PNGs, no interlace; anything else fails cleanly).
#include "asset/AnimatedImage.h"
#include "asset/BinaryReader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

namespace mmdx {
namespace {

constexpr float kDefaultDelay = 0.1f;   // 100 ms, MME / browsers' default for a delay of 0

// ---- little-endian readers ----------------------------------------------------------------------------------------

uint32_t ReadU32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t ReadU16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
// PNG / APNG integers are big-endian (the GIF path uses ReadU16 above, which is little-endian).
uint32_t ReadU32BE(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }
uint16_t ReadU16BE(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]); }

// ---- DEFLATE (RFC 1951) + zlib wrapper (RFC 1950) ------------------------------------------------------------------

struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0, pos = 0;
    uint32_t bitBuf = 0;
    int bitCount = 0;

    bool Bits(int count, uint32_t& out) {
        while (bitCount < count) {
            if (pos >= size) return false;
            bitBuf |= (uint32_t)data[pos++] << bitCount;
            bitCount += 8;
        }
        out = bitBuf & ((1u << count) - 1u);
        bitBuf >>= count;
        bitCount -= count;
        return true;
    }
    void Align() { bitBuf = 0; bitCount = 0; }
};

// One canonical Huffman table: symbol lookup by walking the code bit by bit (simple, adequate for image sizes).
struct Huffman {
    // code length per symbol; a zero length means "not present".
    std::vector<uint8_t> lengths;
    // (count of codes of each length) for the canonical construction
    std::vector<uint16_t> count;
    std::vector<uint16_t> symbol;

    void Build(const uint8_t* lens, size_t n) {
        lengths.assign(lens, lens + n);
        count.assign(16, 0);
        for (uint8_t l : lengths)
            if (l) count[l]++;
        std::vector<uint16_t> offs(16, 0);
        for (int i = 1; i < 16; ++i) offs[i] = offs[i - 1] + count[i - 1];
        symbol.assign(n, 0);
        std::vector<uint16_t> next = offs;
        for (size_t s = 0; s < n; ++s)
            if (lengths[s]) symbol[next[lengths[s]]++] = (uint16_t)s;
    }

    bool Decode(BitReader& br, int& out) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            uint32_t bit;
            if (!br.Bits(1, bit)) return false;
            code |= (int)bit;
            const int cnt = count[len];
            if (code - first < cnt) {
                out = symbol[index + (code - first)];
                return true;
            }
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        return false;
    }
};

bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t expected) {
    out.clear();
    if (expected) out.reserve(expected);
    BitReader br{data, size};

    auto copy = [&](int len, int dist) -> bool {
        if (dist <= 0 || (size_t)dist > out.size()) return false;
        const size_t start = out.size() - (size_t)dist;
        for (int i = 0; i < len; ++i) out.push_back(out[start + (size_t)i]);
        return true;
    };

    for (;;) {
        uint32_t last = 0, type = 0;
        if (!br.Bits(1, last) || !br.Bits(2, type)) return false;
        if (type == 0) {
            br.Align();
            if (br.pos + 4 > br.size) return false;
            const uint16_t len = ReadU16(data + br.pos);
            br.pos += 4;   // LEN + NLEN
            if (br.pos + len > br.size) return false;
            out.insert(out.end(), data + br.pos, data + br.pos + len);
            br.pos += len;
        } else if (type == 1 || type == 2) {
            static const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
                                                  59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
            static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
                                                  5, 5, 5, 5, 0};
            static const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                                   513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
            static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
                                                   10, 11, 11, 12, 12, 13, 13};
            Huffman lit, dist;
            if (type == 1) {
                uint8_t litLens[288];
                for (int i = 0; i < 288; ++i) litLens[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
                lit.Build(litLens, 288);
                uint8_t distLens[30];
                for (int i = 0; i < 30; ++i) distLens[i] = 5;
                dist.Build(distLens, 30);
            } else {
                uint32_t hlit = 0, hdist = 0, hclen = 0;
                if (!br.Bits(5, hlit) || !br.Bits(5, hdist) || !br.Bits(4, hclen)) return false;
                const int nlit = (int)hlit + 257, ndist = (int)hdist + 1, nclen = (int)hclen + 4;
                static const int kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t clenLens[19] = {0};
                for (int i = 0; i < nclen; ++i) {
                    uint32_t v = 0;
                    if (!br.Bits(3, v)) return false;
                    clenLens[kOrder[i]] = (uint8_t)v;
                }
                Huffman clen;
                clen.Build(clenLens, 19);
                std::vector<uint8_t> lens;
                lens.reserve(nlit + ndist);
                while ((int)lens.size() < nlit + ndist) {
                    int sym = 0;
                    if (!clen.Decode(br, sym)) return false;
                    if (sym < 16) {
                        lens.push_back((uint8_t)sym);
                    } else if (sym == 16) {
                        if (lens.empty()) return false;
                        uint32_t rep = 0;
                        if (!br.Bits(2, rep)) return false;
                        const uint8_t prev = lens.back();
                        for (int i = 0; i < 3 + (int)rep; ++i) lens.push_back(prev);
                    } else if (sym == 17) {
                        uint32_t rep = 0;
                        if (!br.Bits(3, rep)) return false;
                        for (int i = 0; i < 3 + (int)rep; ++i) lens.push_back(0);
                    } else if (sym == 18) {
                        uint32_t rep = 0;
                        if (!br.Bits(7, rep)) return false;
                        for (int i = 0; i < 11 + (int)rep; ++i) lens.push_back(0);
                    } else {
                        return false;
                    }
                }
                lit.Build(lens.data(), (size_t)nlit);
                dist.Build(lens.data() + nlit, (size_t)ndist);
            }
            for (;;) {
                int sym = 0;
                if (!lit.Decode(br, sym)) return false;
                if (sym < 256) {
                    out.push_back((uint8_t)sym);
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return false;
                    uint32_t extra = 0;
                    if (kLenExtra[sym] && !br.Bits(kLenExtra[sym], extra)) return false;
                    const int len = kLenBase[sym] + (int)extra;
                    int dsym = 0;
                    if (!dist.Decode(br, dsym) || dsym >= 30) return false;
                    uint32_t dextra = 0;
                    if (kDistExtra[dsym] && !br.Bits(kDistExtra[dsym], dextra)) return false;
                    const int d = kDistBase[dsym] + (int)dextra;
                    if (!copy(len, d)) return false;
                }
            }
        } else {
            return false;   // reserved block type
        }
        if (last) break;
    }
    return true;
}

// zlib stream (RFC 1950): 2-byte header, DEFLATE, 4-byte Adler-32 (not verified).
bool ZlibInflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t expected) {
    if (size < 2) return false;
    return Inflate(data + 2, size - 2, out, expected);
}

// ---- PNG / APNG ---------------------------------------------------------------------------------------------------

struct PngChunk {
    const uint8_t* data;
    uint32_t len;
    const char type[4];
};

uint8_t Paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc) return (uint8_t)b;
    return (uint8_t)c;
}

// Reverses the PNG per-scanline filters in place: `raw` is (stride+1)*h bytes, `out` is stride*h.
void Unfilter(const uint8_t* raw, size_t rawSize, uint32_t w, uint32_t h, int bpp, std::vector<uint8_t>& out) {
    const size_t stride = (size_t)w * bpp;
    out.assign(stride * h, 0);
    const uint8_t* src = raw;
    size_t srcLeft = rawSize;
    for (uint32_t y = 0; y < h; ++y) {
        if (srcLeft < stride + 1) return;   // truncated
        const uint8_t filter = *src++;
        srcLeft--;
        const uint8_t* row = src;
        uint8_t* cur = out.data() + (size_t)y * stride;
        const uint8_t* prev = y ? out.data() + (size_t)(y - 1) * stride : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= (size_t)bpp ? cur[x - bpp] : 0;
            const int b = prev ? prev[x] : 0;
            const int c = (prev && x >= (size_t)bpp) ? prev[x - bpp] : 0;
            const int v = row[x];
            switch (filter) {
                case 0: cur[x] = (uint8_t)v; break;
                case 1: cur[x] = (uint8_t)(v + a); break;
                case 2: cur[x] = (uint8_t)(v + b); break;
                case 3: cur[x] = (uint8_t)(v + ((a + b) >> 1)); break;
                case 4: cur[x] = (uint8_t)(v + Paeth(a, b, c)); break;
                default: cur[x] = (uint8_t)v; break;
            }
        }
        src += stride;
        srcLeft -= stride;
    }
}

struct PngInfo {
    uint32_t w = 0, h = 0;
    int bitDepth = 8, colorType = 6;   // PNG colour types: 0 gray, 2 rgb, 3 palette, 4 gray+a, 6 rgba
    std::vector<uint8_t> palette;      // RGB triplets (colour type 3)
    std::vector<uint8_t> trns;
    std::vector<uint8_t> idat;
    // APNG
    bool isApng = false;
    uint32_t acTL_numFrames = 0, acTL_numPlays = 0;
    struct Fctl { uint32_t seq, w, h, x, y; uint16_t delayNum, delayDen; uint8_t dispose, blend; };
    std::vector<Fctl> fctl;
    struct Fdat { uint32_t seq; const uint8_t* data; uint32_t len; };
    std::vector<Fdat> fdat;
};

bool ParsePng(const uint8_t* b, size_t size, PngInfo& info) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size < 8 || std::memcmp(b, sig, 8) != 0) return false;
    size_t p = 8;
    bool firstFctlDone = false;
    uint32_t fctlSeen = 0;
    while (p + 8 <= size) {
        const uint32_t len = ReadU32BE(b + p);
        const char* type = (const char*)(b + p + 4);
        const uint8_t* data = b + p + 8;
        if (p + 8 + len + 4 > size) return false;
        if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            info.w = ReadU32BE(data);
            info.h = ReadU32BE(data + 4);
            info.bitDepth = data[8];
            info.colorType = data[9];
            if (data[12] != 0) return false;   // interlace: unsupported
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            info.palette.assign(data, data + len);
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            info.trns.assign(data, data + len);
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            info.idat.insert(info.idat.end(), data, data + len);
        } else if (std::memcmp(type, "acTL", 4) == 0 && len >= 8) {
            info.isApng = true;
            info.acTL_numFrames = ReadU32BE(data);
            info.acTL_numPlays = ReadU32BE(data + 4);
        } else if (std::memcmp(type, "fcTL", 4) == 0 && len >= 26) {
            PngInfo::Fctl f{};
            f.seq = ReadU32BE(data);
            f.w = ReadU32BE(data + 4);
            f.h = ReadU32BE(data + 8);
            f.x = ReadU32BE(data + 12);
            f.y = ReadU32BE(data + 16);
            f.delayNum = ReadU16BE(data + 20);
            f.delayDen = ReadU16BE(data + 22);
            f.dispose = data[24];
            f.blend = data[25];
            info.fctl.push_back(f);
            fctlSeen++;
            if (fctlSeen == 1) firstFctlDone = true;
            (void)firstFctlDone;
        } else if (std::memcmp(type, "fdAT", 4) == 0 && len >= 4) {
            info.fdat.push_back({ReadU32BE(data), data + 4, len - 4});
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        p += 8 + len + 4;
    }
    return info.w && info.h;
}

// Decodes the concatenated IDAT (or an fdAT body) into RGBA8 at (w x h), honouring the PNG colour type / palette.
bool DecodePngImage(const PngInfo& info, const std::vector<uint8_t>& idat, uint32_t w, uint32_t h,
                    std::vector<uint8_t>& rgba) {
    if (info.bitDepth != 8) return false;   // only 8-bit is supported
    int channels = info.colorType == 6 ? 4 : info.colorType == 2 ? 3 : info.colorType == 0 ? 1 : info.colorType == 4 ? 2 : 1;
    std::vector<uint8_t> raw;
    if (!ZlibInflate(idat.data(), idat.size(), raw, ((size_t)w * channels + 1) * h)) return false;
    std::vector<uint8_t> unfiltered;
    Unfilter(raw.data(), raw.size(), w, h, channels, unfiltered);
    rgba.assign((size_t)w * h * 4, 0);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        uint8_t r, g, b2, a;
        if (info.colorType == 3) {
            const uint8_t idx = unfiltered[i];
            if (info.palette.size() >= (size_t)idx * 3 + 3) {
                r = info.palette[idx * 3], g = info.palette[idx * 3 + 1], b2 = info.palette[idx * 3 + 2];
            } else {
                r = g = b2 = 0;
            }
            a = idx < info.trns.size() ? info.trns[idx] : 255;
        } else if (info.colorType == 6) {
            r = unfiltered[i * 4], g = unfiltered[i * 4 + 1], b2 = unfiltered[i * 4 + 2], a = unfiltered[i * 4 + 3];
        } else if (info.colorType == 2) {
            r = unfiltered[i * 3], g = unfiltered[i * 3 + 1], b2 = unfiltered[i * 3 + 2], a = 255;
        } else if (info.colorType == 4) {
            r = g = b2 = unfiltered[i * 2], a = unfiltered[i * 2 + 1];
        } else {
            r = g = b2 = unfiltered[i];
            a = 255;
        }
        rgba[i * 4] = r; rgba[i * 4 + 1] = g; rgba[i * 4 + 2] = b2; rgba[i * 4 + 3] = a;
    }
    return true;
}

bool DecodeApng(const PngInfo& info, std::vector<AnimImageFrame>& out) {
    const uint32_t W = info.w, H = info.h;
    // Order fcTL by sequence number (file order is usually already sorted).
    std::vector<PngInfo::Fctl> fctls = info.fctl;
    std::sort(fctls.begin(), fctls.end(), [](const PngInfo::Fctl& a, const PngInfo::Fctl& b) { return a.seq < b.seq; });

    // Frame 0's data is the default IDAT image when fcTL[0] precedes IDAT (its sequence is the smallest); every
    // other frame's data is the run of fdAT chunks whose sequence lies in (fctl[i].seq, fctl[i+1].seq).
    std::vector<std::vector<uint8_t>> frameData(fctls.size());
    for (size_t i = 0; i < fctls.size(); ++i) {
        const uint32_t lo = fctls[i].seq;
        const uint32_t hi = (i + 1 < fctls.size()) ? fctls[i + 1].seq : 0xFFFFFFFFu;
        for (const PngInfo::Fdat& d : info.fdat)
            if (d.seq > lo && d.seq < hi) frameData[i].insert(frameData[i].end(), d.data, d.data + d.len);
    }
    if (!info.idat.empty() && !fctls.empty()) frameData[0] = info.idat;

    std::vector<uint8_t> canvas((size_t)W * H * 4, 0);
    std::vector<uint8_t> savedRegion;
    int prevDispose = 0;
    uint32_t prevX = 0, prevY = 0, prevW = 0, prevH = 0;

    auto clearRegion = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        for (uint32_t yy = 0; yy < h; ++yy)
            for (uint32_t xx = 0; xx < w; ++xx) {
                const uint32_t dx = x + xx, dy = y + yy;
                if (dx >= W || dy >= H) continue;
                std::memset(&canvas[((size_t)dy * W + dx) * 4], 0, 4);
            }
    };
    auto saveRegion = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        savedRegion.assign((size_t)w * h * 4, 0);
        for (uint32_t yy = 0; yy < h; ++yy)
            for (uint32_t xx = 0; xx < w; ++xx) {
                const uint32_t dx = x + xx, dy = y + yy;
                if (dx >= W || dy >= H) continue;
                std::memcpy(&savedRegion[((size_t)yy * w + xx) * 4], &canvas[((size_t)dy * W + dx) * 4], 4);
            }
    };
    auto restoreRegion = [&](uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        for (uint32_t yy = 0; yy < h; ++yy)
            for (uint32_t xx = 0; xx < w; ++xx) {
                const uint32_t dx = x + xx, dy = y + yy;
                if (dx >= W || dy >= H) continue;
                std::memcpy(&canvas[((size_t)dy * W + dx) * 4], &savedRegion[((size_t)yy * w + xx) * 4], 4);
            }
    };

    for (size_t i = 0; i < fctls.size(); ++i) {
        const PngInfo::Fctl& f = fctls[i];
        // Apply the previous frame's dispose op before compositing this frame.
        if (i > 0) {
            if (prevDispose == 1) clearRegion(prevX, prevY, prevW, prevH);
            else if (prevDispose == 2 && !savedRegion.empty()) restoreRegion(prevX, prevY, prevW, prevH);
        }
        if (f.dispose == 2) saveRegion(f.x, f.y, f.w, f.h);   // snapshot for a later PREVIOUS restore

        std::vector<uint8_t> sub;
        if (frameData[i].empty() || !DecodePngImage(info, frameData[i], f.w, f.h, sub)) {
            prevDispose = f.dispose; prevX = f.x; prevY = f.y; prevW = f.w; prevH = f.h;
            continue;   // undecodable frame: skip it, keep the canvas
        }
        // blend_op: 0 = SOURCE (copy), 1 = OVER (alpha blend).
        for (uint32_t y = 0; y < f.h; ++y)
            for (uint32_t x = 0; x < f.w; ++x) {
                const uint32_t dx = f.x + x, dy = f.y + y;
                if (dx >= W || dy >= H) continue;
                const uint8_t* s = &sub[((size_t)y * f.w + x) * 4];
                uint8_t* d = &canvas[((size_t)dy * W + dx) * 4];
                if (f.blend == 1) {
                    const int sa = s[3];
                    d[0] = (uint8_t)((s[0] * sa + d[0] * (255 - sa)) / 255);
                    d[1] = (uint8_t)((s[1] * sa + d[1] * (255 - sa)) / 255);
                    d[2] = (uint8_t)((s[2] * sa + d[2] * (255 - sa)) / 255);
                    d[3] = (uint8_t)std::min(255, d[3] + sa);
                } else {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                }
            }
        AnimImageFrame frame;
        frame.width = W;
        frame.height = H;
        frame.rgba = canvas;
        const float den = f.delayDen ? (float)f.delayDen : 100.0f;
        frame.delay = f.delayNum / den;
        if (frame.delay <= 0.0f) frame.delay = kDefaultDelay;
        out.push_back(std::move(frame));
        prevDispose = f.dispose; prevX = f.x; prevY = f.y; prevW = f.w; prevH = f.h;
    }
    return !out.empty();
}

// ---- GIF ----------------------------------------------------------------------------------------------------------

struct GifFrame {
    std::vector<uint8_t> rgba;   // w*h*4 at the logical screen size
    float delay = kDefaultDelay;
    int dispose = 0;             // 0 none, 1 keep, 2 background, 3 previous
};

bool DecodeGif(const uint8_t* b, size_t size, std::vector<AnimImageFrame>& out) {
    if (size < 13 || std::memcmp(b, "GIF", 3) != 0) return false;
    const uint32_t sw = ReadU16(b + 6), sh = ReadU16(b + 8);
    if (!sw || !sh) return false;
    const uint8_t packed = b[10];
    const bool hasGct = (packed & 0x80) != 0;
    const int gctSize = hasGct ? (2 << (packed & 7)) : 0;
    size_t p = 13;
    std::vector<uint8_t> gct;
    if (hasGct) {
        if (p + (size_t)gctSize * 3 > size) return false;
        gct.assign(b + p, b + p + (size_t)gctSize * 3);
        p += (size_t)gctSize * 3;
    }
    std::vector<uint8_t> canvas((size_t)sw * sh * 4, 0);
    std::vector<uint8_t> savedRegion;
    int prevDispose = 0;
    uint32_t prevX = 0, prevY = 0, prevW = 0, prevH = 0;
    int gceDispose = 0;      // pending Graphic Control Extension values
    int gceTransparent = -1;
    float gceDelay = kDefaultDelay;

    while (p < size) {
        const uint8_t block = b[p++];
        if (block == 0x3B) break;   // trailer
        if (block == 0x21) {        // extension
            if (p >= size) break;
            const uint8_t label = b[p++];
            if (label == 0xF9) {    // graphic control extension
                if (p + 5 > size) break;
                const uint8_t len = b[p];
                if (len < 4) break;
                const uint8_t gcePacked = b[p + 1];
                gceDispose = (gcePacked >> 2) & 7;
                const bool hasTrans = (gcePacked & 1) != 0;
                const uint16_t delayCentis = ReadU16(b + p + 2);
                const uint8_t transIdx = b[p + 4];
                gceDelay = delayCentis > 0 ? delayCentis / 100.0f : kDefaultDelay;
                gceTransparent = hasTrans ? (int)transIdx : -1;
                p += 1 + len;   // len + payload
                if (p < size && b[p] == 0) p++;   // block terminator
            } else {
                while (p < size) {
                    const uint8_t sub = b[p++];
                    if (sub == 0) break;
                    p += sub;
                }
            }
            continue;
        }
        if (block != 0x2C) {   // 0x2C = image descriptor; anything else: skip sub-blocks
            while (p < size) {
                const uint8_t sub = b[p++];
                if (sub == 0) break;
                p += sub;
            }
            continue;
        }
        if (p + 9 > size) return false;
        const uint32_t ix = ReadU16(b + p), iy = ReadU16(b + p + 2);
        const uint32_t iw = ReadU16(b + p + 4), ih = ReadU16(b + p + 6);
        const uint8_t ipacked = b[p + 8];
        p += 9;
        const bool hasLct = (ipacked & 0x80) != 0;
        const bool interlace = (ipacked & 0x40) != 0;
        const int lctSize = hasLct ? (2 << (ipacked & 7)) : 0;
        std::vector<uint8_t> localPalette;
        if (hasLct) {
            if (p + (size_t)lctSize * 3 > size) return false;
            localPalette.assign(b + p, b + p + (size_t)lctSize * 3);
            p += (size_t)lctSize * 3;
        }
        const std::vector<uint8_t>& pal = hasLct ? localPalette : gct;
        if (pal.empty() || !iw || !ih) return false;
        if (p >= size) return false;
        const uint8_t minCodeSize = b[p++];
        if (minCodeSize < 2 || minCodeSize > 8) return false;
        std::vector<uint8_t> lzw;
        while (p < size) {
            const uint8_t sub = b[p++];
            if (sub == 0) break;
            if (p + sub > size) return false;
            lzw.insert(lzw.end(), b + p, b + p + sub);
            p += sub;
        }
        // ---- LZW decode to palette indices ----
        const int clearCode = 1 << minCodeSize;
        const int endCode = clearCode + 1;
        std::vector<std::vector<uint8_t>> dict((size_t)1 << (minCodeSize + 1));
        auto resetDict = [&]() {
            for (auto& e : dict) e.clear();
            for (int i = 0; i < clearCode; ++i) dict[i] = {(uint8_t)i};
        };
        resetDict();
        int codeSize = minCodeSize + 1;
        int next = endCode + 1;
        std::vector<uint8_t> prev;
        std::vector<uint8_t> rows((size_t)iw * ih, 0);
        size_t rowPos = 0;
        uint32_t bitBuf = 0;
        int bitCount = 0;
        size_t lp = 0;
        bool done = false;
        while (!done && rowPos < rows.size()) {
            while (bitCount < codeSize && lp < lzw.size()) {
                bitBuf |= (uint32_t)lzw[lp++] << bitCount;
                bitCount += 8;
            }
            if (bitCount < codeSize) break;
            const int code = bitBuf & ((1 << codeSize) - 1);
            bitBuf >>= codeSize;
            bitCount -= codeSize;
            if (code == clearCode) {
                resetDict();
                codeSize = minCodeSize + 1;
                next = endCode + 1;
                prev.clear();
                continue;
            }
            if (code == endCode) { done = true; break; }
            std::vector<uint8_t> entry;
            if (code < next && !dict[code].empty()) {
                entry = dict[code];
            } else if (!prev.empty()) {
                entry = prev;
                entry.push_back(prev[0]);
            } else {
                break;
            }
            for (uint8_t v : entry) {
                if (rowPos < rows.size()) rows[rowPos++] = v;
            }
            if (!prev.empty() && next < (int)dict.size()) {
                std::vector<uint8_t> ne = prev;
                ne.push_back(entry[0]);
                dict[next++] = std::move(ne);
                if (next >= (1 << codeSize) && codeSize < 12) codeSize++;
            }
            prev = std::move(entry);
        }
        if (interlace && ih > 1) {
            std::vector<uint8_t> de((size_t)iw * ih, 0);
            const int starts[4] = {0, 4, 2, 1};
            const int steps[4] = {8, 8, 4, 2};
            size_t srcRow = 0;
            for (int pass = 0; pass < 4; ++pass)
                for (uint32_t y = starts[pass]; y < ih; y += steps[pass]) {
                    if (srcRow < ih) std::memcpy(&de[(size_t)y * iw], &rows[srcRow * iw], iw);
                    ++srcRow;
                }
            rows.swap(de);
        }
        // ---- apply the previous frame's pending dispose ----
        if (prevDispose == 2) {
            for (uint32_t y = 0; y < prevH; ++y)
                for (uint32_t x = 0; x < prevW; ++x) {
                    const uint32_t dx = prevX + x, dy = prevY + y;
                    if (dx >= sw || dy >= sh) continue;
                    std::memset(&canvas[((size_t)dy * sw + dx) * 4], 0, 4);
                }
        } else if (prevDispose == 3) {
            for (uint32_t y = 0; y < prevH; ++y)
                for (uint32_t x = 0; x < prevW; ++x) {
                    const uint32_t dx = prevX + x, dy = prevY + y;
                    if (dx >= sw || dy >= sh) continue;
                    const size_t idx = ((size_t)y * prevW + x) * 4;
                    if (idx + 4 <= savedRegion.size())
                        std::memcpy(&canvas[((size_t)dy * sw + dx) * 4], &savedRegion[idx], 4);
                }
        }
        // Note: GIF dispose=3 (RESTORE_PREVIOUS) in this decoder restores from `savedRegion`, which is snapshotted
        // below for the NEXT frame; that matches the common two-frame toggle but not a deep undo stack. Acceptable.
        // ---- snapshot for a future RESTORE_PREVIOUS (before drawing) ----
        savedRegion.assign((size_t)iw * ih * 4, 0);
        for (uint32_t y = 0; y < ih; ++y)
            for (uint32_t x = 0; x < iw; ++x) {
                const uint32_t dx = ix + x, dy = iy + y;
                if (dx >= sw || dy >= sh) continue;
                std::memcpy(&savedRegion[((size_t)y * iw + x) * 4], &canvas[((size_t)dy * sw + dx) * 4], 4);
            }
        // ---- blit the frame ----
        for (uint32_t y = 0; y < ih; ++y)
            for (uint32_t x = 0; x < iw; ++x) {
                const uint32_t dx = ix + x, dy = iy + y;
                if (dx >= sw || dy >= sh) continue;
                const uint8_t idx = rows[(size_t)y * iw + x];
                if ((int)idx == gceTransparent) continue;   // transparent: leave the canvas pixel
                if (pal.size() >= (size_t)idx * 3 + 3) {
                    uint8_t* d = &canvas[((size_t)dy * sw + dx) * 4];
                    d[0] = pal[idx * 3]; d[1] = pal[idx * 3 + 1]; d[2] = pal[idx * 3 + 2]; d[3] = 255;
                }
            }
        AnimImageFrame frame;
        frame.width = sw;
        frame.height = sh;
        frame.rgba = canvas;
        frame.delay = gceDelay;
        out.push_back(std::move(frame));

        prevDispose = gceDispose;
        prevX = ix; prevY = iy; prevW = iw; prevH = ih;
        gceDispose = 0;
        gceTransparent = -1;
        gceDelay = kDefaultDelay;
    }
    return !out.empty();
}

} // namespace

// ---- AnimatedImage ------------------------------------------------------------------------------------------------

float AnimatedImage::TotalDuration() const {
    float t = 0.0f;
    for (const AnimImageFrame& f : frames_) t += std::max(0.0f, f.delay);
    return t;
}

uint32_t PackTexelsToFloat4(const uint8_t* rgba, uint32_t width, uint32_t height, float* out, uint32_t maxTexels) {
    if (!rgba || !out) return 0;
    const uint32_t count = std::min(maxTexels, width * height);
    for (uint32_t i = 0; i < count; ++i) {
        out[i * 4 + 0] = rgba[i * 4 + 0] / 255.0f;
        out[i * 4 + 1] = rgba[i * 4 + 1] / 255.0f;
        out[i * 4 + 2] = rgba[i * 4 + 2] / 255.0f;
        out[i * 4 + 3] = rgba[i * 4 + 3] / 255.0f;
    }
    return count;
}

size_t AnimatedImage::FrameAt(float timeSeconds) const {
    if (frames_.empty()) return 0;
    if (frames_.size() == 1) return 0;
    const float total = TotalDuration();
    if (total <= 0.0f) return 0;
    float t = std::fmod(timeSeconds, total);
    if (t < 0.0f) t += total;
    for (size_t i = 0; i < frames_.size(); ++i) {
        t -= std::max(0.0f, frames_[i].delay);
        if (t < 0.0f) return i;
    }
    return frames_.size() - 1;
}

bool AnimatedImage::Load(const std::filesystem::path& path, std::string* error) {
    std::vector<uint8_t> data;
    std::string readError;
    if (!ReadWholeFile(path, data, &readError)) {
        if (error) *error = readError;
        return false;
    }
    return LoadFromMemory(data.data(), data.size(), error);
}

bool AnimatedImage::LoadFromMemory(const uint8_t* bytes, size_t size, std::string* error) {
    frames_.clear();
    if (!bytes || size < 8) {
        if (error) *error = "empty image";
        return false;
    }
    if (std::memcmp(bytes, "GIF", 3) == 0) {
        std::vector<AnimImageFrame> frames;
        if (DecodeGif(bytes, size, frames) && !frames.empty()) {
            frames_ = std::move(frames);
            return true;
        }
        if (error) *error = "GIF decode failed";
        return false;
    }
    PngInfo info;
    if (ParsePng(bytes, size, info)) {
        std::vector<AnimImageFrame> frames;
        if (info.isApng && !info.fctl.empty()) {
            if (DecodeApng(info, frames) && !frames.empty()) {
                frames_ = std::move(frames);
                return true;
            }
            if (error) *error = "APNG decode failed";
            return false;
        }
        std::vector<uint8_t> rgba;
        if (DecodePngImage(info, info.idat, info.w, info.h, rgba)) {
            AnimImageFrame f;
            f.width = info.w;
            f.height = info.h;
            f.rgba = std::move(rgba);
            f.delay = kDefaultDelay;
            frames_.push_back(std::move(f));
            return true;
        }
        if (error) *error = "PNG decode failed";
        return false;
    }
    if (error) *error = "unsupported animated image format";
    return false;
}

} // namespace mmdx
