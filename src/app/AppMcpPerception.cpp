// Perception MCP tools for AI directors: a contact sheet rendered through the studio's real renderer, and lip-sync
// phrase / breath / held-note analysis from motion morph tracks.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include "asset/ImageLoader.h"
#include "core/TextUtil.h"
#include "studio/StudioDoc.h"
#include "studio/StudioMotion.h"

namespace mmdx {

using namespace studio;

namespace {

constexpr int kContactGutter = 2;

double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

// 5x7 bitmap font: digits, ':', '.', ' '.
const char* GlyphRows(char c) {
    switch (c) {
    case '0': return "01110" "10001" "10001" "10001" "10001" "10001" "01110";
    case '1': return "00100" "01100" "00100" "00100" "00100" "00100" "01110";
    case '2': return "01110" "10001" "00001" "00010" "00100" "01000" "11111";
    case '3': return "11111" "00010" "00100" "00010" "00001" "10001" "01110";
    case '4': return "00010" "00110" "01010" "10010" "11111" "00010" "00010";
    case '5': return "11111" "10000" "11110" "00001" "00001" "10001" "01110";
    case '6': return "00110" "01000" "10000" "11110" "10001" "10001" "01110";
    case '7': return "11111" "00001" "00010" "00100" "01000" "01000" "01000";
    case '8': return "01110" "10001" "10001" "01110" "10001" "10001" "01110";
    case '9': return "01110" "10001" "10001" "01111" "00001" "00010" "01100";
    case ':': return "00000" "00100" "00100" "00000" "00100" "00100" "00000";
    case '.': return "00000" "00000" "00000" "00000" "00000" "00110" "00110";
    default: return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
    }
}

void FillRect(std::vector<uint8_t>& px, int w, int h, int x0, int y0, int rw, int rh,
              uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = std::max(0, y0); y < std::min(h, y0 + rh); ++y) {
        for (int x = std::max(0, x0); x < std::min(w, x0 + rw); ++x) {
            uint8_t* p = &px[((size_t)y * (size_t)w + (size_t)x) * 4];
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = a;
        }
    }
}

void DrawText(std::vector<uint8_t>& px, int w, int h, int x0, int y0, int scale, const std::string& text) {
    int x = x0;
    for (char c : text) {
        const char* rows = GlyphRows(c);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (rows[row * 5 + col] == '1')
                    FillRect(px, w, h, x + col * scale, y0 + row * scale, scale, scale, 255, 255, 255, 255);
            }
        }
        x += 6 * scale;
    }
}

std::string FrameLabel(int frame) {
    int total = std::max(0, frame);
    const int ff = total % 30;
    const int secs = total / 30;
    const int ss = secs % 60;
    const int mm = secs / 60;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d %02d:%02d.%02d", frame, mm, ss, ff);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// studio_contact_sheet: one tile per rendered frame, advanced once per frame
// ---------------------------------------------------------------------------

bool App::ExecuteMcpPerception(const std::string& tool, const nlohmann::json& args,
                               std::shared_ptr<McpPromise> promise) {
    if (!promise) return false;
    const bool isContact = tool == "studio_contact_sheet";
    const bool isSinging = tool == "studio_singing";
    if (!isContact && !isSinging) return false;
    if (!studio_) {
        promise->Reject("Studio is not active");
        return true;
    }
    StudioDoc& d = *studio_;

    if (isContact) {
        if (pendingMcpContactSheet_ && pendingMcpContactSheet_->active) {
            promise->Reject("A contact sheet is already running");
            return true;
        }

        std::vector<int> frames;
        if (args.contains("frames")) {
            if (!args["frames"].is_array()) {
                promise->Reject("frames must be an array of frame numbers");
                return true;
            }
            for (const auto& f : args["frames"]) frames.push_back(f.get<int>());
        } else {
            int start = args.value("start", 0);
            int end = args.value("end", d.EndFrame());
            if (end < start) std::swap(start, end);
            start = std::max(0, start);
            end = std::max(start, end);
            if (args.contains("step")) {
                int step = args.value("step", 1);
                if (step < 1) step = 1;
                for (int f = start; f <= end; f += step) frames.push_back(f);
            } else {
                int count = args.value("count", 12);
                if (count < 1) count = 1;
                if (count == 1) {
                    frames.push_back(start);
                } else {
                    for (int i = 0; i < count; ++i)
                        frames.push_back(start + (int)std::llround((double)(end - start) * i / (count - 1)));
                }
            }
        }
        for (int& f : frames) f = std::max(0, f);
        if (frames.empty()) {
            promise->Reject("No frames selected");
            return true;
        }
        if (frames.size() > 48) {
            promise->Reject("Too many tiles: " + std::to_string(frames.size()) + " (max_tiles 48)");
            return true;
        }

        PendingMcpContactSheet st;
        st.active = true;
        st.promise = promise;
        st.frames = std::move(frames);
        st.next = 0;
        st.columns = std::max(1, args.value("columns", 4));
        st.tileWidth = std::clamp(args.value("tile_width", 320), 96, 640);
        st.settleFrames = std::max(0, args.value("settle_frames", 4));
        st.path = args.value("path", "");
        st.returnImage = st.path.empty() || args.value("return_image", false);
        st.savedPlaying = d.playing;
        st.savedFrame = d.Frame();
        st.savedUseMotionCamera = d.useMotionCamera;
        st.savedPhysics = d.physics;
        st.tiles = nlohmann::json::array();

        StudioSetPlaying(false);
        if (!d.SelectedCameraTrack().camera.empty()) d.useMotionCamera = true;
        if (!args.value("physics", true)) {
            d.physics = false;
            d.physicsFrame = -1.0f;
        }
        StudioSeek(st.frames[0] / 30.0);
        d.physicsFrame = -1.0f;
        // +1: AdvanceMcpContactSheet also runs in this same PumpMcp call
        st.waitFrames = st.settleFrames + 1;
        pendingMcpContactSheet_ = std::move(st);
        return true;
    }

    // ---- studio_singing ---------------------------------------------------
    std::vector<int> models;
    if (args.contains("models")) {
        if (!args["models"].is_array()) {
            promise->Reject("models must be an array of model indices");
            return true;
        }
        for (const auto& m : args["models"]) models.push_back(m.get<int>());
    } else {
        for (size_t i = 0; i < d.models.size(); ++i)
            if (d.models[i]->kind == ModelKind::Character) models.push_back((int)i);
    }
    for (int m : models) {
        if (m < 0 || m >= (int)d.models.size()) {
            promise->Reject("Model index out of range: " + std::to_string(m));
            return true;
        }
        if (d.models[(size_t)m]->kind != ModelKind::Character) {
            promise->Reject("studio_singing takes characters only (model " + std::to_string(m) + ")");
            return true;
        }
    }

    int start = args.value("start", 0);
    int end = args.value("end", d.EndFrame());
    if (end < start) std::swap(start, end);
    start = std::max(0, start);
    end = std::max(start, end);
    const float threshold = std::max(0.0f, args.value("threshold", 0.12f));
    const int totalFrames = end - start + 1;

    const std::string vowels[] = {
        "\xE3\x81\x82",  // あ
        "\xE3\x81\x84",  // い
        "\xE3\x81\x86",  // う
        "\xE3\x81\x88",  // え
        "\xE3\x81\x8A",  // お
        "\xE3\x83\xAF",  // ワ
        "\xE2\x88\xA7",  // ∧
        "\xCF\x89",      // ω
        "\xE2\x96\xB2",  // ▲
        "\xE2\x96\xA1",  // □
    };

    nlohmann::json characters = nlohmann::json::array();
    std::vector<std::vector<char>> inPhrase(models.size());
    for (size_t mi = 0; mi < models.size(); ++mi) {
        StudioModel& m = *d.models[(size_t)models[mi]];
        const MotionData& motion = m.motion;
        inPhrase[mi].assign((size_t)totalFrames, 0);

        std::vector<std::string> mouthNames;
        for (const auto& [name, keys] : motion.morphs) {
            if (keys.empty()) continue;
            if (name.find("\xE3\x82\x93") != std::string::npos) continue;  // ん = closed
            for (const std::string& v : vowels)
                if (name.find(v) != std::string::npos) {
                    mouthNames.push_back(name);
                    break;
                }
        }
        const bool hasLipsync = !mouthNames.empty();

        std::vector<float> mouth((size_t)totalFrames, 0.0f);
        std::vector<std::vector<float>> vowelTracks(std::size(vowels));
        for (auto& t : vowelTracks) t.assign((size_t)totalFrames, 0.0f);
        std::vector<char> vowelFound(vowelTracks.size(), 0);
        for (size_t vi = 0; vi < vowelTracks.size(); ++vi) {
            for (const auto& [name, keys] : motion.morphs) {
                if (keys.empty() || name.find("\xE3\x82\x93") != std::string::npos) continue;
                if (name.find(vowels[vi]) == std::string::npos) continue;
                vowelFound[vi] = 1;
                for (int f = start; f <= end; ++f)
                    vowelTracks[vi][(size_t)(f - start)] =
                        std::max(vowelTracks[vi][(size_t)(f - start)], SampleMorph(keys, f));
            }
            // the same 3-frame mean as the mouth
            std::vector<float> smoothed(vowelTracks[vi].size(), 0.0f);
            for (size_t i = 0; i < vowelTracks[vi].size(); ++i) {
                float sum = 0.0f;
                int n = 0;
                for (int k = -1; k <= 1; ++k) {
                    const int j = (int)i + k;
                    if (j < 0 || j >= (int)vowelTracks[vi].size()) continue;
                    sum += vowelTracks[vi][(size_t)j];
                    ++n;
                }
                smoothed[i] = n > 0 ? sum / (float)n : 0.0f;
            }
            vowelTracks[vi].swap(smoothed);
        }
        if (hasLipsync) {
            for (size_t i = 0; i < mouth.size(); ++i)
                for (const std::string& n : mouthNames) {
                    const auto it = motion.morphs.find(n);
                    if (it == motion.morphs.end()) continue;
                    mouth[i] = std::max(mouth[i], SampleMorph(it->second, start + (int)i));
                }
        }

        // 3-frame mean
        std::vector<float> smooth(mouth.size(), 0.0f);
        for (size_t i = 0; i < mouth.size(); ++i) {
            float sum = 0.0f;
            int n = 0;
            for (int k = -1; k <= 1; ++k) {
                const int j = (int)i + k;
                if (j < 0 || j >= (int)mouth.size()) continue;
                sum += mouth[(size_t)j];
                ++n;
            }
            smooth[i] = n > 0 ? sum / (float)n : 0.0f;
        }

        // phrases: threshold runs, merge gaps < 6, drop runs < 12
        struct Run { int start, end; };
        std::vector<Run> runs;
        bool active = false;
        int rs = 0;
        for (int i = 0; i < totalFrames; ++i) {
            if (smooth[(size_t)i] > threshold) {
                if (!active) { active = true; rs = i; }
            } else if (active) {
                runs.push_back({rs, i - 1});
                active = false;
            }
        }
        if (active) runs.push_back({rs, totalFrames - 1});
        std::vector<Run> merged;
        for (const Run& r : runs) {
            if (!merged.empty() && r.start - merged.back().end - 1 < 6) {  // gaps under 0.2 s are not breaths
                merged.back().end = r.end;
            } else {
                merged.push_back(r);
            }
        }
        std::vector<Run> phrases;
        for (const Run& r : merged)
            if (r.end - r.start + 1 >= 12) phrases.push_back(r);

        nlohmann::json phraseJson = nlohmann::json::array();
        int sungFrames = 0;
        for (const Run& r : phrases) {
            float sum = 0.0f;
            int peaks = 0;
            for (int i = r.start; i <= r.end; ++i) {
                sum += smooth[(size_t)i];
                if (smooth[(size_t)i] > threshold) {
                    const float prev = i > r.start ? smooth[(size_t)(i - 1)] : -1.0f;
                    const float next = i < r.end ? smooth[(size_t)(i + 1)] : -1.0f;
                    if (smooth[(size_t)i] >= prev && smooth[(size_t)i] > next) ++peaks;
                }
            }
            const float meanOpen = (r.end - r.start + 1) > 0
                                       ? sum / (float)(r.end - r.start + 1) : 0.0f;
            phraseJson.push_back({{"start", start + r.start},
                                  {"end", start + r.end},
                                  {"mean_open", Round3(meanOpen)},
                                  {"syllables", peaks}});
            sungFrames += r.end - r.start + 1;
            for (int i = r.start; i <= r.end; ++i) inPhrase[mi][(size_t)i] = 1;
        }

        nlohmann::json breathsJson = nlohmann::json::array();
        for (size_t i = 0; i + 1 < phrases.size(); ++i) {
            const int bs = phrases[i].end + 1;
            const int be = phrases[i + 1].start - 1;
            if (be >= bs) breathsJson.push_back({{"start", start + bs}, {"end", start + be}});
        }

        nlohmann::json heldJson = nlohmann::json::array();
        for (size_t vi = 0; vi < vowelTracks.size(); ++vi) {
            if (!vowelFound[vi]) continue;
            const std::vector<float>& track = vowelTracks[vi];
            int hs = -1;
            float lo = 1.0f, hi = 0.0f;
            for (int i = 0; i <= totalFrames; ++i) {
                const bool inside = i < totalFrames && inPhrase[mi][(size_t)i] != 0 &&
                                    track[(size_t)i] >= 0.5f;
                if (inside) {
                    if (hs < 0) { hs = i; lo = hi = track[(size_t)i]; }
                    else { lo = std::min(lo, track[(size_t)i]); hi = std::max(hi, track[(size_t)i]); }
                } else if (hs >= 0) {
                    if (i - hs >= 18 && hi - lo < 0.15f)
                        heldJson.push_back({{"start", start + hs}, {"end", start + i - 1},
                                            {"vowel", vowels[vi]}});
                    hs = -1;
                }
            }
        }

        const double share = totalFrames > 0 ? (double)sungFrames / (double)totalFrames : 0.0;
        characters.push_back({{"model", models[mi]},
                              {"name", m.name},
                              {"has_lipsync", hasLipsync},
                              {"share", Round3(share)},
                              {"phrases", phraseJson},
                              {"breaths", breathsJson},
                              {"held_notes", heldJson}});
    }

    // timeline: contiguous segments with the same singer set
    nlohmann::json timeline = nlohmann::json::array();
    if (totalFrames > 0) {
        auto singersAt = [&](int i) {
            std::vector<int> s;
            for (size_t mi = 0; mi < models.size(); ++mi)
                if (inPhrase[mi][(size_t)i]) s.push_back(models[mi]);
            return s;
        };
        std::vector<int> cur = singersAt(0);
        int segBegin = start;
        for (int i = 1; i <= totalFrames; ++i) {
            std::vector<int> next = i < totalFrames ? singersAt(i) : std::vector<int>{-1};
            if (i == totalFrames || next != cur) {
                nlohmann::json singers = nlohmann::json::array();
                for (int s : cur) singers.push_back(s);
                timeline.push_back({{"start", segBegin}, {"end", start + i - 1}, {"singers", singers}});
                cur = next;
                segBegin = start + i;
            }
        }
    }

    nlohmann::json out;
    out["fps"] = 30;
    out["characters"] = characters;
    out["timeline"] = timeline;
    promise->Resolve(out);
    return true;
}

void App::AdvanceMcpContactSheet() {
    if (!pendingMcpContactSheet_ || !pendingMcpContactSheet_->active) return;
    PendingMcpContactSheet& st = *pendingMcpContactSheet_;

    if (!studio_ || screen_ != Screen::Studio) {
        st.promise->Reject("The studio was left before the contact sheet finished");
        pendingMcpContactSheet_.reset();
        return;
    }
    StudioDoc& d = *studio_;

    if (st.waitFrames > 0) {
        --st.waitFrames;
        return;
    }

    ImageRGBA8 img;
    if (!renderer_.ReadFinalImage(img) || img.mips.empty() || img.mips[0].pixels.empty()) {
        d.playing = st.savedPlaying;
        d.useMotionCamera = st.savedUseMotionCamera;
        d.physics = st.savedPhysics;
        StudioSeek(st.savedFrame / 30.0);
        st.promise->Reject("no scene image for the contact sheet");
        pendingMcpContactSheet_.reset();
        return;
    }

    uint32_t tw = 0, th = 0;
    std::vector<uint8_t> scaled = DownscaleRgba8(img.mips[0].width, img.mips[0].height,
                                                 img.mips[0].pixels.data(), (uint32_t)st.tileWidth, tw, th);
    if (tw == 0 || th == 0) {
        d.playing = st.savedPlaying;
        d.useMotionCamera = st.savedUseMotionCamera;
        d.physics = st.savedPhysics;
        StudioSeek(st.savedFrame / 30.0);
        d.physicsFrame = -1.0f;
        st.promise->Reject("Failed to downscale a contact sheet tile");
        pendingMcpContactSheet_.reset();
        return;
    }

    if (st.gridW == 0) {
        st.tileW = tw;
        st.tileH = th;
        const int rows = ((int)st.frames.size() + st.columns - 1) / st.columns;
        st.gridW = (uint32_t)(st.columns * (int)tw + (st.columns - 1) * kContactGutter);
        st.gridH = (uint32_t)(rows * (int)th + (rows - 1) * kContactGutter);
        st.pixels.assign((size_t)st.gridW * st.gridH * 4, 0);
        FillRect(st.pixels, (int)st.gridW, (int)st.gridH, 0, 0, (int)st.gridW, (int)st.gridH, 12, 12, 12, 255);
    } else if (tw != st.tileW || th != st.tileH) {
        std::vector<uint8_t> fixed((size_t)st.tileW * st.tileH * 4, 0);
        for (uint32_t y = 0; y < st.tileH; ++y) {
            const uint32_t sy = std::min(th - 1, (uint32_t)((uint64_t)y * th / st.tileH));
            const uint32_t sw = std::min(tw, st.tileW);
            std::memcpy(&fixed[(size_t)y * st.tileW * 4], &scaled[(size_t)sy * tw * 4], (size_t)sw * 4);
        }
        scaled.swap(fixed);
    }

    const int index = (int)st.next;
    const int frame = st.frames[st.next];
    const int col = index % st.columns;
    const int row = index / st.columns;
    const int x = col * ((int)st.tileW + kContactGutter);
    const int y = row * ((int)st.tileH + kContactGutter);

    for (uint32_t ty = 0; ty < st.tileH; ++ty)
        std::memcpy(&st.pixels[((size_t)(y + (int)ty) * st.gridW + (size_t)x) * 4],
                    &scaled[(size_t)ty * st.tileW * 4], (size_t)st.tileW * 4);

    const std::string label = FrameLabel(frame);
    const int scale = 2;
    const int textW = (int)label.size() * 6 * scale;
    const int textH = 7 * scale;
    FillRect(st.pixels, (int)st.gridW, (int)st.gridH, x + 2, y + 2, textW + 2 * scale, textH + 2 * scale,
             0, 0, 0, 230);
    DrawText(st.pixels, (int)st.gridW, (int)st.gridH, x + 2 + scale, y + 2 + scale, scale, label);

    st.tiles.push_back({{"index", index},
                        {"frame", frame},
                        {"time", Round3(frame / 30.0)},
                        {"x", x},
                        {"y", y},
                        {"w", (int)st.tileW},
                        {"h", (int)st.tileH}});
    ++st.next;

    if (st.next < st.frames.size()) {
        StudioSeek(st.frames[st.next] / 30.0);
        d.physicsFrame = -1.0f;
        st.waitFrames = st.settleFrames;
        return;
    }

    // done: restore the studio state
    d.playing = st.savedPlaying;
    d.useMotionCamera = st.savedUseMotionCamera;
    d.physics = st.savedPhysics;
    StudioSeek(st.savedFrame / 30.0);
    d.physicsFrame = -1.0f;
    if (d.hasAudio) StudioSeekAudio();  // playback state restored (the sheet paused audio)

    std::vector<uint8_t> png;
    if (!EncodePngRGBA8(st.gridW, st.gridH, st.pixels.data(), st.gridW * 4, png)) {
        st.promise->Reject("Failed to encode PNG");
        pendingMcpContactSheet_.reset();
        return;
    }
    nlohmann::json res;
    if (!st.path.empty()) {
        const std::filesystem::path file = std::filesystem::absolute(Utf8ToPath(st.path));
        std::error_code ec;
        if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
        FILE* f = _wfopen(file.c_str(), L"wb");
        const bool ok = f && std::fwrite(png.data(), 1, png.size(), f) == png.size();
        if (f) std::fclose(f);
        if (!ok) {
            st.promise->Reject("Could not write " + PathToUtf8(file));
            pendingMcpContactSheet_.reset();
            return;
        }
        res["path"] = PathToUtf8(file);
    }
    if (st.returnImage) res["image_base64"] = Base64Encode(png.data(), png.size());
    res["width"] = st.gridW;
    res["height"] = st.gridH;
    res["tiles"] = st.tiles;
    st.promise->Resolve(res);
    pendingMcpContactSheet_.reset();
}

}  // namespace mmdx
