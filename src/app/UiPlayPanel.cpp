// Play-mode "화면 효과" side panel: one wide panel that slides in from the right edge while a play scene runs, with
// three tabs — 효과 (screen effects, applied live), 셰이더 (the per-performer shader choice) and MME · Ray-MMD
// (the previous play panel: a draft that only applies on 적용). Opened from the play bar (##effects / ##shader / the
// MME button). The effect block is shared with the lobby's 세부 tab (DrawScreenEffectControls).
#include "app/App.h"

#include <algorithm>
#include <cmath>

#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui_probe/UiProbe.h"

namespace mmdx {

using namespace ui;

// ---- shared screen-effect block (the play panel's 효과 tab and the lobby 세부 tab) -----------------------------------

void App::DrawScreenEffectControls(float width) {
    const float chipW = (width / Dpi() - 8.0f) * 0.5f;
    // bloom / auto-luminous leave the named graphics presets (as the lobby's toggles do); the rest is per-effect.
    bool preset = false;
    preset |= Switch("##bloom", Tr("블룸"), &settings_.bloom);
    bool bloomFx = false;
    DrawBloomControls(width, &bloomFx);
    preset |= Switch("##autolum", Tr("오토루미너스 발광"), &settings_.autoLuminous,
                     Tr("AutoLuminous 규격 재질의 자체 발광"));
    Gap(14.0f);
    SectionLabel(Tr("효과"));
    bool fx = bloomFx;
    fx |= Switch("##dof", Tr("피사계 심도"), &settings_.dof, Tr("캐릭터에 초점을 맞추고 배경을 흐림"));
    if (settings_.dof) { Gap(6.0f); fx |= SliderRow("##dofap", Tr("조리개"), &settings_.dofAperture, 0.2f, 3.0f, "%.2f"); Gap(6.0f); }
    fx |= Switch("##vol", Tr("볼류메트릭 라이트"), &settings_.volumetric, Tr("빛줄기와 조명 산란"));
    if (settings_.volumetric) { Gap(6.0f); fx |= SliderRow("##vold", Tr("안개 밀도"), &settings_.volumetricDensity, 0.25f, 4.0f, "%.2f"); Gap(6.0f); }
    // DLSS Frame Generation (play scene only; the panel is only drawn in play mode).
    {
        const bool fgOk = renderer_.FrameGenAvailable();
        const int fgMax = std::max(2, renderer_.FrameGenMaxMultiplier());
        Gap(10.0f);
        SectionLabel(Tr("프레임 생성"));
        const char* fgLabels[] = {Tr("끔"), "2x", "3x", "4x"};
        bool fgDisabled[4] = {false, !fgOk, !fgOk || fgMax < 3, !fgOk || fgMax < 4};
        const char* fgTips[4] = {nullptr, nullptr, nullptr, nullptr};
        const char* cap = fgOk ? Tr("이 GPU는 프레임 생성을 2x까지만 지원합니다") : Tr(renderer_.FrameGenReason());
        if (fgOk && fgMax < 3) fgTips[2] = cap;
        if (fgOk && fgMax < 4) fgTips[3] = cap;
        if (!fgOk) fgTips[1] = cap;
        int fg = settings_.dlssFrameGen >= 2 && fgOk ? std::clamp(settings_.dlssFrameGen - 1, 1, fgMax - 1) : 0;
        if (Segmented("##playframegen", fgLabels, 4, &fg, width / Dpi(), 34.0f, nullptr, fgDisabled, fgTips)) {
            settings_.dlssFrameGen = fg == 0 ? 0 : fg + 1;
            fx = true;
        }
        if (!fgOk) {
            PushFont(Font::Regular, size::Caption);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(P().ink3));
            ImGui::TextUnformatted(cap);
            ImGui::PopStyleColor();
            PopFont();
        }
    }
    Gap(10.0f);
    SectionLabel(Tr("컬러 LUT"));
    const int lutCount = 1 + (int)luts_.size();
    for (int i = 0; i < lutCount; ++i) {
        if (i % 2) ImGui::SameLine(0, Dp(8.0f));
        ImGui::PushID(i);
        const ColorLutEntry* e = i > 0 ? &luts_[(size_t)i - 1] : nullptr;
        if (Chip("##lut", e ? Tr(e->displayName.c_str()) : Tr("없음"), icon::Image,
                 settings_.colorLut == (e ? e->id : std::string()), chipW)) {
            settings_.colorLut = e ? e->id : std::string();
            fx = true;
        }
        ImGui::PopID();
    }
    if (!settings_.colorLut.empty()) fx |= SliderRow("##lutint", Tr("LUT 강도"), &settings_.lutIntensity, 0.0f, 1.0f, "%.2f");
    Gap(14.0f);
    SectionLabel(Tr("월드 이펙트"));
    {
        ShaderPackRegistry& reg = ShaderPacks();
        const std::string lang = PackLanguage();
        bool worldChanged = false;
        size_t worldRemove = SIZE_MAX;
        for (size_t i = 0; i < settings_.worldStack.size(); ++i) {
            EffectStackEntry& e = settings_.worldStack[i];
            const ShaderPack* pk = reg.Find(e.pack);
            const bool usable = pk && PackUsable(pk) && pk->type == PackType::World;
            const std::string name = pk ? pk->name.Get(lang) : e.pack;
            ImGui::PushID((int)i);
            bool on = e.enabled;
            if (Switch("##worldon", name.c_str(), &on)) {
                e.enabled = on;
                worldChanged = true;
            }
            ImGui::SameLine();
            if (IconButton("##worldremove", icon::X, Tr("월드에서 빼기"), false, 28.0f)) worldRemove = i;
            if (usable && e.enabled && !pk->params.empty())
                worldChanged |= DrawShaderPackParameters(*pk, e.params, "##world");
            ImGui::PopID();
            Gap(8.0f);
        }
        if (worldRemove != SIZE_MAX) {
            settings_.worldStack.erase(settings_.worldStack.begin() + (long)worldRemove);
            worldChanged = true;
        }
        std::vector<const ShaderPack*> addable;
        for (const ShaderPack& pk : reg.Packs()) {
            if (pk.type != PackType::World || !PackUsable(&pk)) continue;
            bool in = false;
            for (const EffectStackEntry& e : settings_.worldStack) in |= e.pack == pk.id;
            if (!in) addable.push_back(&pk);
        }
        if (!addable.empty()) {
            if (Button("##worldadd", Tr("월드 추가"), icon::Plus, ButtonKind::Secondary))
                ImGui::OpenPopup("##worldaddpopup");
            ImGui::SetNextWindowSize(ImVec2(Dp(300.0f), 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
            if (ImGui::BeginPopup("##worldaddpopup")) {
                for (const ShaderPack* pk : addable) {
                    const std::string rid = "##worldadd_" + pk->id;
                    if (MenuItem(rid.c_str(), pk->name.Get(lang).c_str())) {
                        settings_.worldStack.push_back({pk->id, true, {}, {}});
                        worldChanged = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();
        }
        if (settings_.worldStack.empty())
            ImGui::TextUnformatted(Tr("월드 이펙트가 없습니다. 월드를 추가하면 장면에 배치됩니다."));
        if (worldChanged) {
            settings_.Save(settingsPath_);
            ApplyRenderSettings();
        }
    }
    if (fx) {
        ApplyRenderSettings();
        settings_.Save(settingsPath_);
    }
    if (preset) {
        // touching an effect toggle leaves the named presets
        settings_.graphicsPreset = 4;
        ApplyRenderSettings();
        settings_.Save(settingsPath_);
    }
}

// Bloom detail rows shared by the play panel's 효과 tab / the lobby 세부 tab (DrawScreenEffectControls) and the studio
// effects panel (DrawStudioEffectsPanel). Only drawn while bloom is on; the convolution switch lives here too.
void App::DrawBloomControls(float width, bool* changed) {
    if (!settings_.bloom) return;
    bool c = false;
    c |= SliderRow("##bloomint", Tr("세기"), &settings_.bloomIntensity, 0.0f, 1.5f, "%.2f");
    c |= SliderRow("##bloomthr", Tr("임계값"), &settings_.bloomThreshold, 0.2f, 4.0f, "%.2f");
    const char* blendLabels[] = {Tr("더하기"), Tr("에너지 보존")};
    const char* blendTips[2] = {nullptr, Tr("빛의 총량을 지키며 번짐 · 화면이 뿌옇게 밝아지지 않음")};
    {
        int blend = std::clamp(settings_.bloomLook.blend, 0, 1);
        if (Segmented("##bloomblend", blendLabels, 2, &blend, width / Dpi(), 34.0f, nullptr, nullptr, blendTips)) {
            settings_.bloomLook.blend = blend;
            c = true;
        }
    }
    c |= SliderRow("##bloomdepth", Tr("깊이 마스크"), &settings_.bloomLook.depthMask, 0.0f, 1.0f, "%.2f");
    if (!settings_.bloomConvolution)
        c |= SliderRow("##bloomrad", Tr("반경"), &settings_.bloomLook.radius, 0.0f, 1.0f, "%.2f");
    c |= Switch("##bloomconv", Tr("컨볼루션 블룸"), &settings_.bloomConvolution, Tr("FFT 스타버스트 블룸"));
    if (settings_.bloomConvolution) {
        Gap(6.0f);
        const char* kernelLabels[] = {Tr("시네마"), Tr("별빛"), Tr("글로우"), Tr("아나모픽")};
        int preset = settings_.bloomKernelPreset >= 0 && settings_.bloomKernelPreset < kBloomKernelPresets
                         ? settings_.bloomKernelPreset
                         : -1;   // custom: no segment highlighted
        if (Segmented("##bloomkernel", kernelLabels, kBloomKernelPresets, &preset, width / Dpi(), 34.0f)) {
            settings_.bloomLook.kernel = BloomKernelPreset(preset);
            settings_.bloomKernelPreset = preset;
            c = true;
        }
        Gap(4.0f);
        if (ImGui::TreeNodeEx(Tr("커널 세부"))) {
            bool k = false;
            int rays = settings_.bloomLook.kernel.rays;
            float raysF = (float)rays;
            if (SliderRow("##bloomrays", Tr("빛줄기 수"), &raysF, 0.0f, 16.0f, "%.0f")) {
                rays = (int)std::lround(raysF) & ~1;   // even (two rays per line through the centre)
                rays = std::clamp(rays, 0, 16);
                if (rays != settings_.bloomLook.kernel.rays) {
                    settings_.bloomLook.kernel.rays = rays;
                    k = true;
                }
            }
            k |= SliderRow("##bloomrot", Tr("회전"), &settings_.bloomLook.kernel.rotation, 0.0f, 180.0f, "%.0f°");
            k |= SliderRow("##bloomspikel", Tr("빛줄기 길이"), &settings_.bloomLook.kernel.spikeLength, 0.0f, 1.0f, "%.2f");
            k |= SliderRow("##bloomspikei", Tr("빛줄기 세기"), &settings_.bloomLook.kernel.spikeIntensity, 0.0f, 1.0f, "%.2f");
            k |= SliderRow("##bloomglowr", Tr("글로우 반경"), &settings_.bloomLook.kernel.glowRadius, 0.0f, 1.0f, "%.2f");
            k |= SliderRow("##bloomglowi", Tr("글로우 세기"), &settings_.bloomLook.kernel.glowIntensity, 0.0f, 1.0f, "%.2f");
            k |= SliderRow("##bloomdisp", Tr("색 분산"), &settings_.bloomLook.kernel.dispersion, 0.0f, 1.0f, "%.2f");
            k |= SliderRow("##bloomanam", Tr("아나모픽"), &settings_.bloomLook.kernel.anamorphic, 0.0f, 1.0f, "%.2f");
            if (k) {
                settings_.bloomKernelPreset = 4;   // custom
                c = true;
            }
            if (Button("##bloomreset", Tr("기본값"), nullptr, ButtonKind::Ghost)) {
                settings_.bloomLook = BloomLook{};
                settings_.bloomIntensity = 0.35f;
                settings_.bloomThreshold = 1.1f;
                settings_.bloomKernelPreset = 0;
                c = true;
            }
            ImGui::TreePop();
        }
    }
    Gap(8.0f);
    if (c && changed) *changed = true;
}

// ---- the 셰이더 tab: the old ##shader popup body --------------------------------------------------------------------

void App::DrawPlayShaderTab(float width) {
    SceneRuntime* s = scene_.get();
    if (!s || s->characterId.empty()) {
        PushFont(Font::Regular, size::Small);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(P().ink3));
        ImGui::TextWrapped("%s", Tr("이 장면에는 셰이더를 고를 캐릭터가 없습니다"));
        ImGui::PopStyleColor();
        PopFont();
        return;
    }
    const bool hasExtras = !s->extras.empty();
    if (hasExtras) {   // performer selector, shared with the size popup (same state)
        SectionLabel(Tr("캐릭터"));
        const int cur = playEditPerformer_;
        std::string curLabel = cur == 0 ? s->characterDisplayName : s->extras[(size_t)(cur - 1)].displayName;
        if (cur == 0) curLabel += " " + std::string(Tr("(센터)"));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        PushFont(Font::Regular, size::Small);
        // the app's quiet caret instead of ImGui's filled arrow box (as the other dropdowns), with a popup margin
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(6.0f), Dp(6.0f)));
        const bool performerOpen = ImGui::BeginCombo("##playperformer", curLabel.c_str(), ImGuiComboFlags_NoArrowButton);
        ImGui::PopStyleVar();
        uiprobe::Info(ImGui::GetItemID(), "##playperformer", curLabel.c_str());
        {
            const ImVec2 cmax = ImGui::GetItemRectMax(), cmin = ImGui::GetItemRectMin();
            Icon(ImGui::GetWindowDrawList(), icon::CaretDown, 12.0f, ImVec2(cmax.x - Dp(12.0f), (cmin.y + cmax.y) * 0.5f),
                 P().ink3);
        }
        if (performerOpen) {
            std::string centerName =
                s->characterDisplayName.empty() ? std::string(Tr("센터")) : s->characterDisplayName;
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
    ShaderChoice choice = settings_.CharacterShader(perf.id);
    SectionLabel(Tr("표면 셰이더"));
    const ModelTraits playTraits = perf.gpu ? perf.gpu->Traits() : ModelTraits{};
    bool changed = DrawShaderSelector("##playshader", choice, width, perf.gpu ? &playTraits : nullptr);
    Gap(8.0f);
    changed |= DrawShaderPackParams(choice);
    if (!choice.pack.empty() && perf.instance) {   // per-material classes, folded away until asked for
        Gap(8.0f);
        const int n = MaterialOverrideCount(choice);
        const std::string label =
            std::string(Tr("재질별 셰이딩")) + (n ? "  ·  " + std::to_string(n) + Tr("개 지정") : std::string());
        if (Button("##playmats", label.c_str(), playMaterialsOpen_ ? icon::CaretDown : icon::CaretRight, ButtonKind::Ghost))
            playMaterialsOpen_ = !playMaterialsOpen_;
        if (playMaterialsOpen_)
            changed |= DrawMaterialShaderList("##playmats_list", choice, perf.instance->Model(),
                                              ImGui::GetContentRegionAvail().x, Dp(260.0f));
    }
    if (changed) {
        settings_.SetCharacterShader(perf.id, choice);
        settings_.Save(settingsPath_);
    }
}

// ---- the panel ------------------------------------------------------------------------------------------------------

void App::DrawPlayFxPanel() {
    ImGuiIO& io = ImGui::GetIO();
    if (!scene_) return;

    // slide: 0 closed .. 1 open, ~150 ms
    const float step = std::clamp(io.DeltaTime / 0.15f, 0.0f, 1.0f);
    playPanelT_ = std::clamp(playPanelT_ + (playPanelOpen_ ? step : -step), 0.0f, 1.0f);
    if (!playPanelOpen_ && playPanelT_ <= 0.0f) return;   // fully closed: nothing to draw

    const Palette& p = P();
    const ImVec2 ds = io.DisplaySize;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = std::min(Dp(420.0f), ds.x * 0.45f);
    const float top = vp->WorkPos.y + Dp(12.0f);
    // the play bar spans the bottom: the panel ends just above it (bar height + bottom margin, as the old panel did)
    const float bottom = ds.y - Dp(24.0f) - Dp(76.0f) - Dp(14.0f);
    const float height = std::max(Dp(240.0f), bottom - top);
    const float xOpen = ds.x - width - Dp(20.0f);
    const float xClosed = ds.x + Dp(8.0f);
    const float ease = playPanelT_ * playPanelT_ * (3.0f - 2.0f * playPanelT_);
    const float x = xClosed + (xOpen - xClosed) * ease;

    ImGui::SetNextWindowPos(ImVec2(x, top));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(16.0f), Dp(14.0f)));
    ImGui::Begin("###fxpanel", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    // a hairline edge: the panel floats over the scene, and a light scene makes an unbordered white panel hard to read
    {
        const ImVec2 a = ImGui::GetWindowPos(), b(a.x + ImGui::GetWindowSize().x, a.y + ImGui::GetWindowSize().y);
        dl->AddRect(a, b, p.line, ImGui::GetStyle().WindowRounding);
    }

    // header: title + close
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(dl, Font::Semibold, size::Title, c, p.ink, Tr("화면 효과"));
        ImGui::SetCursorScreenPos(ImVec2(c.x + w - Dp(28.0f), c.y - Dp(4.0f)));
        if (IconButton("##fxpanel_close", icon::X, Tr("닫기"), false, 28.0f)) playPanelOpen_ = false;
        ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(30.0f)));
    }

    // tabs
    int tab = std::clamp(settings_.playPanelTab, 0, 2);
    {
        std::string labels[3] = {Tr("효과"), Tr("셰이더"), std::string("MME · Ray-MMD")};
        if (playMmeDraft_.valid && PlayMmeDraftDirty()) labels[2] += " •";   // pending draft
        const char* tabPtr[3] = {labels[0].c_str(), labels[1].c_str(), labels[2].c_str()};
        if (Segmented("##fxpanel_tabs", tabPtr, 3, &tab, w / Dpi(), 36.0f)) {
            settings_.playPanelTab = tab;
            settings_.Save(settingsPath_);
        }
        Gap(12.0f);
    }

    const float footerH = tab == 2 ? Dp(52.0f) : 0.0f;
    ImGui::BeginChild("##fxpanel_body", ImVec2(0, -footerH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    {
        const float bw = ImGui::GetContentRegionAvail().x;
        if (tab == 2) {
            if (!playMmeDraft_.valid) SyncPlayMmeDraft();
            DrawPlayMmeTab(bw);
        } else if (tab == 1) {
            DrawPlayShaderTab(bw);
        } else {
            // the effect pack stack, live (same save / ApplyRenderSettings as the lobby / old play popup)
            if (DrawEffectStackEditor(settings_.effectStack)) {
                settings_.Save(settingsPath_);
                ApplyRenderSettings();
            }
            Gap(16.0f);
            DrawScreenEffectControls(bw);
        }
    }
    ImGui::EndChild();

    if (tab == 2) {
        Gap(8.0f);
        DrawPlayMmeFooter(w);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace mmdx
