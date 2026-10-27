#pragma once
// Music playback (miniaudio high-level engine). Supports WAV/MP3/FLAC (+ OGG Vorbis via
// miniaudio's stb_vorbis if available). The playback cursor is the master clock for
// motion sync.
#include <filesystem>
#include <memory>
#include <vector>

namespace mmdx {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool Initialize();                                   // creates the ma_engine
    bool Load(const std::filesystem::path& path);        // unloads previous; false on failure
    void Unload();
    bool IsLoaded() const;
    // Decodes the file into the engine's resource cache from any thread (blocking), so a later Load of the same
    // path on the main thread is instant. Each successful Preload needs one ReleasePreload (after the Load).
    bool Preload(const std::filesystem::path& path);
    void ReleasePreload(const std::filesystem::path& path);

    void Play();
    void Pause();
    bool IsPlaying() const;
    bool AtEnd() const;

    void Seek(double seconds);           // clamped to [0, duration]
    double PositionSeconds() const;      // 0 if not loaded
    double DurationSeconds() const;      // 0 if not loaded/unknown

    void SetVolume(float volume01);      // applied to the engine (persists across Load)
    float Volume() const;
    void SetMuted(bool muted);
    bool Muted() const;

    // The file the current sound was loaded from (empty when nothing is loaded).
    const std::filesystem::path& Path() const;

    // Decodes `path` into `bands` envelope samples (0..1, normalised to the global peak) for the timeline waveform.
    // Runs on any thread and never touches the playing sound (it opens its own decoder), so the UI can decode in the
    // background. Returns false when decoding fails; `out` is resized to `bands` (empty on failure).
    // `durationSeconds` is the loaded track's length.
    static bool BuildPeaks(const std::filesystem::path& path, double durationSeconds, int bands,
                           std::vector<float>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mmdx
