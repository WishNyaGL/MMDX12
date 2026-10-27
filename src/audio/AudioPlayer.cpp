#include "audio/AudioPlayer.h"

#include "miniaudio.h"

#include "core/Log.h"
#include "core/TextUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mmdx {

struct AudioPlayer::Impl {
    ma_engine engine{};
    bool engineOk = false;
    ma_sound sound{};
    bool soundOk = false;
    std::filesystem::path path;
    float volume = 0.8f;
    bool muted = false;
    // After the sound reached its end, miniaudio's at-end flag makes the node stop and ignore seek
    // targets; only ma_sound_start clears it, and that rewinds to 0 while at end. Seek records the
    // target here so the next Play can move the cursor back to it.
    bool restartPending = false;
    ma_uint64 restartFrame = 0;
};

AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {}
AudioPlayer::~AudioPlayer() {
    Unload();
    if (impl_->engineOk) ma_engine_uninit(&impl_->engine);
    impl_->engineOk = false;
}

bool AudioPlayer::Initialize() {
    if (impl_->engineOk) return true;
    if (ma_engine_init(nullptr, &impl_->engine) != MA_SUCCESS) return false;
    impl_->engineOk = true;
    ma_engine_set_volume(&impl_->engine, impl_->muted ? 0 : impl_->volume);
    return true;
}

bool AudioPlayer::Load(const std::filesystem::path& path) {
    Unload();
    if (!impl_->engineOk) return false;
    ma_result r = ma_sound_init_from_file_w(&impl_->engine, path.c_str(),
                                            MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                            nullptr, nullptr, &impl_->sound);
    if (r != MA_SUCCESS) {
        LOG_WARN("audio load failed: %s (%d)", PathToUtf8(path).c_str(), (int)r);
        return false;
    }
    impl_->soundOk = true;
    impl_->path = path;
    return true;
}

bool AudioPlayer::Preload(const std::filesystem::path& path) {
    if (!impl_->engineOk) return false;
    ma_resource_manager* rm = ma_engine_get_resource_manager(&impl_->engine);
    return rm && ma_resource_manager_register_file_w(rm, path.c_str(), MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_DECODE) ==
                     MA_SUCCESS;
}

void AudioPlayer::ReleasePreload(const std::filesystem::path& path) {
    if (!impl_->engineOk) return;
    if (ma_resource_manager* rm = ma_engine_get_resource_manager(&impl_->engine))
        ma_resource_manager_unregister_file_w(rm, path.c_str());
}

void AudioPlayer::Unload() {
    if (impl_->soundOk) ma_sound_uninit(&impl_->sound);
    impl_->soundOk = false;
    impl_->restartPending = false;
    impl_->path.clear();
}

bool AudioPlayer::BuildWaveform(const std::filesystem::path& path, int bucketsPerSecond, std::vector<float>& out) {
    out.clear();
    if (path.empty() || bucketsPerSecond <= 0) return false;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 0);  // mono f32, native rate
    ma_decoder decoder;
    if (ma_decoder_init_file_w(path.c_str(), &cfg, &decoder) != MA_SUCCESS) return false;
    const double sr = decoder.outputSampleRate > 0 ? (double)decoder.outputSampleRate : 44100.0;
    const double framesPerBucket = sr / bucketsPerSecond;
    std::vector<float> buf(8192);
    uint64_t fi = 0;
    size_t bucket = 0;
    float peak = 0.0f;
    double sumSq = 0.0;
    uint64_t count = 0;
    const auto flush = [&] {
        out.push_back(peak);
        out.push_back(count > 0 ? (float)std::sqrt(sumSq / (double)count) : 0.0f);
        peak = 0.0f;
        sumSq = 0.0;
        count = 0;
        ++bucket;
    };
    bool ok = true;
    for (;;) {
        ma_uint64 got = 0;
        const ma_result rr = ma_decoder_read_pcm_frames(&decoder, buf.data(), (ma_uint64)buf.size(), &got);
        if (rr == MA_AT_END) break;  // a length that is an exact multiple of the buffer ends here
        if (rr != MA_SUCCESS) {
            ok = false;  // a decode error is not a normal end of file
            break;
        }
        if (got == 0) break;
        for (ma_uint64 i = 0; i < got; ++i) {
            const float s = buf[(size_t)i];
            const float v = std::fabs(s);
            if (v > peak) peak = v;
            sumSq += (double)s * s;
            ++count;
            ++fi;
            if ((double)fi >= (double)(bucket + 1) * framesPerBucket) flush();
        }
        if (got < buf.size()) break;
    }
    ma_decoder_uninit(&decoder);
    if (!ok) {
        // never hand back a partially decoded waveform as if it were complete
        out.clear();
        return false;
    }
    if (count > 0) flush();
    // normalise both to the global peak
    float gmax = 0.0f;
    for (size_t i = 0; i < out.size(); i += 2) gmax = std::max(gmax, out[i]);
    if (gmax > 1e-6f) for (float& v : out) v /= gmax;
    return !out.empty();
}

bool AudioPlayer::IsLoaded() const { return impl_->soundOk; }

const std::filesystem::path& AudioPlayer::Path() const { return impl_->path; }

void AudioPlayer::Play() {
    if (!impl_->soundOk) return;
    ma_sound_start(&impl_->sound);
    // ma_sound_start rewinds to 0 when the sound was at its end: reapply the seek target recorded by
    // Seek so playback resumes at the requested position.
    if (impl_->restartPending) {
        ma_sound_seek_to_pcm_frame(&impl_->sound, impl_->restartFrame);
        impl_->restartPending = false;
    }
}

void AudioPlayer::Pause() {
    if (impl_->soundOk) ma_sound_stop(&impl_->sound);
}

bool AudioPlayer::IsPlaying() const {
    return impl_->soundOk && ma_sound_is_playing(&impl_->sound) != 0;
}

bool AudioPlayer::AtEnd() const {
    return impl_->soundOk && ma_sound_at_end(&impl_->sound) != 0;
}

void AudioPlayer::Seek(double seconds) {
    if (!impl_->soundOk) return;
    float sampleRate = 0;
    ma_format format = ma_format_unknown;
    ma_uint32 channels = 0;
    ma_uint32 sampleRateU32 = 0;
    if (ma_sound_get_data_format(&impl_->sound, &format, &channels, &sampleRateU32, nullptr, 0) != MA_SUCCESS ||
        sampleRateU32 <= 0)
        return;
    sampleRate = (float)sampleRateU32;
    double duration = DurationSeconds();
    if (seconds < 0) seconds = 0;
    if (duration > 0 && seconds > duration) seconds = duration;
    const ma_uint64 targetFrame = (ma_uint64)(seconds * sampleRate);
    if (ma_sound_at_end(&impl_->sound) != 0) {
        // The sound stopped at its end and ignores seek targets until restarted: stop it and record
        // the target; the next Play clears the ended state and reapplies the position.
        ma_sound_stop(&impl_->sound);
        impl_->restartPending = true;
        impl_->restartFrame = targetFrame;
        return;
    }
    ma_sound_seek_to_pcm_frame(&impl_->sound, targetFrame);
}

double AudioPlayer::PositionSeconds() const {
    if (!impl_->soundOk) return 0;
    float pos = 0;
    ma_sound_get_cursor_in_seconds(&impl_->sound, &pos);
    return pos;
}

double AudioPlayer::DurationSeconds() const {
    if (!impl_->soundOk) return 0;
    float len = 0;
    ma_sound_get_length_in_seconds(&impl_->sound, &len);
    return len;
}

void AudioPlayer::SetVolume(float volume01) {
    impl_->volume = volume01;
    if (impl_->engineOk) ma_engine_set_volume(&impl_->engine, impl_->muted ? 0 : impl_->volume);
}

float AudioPlayer::Volume() const { return impl_->volume; }

void AudioPlayer::SetMuted(bool muted) {
    impl_->muted = muted;
    if (impl_->engineOk) ma_engine_set_volume(&impl_->engine, impl_->muted ? 0 : impl_->volume);
}

bool AudioPlayer::Muted() const { return impl_->muted; }

} // namespace mmdx
