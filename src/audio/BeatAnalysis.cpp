// Music structure analysis: spectral-flux onset envelope, autocorrelation tempo, Ellis-style DP beat tracker,
// low-frequency downbeat phase, onsets and coarse energy/centroid sections. Pure CPU, no App / renderer includes.
#include "audio/BeatAnalysis.h"

#pragma warning(push, 0)
#include "miniaudio.h"
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace mmdx {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Small in-place iterative radix-2 FFT with precomputed twiddles.
class Fft {
public:
    explicit Fft(int n) : n_(n), rev_(static_cast<size_t>(n)), cos_(static_cast<size_t>(n / 2)),
                          sin_(static_cast<size_t>(n / 2)) {
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            unsigned r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1u << (bits - 1 - b);
            rev_[static_cast<size_t>(i)] = r;
        }
        for (int k = 0; k < n / 2; ++k) {
            const double a = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
            cos_[static_cast<size_t>(k)] = static_cast<float>(std::cos(a));
            sin_[static_cast<size_t>(k)] = static_cast<float>(-std::sin(a));
        }
    }

    void forward(float* re, float* im) const {
        for (int i = 0; i < n_; ++i) {
            const int j = static_cast<int>(rev_[static_cast<size_t>(i)]);
            if (j > i) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len / 2;
            const int step = n_ / len;
            for (int base = 0; base < n_; base += len) {
                for (int j = 0; j < half; ++j) {
                    const int k = j * step;
                    const float wr = cos_[static_cast<size_t>(k)];
                    const float wi = sin_[static_cast<size_t>(k)];
                    const int a = base + j;
                    const int b = a + half;
                    const float vr = re[b] * wr - im[b] * wi;
                    const float vi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - vr;
                    im[b] = im[a] - vi;
                    re[a] += vr;
                    im[a] += vi;
                }
            }
        }
    }

private:
    int n_ = 0;
    std::vector<unsigned> rev_;
    std::vector<float> cos_;
    std::vector<float> sin_;
};

double Percentile(std::vector<float>& values, double q) {
    if (values.empty()) return 0.0;
    const size_t k = static_cast<size_t>(std::min<double>(
        static_cast<double>(values.size()) - 1.0,
        std::max(0.0, q * (static_cast<double>(values.size()) - 1.0))));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return static_cast<double>(values[k]);
}

double PercentileCopy(const std::vector<float>& values, double q) {
    std::vector<float> copy = values;
    return Percentile(copy, q);
}

// Resamples with linear interpolation; analysis only needs a low rate.
std::vector<float> Resample(const std::vector<float>& in, unsigned inRate, unsigned outRate) {
    if (in.empty() || inRate == 0 || outRate == 0) return {};
    if (inRate == outRate) return in;
    const double ratio = static_cast<double>(outRate) / static_cast<double>(inRate);
    const size_t outCount = static_cast<size_t>(std::floor(static_cast<double>(in.size()) * ratio));
    std::vector<float> out(outCount);
    const double step = static_cast<double>(inRate) / static_cast<double>(outRate);
    for (size_t i = 0; i < outCount; ++i) {
        const double src = static_cast<double>(i) * step;
        const size_t i0 = static_cast<size_t>(src);
        const size_t i1 = std::min(i0 + 1, in.size() - 1);
        const float frac = static_cast<float>(src - static_cast<double>(i0));
        out[i] = in[i0] * (1.0f - frac) + in[i1] * frac;
    }
    return out;
}

BeatSection MakeSection(double start, double end, float energy, const std::string& level) {
    BeatSection s;
    s.start = start;
    s.end = end;
    s.energy = energy;
    s.level = level;
    return s;
}

void AssignLevels(std::vector<BeatSection>& sections) {
    const size_t n = sections.size();
    if (n == 0) return;
    if (n == 1) {
        sections[0].level = "mid";
        return;
    }
    std::vector<float> energies;
    energies.reserve(n);
    for (const BeatSection& s : sections) energies.push_back(s.energy);
    std::vector<float> sorted = energies;
    std::sort(sorted.begin(), sorted.end());
    if (n == 2) {
        sections[0].level = energies[0] <= energies[1] ? "low" : "high";
        sections[1].level = energies[1] <= energies[0] ? "low" : "high";
        return;
    }
    const float lowCut = sorted[n / 3];
    const float highCut = sorted[(2 * n) / 3];
    for (BeatSection& s : sections) {
        if (s.energy < lowCut) s.level = "low";
        else if (s.energy > highCut) s.level = "high";
        else s.level = "mid";
    }
}

BeatAnalysis EmptyResult(double duration) {
    BeatAnalysis result;
    result.ok = true;
    result.duration = duration;
    result.bpm = 0;
    result.confidence = 0;
    float energy = 0;
    result.sections.push_back(MakeSection(0.0, duration, energy, "mid"));
    return result;
}

} // namespace

BeatAnalysis AnalyzeBeatsPcm(const std::vector<float>& mono, unsigned sampleRate) {
    try {
        if (sampleRate == 0) {
            BeatAnalysis result;
            result.ok = false;
            result.error = "invalid sample rate";
            return result;
        }
        const double duration = static_cast<double>(mono.size()) / static_cast<double>(sampleRate);
        if (mono.empty() || duration < 2.0) return EmptyResult(duration);

        double maxAbs = 0.0;
        for (float v : mono) maxAbs = std::max(maxAbs, std::fabs(static_cast<double>(v)));
        if (maxAbs < 1e-5) return EmptyResult(duration);

        // Analysis rate: about 11025 Hz (keep lower native rates).
        const unsigned analysisRate = std::min<unsigned>(sampleRate, 11025);
        const std::vector<float> sig = Resample(mono, sampleRate, analysisRate);
        if (sig.size() < static_cast<size_t>(analysisRate / 2)) return EmptyResult(duration);

        const int win = 1024;
        const int hop = std::max(64, static_cast<int>(std::lround(static_cast<double>(analysisRate) * 0.010)));
        const double hopSec = static_cast<double>(hop) / static_cast<double>(analysisRate);
        const int frameCount = static_cast<int>(std::floor(
            static_cast<double>(sig.size()) / static_cast<double>(hop))) + 1;
        if (frameCount < 8) return EmptyResult(duration);

        Fft fft(win);
        std::vector<float> window(static_cast<size_t>(win));
        for (int i = 0; i < win; ++i)
            window[static_cast<size_t>(i)] =
                static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                                        static_cast<double>(win - 1)));

        std::vector<float> env(static_cast<size_t>(frameCount), 0.0f);
        std::vector<float> lowEnv(static_cast<size_t>(frameCount), 0.0f);
        std::vector<float> frameRms(static_cast<size_t>(frameCount), 0.0f);
        std::vector<float> frameCentroid(static_cast<size_t>(frameCount), 0.0f);
        std::vector<float> prevLog(static_cast<size_t>(win / 2 + 1), 0.0f);
        std::vector<float> re(static_cast<size_t>(win)), im(static_cast<size_t>(win));
        const int lowBin = std::max(1, static_cast<int>(std::lround(250.0 * win / analysisRate)));
        const int half = win / 2;

        for (int t = 0; t < frameCount; ++t) {
            const int center = t * hop;
            const int start = center - win / 2;
            double sumSq = 0.0;
            int valid = 0;
            for (int i = 0; i < win; ++i) {
                const int idx = start + i;
                float v = 0.0f;
                if (idx >= 0 && idx < static_cast<int>(sig.size())) {
                    v = sig[static_cast<size_t>(idx)];
                    sumSq += static_cast<double>(v) * static_cast<double>(v);
                    ++valid;
                }
                re[static_cast<size_t>(i)] = v * window[static_cast<size_t>(i)];
                im[static_cast<size_t>(i)] = 0.0f;
            }
            frameRms[static_cast<size_t>(t)] =
                valid > 0 ? static_cast<float>(std::sqrt(sumSq / valid)) : 0.0f;

            fft.forward(re.data(), im.data());

            double flux = 0.0;
            double lowFlux = 0.0;
            double magSum = 0.0;
            double centroidNum = 0.0;
            for (int b = 1; b <= half; ++b) {
                const float mr = re[static_cast<size_t>(b)];
                const float mi = im[static_cast<size_t>(b)];
                const float mag = std::sqrt(mr * mr + mi * mi);
                const float logMag = std::log1p(mag);
                const float diff = logMag - prevLog[static_cast<size_t>(b)];
                if (diff > 0.0f) {
                    flux += static_cast<double>(diff);
                    if (b <= lowBin) lowFlux += static_cast<double>(diff);
                }
                prevLog[static_cast<size_t>(b)] = logMag;
                magSum += static_cast<double>(mag);
                centroidNum += static_cast<double>(b) * static_cast<double>(mag);
            }
            env[static_cast<size_t>(t)] = static_cast<float>(flux);
            lowEnv[static_cast<size_t>(t)] = static_cast<float>(lowFlux);
            const double hzPerBin = static_cast<double>(analysisRate) / static_cast<double>(win);
            frameCentroid[static_cast<size_t>(t)] =
                magSum > 1e-12 ? static_cast<float>(centroidNum / magSum * hzPerBin) : 0.0f;
        }

        // Light 3-tap smoothing, then normalise by the envelope's standard deviation.
        std::vector<float> smooth(static_cast<size_t>(frameCount), 0.0f);
        for (int t = 0; t < frameCount; ++t) {
            const float a = env[static_cast<size_t>(std::max(0, t - 1))];
            const float b = env[static_cast<size_t>(t)];
            const float c = env[static_cast<size_t>(std::min(frameCount - 1, t + 1))];
            smooth[static_cast<size_t>(t)] = 0.25f * a + 0.5f * b + 0.25f * c;
        }
        double mean = 0.0;
        for (float v : smooth) mean += v;
        mean /= static_cast<double>(smooth.size());
        double variance = 0.0;
        for (float v : smooth) {
            const double d = static_cast<double>(v) - mean;
            variance += d * d;
        }
        variance /= static_cast<double>(smooth.size());
        const double stddev = std::sqrt(variance);
        if (!(stddev > 1e-6)) return EmptyResult(duration);

        std::vector<float> envNorm(static_cast<size_t>(frameCount));
        for (int t = 0; t < frameCount; ++t)
            envNorm[static_cast<size_t>(t)] = static_cast<float>(
                (static_cast<double>(smooth[static_cast<size_t>(t)]) - mean) / stddev);

        // Tempo: autocorrelation over 60..200 BPM, log-Gaussian prior around 120 BPM.
        const int lagMin = std::max(1, static_cast<int>(std::ceil(60.0 / 200.0 / hopSec)));
        const int lagMax = std::min(frameCount - 2, static_cast<int>(std::floor(60.0 / 60.0 / hopSec)));
        if (lagMax <= lagMin) return EmptyResult(duration);

        const double envMean = mean;
        std::vector<double> ac(static_cast<size_t>(lagMax + 1), 0.0);
        for (int lag = lagMin; lag <= lagMax; ++lag) {
            double sum = 0.0;
            const int count = frameCount - lag;
            for (int t = lag; t < frameCount; ++t) {
                sum += (static_cast<double>(envNorm[static_cast<size_t>(t)])) *
                       (static_cast<double>(envNorm[static_cast<size_t>(t - lag)]));
            }
            ac[static_cast<size_t>(lag)] = count > 0 ? sum / static_cast<double>(count) : 0.0;
        }
        double acMean = 0.0;
        int acCount = 0;
        for (int lag = lagMin; lag <= lagMax; ++lag) {
            acMean += ac[static_cast<size_t>(lag)];
            ++acCount;
        }
        acMean /= std::max(1, acCount);

        int bestLag = lagMin;
        double bestScore = -std::numeric_limits<double>::infinity();
        for (int lag = lagMin; lag <= lagMax; ++lag) {
            const double bpm = 60.0 / (static_cast<double>(lag) * hopSec);
            const double octaves = std::log2(bpm / 120.0) / 0.55;
            const double prior = std::exp(-0.5 * octaves * octaves);
            const double score = ac[static_cast<size_t>(lag)] * prior;
            if (score > bestScore) {
                bestScore = score;
                bestLag = lag;
            }
        }
        const double acPeak = ac[static_cast<size_t>(bestLag)];
        float confidence = 0.0f;
        if (acPeak > 0.0) {
            confidence = static_cast<float>(std::clamp((acPeak - acMean) / acPeak, 0.0, 1.0));
        }

        double bpm = 60.0 / (static_cast<double>(bestLag) * hopSec);
        while (bpm > 180.0) bpm *= 0.5;
        while (bpm < 70.0) bpm *= 2.0;
        const double period = 60.0 / (bpm * hopSec);

        // Beat tracker (Ellis 2007): score = onset + max(prev - penalty(log period deviation)).
        const double tightness = 25.0;
        std::vector<double> dp(static_cast<size_t>(frameCount), 0.0);
        std::vector<int> back(static_cast<size_t>(frameCount), -1);
        const int lagLo = std::max(1, static_cast<int>(std::floor(period * 0.75)));
        const int lagHi = std::max(lagLo + 1, static_cast<int>(std::ceil(period * 1.25)));
        for (int t = 0; t < frameCount; ++t) {
            double best = 0.0;
            int bestPrev = -1;
            const int lo = std::max(0, t - lagHi);
            const int hi = t - lagLo;
            for (int prev = lo; prev <= hi; ++prev) {
                const double tau = static_cast<double>(t - prev);
                const double rel = std::log(tau / period);
                const double value = dp[static_cast<size_t>(prev)] - tightness * rel * rel;
                if (value > best) {
                    best = value;
                    bestPrev = prev;
                }
            }
            dp[static_cast<size_t>(t)] = best + static_cast<double>(envNorm[static_cast<size_t>(t)]);
            back[static_cast<size_t>(t)] = bestPrev;
        }

        int endFrame = 0;
        for (int t = 1; t < frameCount; ++t)
            if (dp[static_cast<size_t>(t)] > dp[static_cast<size_t>(endFrame)]) endFrame = t;
        std::vector<int> beatFrames;
        for (int t = endFrame; t >= 0; t = back[static_cast<size_t>(t)]) {
            beatFrames.push_back(t);
            if (back[static_cast<size_t>(t)] < 0) break;
        }
        std::reverse(beatFrames.begin(), beatFrames.end());

        BeatAnalysis result;
        result.ok = true;
        result.duration = duration;
        result.bpm = bpm;
        result.confidence = confidence;
        for (int f : beatFrames) {
            const double time = static_cast<double>(f) * hopSec;
            if (time >= 0.0 && time < duration) result.beats.push_back(time);
        }
        if (result.beats.size() < 2) {
            result.beats.clear();
            result.sections.push_back(MakeSection(0.0, duration, 0.0f, "mid"));
            return result;
        }

        // Downbeat phase: strongest low-frequency activity on every fourth beat.
        int bestPhase = 0;
        double bestPhaseScore = -1.0;
        for (int phase = 0; phase < 4; ++phase) {
            double score = 0.0;
            int count = 0;
            for (size_t i = static_cast<size_t>(phase); i < result.beats.size(); i += 4) {
                const int f = static_cast<int>(std::lround(result.beats[i] / hopSec));
                double local = 0.0;
                for (int d = -2; d <= 2; ++d) {
                    const int idx = f + d;
                    if (idx >= 0 && idx < frameCount)
                        local = std::max(local, static_cast<double>(lowEnv[static_cast<size_t>(idx)]));
                }
                score += local;
                ++count;
            }
            if (count > 0) score /= static_cast<double>(count);
            if (phase == 0 || score > bestPhaseScore) {
                bestPhaseScore = score;
                bestPhase = phase;
            }
        }
        for (size_t i = static_cast<size_t>(bestPhase); i < result.beats.size(); i += 4)
            result.downbeats.push_back(result.beats[i]);

        // Per-beat RMS between consecutive beats, normalised by the 95th percentile.
        std::vector<float> beatRms(result.beats.size(), 0.0f);
        for (size_t i = 0; i < result.beats.size(); ++i) {
            const double start = result.beats[i];
            const double end = i + 1 < result.beats.size() ? result.beats[i + 1]
                                                           : std::min(duration, start + period * hopSec);
            const size_t i0 = std::min(sig.size(), static_cast<size_t>(std::max(0.0, start * analysisRate)));
            const size_t i1 = std::min(sig.size(), static_cast<size_t>(std::max(0.0, end * analysisRate)));
            double sum = 0.0;
            for (size_t k = i0; k < i1; ++k) sum += static_cast<double>(sig[k]) * sig[k];
            beatRms[i] = i1 > i0 ? static_cast<float>(std::sqrt(sum / static_cast<double>(i1 - i0))) : 0.0f;
        }
        const double beatP95 = std::max(1e-9, PercentileCopy(beatRms, 0.95));
        result.beatEnergy.resize(result.beats.size());
        for (size_t i = 0; i < result.beats.size(); ++i) {
            result.beatEnergy[i] = static_cast<float>(
                std::clamp(static_cast<double>(beatRms[i]) / beatP95, 0.0, 1.0));
        }

        // Strong onset peaks above local mean + k * std, at least 80 ms apart.
        struct Peak { double time; float strength; };
        std::vector<Peak> peaks;
        const int localHalf = std::max(5, static_cast<int>(std::lround(0.5 / hopSec)));
        for (int t = 1; t + 1 < frameCount; ++t) {
            const int lo = std::max(0, t - localHalf);
            const int hi = std::min(frameCount - 1, t + localHalf);
            double m = 0.0;
            for (int i = lo; i <= hi; ++i) m += static_cast<double>(envNorm[static_cast<size_t>(i)]);
            m /= static_cast<double>(hi - lo + 1);
            double var = 0.0;
            for (int i = lo; i <= hi; ++i) {
                const double d = static_cast<double>(envNorm[static_cast<size_t>(i)]) - m;
                var += d * d;
            }
            var /= static_cast<double>(hi - lo + 1);
            const double threshold = m + 1.0 * std::sqrt(var);
            const float v = envNorm[static_cast<size_t>(t)];
            if (v > threshold && v >= envNorm[static_cast<size_t>(t - 1)] &&
                v > envNorm[static_cast<size_t>(t + 1)]) {
                peaks.push_back({static_cast<double>(t) * hopSec, v});
            }
        }
        std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) {
            return a.strength > b.strength;
        });
        std::vector<Peak> kept;
        for (const Peak& p : peaks) {
            bool farEnough = true;
            for (const Peak& k : kept) {
                if (std::fabs(k.time - p.time) < 0.08) {
                    farEnough = false;
                    break;
                }
            }
            if (!farEnough) continue;
            kept.push_back(p);
            if (kept.size() >= 2000) break;
        }
        std::sort(kept.begin(), kept.end(), [](const Peak& a, const Peak& b) { return a.time < b.time; });
        std::vector<float> strengths;
        for (const Peak& p : kept) strengths.push_back(p.strength);
        const double onsetP95 = std::max(1e-9, PercentileCopy(strengths, 0.95));
        for (const Peak& p : kept) {
            result.onsets.push_back(p.time);
            result.onsetStrength.push_back(static_cast<float>(
                std::clamp(static_cast<double>(p.strength) / onsetP95, 0.0, 1.0)));
        }

        // Sections: bar boundaries from downbeats, novelty on smoothed per-bar RMS + centroid.
        std::vector<double> bounds;
        bounds.push_back(0.0);
        for (double d : result.downbeats) {
            if (d > bounds.back() + 1e-3 && d < duration - 1e-3) bounds.push_back(d);
        }
        if (duration > bounds.back() + 1e-3) bounds.push_back(duration);
        const int barCount = static_cast<int>(bounds.size()) - 1;

        std::vector<float> sectionEnergies;
        if (barCount >= 8) {
            std::vector<double> barSum(static_cast<size_t>(barCount), 0.0);
            std::vector<double> barCentSum(static_cast<size_t>(barCount), 0.0);
            std::vector<int> barFrames(static_cast<size_t>(barCount), 0);
            for (int t = 0; t < frameCount; ++t) {
                const double time = static_cast<double>(t) * hopSec;
                auto it = std::upper_bound(bounds.begin(), bounds.end(), time);
                int bar = static_cast<int>(it - bounds.begin()) - 1;
                bar = std::max(0, std::min(barCount - 1, bar));
                barSum[static_cast<size_t>(bar)] += frameRms[static_cast<size_t>(t)];
                barCentSum[static_cast<size_t>(bar)] += frameCentroid[static_cast<size_t>(t)];
                ++barFrames[static_cast<size_t>(bar)];
            }
            std::vector<double> barEnergy(static_cast<size_t>(barCount), 0.0);
            std::vector<double> barCentroid(static_cast<size_t>(barCount), 0.0);
            for (int b = 0; b < barCount; ++b) {
                if (barFrames[static_cast<size_t>(b)] > 0) {
                    barEnergy[static_cast<size_t>(b)] =
                        barSum[static_cast<size_t>(b)] / barFrames[static_cast<size_t>(b)];
                    barCentroid[static_cast<size_t>(b)] =
                        barCentSum[static_cast<size_t>(b)] / barFrames[static_cast<size_t>(b)];
                }
            }
            std::vector<double> smoothEnergy(static_cast<size_t>(barCount), 0.0);
            std::vector<double> smoothCentroid(static_cast<size_t>(barCount), 0.0);
            for (int b = 0; b < barCount; ++b) {
                double e = 0.0, c = 0.0;
                int n = 0;
                for (int d = -1; d <= 1; ++d) {
                    const int i = std::max(0, std::min(barCount - 1, b + d));
                    e += barEnergy[static_cast<size_t>(i)];
                    c += barCentroid[static_cast<size_t>(i)];
                    ++n;
                }
                smoothEnergy[static_cast<size_t>(b)] = e / n;
                smoothCentroid[static_cast<size_t>(b)] = c / n;
            }

            std::vector<double> novelty(static_cast<size_t>(barCount), 0.0);
            double sumE = 0.0, sumC = 0.0;
            int count = 0;
            for (int b = 1; b < barCount; ++b) {
                const double de = smoothEnergy[static_cast<size_t>(b)] - smoothEnergy[static_cast<size_t>(b - 1)];
                const double dc = smoothCentroid[static_cast<size_t>(b)] - smoothCentroid[static_cast<size_t>(b - 1)];
                novelty[static_cast<size_t>(b)] = std::fabs(de) + 0.5 * std::fabs(dc);
                sumE += std::fabs(de);
                sumC += std::fabs(dc);
                ++count;
            }
            const double meanE = count > 0 ? sumE / count : 0.0;
            const double meanC = count > 0 ? sumC / count : 0.0;
            double varE = 0.0, varC = 0.0;
            for (int b = 1; b < barCount; ++b) {
                const double de = std::fabs(smoothEnergy[static_cast<size_t>(b)] - smoothEnergy[static_cast<size_t>(b - 1)]);
                const double dc = std::fabs(smoothCentroid[static_cast<size_t>(b)] - smoothCentroid[static_cast<size_t>(b - 1)]);
                varE += (de - meanE) * (de - meanE);
                varC += (dc - meanC) * (dc - meanC);
            }
            const double stdE = std::sqrt(varE / std::max(1, count));
            const double stdC = std::sqrt(varC / std::max(1, count));
            double maxNovelty = 0.0;
            for (int b = 1; b < barCount; ++b) {
                const double de = std::fabs(smoothEnergy[static_cast<size_t>(b)] - smoothEnergy[static_cast<size_t>(b - 1)]);
                const double dc = std::fabs(smoothCentroid[static_cast<size_t>(b)] - smoothCentroid[static_cast<size_t>(b - 1)]);
                const double e = stdE > 1e-12 ? de / stdE : 0.0;
                const double c = stdC > 1e-12 ? dc / stdC : 0.0;
                novelty[static_cast<size_t>(b)] = e + 0.5 * c;
                maxNovelty = std::max(maxNovelty, novelty[static_cast<size_t>(b)]);
            }

            std::vector<int> candidates;
            for (int b = 1; b < barCount; ++b) candidates.push_back(b);
            std::sort(candidates.begin(), candidates.end(), [&](int a, int b) {
                return novelty[static_cast<size_t>(a)] > novelty[static_cast<size_t>(b)];
            });
            std::vector<int> cuts;
            if (maxNovelty > 1e-6) {
                const double threshold = 0.2 * maxNovelty;
                for (int b : candidates) {
                    if (novelty[static_cast<size_t>(b)] < threshold) continue;
                    if (b < 4 || barCount - b < 4) continue;
                    bool ok = true;
                    for (int c : cuts) {
                        if (std::abs(c - b) < 4) {
                            ok = false;
                            break;
                        }
                    }
                    if (ok) cuts.push_back(b);
                }
            }
            std::sort(cuts.begin(), cuts.end());
            std::vector<int> edges;
            edges.push_back(0);
            for (int c : cuts) edges.push_back(c);
            edges.push_back(barCount);

            const double frameP95 = std::max(1e-9, PercentileCopy(frameRms, 0.95));
            for (size_t i = 0; i + 1 < edges.size(); ++i) {
                const double start = bounds[static_cast<size_t>(edges[i])];
                const double end = bounds[static_cast<size_t>(edges[i + 1])];
                double sum = 0.0;
                int n = 0;
                for (int t = 0; t < frameCount; ++t) {
                    const double time = static_cast<double>(t) * hopSec;
                    if (time >= start && time < end) {
                        sum += static_cast<double>(frameRms[static_cast<size_t>(t)]) / frameP95;
                        ++n;
                    }
                }
                const float energy = static_cast<float>(
                    n > 0 ? std::clamp(sum / n, 0.0, 1.0) : 0.0);
                sectionEnergies.push_back(energy);
                result.sections.push_back(MakeSection(start, end, energy, "mid"));
            }
        }

        if (result.sections.empty()) {
            const double frameP95 = std::max(1e-9, PercentileCopy(frameRms, 0.95));
            double sum = 0.0;
            for (float v : frameRms) sum += static_cast<double>(v) / frameP95;
            const float energy = static_cast<float>(
                std::clamp(sum / static_cast<double>(frameRms.size()), 0.0, 1.0));
            result.sections.push_back(MakeSection(0.0, duration, energy, "mid"));
        }
        AssignLevels(result.sections);
        return result;
    } catch (const std::exception& e) {
        BeatAnalysis result;
        result.ok = false;
        result.error = std::string("beat analysis failed: ") + e.what();
        return result;
    } catch (...) {
        BeatAnalysis result;
        result.ok = false;
        result.error = "beat analysis failed";
        return result;
    }
}

BeatAnalysis AnalyzeBeats(const std::filesystem::path& audio) {
    try {
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        ma_decoder decoder;
        if (ma_decoder_init_file_w(audio.c_str(), &config, &decoder) != MA_SUCCESS) {
            BeatAnalysis result;
            result.ok = false;
            result.error = "failed to open audio: " + audio.string();
            return result;
        }

        const unsigned channels = std::max(1u, decoder.outputChannels);
        const unsigned rate = decoder.outputSampleRate;
        std::vector<float> mono;
        ma_uint64 totalFrames = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames) == MA_SUCCESS && rate > 0) {
            mono.reserve(static_cast<size_t>(totalFrames));
        }

        constexpr ma_uint64 kChunkFrames = 8192;
        std::vector<float> chunk(static_cast<size_t>(kChunkFrames * channels));
        while (true) {
            ma_uint64 read = 0;
            const ma_result status = ma_decoder_read_pcm_frames(
                &decoder, chunk.data(), kChunkFrames, &read);
            if (read == 0) break;
            for (ma_uint64 f = 0; f < read; ++f) {
                float sum = 0.0f;
                for (unsigned c = 0; c < channels; ++c)
                    sum += chunk[static_cast<size_t>(f * channels + c)];
                mono.push_back(sum / static_cast<float>(channels));
            }
            if (status != MA_SUCCESS) break;
        }
        ma_decoder_uninit(&decoder);

        if (rate == 0) {
            BeatAnalysis result;
            result.ok = false;
            result.error = "invalid sample rate";
            return result;
        }
        return AnalyzeBeatsPcm(mono, rate);
    } catch (const std::exception& e) {
        BeatAnalysis result;
        result.ok = false;
        result.error = std::string("audio decode failed: ") + e.what();
        return result;
    } catch (...) {
        BeatAnalysis result;
        result.ok = false;
        result.error = "audio decode failed";
        return result;
    }
}

} // namespace mmdx
