#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "app/Icons.h"
#include "app/Lighting.h"
#include "app/UiHelpers.h"
#include "app/UiKit.h"
#include "core/Log.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"

namespace mmdx {

// ---------------------------------------------------------------------------
// Play update
// ---------------------------------------------------------------------------

void App::UpdatePlay(double dt) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) lastMouseMoveTime_ = timeSeconds_;

    const SceneRuntime* s = scene_.get();
    if (!s) return;

    const auto seekTo = [&](double t) {
        playTime_ = std::clamp(t, 0.0, (double)(s->endFrame / s->motionFps));
        if (s->hasAudio) {
            audio_.Seek(playTime_);
            // after the sound reached its end miniaudio's at-end state stops it; seeking back into the
            // song while playing must resume audio there.
            if (playing_ && playTime_ < audio_.DurationSeconds() && !audio_.IsPlaying()) audio_.Play();
        }
    };

    if (!io.WantCaptureKeyboard) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) SetPlaying(!playing_);
        if (ImGui::IsKeyPressed(ImGuiKey_L)) {
            settings_.lighting = (settings_.lighting + 1) % kLightingPresetCount;
            settings_.Save(settingsPath_);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (playPanelOpen_ && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
                playPanelOpen_ = false;   // Esc closes the 화면 효과 panel first
            } else {
                UnloadScene();
                screen_ = Screen::Select;
                return;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F1)) overlayVisible_ ^= 1;
        if (s->camera && ImGui::IsKeyPressed(ImGuiKey_C)) SetPlayMotionCamera(!useMotionCamera_);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) seekTo(playTime_ - 5.0);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) seekTo(playTime_ + 5.0);
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) seekTo(0.0);
    }

    if (playing_) {
        if (s->hasAudio && audio_.IsPlaying()) {
            const double a = audio_.PositionSeconds();
            const double predicted = playTime_ + dt;
            playTime_ = std::fabs(predicted - a) > 0.05 ? a : predicted;
        } else {
            playTime_ += dt;
        }
        if (playTime_ * s->motionFps >= (double)s->endFrame) {
            playTime_ = s->endFrame / s->motionFps;
            playing_ = false;
            audio_.Pause();
        }
    }

    if (!useMotionCamera_ || !s->camera) UpdateFreeCamera();

    // after the time step, so a still taken during playback has the last live frame as shutter open
    if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_P) &&
        renderer_.OfflineSupported())
        StartOfflineStill();
}

void App::SetPlaying(bool play) {
    const SceneRuntime* s = scene_.get();
    if (!s) return;
    if (play && playTime_ * s->motionFps >= s->endFrame) playTime_ = 0;
    playing_ = play;
    if (s->hasAudio) {
        if (playing_) {
            audio_.Seek(playTime_);
            audio_.Play();
        } else {
            audio_.Pause();
        }
    }
}

// ---------------------------------------------------------------------------
// Free camera
// ---------------------------------------------------------------------------

App::FreeCamera App::FreeFromPose(const CameraPose& pose) {
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT3 eye;
    CameraMotion::ToView(pose, &view, &eye);
    // the same eye and forward axis; distance-0 cameras (target = eye) orbit a point just ahead
    FreeCamera c;
    const float dist = std::clamp(std::fabs(pose.distance), 2.0f, 600.0f);
    const DirectX::XMFLOAT3 fwd{view._13, view._23, view._33};
    c.target = {eye.x + fwd.x * dist, eye.y + fwd.y * dist, eye.z + fwd.z * dist};
    c.distance = dist;
    c.pitch = std::clamp(std::asin(std::clamp(-fwd.y, -1.0f, 1.0f)), -1.45f, 1.45f);
    c.yaw = std::atan2(-fwd.x, fwd.z);
    c.fovDeg = pose.fovDeg;
    return c;
}

void App::SetPlayMotionCamera(bool on) {
    const SceneRuntime* s = scene_.get();
    if (on == useMotionCamera_) return;
    if (!on && s && s->camera) {  // continue from the shot on screen instead of jumping to the last free view
        freeCam_ = FreeFromPose(s->camera->Evaluate((float)(playTime_ * s->motionFps)));
    }
    useMotionCamera_ = on;
}

void App::UpdateFreeCamera() {
    ImGuiIO& io = ImGui::GetIO();
    // The play overlay is a full-screen ImGui window, so io.WantCaptureMouse is always set over the scene: only
    // its widgets and the other windows (popups, panels) count as UI. A drag belongs to the camera when it started
    // over the scene, so dragging a slider across the view does not turn it.
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiWindow* hovered = g.HoveredWindow ? g.HoveredWindow->RootWindow : nullptr;
    // Hovering the 화면 효과 panel is "over UI" like any other window (its name is not ##playoverlay), so a drag
    // there never turns the free camera; scrubbing only starts on the seek bar itself.
    const bool overUi = (hovered && std::strcmp(hovered->Name, "##playoverlay") != 0) || ImGui::IsAnyItemHovered() ||
                        ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    for (int b = 0; b < 3; ++b) {
        if (ImGui::IsMouseClicked(b) && !overUi) freeCamDrag_ |= 1 << b;
        if (!ImGui::IsMouseDown(b)) freeCamDrag_ &= ~(1 << b);
    }

    FreeCamera& cam = freeCam_;
    const ImVec2 delta = io.MouseDelta;
    if (freeCamDrag_ & (1 << ImGuiMouseButton_Left)) {   // orbit
        cam.yaw += delta.x * 0.006f;
        cam.pitch = std::clamp(cam.pitch + delta.y * 0.006f, -1.45f, 1.45f);
    }
    if (freeCamDrag_ & ((1 << ImGuiMouseButton_Right) | (1 << ImGuiMouseButton_Middle))) {   // pan
        // right = (cos(yaw), 0, sin(yaw)), up = (0, 1, 0); pan = (-dx*right + dy*up) * distance * 0.0015
        const float rx = std::cos(cam.yaw), rz = std::sin(cam.yaw);
        const float scale = cam.distance * 0.0015f;
        cam.target.x += (-delta.x * rx) * scale;
        cam.target.y += (delta.y) * scale;
        cam.target.z += (-delta.x * rz) * scale;
    }
    if (io.MouseWheel != 0.0f && !overUi)
        cam.distance = std::clamp(cam.distance * std::pow(0.88f, io.MouseWheel), 2.0f, 600.0f);
    // F: frame the performer again (center bone)
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false) && scene_ && scene_->character) {
        const int center = scene_->character->Model().FindBone("\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC");
        if (center >= 0) {
            cam.target = scene_->character->BoneWorldPosition(center);
            cam.target.y += 2.0f;
            // whole-body framing (~24 units tall) for the current lens: handed-over VMD cameras may be narrow
            const float halfFov = DirectX::XMConvertToRadians(std::clamp(cam.fovDeg, 5.0f, 120.0f)) * 0.5f;
            cam.distance = std::clamp(12.0f / std::tan(halfFov), 2.0f, 600.0f);
        }
    }
}


// ---------------------------------------------------------------------------
// Overlay UI
// ---------------------------------------------------------------------------

namespace {

// Seek bar: thin track that thickens on hover, knob while hovered or dragged.
bool SeekBar(const char* id, float width, float height, double* t, double duration) {
    using namespace ui;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImGuiID gid = w->GetID(id);
    const ImRect bb(w->DC.CursorPos, ImVec2(w->DC.CursorPos.x + width, w->DC.CursorPos.y + height));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    ImGui::ButtonBehavior(bb, gid, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
    if (hovered || held) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const Palette& p = P();
    ImDrawList* dl = w->DrawList;
    const float hv = Anim(gid, hovered || held, 14.0f);
    const float th = Dp(4.0f + 2.0f * hv);
    const float cy = bb.Min.y + height * 0.5f;
    const float frac = duration > 0 ? (float)std::clamp(*t / duration, 0.0, 1.0) : 0.0f;
    const float mx = ImGui::GetIO().MousePos.x;
    const float hoverFrac = std::clamp((mx - bb.Min.x) / std::max(width, 1.0f), 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(bb.Min.x, cy - th * 0.5f), ImVec2(bb.Max.x, cy + th * 0.5f), WithAlpha(p.ink, 0.12f), th);
    if (hovered && !held)
        dl->AddRectFilled(ImVec2(bb.Min.x, cy - th * 0.5f), ImVec2(bb.Min.x + width * hoverFrac, cy + th * 0.5f),
                          WithAlpha(p.ink, 0.10f), th);
    dl->AddRectFilled(ImVec2(bb.Min.x, cy - th * 0.5f), ImVec2(bb.Min.x + width * frac, cy + th * 0.5f), p.accent, th);
    if (hv > 0.01f) {
        const ImVec2 kc(bb.Min.x + width * frac, cy);
        const float kr = Dp(7.0f) * hv;
        dl->AddCircleFilled(ImVec2(kc.x, kc.y + Dp(1.0f)), kr + Dp(1.0f), WithAlpha(IM_COL32(10, 40, 40, 255), 0.2f * hv), 24);
        dl->AddCircleFilled(kc, kr, p.knob, 24);
        dl->AddCircle(kc, kr, p.accent, 24, Dp(2.0f));
    }
    if (hovered && !held) {
        const std::string label = MinSec(duration * hoverFrac);
        const ImVec2 ls = TextSize(Font::Semibold, size::Caption, label.c_str());
        const ImVec2 ta(mx - ls.x * 0.5f - Dp(8.0f), bb.Min.y - ls.y - Dp(14.0f));
        dl->AddRectFilled(ta, ImVec2(ta.x + ls.x + Dp(16.0f), ta.y + ls.y + Dp(8.0f)), p.ink, Dp(6.0f));
        Text(dl, Font::Semibold, size::Caption, ImVec2(ta.x + Dp(8.0f), ta.y + Dp(4.0f)), p.surface, label.c_str());
    }
    if (held && duration > 0) {
        *t = duration * hoverFrac;
        return true;
    }
    return false;
}

} // namespace

void App::DrawPlayOverlay() {
    using namespace ui;
    ImGuiIO& io = ImGui::GetIO();
    const SceneRuntime* s = scene_.get();
    if (!s) return;
    const Palette& p = P();

    const bool showBar = overlayVisible_ && (!playing_ || playPanelOpen_ || timeSeconds_ - lastMouseMoveTime_ < 3.0);
    const float show = Anim(ImGui::GetID("##overlayShow"), showBar, 7.0f);
    if (show < 0.005f) return;

    float rect[4];
    renderer_.PresentRect(rect[0], rect[1], rect[2], rect[3]);
    const ImTextureID backdrop = (ImTextureID)renderer_.UiBackdropTexture();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##playoverlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float ease = 1.0f - (1.0f - show) * (1.0f - show) * (1.0f - show);
    const ImVec2 ds = io.DisplaySize;

    // ---- title block (top left), slides up when hidden
    {
        const SongAsset* song = selSong_ >= 0 ? &library_.songs[(size_t)selSong_] : nullptr;
        const CharacterAsset* ch = selCharacter_ >= 0 ? &library_.characters[(size_t)selCharacter_] : nullptr;
        const StageAsset* st = selStage_ >= 0 ? &library_.stages[(size_t)selStage_] : nullptr;
        const std::string title = song ? song->displayName : std::string(Tr("재생 중"));
        std::string charDesc;
        if (s && !s->extras.empty()) {
            std::string centerName = !s->characterDisplayName.empty() ? s->characterDisplayName : (ch ? ch->displayName : "");
            char buf[64];
            std::snprintf(buf, sizeof(buf), Tr(" 외 %d명"), (int)s->extras.size());
            charDesc = centerName + buf;
        } else {   // the loaded performer (a one-performer formation is not the lobby's character)
            charDesc = s && !s->characterDisplayName.empty() ? s->characterDisplayName
                                                              : (ch ? ch->displayName : std::string());
        }
        const std::string sub =
            charDesc + "  ·  " + (st ? st->displayName : std::string(Tr("스튜디오")));
        const ImVec2 ts = TextSize(Font::Semibold, size::Title, title.c_str());
        const ImVec2 ss = TextSize(Font::Regular, size::Caption, sub.c_str());
        const float w = std::min(std::max(ts.x, ss.x) + Dp(40.0f), Dp(520.0f));
        const float h = Dp(62.0f);
        const float y = Dp(20.0f) - (h + Dp(30.0f)) * (1.0f - ease);
        const ImVec2 a(Dp(20.0f), y), b(a.x + w, y + h);
        FrostedPanel(dl, a, b, Dp(16.0f), backdrop, rect);
        TextEllipsis(dl, Font::Semibold, size::Title, ImVec2(a.x + Dp(20.0f), a.y + Dp(11.0f)), b.x - Dp(16.0f), p.ink,
                     title.c_str());
        TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(a.x + Dp(20.0f), a.y + Dp(36.0f)), b.x - Dp(16.0f), p.ink2,
                     sub.c_str());
    }

    // ---- performance pill (top right)
    {
        char buf[200];
        const RenderStats& rs = renderer_.Stats();
        int n = std::snprintf(buf, sizeof(buf), "%.0f FPS   GPU %.2f ms   %ux%u", io.Framerate, rs.gpuFrameMs,
                              rs.internalWidth, rs.internalHeight);
        if (rs.outputWidth != rs.internalWidth || rs.outputHeight != rs.internalHeight)
            n += std::snprintf(buf + n, sizeof(buf) - n, " → %ux%u", rs.outputWidth, rs.outputHeight);
        if (rs.renderPath == RenderPath::Raster && renderer_.Settings().upscaler == UpscalerKind::None) {
            n += std::snprintf(buf + n, sizeof(buf) - n, "   MSAA %ux", renderer_.Settings().msaaSamples);
        } else if (rs.renderPath == RenderPath::RayTraced) {
            n += std::snprintf(buf + n, sizeof(buf) - n, "   RT");
        } else if (rs.renderPath == RenderPath::PathTraced) {
            n += std::snprintf(buf + n, sizeof(buf) - n, "   PT");
        }
        if (rs.upscaler != UpscalerKind::None) {
            const char* upName = rs.upscaler == UpscalerKind::DLSS ? "DLSS"
                                 : rs.upscaler == UpscalerKind::FSR ? "FSR"
                                 : "XeSS";
            n += std::snprintf(buf + n, sizeof(buf) - n, "   %s", upName);
            if (rs.upscaler == UpscalerKind::DLSS) {
                static const char* const kModelNames[] = {"Auto", "CNN", "Transformer", "Transformer 2"};
                n += std::snprintf(buf + n, sizeof(buf) - n, " %s",
                                   kModelNames[std::clamp((int)rs.dlssModel, 0, 3)]);
                if (rs.dlssRayReconstruction) n += std::snprintf(buf + n, sizeof(buf) - n, " RR");
            }
        }
        if (rs.frameGenMultiplier >= 2) {
            // DLSS Frame Generation: rendered rate x multiplier = presented rate.
            n += std::snprintf(buf + n, sizeof(buf) - n, "   FG %dx (%.0f)", rs.frameGenMultiplier,
                               io.Framerate * (float)rs.frameGenMultiplier);
        }
        (void)n;
        const ImVec2 bs = TextSize(Font::Semibold, size::Caption, buf);
        const float w = bs.x + Dp(28.0f), h = Dp(34.0f);
        const float y = Dp(20.0f) - (h + Dp(30.0f)) * (1.0f - ease);
        const ImVec2 a(ds.x - Dp(20.0f) - w, y), b(ds.x - Dp(20.0f), y + h);
        FrostedPanel(dl, a, b, h * 0.5f, backdrop, rect);
        Text(dl, Font::Semibold, size::Caption, ImVec2(a.x + Dp(14.0f), a.y + (h - bs.y) * 0.5f), p.ink2, buf);
    }

    // ---- control bar (bottom), slides down when hidden
    const float barW = std::min(ds.x - Dp(40.0f), Dp(1000.0f));
    const float barH = Dp(76.0f);
    const float barY = ds.y - Dp(24.0f) - barH + (barH + Dp(40.0f)) * (1.0f - ease);
    const ImVec2 ba((ds.x - barW) * 0.5f, barY), bb(ba.x + barW, barY + barH);
    FrostedPanel(dl, ba, bb, Dp(22.0f), backdrop, rect, 0.68f);
    const float cy = ba.y + barH * 0.5f;
    float x = ba.x + Dp(16.0f);

    // play / pause
    {
        const float d = Dp(48.0f);
        const ImVec2 a(x, cy - d * 0.5f);
        ImGui::SetCursorScreenPos(a);
        const bool pressed = ImGui::InvisibleButton("##playpause", ImVec2(d, d));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float hv = Anim(ImGui::GetID("##pphv"), hovered);
        SoftShadow(dl, a, ImVec2(a.x + d, a.y + d), d * 0.5f, Dp(8.0f), 0.14f + 0.06f * hv, ImVec2(0, Dp(2.0f)));
        dl->AddCircleFilled(ImVec2(a.x + d * 0.5f, a.y + d * 0.5f), d * 0.5f, Mix(p.accent, p.accentHover, hv), 32);
        const char* glyph = playing_ ? icon::Pause : icon::Play;
        PushFont(Font::Bold, 22.0f);  // the bold face carries the filled icon set
        const ImVec2 gs = ImGui::CalcTextSize(glyph);
        dl->AddText(ImVec2(std::round(a.x + (d - gs.x) * 0.5f + (playing_ ? 0.0f : Dp(1.5f))),
                           std::round(a.y + (d - gs.y) * 0.5f)),
                    p.onAccent, glyph);
        PopFont();
        if (hovered) Tooltip(playing_ ? Tr("일시정지 (Space)") : Tr("재생 (Space)"));
        if (pressed) SetPlaying(!playing_);
        x += d + Dp(8.0f);
    }
    ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
    if (IconButton("##restart", icon::SkipBack, Tr("처음부터"), false, 40.0f)) {
        playTime_ = 0;
        if (s->hasAudio) {
            audio_.Seek(playTime_);
            if (playing_ && !audio_.IsPlaying()) audio_.Play();
        }
    }
    x += Dp(40.0f) + Dp(12.0f);

    // time
    const double duration = s->endFrame / s->motionFps;
    {
        const std::string cur = MinSec(playTime_), tot = " / " + MinSec(duration);
        const ImVec2 cs = TextSize(Font::Semibold, size::Body, cur.c_str());
        Text(dl, Font::Semibold, size::Body, ImVec2(x, cy - cs.y * 0.5f), p.ink, cur.c_str());
        Text(dl, Font::Regular, size::Body, ImVec2(x + cs.x, cy - cs.y * 0.5f), p.ink3, tot.c_str());
        x += Dp(96.0f);
    }

    // right cluster: volume icon + slider, offline still, camera, lighting, divider, exit
    // (volume, still, camera, light, [size], [shader], effects, MME, divider, exit)
    const int optionalButtons = s->characterId.empty() ? 3 : 4;   // size + shader only with a character; effects + MME always
    const float rightW = Dp(36.0f + 4.0f + 88.0f + 12.0f + 44.0f + 44.0f + 50.0f + 44.0f * optionalButtons + 11.0f + 40.0f + 16.0f);
    const float seekW = std::max(Dp(120.0f), bb.x - rightW - x - Dp(20.0f));
    ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(14.0f)));
    double t = playTime_;
    if (SeekBar("##seek", seekW, Dp(28.0f), &t, duration)) {
        playTime_ = t;
        if (s->hasAudio) {
            audio_.Seek(playTime_);
            if (playing_ && playTime_ < audio_.DurationSeconds() && !audio_.IsPlaying()) audio_.Play();
        }
    }
    x += seekW + Dp(20.0f);

    // volume
    ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(18.0f)));
    const bool muted = settings_.volume <= 0.001f;
    if (IconButton("##mute", muted ? icon::SpeakerX : (settings_.volume < 0.5f ? icon::SpeakerLow : icon::SpeakerHigh),
                   muted ? Tr("소리 켜기") : Tr("음소거"), false, 36.0f)) {
        static float lastVolume = 0.8f;
        if (muted) {
            settings_.volume = lastVolume > 0.01f ? lastVolume : 0.8f;
        } else {
            lastVolume = settings_.volume;
            settings_.volume = 0.0f;
        }
        audio_.SetVolume(settings_.volume);
    }
    x += Dp(36.0f) + Dp(4.0f);
    {
        const float vw = Dp(88.0f);
        ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(10.0f)));
        ImGui::InvisibleButton("##vol", ImVec2(vw, Dp(20.0f)));
        const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        if (hovered || held) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (held) {
            settings_.volume = std::clamp((io.MousePos.x - x) / vw, 0.0f, 1.0f);
            audio_.SetVolume(settings_.volume);
        }
        const float th = Dp(4.0f);
        dl->AddRectFilled(ImVec2(x, cy - th * 0.5f), ImVec2(x + vw, cy + th * 0.5f), WithAlpha(p.ink, 0.12f), th);
        dl->AddRectFilled(ImVec2(x, cy - th * 0.5f), ImVec2(x + vw * settings_.volume, cy + th * 0.5f), p.ink2, th);
        const float hv = Anim(ImGui::GetID("##volhv"), hovered || held);
        if (hv > 0.01f) dl->AddCircleFilled(ImVec2(x + vw * settings_.volume, cy), Dp(6.0f) * hv, p.ink, 24);
        if (hovered || held) {
            char vbuf[32];
            std::snprintf(vbuf, sizeof(vbuf), "%s %d%%", Tr("볼륨"), (int)std::lround(settings_.volume * 100.0f));
            ImGui::SetTooltip("%s", vbuf);
        }
        x += vw + Dp(12.0f);
    }

    // offline render: high-quality still (videos are rendered from the select screen)
    {
        const bool ok = renderer_.OfflineSupported();
        ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
        ImGui::BeginDisabled(!ok);
        if (IconButton("##shot", icon::Camera, ok ? Tr("고품질 스크린샷 (P)") : Tr("고품질 렌더는 DXR 지원 GPU가 필요합니다"),
                       false, 40.0f))
            StartOfflineStill();
        x += Dp(44.0f);
        ImGui::EndDisabled();
    }

    // camera, lighting
    ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
    ImGui::BeginDisabled(!s->camera);
    if (IconButton("##cam", icon::VideoCamera, useMotionCamera_ ? Tr("모션 카메라 켜짐 (C)") : Tr("자유 카메라 (C)"),
                   useMotionCamera_ && s->camera, 40.0f))
        SetPlayMotionCamera(!useMotionCamera_);
    ImGui::EndDisabled();
    x += Dp(44.0f);
    {
        const char* lightIcons[] = {icon::Sun, icon::CircleHalf, icon::Sparkle, icon::Moon};
        const std::string tip = std::string(Tr("조명: ")) + LightingPresetName((LightingPreset)settings_.lighting) + " (L)";
        // a menu of the named presets (clicking used to cycle blindly through them; L still cycles)
        ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
        if (IconButton("##light", lightIcons[settings_.lighting], tip.c_str(), false, 40.0f)) ImGui::OpenPopup("##lightmenu");
        ImGui::SetNextWindowPos(ImVec2(x, cy - Dp(28.0f)), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(Dp(200.0f), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(8.0f), Dp(8.0f)));
        if (ImGui::BeginPopup("##lightmenu")) {
            for (int i = 0; i < kLightingPresetCount; ++i) {
                ImGui::PushID(i);
                const char* hint = i == settings_.lighting ? Tr("사용 중") : nullptr;
                if (MenuItem("##preset", LightingPresetName((LightingPreset)i), lightIcons[i], hint)) {
                    settings_.lighting = i;
                    settings_.Save(settingsPath_);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        x += Dp(40.0f) + Dp(10.0f);
    }
    // character size (saved per character; for models whose height does not suit the song's camera)
    if (!s->characterId.empty()) {
        const bool hasExtras = !s->extras.empty();   // formation scene: the popup picks the edited performer
        const PlayPerformerRef shown = PlayPerformerAt(playEditPerformer_);
        const float scale = settings_.CharacterScale(shown.id);
        ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
        const ImVec2 btn = ImGui::GetCursorScreenPos();
        if (IconButton("##size", icon::ArrowsOut, Tr("캐릭터 크기"), std::fabs(scale - 1.0f) > 0.005f, 40.0f))
            ImGui::OpenPopup("##sizepopup");
        ImGui::SetNextWindowPos(ImVec2(btn.x + Dp(20.0f), btn.y - Dp(10.0f)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(Dp(260.0f), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(16.0f), Dp(14.0f)));
        if (ImGui::BeginPopup("##sizepopup")) {
            if (hasExtras) {   // performer selector (center / extras); stable id for the UI probe
                SectionLabel(Tr("캐릭터"));
                const int cur = playEditPerformer_;
                std::string curLabel = cur == 0 ? s->characterDisplayName
                                                : s->extras[(size_t)(cur - 1)].displayName;
                if (cur == 0) curLabel += " " + std::string(Tr("(센터)"));
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                PushFont(Font::Regular, size::Small);
                if (ImGui::BeginCombo("##playperformer", curLabel.c_str())) {
                    std::string centerName = s->characterDisplayName.empty() ? std::string(Tr("센터"))
                                                                            : s->characterDisplayName;
                    centerName += " " + std::string(Tr("(센터)"));
                    if (ImGui::Selectable(centerName.c_str(), cur == 0)) playEditPerformer_ = 0;
                    for (size_t i = 0; i < s->extras.size(); ++i) {
                        const std::string& n = s->extras[i].displayName;
                        const std::string name = n.empty() ? s->extras[i].characterId : n;
                        ImGui::PushID((int)i);
                        if (ImGui::Selectable(name.c_str(), cur == (int)i + 1)) playEditPerformer_ = (int)i + 1;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                PopFont();
                Gap(8.0f);
            }
            const PlayPerformerRef perf = PlayPerformerAt(playEditPerformer_);
            float v = settings_.CharacterScale(perf.id);
            if (SliderRow("##charscale", Tr("캐릭터 크기"), &v, 0.5f, 2.0f, "%.2fx")) {
                v = std::round(v * 100.0f) / 100.0f;
                if (std::fabs(v - 1.0f) < 0.03f) v = 1.0f;  // snap to the original size
                settings_.SetCharacterScale(perf.id, v);
                settings_.Save(settingsPath_);
            }
            Gap(8.0f);
            if (Button("##charscalereset", Tr("원래 크기"), icon::Refresh, ButtonKind::Ghost)) {
                settings_.SetCharacterScale(perf.id, 1.0f);
                settings_.Save(settingsPath_);
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        x += Dp(44.0f);
    }
    // shader / effects: the 화면 효과 panel's entry buttons (the old ##shader and ##effects popups moved into its
    // tabs; the MME button folded into the same panel). Clicking the button of the open tab closes the panel.
    const auto panelEntry = [&](const char* id, const char* icon, const char* tip, bool extraActive, int tab) {
        ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
        const bool active = (playPanelOpen_ && settings_.playPanelTab == tab) || extraActive;
        if (IconButton(id, icon, tip, active, 40.0f)) {
            if (playPanelOpen_ && settings_.playPanelTab == tab) {
                playPanelOpen_ = false;
            } else {
                playPanelOpen_ = true;
                settings_.playPanelTab = tab;
                settings_.Save(settingsPath_);
            }
        }
        x += Dp(44.0f);
    };
    if (!s->characterId.empty()) {   // shader pack (saved per character; render/ShaderPack.h)
        const PlayPerformerRef shown = PlayPerformerAt(playEditPerformer_);
        const bool hasPack = shown.gpu && !PlayShaderChoice(*shown.gpu, shown.id).pack.empty();
        panelEntry("##shader", icon::Diamond, Tr("셰이더"), hasPack, 1);
    }
    panelEntry("##effects", icon::Sparkle, Tr("화면 효과"), !settings_.effectStack.empty(), 0);
    panelEntry("##mmepanel", icon::Sliders, Tr("MME · Ray-MMD"),
               (!MmeGlobalEnabled() && !RayMmdGlobalEnabled()) || MmeAutoWorkflow(), 2);
    dl->AddLine(ImVec2(x, cy - Dp(14.0f)), ImVec2(x, cy + Dp(14.0f)), WithAlpha(p.ink, 0.12f));
    x += Dp(11.0f);
    ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(20.0f)));
    const bool exit = IconButton("##exit", icon::X, Tr("라이브러리로 (Esc)"), false, 40.0f);
    ImGui::End();
    DrawPlayFxPanel();   // the 화면 효과 side panel (slides in from the right; keeps the bar visible)
    if (exit) {
        UnloadScene();
        screen_ = Screen::Select;
    }
}

} // namespace mmdx
