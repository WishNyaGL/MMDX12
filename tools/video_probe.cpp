// video_probe: print what VideoReader sees for a video file and write one frame as PNG (orientation / colour check).
//
//   video_probe <file> [frameIndex] [out.png]
//
// With no frame index, only the header info is printed. `frameIndex` is a zero-based decoded frame
// (the reader's ReadFrame is time-based, so the index is converted with the file's fps).
#include "asset/ImageLoader.h"
#include "asset/VideoReader.h"
#include "core/TextUtil.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace mmdx;

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: video_probe <file> [frameIndex] [out.png]\n");
        return 2;
    }
    const std::filesystem::path file = argv[1];
    VideoReader reader;
    std::string error;
    if (!reader.Open(file, &error)) {
        std::printf("open failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("file     : %s\n", PathToUtf8(file).c_str());
    std::printf("size     : %ux%u\n", reader.Width(), reader.Height());
    std::printf("fps      : %.4f\n", reader.Fps());
    std::printf("duration : %.3f s\n", reader.Duration());
    std::printf("frames   : %llu\n", (unsigned long long)reader.FrameCount());

    if (argc >= 3) {
        const long long index = _wtoi64(argv[2]);
        const double seconds = reader.Fps() > 0 ? (double)index / reader.Fps() : 0.0;
        std::vector<uint8_t> bgra;
        if (!reader.ReadFrame(seconds, bgra)) {
            std::printf("ReadFrame(%.3f s) failed\n", seconds);
            return 1;
        }
        std::printf("frame %lld (%.3f s): %zu bytes\n", index, seconds, bgra.size());
        if (argc >= 4 && reader.Width() && reader.Height()) {
            // The reader returns BGR(A): swap to RGBA for the PNG writer.
            std::vector<uint8_t> rgba(bgra.size());
            for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
                rgba[i] = bgra[i + 2];
                rgba[i + 1] = bgra[i + 1];
                rgba[i + 2] = bgra[i];
                rgba[i + 3] = 255;
            }
            const std::filesystem::path out = argv[3];
            if (SavePngRGBA8(out, reader.Width(), reader.Height(), rgba.data(), reader.Width() * 4))
                std::printf("wrote %s\n", PathToUtf8(out).c_str());
            else
                std::printf("could not write %s\n", PathToUtf8(out).c_str());
        }
    }
    return 0;
}
