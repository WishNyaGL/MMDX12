// See VideoReader.h. Media Foundation IMFSourceReader for the container formats it handles, a built-in RIFF AVI
// reader for uncompressed rawvideo DIB AVIs (MMD's background-AVI format, which MF has no source for).
#include "asset/VideoReader.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace mmdx {

using Microsoft::WRL::ComPtr;

namespace {

bool IsVideoExt(const std::string& ext) {
    static const char* kExt[] = {".mp4", ".mov", ".wmv", ".avi", ".mkv"};
    for (const char* e : kExt)
        if (ext == e) return true;
    return false;
}

std::string LowerExt(const std::filesystem::path& p) {
    return ToLowerAscii(PathToUtf8(p.extension()));
}

// ---- RIFF AVI (uncompressed DIB) ------------------------------------------------------------------------------

struct AviVideoChunk {
    uint64_t offset = 0;   // file offset of the chunk's payload
    uint32_t size = 0;     // payload bytes
};

// Reads a little-endian uint32 / uint16 / fourcc from a buffer.
uint32_t Rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint32_t Rd16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

struct AviReader {
    std::filesystem::path path;
    std::ifstream file;
    uint32_t width = 0, height = 0;   // height < 0 (as uint32 cast): bottom-up DIB
    bool bottomUp = true;
    uint32_t bitCount = 24;           // 24 or 32
    double fps = 30.0;
    std::vector<AviVideoChunk> frames;

    double Duration() const { return fps > 0 ? (double)frames.size() / fps : 0.0; }

    bool Open(const std::filesystem::path& p, std::string* error) {
        path = p;
        file.open(p, std::ios::binary);
        if (!file) { if (error) *error = "cannot open AVI"; return false; }

        uint8_t hdr[12];
        file.read((char*)hdr, 12);
        if (file.gcount() < 12 || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "AVI ", 4) != 0) {
            if (error) *error = "not a RIFF AVI";
            return false;
        }
        const uint64_t riffEnd = 8 + (uint64_t)Rd32(hdr + 4);

        bool haveVideoStream = false;
        uint32_t totalFrames = 0;
        uint32_t aviFpsMicro = 0;

        // Walk top-level chunks; descend into hdrl (streams) and movi (video chunks).
        uint64_t pos = 12;
        while (pos + 8 <= riffEnd) {
            uint8_t ch[8];
            file.seekg((std::streamoff)pos);
            file.read((char*)ch, 8);
            if (file.gcount() < 8) break;
            const uint32_t size = Rd32(ch + 4);
            if (std::memcmp(ch, "LIST", 4) == 0) {
                uint8_t type[4];
                file.read((char*)type, 4);
                const uint64_t listEnd = pos + 8 + size;
                if (std::memcmp(type, "hdrl", 4) == 0) {
                    ParseHdrl(pos + 12, listEnd, haveVideoStream, totalFrames, aviFpsMicro);
                } else if (std::memcmp(type, "movi", 4) == 0) {
                    ParseMovi(pos + 12, listEnd);
                }
                pos = listEnd + (size & 1);
            } else {
                pos += 8 + size + (size & 1);
            }
        }
        if (!haveVideoStream || frames.empty()) {
            if (error) *error = "AVI: no video stream";
            return false;
        }
        if (aviFpsMicro > 0) fps = 1000000.0 / (double)aviFpsMicro;
        else if (totalFrames > 0 && fps <= 0) fps = 30.0;
        if (fps <= 0.0) fps = 30.0;
        if ((bitCount != 24 && bitCount != 32) || width == 0 || height == 0) {
            if (error) *error = "AVI: unsupported DIB format";
            return false;
        }
        return true;
    }

    // hdrl: find the video stream's strh (fps) and strf (BITMAPINFOHEADER).
    void ParseHdrl(uint64_t pos, uint64_t end, bool& haveVideo, uint32_t& totalFrames, uint32_t& fpsMicro) {
        while (pos + 8 <= end) {
            uint8_t ch[8];
            file.seekg((std::streamoff)pos);
            file.read((char*)ch, 8);
            if (file.gcount() < 8) return;
            const uint32_t size = Rd32(ch + 4);
            if (std::memcmp(ch, "LIST", 4) == 0) {
                uint8_t type[4];
                file.read((char*)type, 4);
                const uint64_t listEnd = pos + 8 + size;
                if (std::memcmp(type, "strl", 4) == 0) ParseStrl(pos + 12, listEnd, haveVideo, totalFrames, fpsMicro);
                pos = listEnd + (size & 1);
            } else if (std::memcmp(ch, "avih", 4) == 0 && size >= 40) {
                uint8_t b[40];
                file.read((char*)b, 40);
                fpsMicro = Rd32(b + 0);        // dwMicroSecPerFrame
                totalFrames = Rd32(b + 16);    // dwTotalFrames
                pos += 8 + size + (size & 1);
            } else {
                pos += 8 + size + (size & 1);
            }
        }
    }

    void ParseStrl(uint64_t pos, uint64_t end, bool& haveVideo, uint32_t& totalFrames, uint32_t& fpsMicro) {
        uint32_t strhSize = 0;
        uint8_t strh[56] = {};
        uint8_t strf[40] = {};
        uint32_t strfSize = 0;
        while (pos + 8 <= end) {
            uint8_t ch[8];
            file.seekg((std::streamoff)pos);
            file.read((char*)ch, 8);
            if (file.gcount() < 8) break;
            const uint32_t size = Rd32(ch + 4);
            if (std::memcmp(ch, "strh", 4) == 0 && size >= 40) {
                strhSize = std::min<uint32_t>(size, sizeof(strh));
                file.read((char*)strh, strhSize);
            } else if (std::memcmp(ch, "strf", 4) == 0 && size >= 40) {
                strfSize = std::min<uint32_t>(size, sizeof(strf));
                file.read((char*)strf, strfSize);
            }
            pos += 8 + size + (size & 1);
        }
        if (strhSize < 40 || strfSize < 40) return;
        // fccType must be 'vids'
        if (std::memcmp(strh, "vids", 4) != 0) return;
        const uint32_t scale = Rd32(strh + 20);
        const uint32_t rate = Rd32(strh + 24);
        const uint32_t length = Rd32(strh + 32);
        if (rate > 0 && scale > 0) fpsMicro = (uint32_t)std::lround(1000000.0 * (double)scale / (double)rate);
        if (length > 0) totalFrames = length;
        // BITMAPINFOHEADER: biWidth, biHeight (signed; negative = top-down), biBitCount, biCompression
        const int32_t bw = (int32_t)Rd32(strf + 4);
        const int32_t bh = (int32_t)Rd32(strf + 8);
        const uint32_t compression = Rd32(strf + 16);
        const uint32_t bits = Rd16(strf + 14);
        if (compression != 0) return;   // 0 = BI_RGB
        width = (uint32_t)std::abs(bw);
        height = (uint32_t)std::abs(bh);
        bottomUp = bh >= 0;
        bitCount = bits;
        haveVideo = true;
    }

    // movi: collect 00db / 00dc (uncompressed DIB) video chunks in order.
    void ParseMovi(uint64_t pos, uint64_t end) {
        while (pos + 8 <= end) {
            uint8_t ch[8];
            file.seekg((std::streamoff)pos);
            file.read((char*)ch, 8);
            if (file.gcount() < 8) return;
            const uint32_t size = Rd32(ch + 4);
            if (std::memcmp(ch, "LIST", 4) == 0) {
                // nested rec lists: descend
                uint8_t type[4];
                file.read((char*)type, 4);
                (void)type;
                ParseMovi(pos + 12, pos + 8 + size);
            } else if ((std::memcmp(ch, "00db", 4) == 0 || std::memcmp(ch, "00dc", 4) == 0) && size > 0) {
                frames.push_back({pos + 8, size});
            }
            pos += 8 + size + (size & 1);
        }
    }

    // Decodes frame `index` (top-down BGRA). BGR24/BGR32 bottom-up -> top-down.
    bool ReadFrame(uint64_t index, std::vector<uint8_t>& bgra) {
        if (index >= frames.size()) return false;
        const AviVideoChunk& c = frames[(size_t)index];
        std::vector<uint8_t> raw(c.size);
        file.seekg((std::streamoff)c.offset);
        file.read((char*)raw.data(), c.size);
        if ((uint32_t)file.gcount() < c.size) return false;

        const uint32_t w = width, h = height;
        const uint32_t srcStride = w * (bitCount / 8);
        bgra.resize((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; ++y) {
            const uint32_t srcRow = bottomUp ? (h - 1 - y) : y;
            const uint8_t* src = raw.data() + (size_t)srcRow * srcStride;
            uint8_t* dst = bgra.data() + (size_t)y * w * 4;
            if (bitCount == 24) {
                for (uint32_t x = 0; x < w; ++x, src += 3, dst += 4) {
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
                }
            } else {   // 32
                for (uint32_t x = 0; x < w; ++x, src += 4, dst += 4) {
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
                }
            }
        }
        return true;
    }
};

// ---- Media Foundation -----------------------------------------------------------------------------------------

struct MfReader {
    ComPtr<IMFSourceReader> reader;
    uint32_t width = 0, height = 0;
    double fps = 30.0, duration = 0.0;
    uint64_t frameCount = 0;
    LONGLONG lastReadTime = -1;   // 100ns time of the last returned sample (-1 = none yet)
    LONGLONG lastReadDur = 0;     // 100ns duration of the last returned sample (0 = unknown)
    ComPtr<IMFSample> lastSample; // last returned sample: re-fill path without re-reading

    bool Open(const std::filesystem::path& path, std::string* error) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comInit = SUCCEEDED(hr);   // RPC_E_CHANGED_MODE fine; do not uninitialize
        hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            if (error) *error = "Media Foundation init failed";
            if (comInit) CoUninitialize();
            return false;
        }
        cleanupCom_ = comInit;
        mfStarted_ = true;

        // Only ENABLE_VIDEO_PROCESSING: asking for the advanced processing transform at the same time makes MF fail
        // to create the source reader for ordinary H.264 mp4 (verified: the plain flag decodes the QA overlay fine).
        ComPtr<IMFAttributes> attr;
        hr = MFCreateAttributes(attr.GetAddressOf(), 1);
        if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        if (SUCCEEDED(hr))
            hr = MFCreateSourceReaderFromURL(path.c_str(), attr.Get(), reader.GetAddressOf());
        if (FAILED(hr)) {
            if (error) {
                char buf[96];
                snprintf(buf, sizeof(buf), "Media Foundation cannot open the file (0x%08X)", (unsigned)hr);
                *error = buf;
            }
            return false;
        }

        // Native type: frame size, frame rate, duration.
        ComPtr<IMFMediaType> native;
        if (SUCCEEDED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, native.GetAddressOf()))) {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
            width = w; height = h;
            UINT32 num = 0, den = 1;
            if (SUCCEEDED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den)) && num > 0)
                fps = (double)num / (double)std::max(1u, den);
        }

        // Request RGB32 output (top-down BGRX).
        ComPtr<IMFMediaType> out;
        if (SUCCEEDED(MFCreateMediaType(out.GetAddressOf()))) {
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (width && height) MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, width, height);
            hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get());
        }
        if (FAILED(hr)) {
            if (error) *error = "Media Foundation cannot decode to RGB32";
            return false;
        }

        // Actual output type (the decoder may adjust the size).
        ComPtr<IMFMediaType> cur;
        if (SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, cur.GetAddressOf()))) {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
            if (w) width = w;
            if (h) height = h;
        }

        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)) &&
            var.vt == VT_UI8) {
            duration = (double)var.uhVal.QuadPart / 10000000.0;
        }
        PropVariantClear(&var);
        if (duration > 0.0 && fps > 0.0) frameCount = (uint64_t)std::llround(duration * fps);
        lastReadTime = -1;
        lastReadDur = 0;
        lastSample.Reset();
        return true;
    }

    ~MfReader() {
        reader.Reset();
        if (mfStarted_) MFShutdown();
        if (cleanupCom_) CoUninitialize();
    }

    // Converts one decoded sample to top-down BGRA with an opaque alpha.
    bool CopyToBgra(IMFSample* s, std::vector<uint8_t>& bgra) {
        if (!s) return false;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(s->ConvertToContiguousBuffer(buf.GetAddressOf()))) return false;
        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buf->Lock(&data, &maxLen, &curLen))) return false;
        const uint32_t srcStride = width * 4;
        if (curLen < (DWORD)(srcStride * height)) {
            buf->Unlock();
            return false;
        }
        bgra.resize((size_t)width * height * 4);
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(bgra.data() + (size_t)y * srcStride, data + (size_t)y * srcStride, srcStride);
            // RGB32 from MF is BGRX: alpha byte is undefined; force opaque (overlay uses black-key on rgb).
            uint8_t* row = bgra.data() + (size_t)y * srcStride;
            for (uint32_t x = 0; x < width; ++x) row[x * 4 + 3] = 255;
        }
        buf->Unlock();
        return true;
    }

    bool ReadFrame(double seconds, std::vector<uint8_t>& bgra) {
        if (!reader || width == 0 || height == 0) return false;
        const double t = std::max(0.0, seconds);
        const LONGLONG target = (LONGLONG)std::llround(t * 10000000.0);
        const LONGLONG nominalDur = (LONGLONG)std::llround(10000000.0 / std::max(1.0, fps));
        const LONGLONG tol = nominalDur / 2;   // half-frame tolerance: float times must not skip the right frame

        // Fast path: the target is still inside the last returned sample's interval -> no new sample, but the
        // frame is copied again (callers reuse and modify their vector, e.g. PassVideo's BGRA -> RGBA swap).
        if (lastReadTime >= 0 && lastSample) {
            const LONGLONG dur = lastReadDur > 0 ? lastReadDur : nominalDur;
            if (target >= lastReadTime - tol && target < lastReadTime + dur) return CopyToBgra(lastSample.Get(), bgra);
        }

        // Seek only when the target is behind the last decoded sample, on the first read, or far ahead
        // (decoding forward within a keyframe interval is cheaper than a seek).
        const bool first = lastReadTime < 0;
        const bool behind = !first && target + tol < lastReadTime;
        const bool farAhead = !first && (target - lastReadTime) > 20000000ll;   // > 2 s
        if (first || behind || farAhead) {
            PROPVARIANT var;
            PropVariantInit(&var);
            var.vt = VT_I8;
            var.hVal.QuadPart = target;
            HRESULT hr = reader->SetCurrentPosition(GUID_NULL, var);
            PropVariantClear(&var);
            if (FAILED(hr)) return false;
            lastReadTime = -1;
            lastReadDur = 0;
            lastSample.Reset();
        }

        // Decode forward and DISCARD samples whose interval ends before the target; only the returned sample is
        // converted. The discard count is bounded (a few seconds at the stream fps) and then the latest sample wins.
        const int maxDiscard = std::max(8, (int)std::ceil(std::max(1.0, fps) * 4.0));
        ComPtr<IMFSample> sample;
        LONGLONG ts = 0, dur = 0;
        for (int i = 0; i <= maxDiscard; ++i) {
            DWORD flags = 0, actualStream = 0;
            sample.Reset();
            HRESULT hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &actualStream, &flags, &ts,
                                            sample.GetAddressOf());
            if (FAILED(hr)) return false;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
            if (!sample || (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) continue;
            dur = 0;
            sample->GetSampleDuration(&dur);
            const LONGLONG d = dur > 0 ? dur : nominalDur;
            if (ts + d > target) break;   // this sample contains the target (or starts after it, right after a seek)
        }
        if (!sample) return false;

        lastReadTime = ts;
        lastReadDur = dur;
        lastSample = sample;
        return CopyToBgra(sample.Get(), bgra);
    }

    ComPtr<IMFSourceReader> readerFor() const { return reader; }

private:
    bool mfStarted_ = false, cleanupCom_ = false;
};

} // namespace

struct VideoReader::Impl {
    bool avi = false;
    AviReader aviReader;
    MfReader mf;
    uint32_t width = 0, height = 0;
    double fps = 30.0, duration = 0.0;
    uint64_t frameCount = 0;
    bool open = false;
};

VideoReader::VideoReader() : impl_(std::make_unique<Impl>()) {}
VideoReader::~VideoReader() = default;

bool VideoReader::IsVideoFile(const std::filesystem::path& path) { return IsVideoExt(LowerExt(path)); }

bool VideoReader::Open(const std::filesystem::path& path, std::string* error) {
    Impl& impl = *impl_;
    Close();
    std::string localError;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        if (error) *error = "file not found";
        return false;
    }
    const std::string ext = LowerExt(path);
    if (!IsVideoExt(ext)) {
        if (error) *error = "unsupported video extension";
        return false;
    }
    if (ext == ".avi") {
        if (!impl.aviReader.Open(path, &localError)) {
            if (error) *error = localError;
            return false;
        }
        impl.avi = true;
        impl.width = impl.aviReader.width;
        impl.height = impl.aviReader.height;
        impl.fps = impl.aviReader.fps;
        impl.duration = impl.aviReader.Duration();
        impl.frameCount = impl.aviReader.frames.size();
    } else {
        if (!impl.mf.Open(path, &localError)) {
            if (error) *error = localError;
            return false;
        }
        impl.avi = false;
        impl.width = impl.mf.width;
        impl.height = impl.mf.height;
        impl.fps = impl.mf.fps;
        impl.duration = impl.mf.duration;
        impl.frameCount = impl.mf.frameCount;
    }
    impl.open = impl.width > 0 && impl.height > 0;
    if (!impl.open) {
        if (error) *error = "video has no usable video stream";
        return false;
    }
    return true;
}

void VideoReader::Close() {
    Impl& impl = *impl_;
    impl.avi = false;
    impl.open = false;
    impl.aviReader = AviReader{};   // closes the file stream
    impl.mf = MfReader{};           // releases the reader, MF shutdown once
    impl.width = impl.height = 0;
    impl.fps = 30.0;
    impl.duration = 0.0;
    impl.frameCount = 0;
}

bool VideoReader::IsOpen() const { return impl_ && impl_->open; }
uint32_t VideoReader::Width() const { return impl_ ? impl_->width : 0; }
uint32_t VideoReader::Height() const { return impl_ ? impl_->height : 0; }
double VideoReader::Fps() const { return impl_ ? impl_->fps : 30.0; }
double VideoReader::Duration() const { return impl_ ? impl_->duration : 0.0; }
uint64_t VideoReader::FrameCount() const { return impl_ ? impl_->frameCount : 0; }

bool VideoReader::ReadFrame(double seconds, std::vector<uint8_t>& bgra) {
    if (!impl_ || !impl_->open) return false;
    Impl& impl = *impl_;
    if (impl.avi) {
        const uint64_t index = impl.fps > 0 ? (uint64_t)std::llround(std::max(0.0, seconds) * impl.fps) : 0;
        return impl.aviReader.ReadFrame(std::min<uint64_t>(index, impl.aviReader.frames.empty()
                                                                       ? 0
                                                                       : impl.aviReader.frames.size() - 1),
                                        bgra);
    }
    return impl.mf.ReadFrame(seconds, bgra);
}

} // namespace mmdx
