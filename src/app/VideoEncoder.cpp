// MP4 writer for the offline video render: H.264 video + AAC audio through Media Foundation
// (IMFSinkWriter, hardware encoders allowed). Frames arrive as RGBA8 images (top-down) and are
// converted to NV12 (BT.709, limited range) on the CPU. The audio track is decoded from the song
// file with miniaudio and written in step with the video, covering exactly the frames written.
#include "core/I18n.h"
#include "app/VideoEncoder.h"
#include "asset/ImageLoader.h"

#include "core/Log.h"
#include "core/TextUtil.h"
#include "miniaudio.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace mmdx {

namespace {
constexpr uint32_t kAudioRate = 48000;
constexpr uint32_t kAudioChannels = 2;
} // namespace

struct VideoEncoder::Impl {
    Desc desc;
    std::filesystem::path path;
    Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
    DWORD videoStream = 0, audioStream = 0;
    bool hasAudio = false;
    bool open = false;
    bool mfStarted = false;
    bool comInit = false;
    DWORD comThread = 0;              // the thread that CoInitializeEx'd (CoUninitialize only balances there)
    uint32_t frames = 0;              // video frames written
    uint64_t audioFrames = 0;         // PCM frames (48 kHz stereo) written
    uint64_t leadSilence = 0;         // PCM frames of silence before the song (negative audioStartSeconds)
    ma_decoder decoder{};
    bool decoderOk = false;
    std::vector<int16_t> pcm;         // scratch
    std::vector<uint8_t> nv12;        // scratch (unused if converting straight into the MF buffer)
};

namespace {

// Creates the sink writer and streams. On failure resets `writer` and returns the failing HRESULT
// (the partially created file is overwritten by the retry).
HRESULT CreateWriter(const std::filesystem::path& path, const VideoEncoder::Desc& desc, bool withAudio,
                     Microsoft::WRL::ComPtr<IMFSinkWriter>& writer,
                     DWORD& videoStream, DWORD& audioStream, bool& hasAudio) {
    HRESULT hr;

    Microsoft::WRL::ComPtr<IMFAttributes> attr;
    hr = MFCreateAttributes(attr.GetAddressOf(), 3);
    if (FAILED(hr)) { writer.Reset(); return hr; }
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attr.Get(), writer.GetAddressOf());
    if (FAILED(hr)) { writer.Reset(); return hr; }

    // Video output type (H.264 High profile, progressive, BT.709 limited range).
    Microsoft::WRL::ComPtr<IMFMediaType> out;
    hr = MFCreateMediaType(out.GetAddressOf());
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AVG_BITRATE, desc.videoBitrate);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, desc.width, desc.height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, desc.fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    if (SUCCEEDED(hr)) hr = writer->AddStream(out.Get(), &videoStream);

    // Video input type: uncompressed NV12 frames of the same geometry and colour space.
    Microsoft::WRL::ComPtr<IMFMediaType> in;
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(in.GetAddressOf());
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, desc.width, desc.height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, desc.fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_DEFAULT_STRIDE, desc.width);
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(videoStream, in.Get(), nullptr);

    // Audio output (AAC 192 kbps) + uncompressed PCM input.
    if (SUCCEEDED(hr) && withAudio) {
        Microsoft::WRL::ComPtr<IMFMediaType> aout;
        hr = MFCreateMediaType(aout.GetAddressOf());
        if (SUCCEEDED(hr)) hr = aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
        if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kAudioChannels);
        if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000); // 192 kbps
        if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        if (SUCCEEDED(hr)) hr = writer->AddStream(aout.Get(), &audioStream);

        Microsoft::WRL::ComPtr<IMFMediaType> ain;
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(ain.GetAddressOf());
        if (SUCCEEDED(hr)) hr = ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
        if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kAudioChannels);
        if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
        if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(audioStream, ain.Get(), nullptr);
    }

    if (FAILED(hr)) { writer.Reset(); return hr; }
    hasAudio = withAudio;
    return S_OK;
}

} // namespace

VideoEncoder::VideoEncoder() : impl_(std::make_unique<Impl>()) {}
VideoEncoder::~VideoEncoder() { Finish(); }

bool VideoEncoder::Open(const std::filesystem::path& mp4Path, const Desc& desc, std::string* error) {
    Impl& impl = *impl_;
    if (impl.open) Finish();

    if (desc.width == 0 || desc.height == 0 || desc.fps == 0 ||
        (desc.width & 1) != 0 || (desc.height & 1) != 0) {
        if (error) *error = Tr("영상 크기가 올바르지 않습니다");
        return false;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    impl.comInit = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE is fine; we just don't uninitialize later
    impl.comThread = impl.comInit ? GetCurrentThreadId() : 0;
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    impl.mfStarted = SUCCEEDED(hr);
    if (FAILED(hr)) {
        if (error) *error = Tr("Media Foundation을 초기화할 수 없습니다");
        if (impl.comInit) { CoUninitialize(); impl.comInit = false; }
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(mp4Path.parent_path(), ec);
    std::filesystem::remove(mp4Path, ec);

    if (!desc.audioPath.empty()) {
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, kAudioChannels, kAudioRate);
        if (ma_decoder_init_file_w(desc.audioPath.c_str(), &cfg, &impl.decoder) == MA_SUCCESS &&
            ma_decoder_seek_to_pcm_frame(
                &impl.decoder,
                (ma_uint64)std::llround(std::max(0.0, desc.audioStartSeconds) * 48000.0)) == MA_SUCCESS) {
            impl.decoderOk = true;
        } else {
            LOG_WARN("video: audio decode failed: %s", PathToUtf8(desc.audioPath).c_str());
        }
    }

    auto cleanup = [&impl]() {
        if (impl.decoderOk) { ma_decoder_uninit(&impl.decoder); impl.decoderOk = false; }
        if (impl.mfStarted) { MFShutdown(); impl.mfStarted = false; }
        if (impl.comInit) { CoUninitialize(); impl.comInit = false; }
    };
    auto failWith = [&](HRESULT err) {
        char buf[128];
        snprintf(buf, sizeof(buf), Tr("MP4 인코더를 만들 수 없습니다 (HRESULT 0x%08X)"), (unsigned)err);
        if (error) *error = buf;
        cleanup();
        return false;
    };

    hr = CreateWriter(mp4Path, desc, impl.decoderOk, impl.writer,
                      impl.videoStream, impl.audioStream, impl.hasAudio);
    if (FAILED(hr) && impl.decoderOk) {
        LOG_WARN("video: AAC stream setup failed (0x%08X), writing without audio", (unsigned)hr);
        ma_decoder_uninit(&impl.decoder);
        impl.decoderOk = false;
        hr = CreateWriter(mp4Path, desc, false, impl.writer,
                          impl.videoStream, impl.audioStream, impl.hasAudio);
    }
    if (FAILED(hr)) return failWith(hr);

    hr = impl.writer->BeginWriting();
    if (FAILED(hr)) return failWith(hr);

    impl.path = mp4Path;
    impl.desc = desc;
    impl.open = true;
    impl.frames = 0;
    impl.audioFrames = 0;
    impl.leadSilence = desc.audioStartSeconds < 0.0 ? (uint64_t)std::llround(-desc.audioStartSeconds * 48000.0) : 0;
    LOG_INFO("video: writing %s (%ux%u @ %u fps, audio %s)", PathToUtf8(mp4Path).c_str(),
             desc.width, desc.height, desc.fps, impl.hasAudio ? "yes" : "no");
    return true;
}

bool VideoEncoder::AddFrame(const ImageRGBA8& frame) {
    Impl& impl = *impl_;
    if (!impl.open) return false;
    if (frame.Width() != impl.desc.width || frame.Height() != impl.desc.height) return false;

    const uint32_t w = impl.desc.width, h = impl.desc.height;
    const size_t ySize = (size_t)w * h, uvSize = ySize / 2;

    HRESULT hr;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
    Microsoft::WRL::ComPtr<IMFSample> sample;

    // Convert RGBA -> NV12 (BT.709 limited range) directly into the MF buffer.
    hr = MFCreateMemoryBuffer((DWORD)(ySize + uvSize), buf.GetAddressOf());
    BYTE* dst = nullptr;
    if (SUCCEEDED(hr)) hr = buf->Lock(&dst, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        const uint8_t* src = frame.mips[0].pixels.data();
        uint8_t* yPlane = dst;
        uint8_t* uvPlane = dst + ySize;
        for (size_t i = 0; i < ySize; ++i, src += 4) {
            const int y = ((47 * src[0] + 157 * src[1] + 16 * src[2] + 128) >> 8) + 16;
            yPlane[i] = (uint8_t)std::clamp(y, 0, 255);
        }
        for (uint32_t cy = 0; cy < h; cy += 2) {
            const uint8_t* row0 = frame.mips[0].pixels.data() + (size_t)cy * w * 4;
            const uint8_t* row1 = row0 + (size_t)w * 4;
            for (uint32_t cx = 0; cx < w; cx += 2) {
                const uint8_t* p00 = row0 + (size_t)cx * 4;
                const uint8_t* p01 = p00 + 4;
                const uint8_t* p10 = row1 + (size_t)cx * 4;
                const uint8_t* p11 = p10 + 4;
                const int r = (p00[0] + p01[0] + p10[0] + p11[0] + 2) / 4;
                const int g = (p00[1] + p01[1] + p10[1] + p11[1] + 2) / 4;
                const int b = (p00[2] + p01[2] + p10[2] + p11[2] + 2) / 4;
                const int u = ((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128;
                const int v = ((112 * r - 102 * g - 10 * b + 128) >> 8) + 128;
                uvPlane[0] = (uint8_t)std::clamp(u, 0, 255);
                uvPlane[1] = (uint8_t)std::clamp(v, 0, 255);
                uvPlane += 2;
            }
        }
        hr = buf->Unlock();
    }
    if (SUCCEEDED(hr)) hr = buf->SetCurrentLength((DWORD)(ySize + uvSize));
    if (SUCCEEDED(hr)) hr = MFCreateSample(sample.GetAddressOf());
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf.Get());
    if (SUCCEEDED(hr)) {
        const LONGLONG time = (LONGLONG)impl.frames * 10'000'000LL / impl.desc.fps;
        const LONGLONG duration = (LONGLONG)(impl.frames + 1) * 10'000'000LL / impl.desc.fps - time;
        hr = sample->SetSampleTime(time);
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    }
    if (SUCCEEDED(hr)) hr = impl.writer->WriteSample(impl.videoStream, sample.Get());
    if (FAILED(hr)) {
        LOG_ERROR("video: WriteSample failed 0x%08X", (unsigned)hr);
        return false;
    }

    // Audio up to the end time of this frame.
    if (impl.hasAudio) {
        const uint64_t target = (uint64_t)std::llround((impl.frames + 1) * 48000.0 / impl.desc.fps);
        const uint64_t need = target - impl.audioFrames;
        if (need > 0) {
            impl.pcm.resize((size_t)need * 2);
            // the song starts later than the video (negative start): silence first
            const uint64_t quiet = impl.audioFrames < impl.leadSilence ? std::min(need, impl.leadSilence - impl.audioFrames) : 0;
            std::memset(impl.pcm.data(), 0, (size_t)quiet * 2 * sizeof(int16_t));
            ma_uint64 read = 0;
            if (need > quiet) ma_decoder_read_pcm_frames(&impl.decoder, impl.pcm.data() + quiet * 2, need - quiet, &read);
            read += quiet;
            if (read < need)
                std::memset(impl.pcm.data() + read * 2, 0, (size_t)(need - read) * 2 * sizeof(int16_t));

            hr = MFCreateMemoryBuffer((DWORD)(need * 4), buf.ReleaseAndGetAddressOf());
            BYTE* adst = nullptr;
            if (SUCCEEDED(hr)) hr = buf->Lock(&adst, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                memcpy(adst, impl.pcm.data(), (size_t)need * 4);
                hr = buf->Unlock();
            }
            if (SUCCEEDED(hr)) hr = buf->SetCurrentLength((DWORD)(need * 4));
            if (SUCCEEDED(hr)) hr = MFCreateSample(sample.ReleaseAndGetAddressOf());
            if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf.Get());
            if (SUCCEEDED(hr)) {
                const LONGLONG time = (LONGLONG)(impl.audioFrames * 10'000'000ULL / 48000ULL);
                const LONGLONG duration = (LONGLONG)(need * 10'000'000ULL / 48000ULL);
                hr = sample->SetSampleTime(time);
                if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
            }
            if (SUCCEEDED(hr)) hr = impl.writer->WriteSample(impl.audioStream, sample.Get());
            if (FAILED(hr)) {
                LOG_ERROR("video: WriteSample failed 0x%08X", (unsigned)hr);
                return false;
            }
            impl.audioFrames = target;
        }
    }

    ++impl.frames;
    return true;
}

bool VideoEncoder::IsOpen() const { return impl_ && impl_->open; }

uint32_t VideoEncoder::FramesWritten() const { return impl_ ? impl_->frames : 0; }

bool VideoEncoder::Finish() {
    if (!impl_) return true;
    Impl& impl = *impl_;
    if (!impl.open) {
        if (impl.decoderOk) { ma_decoder_uninit(&impl.decoder); impl.decoderOk = false; }
        if (impl.mfStarted) { MFShutdown(); impl.mfStarted = false; }
        if (impl.comInit) { CoUninitialize(); impl.comInit = false; }
        return true;
    }
    HRESULT hr = impl.writer->Finalize();
    impl.writer.Reset();
    impl.open = false;
    if (impl.decoderOk) { ma_decoder_uninit(&impl.decoder); impl.decoderOk = false; }
    if (impl.mfStarted) { MFShutdown(); impl.mfStarted = false; }
    if (impl.comInit) { CoUninitialize(); impl.comInit = false; }
    LOG_INFO("video: finished %s (%u frames)", PathToUtf8(impl.path).c_str(), impl.frames);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// AsyncVideoSink: encoding on a worker thread, bounded queue, back-pressure
// ---------------------------------------------------------------------------

struct AsyncVideoSink::Impl {
    std::unique_ptr<VideoEncoder> encoder;
    WriteFn writer;
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable cvPush, cvPop;
    std::deque<std::pair<ImageRGBA8, int>> queue;
    size_t maxQueued = 4;
    bool open = false;
    bool stopping = false;      // Finish: drain the queue, then stop
    bool aborting = false;      // Abort: drop the queue
    bool finished = false;
    bool failed = false;
    std::string error;
    uint32_t written = 0;
    double encodeTotal = 0.0;
    void Run() {
        // Media Foundation is process-wide but the sink writer is used from this thread: keep COM / MF initialised
        // here while frames are written (Open / Finish run on the app thread and balance their own calls).
        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        for (;;) {
            std::pair<ImageRGBA8, int> item;
            {
                std::unique_lock<std::mutex> l(mutex);
                cvPop.wait(l, [&] { return aborting || stopping || !queue.empty(); });
                if (aborting || queue.empty()) break;
                item = std::move(queue.front());
                queue.pop_front();
            }
            cvPush.notify_one();
            const auto begin = std::chrono::steady_clock::now();
            bool ok = false;
            if (writer) {
                ok = writer(item.first, item.second);
            } else if (encoder) {
                ok = encoder->AddFrame(item.first);
            }
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            {
                std::lock_guard<std::mutex> l(mutex);
                encodeTotal += secs;
                if (!ok) {
                    if (!failed) {
                        failed = true;
                        error = "frame write failed";
                    }
                    // drop the rest: a failed encode cannot continue
                    queue.clear();
                    cvPush.notify_all();
                } else {
                    ++written;
                }
            }
        }
        if (SUCCEEDED(mfHr)) MFShutdown();
        if (SUCCEEDED(comHr)) CoUninitialize();
    }
};

AsyncVideoSink::AsyncVideoSink() : impl_(std::make_unique<Impl>()) {}
AsyncVideoSink::~AsyncVideoSink() { Finish(); }

bool AsyncVideoSink::Open(std::unique_ptr<VideoEncoder> encoder, WriteFn writer, size_t maxQueued) {
    Impl& m = *impl_;
    if (m.open) Finish();
    if (!encoder || !encoder->IsOpen()) return false;
    m.encoder = std::move(encoder);
    m.writer = std::move(writer);
    m.maxQueued = std::max<size_t>(1, maxQueued);
    m.queue.clear();
    m.stopping = m.aborting = m.finished = m.failed = false;
    m.error.clear();
    m.written = 0;
    m.encodeTotal = 0.0;
    m.open = true;
    m.worker = std::thread(&Impl::Run, &m);
    return true;
}

bool AsyncVideoSink::Open(WriteFn writer, size_t maxQueued) {
    Impl& m = *impl_;
    if (m.open) Finish();
    if (!writer) return false;
    m.encoder.reset();
    m.writer = std::move(writer);
    m.maxQueued = std::max<size_t>(1, maxQueued);
    m.queue.clear();
    m.stopping = m.aborting = m.finished = m.failed = false;
    m.error.clear();
    m.written = 0;
    m.encodeTotal = 0.0;
    m.open = true;
    m.worker = std::thread(&Impl::Run, &m);
    return true;
}

bool AsyncVideoSink::IsOpen() const { return impl_ && impl_->open; }

bool AsyncVideoSink::Push(ImageRGBA8&& frame, int index) {
    Impl& m = *impl_;
    std::unique_lock<std::mutex> l(m.mutex);
    if (!m.open || m.stopping || m.aborting) return false;
    m.cvPush.wait(l, [&] { return m.failed || m.aborting || m.queue.size() < m.maxQueued; });
    if (m.failed || m.aborting) return false;
    m.queue.emplace_back(std::move(frame), index);
    m.cvPop.notify_one();
    return true;
}

uint32_t AsyncVideoSink::Queued() const {
    std::lock_guard<std::mutex> l(impl_->mutex);
    return (uint32_t)impl_->queue.size();
}

uint32_t AsyncVideoSink::FramesWritten() const {
    std::lock_guard<std::mutex> l(impl_->mutex);
    return impl_->written;
}

double AsyncVideoSink::AverageEncodeSeconds() const {
    std::lock_guard<std::mutex> l(impl_->mutex);
    return impl_->written ? impl_->encodeTotal / impl_->written : 0.0;
}

bool AsyncVideoSink::Failed(std::string* error) const {
    std::lock_guard<std::mutex> l(impl_->mutex);
    if (error) *error = impl_->error;
    return impl_->failed;
}

bool AsyncVideoSink::Finish() {
    Impl& m = *impl_;
    if (!m.open) return !m.failed;
    {
        std::lock_guard<std::mutex> l(m.mutex);
        m.stopping = true;
    }
    m.cvPop.notify_all();
    if (m.worker.joinable()) m.worker.join();
    bool ok = !m.failed;
    if (m.encoder) ok = m.encoder->Finish() && ok;
    m.encoder.reset();
    m.writer = nullptr;
    m.open = false;
    m.finished = true;
    return ok;
}

void AsyncVideoSink::Abort() {
    Impl& m = *impl_;
    if (!m.open) return;
    {
        std::lock_guard<std::mutex> l(m.mutex);
        m.aborting = true;
        m.queue.clear();
    }
    m.cvPop.notify_all();
    m.cvPush.notify_all();
    if (m.worker.joinable()) m.worker.join();
    if (m.encoder) m.encoder->Finish();   // the file stays playable up to the last written frame
    m.encoder.reset();
    m.writer = nullptr;
    m.open = false;
}

} // namespace mmdx
