// MME / Ray-MMD settings screen (Screen::Mme, the "MME" top tab) and the per-model MME list: the global
// switch, the workflow (고급 수동 / 완전 자동), the MME post stack and every library model's MME object effect
// choice + material source. The play-mode "화면 효과" panel's MME tab (UiPlayPanel.cpp) draws the same content as a draft.
#include "app/App.h"

#include <algorithm>

#include "app/Icons.h"
#include "app/UiKit.h"
#include "asset/MmeHints.h"
#include "asset/PmxModel.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace mmdx {

using namespace ui;

namespace {
constexpr float kAppBarH = 64.0f;   // as UiSelect.cpp
constexpr float kPad = 28.0f;
constexpr float kPanelW = 460.0f;

void Para(const std::string& text, ImU32 col, float sizePx = size::Small, Font font = Font::Regular) {
    if (text.empty()) return;
    PushFont(font, sizePx);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    PopFont();
}

// The resolved status of one model (the read-only list rows in Auto mode / with the switch off).
std::string MmeModeLabel(const MmeChoice& c) {
    switch (c.mode) {
        case MmeChoice::Auto: return Tr("모델 동봉 효과");
        case MmeChoice::File: return PathToUtf8(c.file.filename());
        default: return Tr("없음");
    }
}

// The Auto-skip caption ("자동 제외 · 컨트롤러 필요: <name>") under a model's resolved status. Manual rows show it as a
// warning (the choice still applies: the user picked it); Auto rows show it as the skip reason.
void MmeSkipCaption(const std::string& controller, ImU32 col, float sizePx = size::Caption) {
    if (controller.empty()) return;
    const std::string line = std::string(Tr("자동 제외 · 컨트롤러 필요: ")) + controller;
    Para(line, col, sizePx);
}
} // namespace

// ---- the MME / Ray-MMD settings screen ------------------------------------------------------------------------------

App::MmeDraft App::CaptureMmeDraft() const {
    MmeDraft d;
    d.valid = true;
    d.enabled = settings_.mmeEnabled;
    d.workflow = settings_.mmeWorkflow;
    d.mmeEffects = settings_.mmeEffects;
    d.characterMmes = settings_.characterMmes;
    d.stageMmes = settings_.stageMmes;
    d.characterMaterials = settings_.characterMaterials;
    return d;
}

void App::WriteMmeDraft(const MmeDraft& d) {
    settings_.mmeEnabled = d.enabled;
    settings_.mmeWorkflow = d.workflow;
    settings_.mmeEffects = d.mmeEffects;
    // prune the "no choice" entries so the ini stays as the setters would write it
    settings_.characterMmes.clear();
    for (const auto& [id, c] : d.characterMmes)
        if (c.mode != MmeChoice::None) settings_.characterMmes[id] = c;
    static const MmeChoice kStageDefault{MmeChoice::Auto, {}};
    settings_.stageMmes.clear();
    for (const auto& [id, c] : d.stageMmes)
        if (!(c == kStageDefault)) settings_.stageMmes[id] = c;
    settings_.characterMaterials.clear();
    for (const auto& [id, c] : d.characterMaterials)
        if (c.ray) settings_.characterMaterials[id] = c;
    settings_.Save(settingsPath_);
}

void App::SyncPlayMmeDraft() {
    playMmeDraft_ = CaptureMmeDraft();
}

void App::DrawMme() {
    BeginScreen("##mme");
    DrawAppBar(2);
    const Palette& p = P();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float pad = Dp(kPad), gap = Dp(24.0f);
    const float top = Dp(kAppBarH) + pad;
    // two columns that share the width: settings on the left, the per-model list fills the rest
    const float leftX = pad;
    const float leftW = std::clamp((ds.x - pad * 2.0f - gap) * 0.42f, Dp(320.0f), Dp(560.0f));
    const float panelX = leftX + leftW + gap;
    const float panelW = std::clamp(ds.x - pad - panelX, Dp(280.0f), Dp(760.0f));

    const bool enabled = MmeGlobalEnabled();
    const bool autoWf = MmeAutoWorkflow();

    // ---- header
    {
        const ImVec2 c(leftX, top);
        Text(ImGui::GetWindowDrawList(), Font::Bold, size::Heading, c, p.ink, Tr("MME · Ray-MMD"));
    }

    // ---- left column: global switch, workflow, post stack
    const float colTop = top + Dp(46.0f);
    ImGui::SetCursorScreenPos(ImVec2(leftX, colTop));
    ImGui::BeginChild("##mmeleft", ImVec2(leftW, ds.y - colTop - pad), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    {
        SectionLabel(Tr("전역 설정"));
        bool on = enabled;
        // The switch edits the saved setting; a run-wide --mme-global override just shows disabled.
        if (Switch("##mme_global", Tr("MME 사용"), &on, Tr("끄면 모든 MME / Ray-MMD 기능이 꺼집니다"))) {
            settings_.mmeEnabled = on;
            settings_.Save(settingsPath_);
            ApplyRenderSettings();
        }
        if (options_.mmeGlobal >= 0) {
            PushFont(Font::Regular, size::Caption);
            ImGui::TextDisabled("%s", Tr("명령줄 설정이 이번 실행 동안 우선합니다"));
            PopFont();
        }
        Gap(16.0f);

        SectionLabel(Tr("워크플로"));
        // two option cards: 고급 수동 / 완전 자동
        {
            const float w = ImGui::GetContentRegionAvail().x;
            const float cw = (w - Dp(12.0f)) * 0.5f;
            const float ch = Dp(74.0f);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const auto card = [&](const char* id, int index, const char* title, const char* desc) {
                const ImVec2 a(origin.x + index * (cw + Dp(12.0f)), origin.y);
                const ImVec2 b(a.x + cw, a.y + ch);
                bool hovered = false;
                const bool clicked = CardItem(id, a, b, &hovered, title);
                const bool sel = settings_.mmeWorkflow == index;
                const float hv = Anim(ImGui::GetID(id), hovered && enabled);
                const float fade = enabled ? 1.0f : 0.4f;   // the switch is off: the cards read as disabled
                dl->AddRectFilled(a, b, WithAlpha(sel ? p.accentSoft : Mix(p.surface, p.sunken, 0.3f + 0.5f * hv), fade),
                                  Dp(12.0f));
                dl->AddRect(a, b, WithAlpha(sel ? p.accent : Mix(p.line, p.lineStrong, hv), fade), Dp(12.0f), 0,
                            sel ? Dp(2.0f) : Dp(1.0f));
                if (sel) Icon(dl, icon::Check, 14.0f, ImVec2(b.x - Dp(16.0f), a.y + Dp(16.0f)), WithAlpha(p.accentInk, fade));
                Text(dl, Font::Semibold, size::Small, ImVec2(a.x + Dp(14.0f), a.y + Dp(12.0f)),
                     WithAlpha(sel ? p.ink : p.ink2, fade), title);
                TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(a.x + Dp(14.0f), a.y + Dp(38.0f)),
                             b.x - Dp(14.0f), WithAlpha(p.ink3, fade), desc);
                return clicked && enabled;
            };
            if (card("##mme_workflow_manual", 0, Tr("고급 수동"),
                     Tr("모델마다 저장한 MME · 재질 설정을 그대로 씁니다"))) {
                settings_.mmeWorkflow = 0;
                settings_.Save(settingsPath_);
                ApplyRenderSettings();
            }
            if (card("##mme_workflow_auto", 1, Tr("완전 자동"),
                     Tr("모든 모델에 자동 효과 · Ray 재질을 적용합니다"))) {
                settings_.mmeWorkflow = 1;
                settings_.Save(settingsPath_);
                ApplyRenderSettings();
            }
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + ch));
            ImGui::Dummy(ImVec2(w, 0));
        }
        if (options_.mmeWorkflow >= 0) {
            PushFont(Font::Regular, size::Caption);
            ImGui::TextDisabled("%s", Tr("명령줄 설정이 이번 실행 동안 우선합니다"));
            PopFont();
        }
        if (MmeCliOverrideActive()) {
            PushFont(Font::Regular, size::Caption);
            ImGui::TextDisabled("%s", Tr("이번 실행은 --mme-workflow / --mme-global 이 우선합니다"));
            PopFont();
        }
        Gap(16.0f);

        ImGui::BeginDisabled(!enabled);
        SectionLabel(Tr("MME 효과 (후처리 스택)"));
        if (DrawMmePostStackEditor(settings_.mmeEffects, false)) {
            settings_.Save(settingsPath_);
            ApplyRenderSettings();
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    // ---- right panel: per-model list
    ImGui::SetCursorScreenPos(ImVec2(panelX, top));
    ImGui::BeginChild("##mmemodels", ImVec2(panelW, ds.y - top - pad), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(dl, Font::Semibold, size::Body, c, p.ink, Tr("모델별 MME · 재질"));
        ImGui::Dummy(ImVec2(w, Dp(28.0f)));
        if (!enabled) {
            Para(Tr("MME가 꺼져 있습니다. 켜면 모델별 설정을 편집할 수 있습니다."), p.warn, size::Small);
        } else if (autoWf) {
            Para(Tr("완전 자동: 모든 모델이 자동 효과 · Ray 재질을 씁니다. 저장한 설정은 그대로 남습니다."), p.ink3,
                 size::Small);
        } else {
            Para(Tr("모델마다 어떤 MME 효과와 재질 데이터를 쓸지 정합니다."), p.ink3, size::Small);
        }
        Gap(8.0f);

        SearchField("##mmesearch", mmeFilter_, sizeof(mmeFilter_), Tr("이름으로 검색"), w / Dpi());
        Gap(8.0f);
        {
            const char* labels[] = {Tr("캐릭터"), Tr("스테이지")};
            int mode = mmeScreenShowStages_ ? 1 : 0;
            if (Segmented("##mmetab", labels, 2, &mode, w / Dpi(), 32.0f)) mmeScreenShowStages_ = mode == 1;
        }
        Gap(8.0f);

        const std::string needle = ToLowerAscii(mmeFilter_);
        const auto matches = [&](const std::string& name, const std::string& id) {
            if (needle.empty()) return true;
            return ToLowerAscii(name).find(needle) != std::string::npos ||
                   ToLowerAscii(id).find(needle) != std::string::npos;
        };

        ImGui::BeginChild("##mmelist", ImVec2(w, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground);
        const float lw = ImGui::GetContentRegionAvail().x - Dp(4.0f);   // row width inside the scrollbar
        if (!mmeScreenShowStages_) {
            for (size_t i = 0; i < library_.characters.size(); ++i) {
                const CharacterAsset& ch = library_.characters[i];
                if (!matches(ch.displayName, ch.id)) continue;
                ImGui::PushID((int)i);
                // header row (name + kind)
                {
                    ImDrawList* ldl = ImGui::GetWindowDrawList();
                    const ImVec2 h = ImGui::GetCursorScreenPos();
                    TextEllipsis(ldl, Font::Semibold, size::Small, h, h.x + lw, p.ink, ch.displayName.c_str());
                    ImGui::Dummy(ImVec2(lw, Dp(22.0f)));
                }
                ImGui::BeginDisabled(!enabled || autoWf);
                if (enabled && !autoWf) {
                    MmeChoice mme = settings_.CharacterMme(ch.id);
                    if (DrawMmeRow("##mmemodel", mme, ch.id, ch.modelPath, lw)) {
                        settings_.SetCharacterMme(ch.id, mme);
                        settings_.Save(settingsPath_);
                    }
                    if (mme.mode == MmeChoice::Auto)
                        if (ImGui::IsRectVisible(ImVec2(lw, Dp(20.0f)))) MmeSkipCaption(MmeControllerSkip(ch.modelPath), p.warn);
                } else {
                    const MmeChoice mme = ResolveMmeChoice(ch.id, /*isStage=*/false, ch.modelPath);
                    Text(ImGui::GetWindowDrawList(), Font::Regular, size::Small, ImGui::GetCursorScreenPos(),
                         p.ink2, (std::string(Tr("MME: ")) + MmeModeLabel(mme)).c_str());
                    ImGui::Dummy(ImVec2(lw, Dp(22.0f)));
                    if (mme.mode == MmeChoice::Auto)
                        if (ImGui::IsRectVisible(ImVec2(lw, Dp(20.0f)))) MmeSkipCaption(MmeControllerSkip(ch.modelPath), p.warn);
                }
                ImGui::EndDisabled();
                // P5: material source, only when the model has options
                if (enabled && !autoWf) {
                    MaterialChoice material = settings_.CharacterMaterial(ch.id);
                    if (HasMaterialOptions(ch.id, ch.modelPath, (int)ch.materialCount, material)) {
                        if (DrawMaterialSelector("##mmemat", material, ch.id, ch.modelPath, (int)ch.materialCount, lw)) {
                            settings_.SetCharacterMaterial(ch.id, material);
                            settings_.Save(settingsPath_);
                        }
                    }
                } else {
                    const MaterialChoice material = ResolveMaterialChoice(ch.id, ch.modelPath);
                    if (HasMaterialOptions(ch.id, ch.modelPath, (int)ch.materialCount, material)) {
                        Text(ImGui::GetWindowDrawList(), Font::Regular, size::Small, ImGui::GetCursorScreenPos(),
                             p.ink3, (std::string(Tr("재질: ")) + (material.ray ? "Ray-MMD" : Tr("기본"))).c_str());
                        ImGui::Dummy(ImVec2(lw, Dp(20.0f)));
                    }
                }
                Gap(14.0f);
                ImGui::PopID();
            }
        } else {
            for (size_t i = 0; i < library_.stages.size(); ++i) {
                const StageAsset& st = library_.stages[i];
                if (st.parts.empty() || !matches(st.displayName, st.id)) continue;
                ImGui::PushID((int)i + 100000);
                {
                    ImDrawList* ldl = ImGui::GetWindowDrawList();
                    const ImVec2 h = ImGui::GetCursorScreenPos();
                    TextEllipsis(ldl, Font::Semibold, size::Small, h, h.x + lw, p.ink, st.displayName.c_str());
                    ImGui::Dummy(ImVec2(lw, Dp(22.0f)));
                }
                ImGui::BeginDisabled(!enabled || autoWf);
                if (enabled && !autoWf) {
                    MmeChoice mme = settings_.StageMme(st.id);
                    if (DrawMmeRow("##mmemodel", mme, st.id, st.parts[0], lw)) {
                        settings_.SetStageMme(st.id, mme);
                        settings_.Save(settingsPath_);
                    }
                    if (mme.mode == MmeChoice::Auto)
                        if (ImGui::IsRectVisible(ImVec2(lw, Dp(20.0f)))) MmeSkipCaption(MmeControllerSkip(st.parts[0]), p.warn);
                } else {
                    const MmeChoice mme = ResolveMmeChoice(st.id, /*isStage=*/true, st.parts[0]);
                    Text(ImGui::GetWindowDrawList(), Font::Regular, size::Small, ImGui::GetCursorScreenPos(),
                         p.ink2, (std::string(Tr("MME: ")) + MmeModeLabel(mme)).c_str());
                    ImGui::Dummy(ImVec2(lw, Dp(22.0f)));
                    if (mme.mode == MmeChoice::Auto)
                        if (ImGui::IsRectVisible(ImVec2(lw, Dp(20.0f)))) MmeSkipCaption(MmeControllerSkip(st.parts[0]), p.warn);
                }
                ImGui::EndDisabled();
                const int stageMaterialCount = ModelMaterialCount(st.id, st.parts[0]);
                if (enabled && !autoWf) {
                    MaterialChoice material = settings_.CharacterMaterial(st.id);
                    if (HasMaterialOptions(st.id, st.parts[0], stageMaterialCount, material)) {
                        if (DrawMaterialSelector("##mmemat", material, st.id, st.parts[0], stageMaterialCount, lw)) {
                            settings_.SetCharacterMaterial(st.id, material);
                            settings_.Save(settingsPath_);
                        }
                    }
                } else {
                    const MaterialChoice material = ResolveMaterialChoice(st.id, st.parts[0]);
                    if (HasMaterialOptions(st.id, st.parts[0], stageMaterialCount, material)) {
                        Text(ImGui::GetWindowDrawList(), Font::Regular, size::Small, ImGui::GetCursorScreenPos(),
                             p.ink3, (std::string(Tr("재질: ")) + (material.ray ? "Ray-MMD" : Tr("기본"))).c_str());
                        ImGui::Dummy(ImVec2(lw, Dp(20.0f)));
                    }
                }
                Gap(14.0f);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    EndScreen();
}

// ---- the play-mode "화면 효과" panel: the MME · Ray-MMD tab --------------------------------------------------------

bool App::PlayMmeDraftDirty() const {
    const MmeDraft& d = playMmeDraft_;
    return !(d.enabled == settings_.mmeEnabled && d.workflow == settings_.mmeWorkflow && d.mmeEffects == settings_.mmeEffects &&
             d.characterMmes == settings_.characterMmes && d.stageMmes == settings_.stageMmes &&
             d.characterMaterials == settings_.characterMaterials);
}

// The MME · Ray-MMD tab body (drawn inside the panel's scrolling body child). The caller keeps the draft valid.
void App::DrawPlayMmeTab(float w) {
    using namespace ui;
    if (!scene_) return;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    MmeDraft& draft = playMmeDraft_;
    const bool autoWf = draft.workflow == 1;

    // global switch + workflow (draft)
    bool on = draft.enabled;
    if (Switch("##mme_global", Tr("MME 사용"), &on, Tr("끄면 모든 MME / Ray-MMD 기능이 꺼집니다"))) {
        draft.enabled = on;
    }
    Gap(10.0f);
    SectionLabel(Tr("워크플로"));
    {
        const float cw = (w - Dp(12.0f)) * 0.5f;
        const float ch = Dp(66.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const auto card = [&](const char* id, int index, const char* title, const char* desc) {
            const ImVec2 a(origin.x + index * (cw + Dp(12.0f)), origin.y);
            const ImVec2 b(a.x + cw, a.y + ch);
            bool hovered = false;
            const bool clicked = CardItem(id, a, b, &hovered, title);
            const bool sel = draft.workflow == index;
            const float hv = Anim(ImGui::GetID(id), hovered && draft.enabled);
            dl->AddRectFilled(a, b, sel ? p.accentSoft : Mix(p.surface, p.sunken, 0.3f + 0.5f * hv), Dp(12.0f));
            dl->AddRect(a, b, sel ? p.accent : Mix(p.line, p.lineStrong, hv), Dp(12.0f), 0, sel ? Dp(2.0f) : Dp(1.0f));
            if (sel) Icon(dl, icon::Check, 14.0f, ImVec2(b.x - Dp(16.0f), a.y + Dp(14.0f)), p.accentInk);
            TextEllipsis(dl, Font::Semibold, size::Small, ImVec2(a.x + Dp(12.0f), a.y + Dp(10.0f)), b.x - Dp(12.0f),
                         sel ? p.ink : p.ink2, title);
            TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(a.x + Dp(12.0f), a.y + Dp(34.0f)), b.x - Dp(12.0f),
                         p.ink3, desc);
            return clicked && draft.enabled;
        };
        if (card("##mme_workflow_manual", 0, Tr("고급 수동"), Tr("모델마다 저장한 설정을 그대로"))) draft.workflow = 0;
        if (card("##mme_workflow_auto", 1, Tr("완전 자동"), Tr("모든 모델에 자동 효과 · Ray 재질"))) draft.workflow = 1;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + ch));
        ImGui::Dummy(ImVec2(w, 0));
    }
    if (MmeCliOverrideActive()) {
        PushFont(Font::Regular, size::Caption);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
        ImGui::TextWrapped("%s", Tr("이번 실행은 --mme-workflow / --mme-global 이 우선합니다"));
        ImGui::PopStyleColor();
        PopFont();
    }
    Gap(10.0f);

    ImGui::BeginDisabled(!draft.enabled);
    if (autoWf) Para(Tr("완전 자동: 모든 모델이 자동 효과 · Ray 재질을 씁니다."), p.ink3, size::Caption);

    const auto modelRow = [&](const char* id, const std::string& label, const std::string& modelId,
                              const std::filesystem::path& modelPath, int materialCount, bool isStage) {
        SectionLabel(label.empty() ? Tr("(이름 없음)") : label.c_str());
        if (!draft.enabled || autoWf) {
            const MmeChoice mme = draft.enabled
                                      ? ResolveMmeChoiceFor(isStage ? settings_.StageMme(modelId)
                                                                    : settings_.CharacterMme(modelId),
                                                            isStage, modelPath)
                                      : MmeChoice{};
            Text(dl, Font::Regular, size::Small, ImGui::GetCursorScreenPos(), p.ink2,
                 (std::string(Tr("MME: ")) + (mme.mode == MmeChoice::Auto ? Tr("모델 동봉 효과")
                                          : mme.mode == MmeChoice::File ? PathToUtf8(mme.file.filename())
                                                                        : Tr("없음"))).c_str());
            ImGui::Dummy(ImVec2(w, Dp(20.0f)));
            if (draft.enabled && mme.mode == MmeChoice::Auto)
                MmeSkipCaption(MmeControllerSkip(modelPath), p.warn, size::Small);
            const MaterialChoice mc = ResolveMaterialChoiceFrom(draft.characterMaterials, draft.enabled, autoWf,
                                                               modelId, modelPath);
            if (HasMaterialOptions(modelId, modelPath, materialCount, mc)) {
                Text(dl, Font::Regular, size::Small, ImGui::GetCursorScreenPos(), p.ink3,
                     (std::string(Tr("재질: ")) + (mc.ray ? "Ray-MMD" : Tr("기본"))).c_str());
                ImGui::Dummy(ImVec2(w, Dp(18.0f)));
            }
        } else {
            const auto stIt = draft.stageMmes.find(modelId);
            const auto chIt = draft.characterMmes.find(modelId);
            MmeChoice mme = isStage ? (stIt != draft.stageMmes.end() ? stIt->second : settings_.StageMme(modelId))
                                    : (chIt != draft.characterMmes.end() ? chIt->second
                                                                         : settings_.CharacterMme(modelId));
            if (DrawMmeRow((std::string(id) + "_mme").c_str(), mme, modelId, modelPath, w)) {
                if (isStage) draft.stageMmes[modelId] = mme;
                else draft.characterMmes[modelId] = mme;
            }
            if (mme.mode == MmeChoice::Auto)
                MmeSkipCaption(MmeControllerSkip(modelPath), p.warn);
            if (modelId.empty() || modelPath.empty()) { Gap(12.0f); return; }
            MaterialChoice material = draft.characterMaterials.count(modelId)
                                          ? draft.characterMaterials[modelId]
                                          : settings_.CharacterMaterial(modelId);
            if (HasMaterialOptions(modelId, modelPath, materialCount, material)) {
                if (DrawMaterialSelector((std::string(id) + "_mat").c_str(), material, modelId, modelPath,
                                         materialCount, w)) {
                    draft.characterMaterials[modelId] = material;
                }
            }
        }
        Gap(12.0f);
    };

    // center performer
    if (scene_->character) {
        const std::string label = scene_->characterDisplayName.empty() ? scene_->characterId
                                                                      : scene_->characterDisplayName;
        modelRow("##mmepp_center", label + " " + Tr("(센터)"), scene_->characterId,
                 scene_->character->Model().sourcePath, (int)scene_->character->Model().materials.size(), false);
    }
    // extra performers
    for (size_t i = 0; i < scene_->extras.size(); ++i) {
        SceneRuntime::Performer& e = scene_->extras[i];
        if (!e.instance) continue;
        ImGui::PushID((int)i);
        const std::string label = e.displayName.empty() ? e.characterId : e.displayName;
        modelRow("##mmepp_extra", label, e.characterId, e.instance->Model().sourcePath,
                 (int)e.instance->Model().materials.size(), false);
        ImGui::PopID();
    }
    // stages
    for (size_t i = 0; i < scene_->stages.size(); ++i) {
        if (i >= scene_->stageGpu.size()) break;
        ImGui::PushID((int)i + 100000);
        const std::string label = scene_->stages.size() > 1
                                      ? std::string(Tr("스테이지 ")) + std::to_string(i + 1)
                                      : std::string(Tr("스테이지"));
        modelRow("##mmepp_stage", label, scene_->stageId, scene_->stages[i]->Model().sourcePath,
                 (int)scene_->stages[i]->Model().materials.size(), true);
        ImGui::PopID();
    }

    Gap(6.0f);
    SectionLabel(Tr("MME 효과 (후처리 스택)"));
    DrawMmePostStackEditor(draft.mmeEffects, false);
    ImGui::EndDisabled();
}

// The panel's sticky footer on the MME tab: 되돌리기 / 적용.
void App::DrawPlayMmeFooter(float w) {
    using namespace ui;
    if (!scene_) return;
    MmeDraft& draft = playMmeDraft_;
    // does applying need a scene reload? compare each loaded model's resolved material source before / after
    const bool autoAfter = draft.workflow == 1;
    bool reload = false;
    const auto checkMat = [&](const std::string& id, const std::filesystem::path& path) {
        if (path.empty()) return;
        const MaterialChoice before = ResolveMaterialChoice(id, path);
        const MaterialChoice after =
            ResolveMaterialChoiceFrom(draft.characterMaterials, draft.enabled, autoAfter, id, path);
        if (!(before == after)) reload = true;
    };
    if (scene_->character) checkMat(scene_->characterId, scene_->character->Model().sourcePath);
    for (auto& e : scene_->extras) if (e.instance) checkMat(e.characterId, e.instance->Model().sourcePath);
    for (auto& st : scene_->stages) checkMat(scene_->stageId, st->Model().sourcePath);

    const bool dirty = PlayMmeDraftDirty();
    const char* applyLabel = reload ? Tr("적용 (씬 다시 로드)") : Tr("적용");
    const float bw = (w - Dp(10.0f)) * 0.5f;
    ImGui::BeginDisabled(!dirty);
    if (Button("##mme_revert", Tr("되돌리기"), nullptr, ButtonKind::Secondary, ImVec2(bw / Dpi(), 38.0f))) {
        SyncPlayMmeDraft();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, Dp(10.0f));
    ImGui::BeginDisabled(!dirty);
    if (Button("##mme_apply", applyLabel, nullptr, ButtonKind::Primary, ImVec2(bw / Dpi(), 38.0f))) {
        ApplyPlayMmeDraft(draft, reload);
        SyncPlayMmeDraft();
    }
    ImGui::EndDisabled();
}

void App::ApplyPlayMmeDraft(const MmeDraft& draft, bool reloadNeeded) {
    WriteMmeDraft(draft);
    if (reloadNeeded) {
        playPanelReopen_ = true;
        ReloadPlaySceneForMaterial();   // keeps time, play/pause and the formation
    } else {
        ApplyRenderSettings();   // MME post stack; the object choices apply per frame in UpdateScene
    }
}

} // namespace mmdx
