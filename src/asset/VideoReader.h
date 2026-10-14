#pragma once
// Video frame reader for the MV packages (docs/mme/mme_design.md §5b). Two backends behind one interface:
//   - Media Foundation IMFSourceReader (mp4/mov/wmv/mkv; H.264/HEVC/WMV, hardware decode allowed) to BGRA;
//   - a small built-in RIFF AVI reader for uncompressed DIB AVIs (rawvideo BI_RGB 24/32 bit -- MMD's
//     background-AVI format, which Media Foundation cannot open).
// Anything else fails cleanly. Frames come back as top-down BGR(A) rows (RGBA8 view: .r = blue, .b = red).
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mmdx {

class VideoReader {
public:
    VideoReader();
    ~VideoReader();
    VideoReader(const VideoReader&) = delete;
    VideoReader& operator=(const VideoReader&) = delete;

    // Opens `path` (header only: cheap) and fills the info accessors. False (with *error) on failure.
    bool Open(const std::filesystem::path& path, std::string* error = nullptr);
    void Close();
    bool IsOpen() const;

    uint32_t Width() const;
    uint32_t Height() const;
    double Fps() const;
    double Duration() const;   // seconds
    uint64_t FrameCount() const;

    // Decodes the frame nearest `seconds` into `bgra` (top-down, row-major, width*height*4 bytes).
    // Sequential reads are cheap; a backward or distant jump seeks. False on failure.
    bool ReadFrame(double seconds, std::vector<uint8_t>& bgra);

    // Extension check used by the library scan (mp4, mov, wmv, avi, mkv).
    static bool IsVideoFile(const std::filesystem::path& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mmdx
