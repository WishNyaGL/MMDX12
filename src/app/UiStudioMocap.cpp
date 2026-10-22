#include "app/App.h"
#include "anim/ModelInstance.h"
#include "app/Icons.h"
#include "app/UiKit.h"
#include "asset/VideoReader.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "mocap/MocapExtractor.h"
#include "mocap/MocapSolver.h"
#include "mocap/OrtRuntime.h"
#include "studio/FileDialog.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>

namespace mmdx {

using namespace studio;

void App::StudioOpenMocapDialog() {
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m || m->kind != ModelKind::Character) return;

    studioMocapOptAtFrame_ = d.Frame();
    studioMocapOptSmoothing_ = 0.5f;
    studioMocapOptHands_ = true;
    studioMocapOptFace_ = true;

    // Check ORT and model files
    std::string reason;
    studioMocapAvailable_ = mocap::OrtAvailable(&reason);
    if (studioMocapAvailable_) {
        const char* const requiredModels[] = {
            "person_det.onnx",
            "pose_landmark.onnx",
            "hand_landmark.onnx",
            "palm_det.onnx",
            "face_landmark.onnx",
            "face_blendshapes.onnx"
        };
        for (const char* name : requiredModels) {
            const auto p = mocap::GetMocapModelPath(name);
            if (!std::filesystem::exists(p)) {
                studioMocapAvailable_ = false;
                reason = "mocap model file missing: " + PathToUtf8(p);
                break;
            }
        }
    }
    studioMocapAvailReason_ = std::move(reason);
    studioMocapDialogOpen_ = true;
}

bool App::StudioStartMocap(const std::filesystem::path& video, double start, double end, bool hands, bool face,
                          float smoothing, int atFrame) {
    StudioDoc& d = *studio_;
    StudioModel* m = d.Selected();
    if (!m || m->kind != ModelKind::Character || !m->pmx) return false;

    if (video.empty() || !std::filesystem::exists(video)) {
        toast_ = {Tr("영상 파일을 찾을 수 없습니다"), PathToUtf8(video), {}, true, timeSeconds_ + 4.0};
        return false;
    }

    std::string reason;
    if (!mocap::OrtAvailable(&reason)) {
        toast_ = {Tr("모션 캡처를 시작할 수 없습니다"), reason, {}, true, timeSeconds_ + 4.0};
        return false;
    }

    std::shared_ptr<const PmxModel> pmx = m->pmx;
    studioMocapCancel_.store(false);
    studioMocapProgress_.store(0.0f);
    studioMocapResult_ = {};
    studioMocapReport_ = {};
    studioMocapError_.clear();
    studioMocapLastError_.clear();
    studioMocapModelUid_ = m->uid;
    studioMocapAtFrame_ = atFrame;
    studioMocapRunning_ = true;
    studioMocapDialogOpen_ = true;
    studioMocapStartWall_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    studioMocapFuture_ = std::async(std::launch::async, [this, video, start, end, hands, face, smoothing, pmx] {
        mocap::ExtractOptions eo;
        eo.startSeconds = std::max(0.0, start);
        eo.endSeconds = end > 0.0 ? end : -1.0;
        eo.fps = 30.0;
        eo.hands = hands;
        eo.face = face;

        mocap::MocapClip clip;
        std::string err;
        const bool extOk = mocap::ExtractMocap(video, eo, clip, [this](float f) {
            studioMocapProgress_.store(std::clamp(f * 0.9f, 0.0f, 0.9f));
            return !studioMocapCancel_.load();
        }, &err);

        if (studioMocapCancel_.load()) {
            studioMocapError_ = "Cancelled";
            return false;
        }
        if (!extOk) {
            studioMocapError_ = err.empty() ? "Video landmark extraction failed" : err;
            return false;
        }

        studioMocapProgress_.store(0.9f);

        mocap::SolveOptions so;
        so.fingers = hands;
        so.face = face;
        so.smoothing = smoothing;

        VmdMotion vmd;
        mocap::SolveReport rep;
        const bool solveOk = mocap::SolveMotion(clip, *pmx, so, vmd, &rep, &err);

        if (studioMocapCancel_.load()) {
            studioMocapError_ = "Cancelled";
            return false;
        }
        if (!solveOk) {
            studioMocapError_ = err.empty() ? "Motion solve failed" : err;
            return false;
        }

        studioMocapResult_ = std::move(vmd);
        studioMocapReport_ = std::move(rep);
        studioMocapProgress_.store(1.0f);
        return true;
    });

    LOG_INFO("studio: mocap started for model uid %u, video %s, at frame %d",
             studioMocapModelUid_, PathToUtf8(video).c_str(), atFrame);
    return true;
}

void App::StudioPollMocap() {
    if (!studioMocapRunning_ || !studioMocapFuture_.valid()) return;
    if (studioMocapFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;

    const bool ok = studioMocapFuture_.get();
    studioMocapRunning_ = false;
    studioMocapDialogOpen_ = false;

    StudioDoc& d = *studio_;
    const int modelIdx = d.IndexOfUid(studioMocapModelUid_);
    const double nowWall = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double elapsedSec = nowWall - studioMocapStartWall_;

    if (!ok) {
        studioMocapLastError_ = studioMocapError_.empty() ? (studioMocapCancel_.load() ? "Cancelled" : "Mocap failed") : studioMocapError_;
        if (studioMocapCancel_.load()) {
            toast_ = {Tr("영상 모션 캡처를 취소했어요"), {}, {}, true, timeSeconds_ + 4.0};
        } else {
            toast_ = {Tr("영상 모션 캡처에 실패했습니다"), studioMocapError_, {}, true, timeSeconds_ + 4.0};
        }
    } else if (modelIdx < 0 || modelIdx >= (int)d.models.size()) {
        studioMocapLastError_ = "Target model removed";
        LOG_INFO("studio: mocap target model removed, result dropped");
    } else {
        studioMocapLastError_.clear();
        StudioModel* m = d.models[(size_t)modelIdx].get();
        if (m && m->kind == ModelKind::Character && m->pmx) {
            MotionData in = MotionData::FromVmd(studioMocapResult_);
            in.camera.clear();
            in.light.clear();
            in.shadow.clear();
            in.visibility.clear();

            const int resultLen = in.EndFrame();
            const int atFrame = studioMocapAtFrame_;
            const int rangeStart = atFrame;
            const int rangeEnd = atFrame + resultLen;

            for (auto& [name, keys] : in.bones) {
                for (BoneKf& k : keys) k.frame += atFrame;
            }
            for (auto& [name, keys] : in.morphs) {
                for (MorphKf& k : keys) k.frame += atFrame;
            }
            for (auto& [name, keys] : in.ik) {
                for (IkKf& k : keys) k.frame += atFrame;
            }

            in.CanonicalizeNames(*m->pmx);

            MotionData after = m->motion;
            for (const auto& [name, _] : in.bones) {
                auto it = after.bones.find(name);
                if (it != after.bones.end()) {
                    std::erase_if(it->second, [rangeStart, rangeEnd](const BoneKf& k) {
                        return k.frame >= rangeStart && k.frame <= rangeEnd;
                    });
                    if (it->second.empty()) after.bones.erase(it);
                }
                after.staticBones.erase(name);
            }
            for (const auto& [name, _] : in.morphs) {
                auto it = after.morphs.find(name);
                if (it != after.morphs.end()) {
                    std::erase_if(it->second, [rangeStart, rangeEnd](const MorphKf& k) {
                        return k.frame >= rangeStart && k.frame <= rangeEnd;
                    });
                    if (it->second.empty()) after.morphs.erase(it);
                }
                after.staticMorphs.erase(name);
            }
            for (const auto& [name, _] : in.ik) {
                auto it = after.ik.find(name);
                if (it != after.ik.end()) {
                    std::erase_if(it->second, [rangeStart, rangeEnd](const IkKf& k) {
                        return k.frame >= rangeStart && k.frame <= rangeEnd;
                    });
                    if (it->second.empty()) after.ik.erase(it);
                }
            }

            // the capture turns leg IK off (FK legs) at its first frame: give the keys after the range their IK back
            if (after.EndFrame() > rangeEnd) {
                for (const auto& [name, keys] : in.ik) {
                    bool wasOn = true;   // IK state the original motion had at rangeEnd + 1
                    if (auto it = m->motion.ik.find(name); it != m->motion.ik.end())
                        for (const IkKf& k : it->second)
                            if (k.frame <= rangeEnd + 1) wasOn = k.enabled;
                    if (!wasOn) continue;
                    auto& dst = after.ik[name];
                    if (std::none_of(dst.begin(), dst.end(), [&](const IkKf& k) { return k.frame == rangeEnd + 1; }))
                        dst.push_back({rangeEnd + 1, true});
                    std::sort(dst.begin(), dst.end(), [](const IkKf& a, const IkKf& b) { return a.frame < b.frame; });
                }
            }
            after.Merge(in);

            d.history.Push(std::make_unique<MotionSwapCommand>(d, modelIdx, Tr("영상 모션 캡처"), m->motion, std::move(after)));
            StudioSelectModel(modelIdx);
            d.selection.clear();
            d.rowsKey = ~0ull;

            const int totalKeys = studioMocapReport_.boneKeys + studioMocapReport_.morphKeys;
            char detail[256];
            if (!studioMocapReport_.missingBones.empty() || !studioMocapReport_.missingMorphs.empty()) {
                std::snprintf(detail, sizeof(detail), Tr("키 %d개 · 누락 본 %d개, 표정 %d개"),
                              totalKeys, (int)studioMocapReport_.missingBones.size(),
                              (int)studioMocapReport_.missingMorphs.size());
            } else {
                std::snprintf(detail, sizeof(detail), Tr("키 %d개"), totalKeys);
            }
            toast_ = {Tr("영상에서 모션을 만들었어요"), detail, {}, false, timeSeconds_ + 5.0};

            LOG_INFO("MOCAP studio: %d frames, %d keys in %.2f s",
                     studioMocapReport_.frames, totalKeys, elapsedSec);
        }
    }

    studioMocapResult_ = {};
    studioMocapReport_ = {};
    studioMocapError_.clear();
    studioMocapModelUid_ = 0;
    studioMocapCancel_.store(false);
}

void App::DrawStudioMocapDialog() {
    using namespace ui;
    if (!studioMocapDialogOpen_ && !studioMocapRunning_) return;
    if (!studio_) {
        studioMocapDialogOpen_ = false;
        return;
    }
    const Palette& p = P();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(460.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(p.surface));

    if (!ImGui::IsPopupOpen("##mocapdlg")) ImGui::OpenPopup("##mocapdlg");
    if (ImGui::BeginPopupModal("##mocapdlg", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        TextEllipsis(dl, Font::Semibold, size::Title, c, c.x + Dp(412.0f), p.ink, Tr("영상에서 모션 만들기"));

        if (studioMocapRunning_) {
            ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(30.0f)));
            ImGui::PushTextWrapPos(c.x + Dp(412.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink2));
            PushFont(Font::Regular, size::Small);

            const double nowWall = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            const double elapsed = nowWall - studioMocapStartWall_;
            const float frac = studioMocapProgress_.load();
            char status[128];
            std::snprintf(status, sizeof(status), "%s (%.1f%s)",
                          frac < 0.9f ? Tr("랜드마크 추출 중…") : Tr("모션 변환 중…"),
                          elapsed, Tr("초"));
            ImGui::TextWrapped("%s", status);
            PopFont();
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();

            ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(74.0f)));
            const ImVec2 a = ImGui::GetCursorScreenPos();
            ProgressBar(dl, a, ImVec2(a.x + Dp(412.0f), a.y + Dp(8.0f)), frac);
            ImGui::Dummy(ImVec2(0.0f, Dp(22.0f)));

            ImGui::SetCursorScreenPos(ImVec2(c.x + Dp(116.0f), c.y + Dp(110.0f)));
            if (Button("##mocapcancelrun", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(180.0f, 40.0f))) {
                studioMocapCancel_.store(true);
            }
        } else {
            ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(34.0f)));

            // 1. Video file selection row
            {
                if (Button("##mocapselectvideo", Tr("영상 선택…"), icon::VideoCamera, ButtonKind::Secondary, ImVec2(120.0f, 32.0f))) {
                    const auto picked = OpenFileDialog(hwnd_, {
                        {L"Video files", L"*.mp4;*.mov;*.m4v;*.avi;*.mkv;*.wmv;*.webm"}
                    });
                    if (!picked.empty()) {
                        studioMocapOptVideo_ = picked;
                        VideoReader vr;
                        std::string err;
                        if (vr.Open(picked, &err)) {
                            studioMocapOptDuration_ = vr.Duration();
                            studioMocapOptStartSec_ = 0.0f;
                            studioMocapOptEndSec_ = (float)studioMocapOptDuration_;
                        } else {
                            studioMocapOptDuration_ = 0.0;
                            studioMocapOptStartSec_ = 0.0f;
                            studioMocapOptEndSec_ = 0.0f;
                        }
                    }
                }
                ImGui::SameLine(0, Dp(10.0f));
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Dp(6.0f));
                std::string desc;
                if (!studioMocapOptVideo_.empty()) {
                    desc = PathToUtf8(studioMocapOptVideo_.filename());
                    if (studioMocapOptDuration_ > 0.0) {
                        char dur[32];
                        std::snprintf(dur, sizeof(dur), " (%.1f%s)", studioMocapOptDuration_, Tr("초"));
                        desc += dur;
                    }
                } else {
                    desc = Tr("영상을 선택하세요");
                }
                PushFont(Font::Regular, size::Small);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(studioMocapOptVideo_.empty() ? p.ink3 : p.ink), "%s", desc.c_str());
                PopFont();
            }

            ImGui::Dummy(ImVec2(0.0f, Dp(8.0f)));

            const auto dragFloatRow = [&](const char* label, float* v, float lo, float hi, const char* id) {
                const ImVec2 r = ImGui::GetCursorScreenPos();
                Text(dl, Font::Regular, size::Small, ImVec2(r.x, r.y + Dp(5.0f)), p.ink3, label);
                ImGui::SetCursorScreenPos(ImVec2(r.x + Dp(120.0f), r.y));
                ImGui::SetNextItemWidth(Dp(292.0f));
                PushFont(Font::Regular, size::Small);
                ImGui::DragFloat(id, v, 0.1f, lo, hi, "%.2f s");
                PopFont();
                ImGui::Dummy(ImVec2(0.0f, Dp(6.0f)));
            };

            const auto dragIntRow = [&](const char* label, int* v, int lo, int hi, const char* id) {
                const ImVec2 r = ImGui::GetCursorScreenPos();
                Text(dl, Font::Regular, size::Small, ImVec2(r.x, r.y + Dp(5.0f)), p.ink3, label);
                ImGui::SetCursorScreenPos(ImVec2(r.x + Dp(120.0f), r.y));
                ImGui::SetNextItemWidth(Dp(292.0f));
                PushFont(Font::Regular, size::Small);
                ImGui::DragInt(id, v, 0.5f, lo, hi);
                PopFont();
                ImGui::Dummy(ImVec2(0.0f, Dp(6.0f)));
            };

            // 2. Start / end seconds
            const float maxDur = studioMocapOptDuration_ > 0.0 ? (float)studioMocapOptDuration_ : 100000.0f;
            dragFloatRow(Tr("시작 시간 (초)"), &studioMocapOptStartSec_, 0.0f, maxDur, "##mocapstart");
            dragFloatRow(Tr("끝 시간 (초)"), &studioMocapOptEndSec_, 0.0f, maxDur, "##mocapend");
            studioMocapOptStartSec_ = std::clamp(studioMocapOptStartSec_, 0.0f, maxDur);
            studioMocapOptEndSec_ = std::clamp(studioMocapOptEndSec_, studioMocapOptStartSec_, maxDur);

            // 3. Checkboxes (hands / face)
            Switch("##mocaphands", Tr("손가락"), &studioMocapOptHands_,
                   Tr("손가락 마디 모션을 캡처해요"));
            Switch("##mocapface", Tr("표정 · 립싱크"), &studioMocapOptFace_,
                   Tr("얼굴 표정과 입모양을 캡처해요"));

            // 4. Smoothing slider
            SliderRow("##mocapsmooth", Tr("스무딩"), &studioMocapOptSmoothing_, 0.0f, 1.0f, "%.2f");
            studioMocapOptSmoothing_ = std::clamp(studioMocapOptSmoothing_, 0.0f, 1.0f);

            // 5. At frame
            dragIntRow(Tr("넣을 위치"), &studioMocapOptAtFrame_, 0, 1000000, "##mocapframe");
            studioMocapOptAtFrame_ = std::max(0, studioMocapOptAtFrame_);

            // 6. Note about replacement
            {
                PushFont(Font::Regular, size::Caption);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
                ImGui::TextWrapped("%s", Tr("해당 범위에 있는 기존 본 · 표정 · IK 키는 대체됩니다."));
                ImGui::PopStyleColor();
                PopFont();
            }

            // 7. Missing model / ort error banner if any
            if (!studioMocapAvailable_) {
                PushFont(Font::Regular, size::Small);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.danger));
                ImGui::TextWrapped("%s", studioMocapAvailReason_.c_str());
                ImGui::PopStyleColor();
                PopFont();
            }

            ImGui::Dummy(ImVec2(0.0f, Dp(8.0f)));

            bool close = false;
            if (Button("##mocapcancel", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(196.0f, 40.0f)) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                close = true;
            }
            ImGui::SameLine(0, Dp(12.0f));

            const bool canStart = studioMocapAvailable_ && !studioMocapOptVideo_.empty() &&
                                  std::filesystem::exists(studioMocapOptVideo_);
            if (!canStart) {
                ImGui::BeginDisabled();
            }
            if (Button("##mocapok", Tr("시작"), icon::VideoCamera, ButtonKind::Primary, ImVec2(196.0f, 40.0f)) ||
                (canStart && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))) {
                if (StudioStartMocap(studioMocapOptVideo_, studioMocapOptStartSec_, studioMocapOptEndSec_,
                                     studioMocapOptHands_, studioMocapOptFace_, studioMocapOptSmoothing_,
                                     studioMocapOptAtFrame_)) {
                    close = false;
                } else {
                    close = true;
                }
            }
            if (!canStart) {
                ImGui::EndDisabled();
            }

            if (close) {
                studioMocapDialogOpen_ = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace mmdx
