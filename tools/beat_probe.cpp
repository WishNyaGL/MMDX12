// beat_probe: command line probe for src/audio/BeatAnalysis (tempo / beats / sections), plus a synthetic self test.
#include "audio/BeatAnalysis.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <Windows.h>

namespace {

constexpr double kPi = 3.14159265358979323846;

void PrintAnalysis(const mmdx::BeatAnalysis& a, bool json) {
    if (json) {
        printf("{\n");
        printf("  \"ok\": %s,\n", a.ok ? "true" : "false");
        if (!a.ok) {
            printf("  \"error\": \"%s\"\n}\n", a.error.c_str());
            return;
        }
        printf("  \"duration\": %.3f,\n", a.duration);
        printf("  \"bpm\": %.3f,\n", a.bpm);
        printf("  \"confidence\": %.4f,\n", a.confidence);
        printf("  \"beats\": %zu,\n", a.beats.size());
        printf("  \"downbeats\": %zu,\n", a.downbeats.size());
        printf("  \"beatTimes\": [");
        const size_t limit = std::min<size_t>(16, a.beats.size());
        for (size_t i = 0; i < limit; ++i) {
            if (i) printf(", ");
            printf("%.3f", a.beats[i]);
        }
        printf("],\n");
        printf("  \"sections\": [");
        for (size_t i = 0; i < a.sections.size(); ++i) {
            const mmdx::BeatSection& s = a.sections[i];
            if (i) printf(", ");
            printf("{\"start\": %.3f, \"end\": %.3f, \"level\": \"%s\", \"energy\": %.3f}",
                   s.start, s.end, s.level.c_str(), s.energy);
        }
        printf("]\n}\n");
        return;
    }
    if (!a.ok) {
        printf("ERROR: %s\n", a.error.c_str());
        return;
    }
    printf("duration: %.3f s\n", a.duration);
    printf("bpm: %.2f\n", a.bpm);
    printf("confidence: %.3f\n", a.confidence);
    printf("beats: %zu\n", a.beats.size());
    printf("downbeats: %zu\n", a.downbeats.size());
    printf("first beats:");
    const size_t limit = std::min<size_t>(16, a.beats.size());
    for (size_t i = 0; i < limit; ++i) printf(" %.3f", a.beats[i]);
    printf("\n");
    printf("sections:\n");
    for (const mmdx::BeatSection& s : a.sections)
        printf("  %.2f-%.2f  %-4s  energy=%.3f\n", s.start, s.end, s.level.c_str(), s.energy);
}

struct ClickTrack {
    std::vector<float> pcm;
    unsigned rate = 44100;
    std::vector<double> beatTimes;
};

ClickTrack MakeClickTrack(double bpm, double seconds, bool energyChange) {
    ClickTrack track;
    track.rate = 44100;
    const size_t total = static_cast<size_t>(seconds * track.rate);
    track.pcm.assign(total, 0.0f);
    const double period = 60.0 / bpm;
    int beat = 0;
    for (double t = 0.0; t + 0.03 < seconds; t += period, ++beat) {
        track.beatTimes.push_back(t);
        const bool downbeat = beat % 4 == 0;
        double amplitude = downbeat ? 0.9 : 0.5;
        if (energyChange && t >= seconds * 0.5) amplitude *= 1.6;
        const size_t start = static_cast<size_t>(t * track.rate);
        const size_t length = static_cast<size_t>(0.03 * track.rate);
        for (size_t i = 0; i < length && start + i < total; ++i) {
            const double x = static_cast<double>(i) / track.rate;
            const double env = std::exp(-x * 90.0);
            const double sample = amplitude * env * std::sin(2.0 * kPi * 1000.0 * x);
            track.pcm[start + i] += static_cast<float>(sample);
        }
    }
    return track;
}

bool CheckTrack(const char* label, double bpm, double seconds, bool energyChange) {
    ClickTrack track = MakeClickTrack(bpm, seconds, energyChange);
    const mmdx::BeatAnalysis a = mmdx::AnalyzeBeatsPcm(track.pcm, track.rate);

    bool ok = true;
    if (!a.ok) {
        printf("[%s] FAIL analysis: %s\n", label, a.error.c_str());
        return false;
    }
    if (std::fabs(a.bpm - bpm) > 1.5) {
        printf("[%s] FAIL bpm %.2f (expected %.2f)\n", label, a.bpm, bpm);
        ok = false;
    }
    int matched = 0;
    for (double expected : track.beatTimes) {
        double best = 1e9;
        for (double got : a.beats) best = std::min(best, std::fabs(got - expected));
        if (best <= 0.05) ++matched;
    }
    const double ratio = track.beatTimes.empty()
                             ? 0.0
                             : static_cast<double>(matched) / static_cast<double>(track.beatTimes.size());
    if (ratio < 0.90) {
        printf("[%s] FAIL matched %.1f%% of synthetic beats\n", label, ratio * 100.0);
        ok = false;
    }

    // Downbeat phase: synthetic downbeats are every 4th beat starting at 0.
    int goodDownbeats = 0;
    for (double d : a.downbeats) {
        const double phase = std::fmod(d, 60.0 / bpm * 4.0);
        if (phase < 0.05 || phase > 60.0 / bpm * 4.0 - 0.05) ++goodDownbeats;
    }
    const double downRatio = a.downbeats.empty()
                                 ? 0.0
                                 : static_cast<double>(goodDownbeats) / static_cast<double>(a.downbeats.size());
    if (downRatio < 0.90) {
        printf("[%s] FAIL downbeat phase %.1f%% correct\n", label, downRatio * 100.0);
        ok = false;
    }

    if (energyChange) {
        const double expected = seconds * 0.5;
        double nearest = 1e9;
        for (const mmdx::BeatSection& s : a.sections) {
            if (s.start > 0.5) nearest = std::min(nearest, std::fabs(s.start - expected));
        }
        const double bar = 60.0 / bpm * 4.0;
        if (nearest > 2.0 * bar) {
            printf("[%s] FAIL no section boundary near %.2f s (nearest %.2f s)\n", label, expected,
                   std::isinf(nearest) ? -1.0 : nearest);
            ok = false;
        }
    }

    printf("[%s] %s bpm=%.2f beats=%zu downbeats=%zu matched=%.1f%% sections=%zu\n",
           label, ok ? "PASS" : "FAIL", a.bpm, a.beats.size(), a.downbeats.size(), ratio * 100.0,
           a.sections.size());
    return ok;
}

int RunSelfTest() {
    bool ok = true;
    ok = CheckTrack("128 BPM", 128.0, 60.0, true) && ok;
    ok = CheckTrack("95 BPM", 95.0, 60.0, true) && ok;
    printf("%s\n", ok ? "SELFTEST PASS" : "SELFTEST FAIL");
    return ok ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        fprintf(stderr, "usage: beat_probe <audio file> [--json]\n       beat_probe --selftest\n");
        return 2;
    }
    const std::wstring first = argv[1];
    if (first == L"--selftest") return RunSelfTest();

    bool json = false;
    std::filesystem::path path;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--json") json = true;
        else path = arg;
    }
    if (path.empty()) {
        fprintf(stderr, "usage: beat_probe <audio file> [--json]\n");
        return 2;
    }

    const mmdx::BeatAnalysis a = mmdx::AnalyzeBeats(path);
    PrintAnalysis(a, json);
    return a.ok ? 0 : 1;
}
