// S9b-3: the sky image decoder (see render/SkyMap.h). CPU only: one linear-radiance equirect panorama of RGBA16F.
#include "render/SkyMap.h"
#include "asset/DdsLoader.h"
#include "asset/ImageLoader.h"
#include "core/TextUtil.h"

// Declarations only: STB_IMAGE_IMPLEMENTATION lives in asset/ImageLoader.cpp (one copy per process). stbi_loadf reads
// a Radiance .hdr; every other 8-bit format goes through the project's own image loader.
#include <stb_image.h>

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace mmdx {

namespace {

// The sRGB -> linear curve on an 8-bit channel (the standard transfer, as common.hlsli's SrgbToLinear).
float SrgbChannel(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// One RGBA8 texel -> linear radiance. encoding: 0 auto (resolved by the caller), 1 linear, 2 sRGB, 3 Ray-MMD RGBT.
DirectX::XMFLOAT3 DecodeTexel(const uint8_t* rgba, int encoding) {
    const float r = (float)rgba[0] / 255.0f;
    const float g = (float)rgba[1] / 255.0f;
    const float b = (float)rgba[2] / 255.0f;
    if (encoding == 1) return {r, g, b};
    if (encoding == 2) return {SrgbChannel(r), SrgbChannel(g), SrgbChannel(b)};
    // Ray-MMD's RGBT: the alpha carries the exponent's scale over a range of 1024
    const float a = (float)rgba[3] / 255.0f;
    const float t = a / (1.0f + 1.0f / 1024.0f - a);
    return {r * t, g * t, b * t};
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out, std::string* error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (error) *error = "cannot open the file";
        return false;
    }
    const std::streamoff size = in.tellg();
    if (size <= 0) {
        if (error) *error = "the file is empty";
        return false;
    }
    out.resize((size_t)size);
    in.seekg(0, std::ios::beg);
    if (!in.read((char*)out.data(), size)) {
        if (error) *error = "cannot read the file";
        return false;
    }
    return true;
}

// Integer box downsample so the panorama's width is at most 4096 texels (the shaders' equirect budget).
void DownsampleToBudget(std::vector<DirectX::XMFLOAT3>& px, uint32_t& w, uint32_t& h) {
    while (w > 4096) {
        const uint32_t f = (w + 4095) / 4096;
        const uint32_t nw = std::max(1u, w / f);
        const uint32_t nh = std::max(1u, h / f);
        std::vector<DirectX::XMFLOAT3> out((size_t)nw * nh);
        for (uint32_t y = 0; y < nh; ++y) {
            const uint32_t y0 = std::min(y * f, h - 1), y1 = std::min(y0 + f, h);
            for (uint32_t x = 0; x < nw; ++x) {
                const uint32_t x0 = std::min(x * f, w - 1), x1 = std::min(x0 + f, w);
                double r = 0, g = 0, b = 0;
                uint32_t n = 0;
                for (uint32_t sy = y0; sy < y1; ++sy)
                    for (uint32_t sx = x0; sx < x1; ++sx) {
                        const DirectX::XMFLOAT3& t = px[(size_t)sy * w + sx];
                        r += t.x;
                        g += t.y;
                        b += t.z;
                        ++n;
                    }
                const double inv = n ? 1.0 / (double)n : 0.0;
                out[(size_t)y * nw + x] = {(float)(r * inv), (float)(g * inv), (float)(b * inv)};
            }
        }
        px = std::move(out);
        w = nw;
        h = nh;
    }
}

// The mean linear rgb of the rows whose centre v = (y + 0.5) / h lies in (lo, hi) (`upper` false: v > lo only).
DirectX::XMFLOAT3 BandMean(const std::vector<DirectX::XMFLOAT3>& px, uint32_t w, uint32_t h, float lo, float hi,
                           bool hasUpper) {
    double r = 0, g = 0, b = 0;
    uint64_t n = 0;
    for (uint32_t y = 0; y < h; ++y) {
        const float v = ((float)y + 0.5f) / (float)h;
        if (v <= lo || (hasUpper && v >= hi)) continue;
        for (uint32_t x = 0; x < w; ++x) {
            const DirectX::XMFLOAT3& t = px[(size_t)y * w + x];
            r += t.x;
            g += t.y;
            b += t.z;
            ++n;
        }
    }
    if (n == 0) return {0.0f, 0.0f, 0.0f};
    const double inv = 1.0 / (double)n;
    return {(float)(r * inv), (float)(g * inv), (float)(b * inv)};
}

// Converts the panorama to the RGBA16F halves the texture holds (alpha 1).
void PackHalves(const std::vector<DirectX::XMFLOAT3>& px, std::vector<uint16_t>& out) {
    out.resize(px.size() * 4);
    for (size_t i = 0; i < px.size(); ++i) {
        out[i * 4 + 0] = DirectX::PackedVector::XMConvertFloatToHalf(px[i].x);
        out[i * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(px[i].y);
        out[i * 4 + 2] = DirectX::PackedVector::XMConvertFloatToHalf(px[i].z);
        out[i * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
    }
}

// A cube map's six faces (+X -X +Y -Y +Z -Z, size S) -> an equirect panorama W = min(4 S, 4096) x W / 2. The texel
// direction is the shader's (shaders/sky_map.hlsli): phi = (u - 0.5) 2 PI, theta = v PI,
// d = (sin theta sin phi, cos theta, sin theta cos phi); the face is the largest |component| and the face-local
// coordinates come from the other two.
bool CubeToEquirect(const std::array<ImageRGBA8, 6>& faces, int encoding, std::vector<DirectX::XMFLOAT3>& px,
                    uint32_t& w, uint32_t& h, std::string* error) {
    const uint32_t S = faces[0].Width();
    for (const ImageRGBA8& f : faces)
        if (f.Empty() || f.Width() != S || f.Height() != S) {
            if (error) *error = "dds: the cube faces are not equal squares";
            return false;
        }
    if (S == 0) {
        if (error) *error = "dds: empty cube faces";
        return false;
    }
    w = std::min(4u * S, 4096u);
    h = std::max(1u, w / 2);
    px.assign((size_t)w * h, DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f});
    for (uint32_t y = 0; y < h; ++y) {
        const float v = ((float)y + 0.5f) / (float)h;
        const float theta = v * 3.14159265358979323846f;
        const float st = std::sin(theta), ct = std::cos(theta);
        for (uint32_t x = 0; x < w; ++x) {
            const float u = ((float)x + 0.5f) / (float)w;
            const float phi = (u - 0.5f) * 2.0f * 3.14159265358979323846f;
            const float dx = st * std::sin(phi), dy = ct, dz = st * std::cos(phi);
            const float ax = std::fabs(dx), ay = std::fabs(dy), az = std::fabs(dz);
            uint32_t face = 0;
            float sc = 0.0f, tc = 0.0f;
            if (ax >= ay && ax >= az) {
                face = dx >= 0.0f ? 0u : 1u;   // +X / -X
                sc = dx >= 0.0f ? -dz : dz;
                tc = -dy;
            } else if (ay >= az) {
                face = dy >= 0.0f ? 2u : 3u;   // +Y / -Y
                sc = dx;
                tc = dy >= 0.0f ? dz : -dz;
            } else {
                face = dz >= 0.0f ? 4u : 5u;   // +Z / -Z
                sc = dz >= 0.0f ? dx : -dx;
                tc = -dy;
            }
            const float ma = std::max(ax, std::max(ay, az));
            const float inv = ma > 1e-20f ? 1.0f / ma : 0.0f;
            const float fu = (sc * inv + 1.0f) * 0.5f;
            const float fv = (tc * inv + 1.0f) * 0.5f;
            const uint32_t tx = std::min((uint32_t)std::max(0.0f, fu * (float)S), S - 1);
            const uint32_t ty = std::min((uint32_t)std::max(0.0f, fv * (float)S), S - 1);
            const uint8_t* rgba = faces[face].mips[0].pixels.data() + ((size_t)ty * S + tx) * 4;
            px[(size_t)y * w + x] = DecodeTexel(rgba, encoding);
        }
    }
    return true;
}

// True for a DDS whose blocks are BC6H / BC7: the two block formats the RGBA8 decoder cannot expand.
bool DdsIsCompressedOnly(const std::vector<uint8_t>& bytes) {
    if (bytes.empty() || !IsDdsImage(bytes.data(), bytes.size())) return false;
    CompressedImage image;
    if (!LoadDdsCompressed(bytes.data(), bytes.size(), image, nullptr) || image.Empty()) return false;
    return image.format == DXGI_FORMAT_BC6H_UF16 || image.format == DXGI_FORMAT_BC7_UNORM;
}

} // namespace

bool LoadSkyMap(const std::filesystem::path& path, int encoding, std::vector<uint16_t>& halfRgba, uint32_t& w,
                uint32_t& h, DirectX::XMFLOAT3 avg[3], std::string* error) {
    halfRgba.clear();
    w = h = 0;
    for (int i = 0; i < 3; ++i) avg[i] = {0.0f, 0.0f, 0.0f};
    encoding = std::clamp(encoding, 0, 3);
    const std::string ext = ToLowerAscii(PathToUtf8(path.extension()));

    std::vector<DirectX::XMFLOAT3> px;   // linear radiance, row-major
    uint32_t pw = 0, ph = 0;

    if (ext == ".exr") {
        // No vendored EXR decoder: fail loudly rather than render a black sky.
        if (error) *error = "exr is not supported";
        return false;
    }

    if (ext == ".hdr") {
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(path, bytes, error)) return false;
        int iw = 0, ih = 0, comp = 0;
        float* data = stbi_loadf_from_memory(bytes.data(), (int)bytes.size(), &iw, &ih, &comp, 3);
        if (!data || iw <= 0 || ih <= 0) {
            if (data) stbi_image_free(data);
            if (error) *error = "cannot decode the Radiance image";
            return false;
        }
        pw = (uint32_t)iw;
        ph = (uint32_t)ih;
        px.resize((size_t)pw * ph);
        // stbi_loadf already gives linear floats: every encoding is read as linear (there is no 8-bit to curve).
        for (size_t i = 0; i < px.size(); ++i)
            px[i] = {std::max(0.0f, data[i * 3 + 0]), std::max(0.0f, data[i * 3 + 1]),
                     std::max(0.0f, data[i * 3 + 2])};
        stbi_image_free(data);
    } else if (ext == ".dds") {
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(path, bytes, error)) return false;
        if (IsDdsCube(bytes.data(), bytes.size())) {
            std::array<ImageRGBA8, 6> faces;
            std::string ddsError;
            if (!LoadDdsCube(bytes.data(), bytes.size(), faces, &ddsError)) {
                if (error) *error = DdsIsCompressedOnly(bytes) ? "compressed format not supported" : ddsError;
                return false;
            }
            bool anyAlpha = false;
            for (const ImageRGBA8& f : faces) anyAlpha = anyAlpha || f.hasAlpha;
            int enc = encoding;
            if (enc == 0) enc = anyAlpha ? 3 : 2;   // Ray-MMD RGBT domes carry the exponent in the alpha
            if (!CubeToEquirect(faces, enc, px, pw, ph, error)) return false;
        } else {
            ImageRGBA8 image;
            std::string ddsError;
            if (!LoadDdsLevel0(bytes.data(), bytes.size(), image, &ddsError) || image.Empty()) {
                if (error) *error = DdsIsCompressedOnly(bytes) ? "compressed format not supported" : ddsError;
                return false;
            }
            int enc = encoding;
            if (enc == 0) enc = image.hasAlpha ? 3 : 2;
            pw = image.Width();
            ph = image.Height();
            const std::vector<uint8_t>& pixels = image.mips[0].pixels;
            px.resize((size_t)pw * ph);
            for (size_t i = 0; i < px.size(); ++i) px[i] = DecodeTexel(pixels.data() + i * 4, enc);
        }
    } else {
        // .png / .jpg / .jpeg and anything else the image loader knows (8-bit RGBA)
        ImageRGBA8 image;
        std::string loadError;
        if (!LoadImageRGBA8(path, image, &loadError) || image.Empty()) {
            if (error) *error = loadError.empty() ? "cannot decode the image" : loadError;
            return false;
        }
        pw = image.Width();
        ph = image.Height();
        const int enc = encoding == 0 ? 2 : encoding;   // 8-bit images are sRGB
        const std::vector<uint8_t>& pixels = image.mips[0].pixels;
        px.resize((size_t)pw * ph);
        for (size_t i = 0; i < px.size(); ++i) px[i] = DecodeTexel(pixels.data() + i * 4, enc);
    }

    if (pw == 0 || ph == 0 || px.empty()) {
        if (error) *error = "the image has no texels";
        return false;
    }
    DownsampleToBudget(px, pw, ph);
    avg[0] = BandMean(px, pw, ph, 0.10f, 1.0f, false);    // zenith: v < 0.10
    avg[1] = BandMean(px, pw, ph, 0.45f, 0.55f, true);    // horizon: 0.45 < v < 0.55
    avg[2] = BandMean(px, pw, ph, 0.70f, 1.0f, false);    // ground: v > 0.70
    PackHalves(px, halfRgba);
    w = pw;
    h = ph;
    return true;
}

} // namespace mmdx
