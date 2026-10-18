#pragma once
// MP4 writer for the offline video render: H.264 video + AAC audio through Media Foundation
// (IMFSinkWriter, hardware encoders allowed). Frames arrive as RGBA8 images (top-down) and are
// converted to NV12 (BT.709, limited range) on the CPU. The audio track is decoded from the song
// file with miniaudio and written in step with the video, covering exactly the frames written.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace mmdx {

struct ImageRGBA8;

class VideoEncoder {
public:
    struct Desc {
        uint32_t width = 1920, height = 1080;  // even values
        uint32_t fps = 30;
        uint32_t videoBitrate = 40'000'000;    // bits per second (H.264 High profile)
        std::filesystem::path audioPath;       // song file; empty = no audio track
        double audioStartSeconds = 0.0;        // song position of video frame 0 (negative: silence until the song starts)
    };

    VideoEncoder();
    ~VideoEncoder();  // Finish() if still open
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // Creates the file (parent directories too). On failure returns false and sets `error`
    // (Korean, user-facing). An audio file that fails to decode only drops the audio track.
    bool Open(const std::filesystem::path& mp4Path, const Desc& desc, std::string* error);
    bool IsOpen() const;
    // Appends one frame (must be desc.width x desc.height) and the audio up to its end time.
    bool AddFrame(const ImageRGBA8& frame);
    uint32_t FramesWritten() const;
    // Finalizes the file (playable even when the render was cancelled midway). Idempotent.
    bool Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Encodes frames on a worker thread with a small bounded queue (back-pressure: Push blocks while the queue is
// full), so the render thread never waits for Media Foundation. Owns the VideoEncoder, or a callback that writes
// the alpha PNG sequence. Errors are latched and reported through Push / Finish.
class AsyncVideoSink {
public:
    // Writes one frame; `index` is the 0-based frame number the image belongs to. Runs on the worker thread.
    using WriteFn = std::function<bool(const ImageRGBA8&, int index)>;

    AsyncVideoSink();
    ~AsyncVideoSink();  // Finish() when still open
    AsyncVideoSink(const AsyncVideoSink&) = delete;
    AsyncVideoSink& operator=(const AsyncVideoSink&) = delete;

    // Video file: `encoder` must already be Open(). `writer` is optional (null: encoder->AddFrame).
    bool Open(std::unique_ptr<VideoEncoder> encoder, WriteFn writer = {}, size_t maxQueued = 4);
    // Alpha PNG sequence: only the callback.
    bool Open(WriteFn writer, size_t maxQueued = 4);
    bool IsOpen() const;
    // Queues `frame` (moved) for frame number `index`. Blocks while the queue is full (back-pressure).
    // False once the sink failed.
    bool Push(ImageRGBA8&& frame, int index);
    // Frames in the queue (not yet encoded).
    uint32_t Queued() const;
    // Frames the writer has completed.
    uint32_t FramesWritten() const;
    // Mean wall time of one write on the worker thread (seconds).
    double AverageEncodeSeconds() const;
    bool Failed(std::string* error = nullptr) const;
    // Drains the queue, joins the worker and finalizes the encoder. Idempotent; false on a latched error.
    bool Finish();
    // Drops the queued frames, joins the worker and finalizes the encoder (a cancelled render stays playable).
    void Abort();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mmdx
