#pragma once
// Music structure analysis for the studio's AI tools (MCP studio_audio_analysis) and tools/beat_probe: tempo, beat
// grid, downbeats, per-beat energy, onsets and coarse sections of one audio file. Pure CPU code (miniaudio decode +
// DSP), no App / renderer includes. Times are seconds from the start of the file; callers convert to timeline frames
// (frame = (seconds + audioOffset) * 30).
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

struct BeatSection {
    double start = 0, end = 0;   // seconds
    float energy = 0;            // mean normalised RMS over the section, 0..1
    std::string level;           // "low" | "mid" | "high" (energy terciles over the whole song)
};

struct BeatAnalysis {
    bool ok = false;
    std::string error;                 // set when !ok
    double duration = 0;               // seconds
    double bpm = 0;                    // in 70..180 (octave-folded)
    float confidence = 0;              // 0..1: how clearly the tempo stands out
    std::vector<double> beats;         // beat times, seconds, ascending, covering the whole song
    std::vector<double> downbeats;     // bar starts (4/4 assumed), a subset of beats
    std::vector<float> beatEnergy;     // per beat: normalised RMS 0..1 (same size as beats)
    std::vector<double> onsets;        // strong onsets (peaks of the onset envelope), seconds
    std::vector<float> onsetStrength;  // per onset, 0..1 (same size as onsets)
    std::vector<BeatSection> sections; // contiguous, cover [0, duration], each >= 4 bars where possible
};

// Decodes the file (any format miniaudio reads: wav / mp3 / flac / ogg) and analyses it. Never throws.
BeatAnalysis AnalyzeBeats(const std::filesystem::path& audio);

// The same on mono float PCM (used by the self test).
BeatAnalysis AnalyzeBeatsPcm(const std::vector<float>& mono, unsigned sampleRate);

} // namespace mmdx
