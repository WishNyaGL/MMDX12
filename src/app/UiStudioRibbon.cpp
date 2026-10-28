// Studio ribbon: a header row (back / title / project / tabs / undo, redo, theme, help, MCP, collapse, save) over a
// tool row of grouped, labelled buttons (one group set per tab). It replaces the old flat top bar and calls the same
// App functions the transport, viewport overlay, outliner and menus call, so nothing is duplicated.
#include "app/App.h"
#include "ui_probe/UiProbe.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "app/Icons.h"
#include "app/Lighting.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "studio/SceneLight.h"
#include "studio/StudioDoc.h"
#include "studio/StudioProject.h"

namespace mmdx {

using namespace studio;

namespace {

// App::kRibbonHeaderH / kRibbonToolsH live in App.h (UiStudio.cpp sizes the top window with them).
constexpr float kRibbonHeaderH = App::kRibbonHeaderH, kRibbonToolsH = App::kRibbonToolsH;
constexpr float kRibbonButtonH = 52.0f;

// One tab of the header row: id string (script / MCP) and its visible label (Korean source text for Tr()).
struct RibbonTab {
    const char* id;
    const char* label;
};
const RibbonTab kRibbonTabs[] = {
    {"home", "홈"},
    {"anim", "애니메이션"},
    {"camera", "카메라"},
    {"light", "조명"},
    {"render", "렌더"},
    {"view", "보기"},
};
constexpr int kRibbonTabCount = 6;

// A large ribbon button: an icon over a caption (60 x 52, wider when the caption needs it). `active` marks a toggle
// that is on, `menu` a button that opens a popup (a small caret after the caption). A disabled button draws at 40 %
// and shows no hover fill, but its tooltip still says why.
bool RibbonButton(const char* id, const char* icon, const char* label, const char* tooltip, bool enabled = true,
                  bool active = false, bool menu = false) {
    using namespace ui;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (w->SkipItems) return false;
    const ImGuiID gid = w->GetID(id);
    const ImVec2 ls = label && *label ? TextSize(Font::Regular, size::Caption, label) : ImVec2(0, 0);
    const float caretW = menu ? Dp(12.0f) : 0.0f;
    const float bw = std::max(Dp(60.0f), ls.x + caretW + Dp(16.0f));
    const float bh = Dp(kRibbonButtonH);
    const ImRect bb(w->DC.CursorPos, ImVec2(w->DC.CursorPos.x + bw, w->DC.CursorPos.y + bh));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    uiprobe::Info(gid, id, label);
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    // BeginDisabled (a modal over the studio) must swallow the click too: ButtonBehavior does not check the flag itself.
    const bool disabled = !enabled || (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    if (hovered && !disabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float h = Anim(gid, hovered);
    const Palette& p = P();
    ImDrawList* dl = w->DrawList;
    const float r = Dp(10.0f);
    if (!disabled) {
        if (active)
            dl->AddRectFilled(bb.Min, bb.Max, Mix(p.accentSoft, p.accent, held ? 0.25f : h * 0.12f), r);
        else if (h > 0.01f || held)
            dl->AddRectFilled(bb.Min, bb.Max, WithAlpha(p.ink, held ? 0.10f : 0.06f * h), r);
    }
    const float alpha = disabled ? 0.4f : 1.0f;
    // the caption carries the meaning (ink); the icon is the secondary cue (ink2, ink on hover)
    const ImU32 iconCol = WithAlpha(disabled ? p.ink3 : (active ? p.accentInk : Mix(p.ink2, p.ink, h)), alpha);
    const ImU32 labelCol = WithAlpha(disabled ? p.ink3 : (active ? p.accentInk : p.ink), alpha);
    if (icon) Icon(dl, icon, 20.0f, ImVec2(bb.Min.x + bw * 0.5f, bb.Min.y + Dp(19.0f)), iconCol);
    if (label && *label) {
        const float lx = bb.Min.x + (bw - ls.x - caretW) * 0.5f;
        Text(dl, Font::Regular, size::Caption, ImVec2(lx, bb.Min.y + Dp(33.0f)), labelCol, label);
        if (menu)
            Icon(dl, icon::CaretDown, 10.0f, ImVec2(lx + ls.x + Dp(7.0f), bb.Min.y + Dp(33.0f) + ls.y * 0.5f), labelCol);
    }
    ImGui::RenderNavCursor(bb, gid);
    if (tooltip && hovered) Tooltip(tooltip);
    return pressed && !disabled;
}

} // namespace

void App::DrawStudioRibbon(float x0, float y0, float x1, float y1) {
    using namespace ui;
    if (!studio_) return;
    StudioDoc& d = *studio_;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), p.surface);

    const float hh = Dp(kRibbonHeaderH);
    const float cy = y0 + hh * 0.5f;
    const float iconS = Dp(36.0f);
    bool collapsed = studioRibbonCollapsed_;
    int tab = std::clamp(studioRibbonTab_, 0, kRibbonTabCount - 1);

    // --- right cluster, laid out from the right edge
    const ImVec2 saveLs = TextSize(Font::Semibold, size::Body, Tr("저장"));
    const float saveW = Dp(32.0f) + Dp(18.0f) + Dp(8.0f) + saveLs.x;
    const bool mcpConnected = mcpServer_ && mcpServer_->IsConnected();
    float clusterW = iconS + Dp(4.0f) + iconS;                 // undo / redo
    clusterW += Dp(13.0f) + 1.0f + Dp(13.0f);                  // separator
    clusterW += iconS + Dp(4.0f) + iconS;                      // theme / help
    if (mcpConnected) clusterW += Dp(8.0f) + Dp(64.0f);        // MCP badge
    clusterW += iconS + Dp(8.0f) + saveW;                      // collapse / save
    const float rx = x1 - Dp(14.0f) - clusterW;

    ImGui::SetCursorScreenPos(ImVec2(rx, cy - Dp(18.0f)));
    ImGui::BeginDisabled(!d.history.CanUndo());
    const std::string undoTip = d.history.CanUndo() ? std::string(Tr("실행 취소: ")) + d.history.UndoName() + "  (Ctrl+Z)"
                                                    : std::string(Tr("실행 취소"));
    if (IconButton("##undo", icon::ArrowCcw, undoTip.c_str())) {
        d.history.Undo();
        d.selection.clear();
        d.rowsKey = ~0ull;
    }
    ImGui::EndDisabled();
    ImGui::SetCursorScreenPos(ImVec2(rx + iconS + Dp(4.0f), cy - Dp(18.0f)));
    ImGui::BeginDisabled(!d.history.CanRedo());
    const std::string redoTip = d.history.CanRedo() ? std::string(Tr("다시 실행: ")) + d.history.RedoName() + "  (Ctrl+Y)"
                                                    : std::string(Tr("다시 실행"));
    if (IconButton("##redo", icon::ArrowCw, redoTip.c_str())) {
        d.history.Redo();
        d.selection.clear();
        d.rowsKey = ~0ull;
    }
    ImGui::EndDisabled();
    float sx = rx + 2.0f * iconS + Dp(4.0f) + Dp(13.0f);
    dl->AddLine(ImVec2(sx, cy - Dp(11.0f)), ImVec2(sx, cy + Dp(11.0f)), p.line);
    sx += 1.0f + Dp(13.0f);
    DrawThemeButton(ImVec2(sx, cy - Dp(18.0f)), sx + Dp(36.0f), y0 + hh + Dp(6.0f));
    sx += iconS + Dp(4.0f);
    ImGui::SetCursorScreenPos(ImVec2(sx, cy - Dp(18.0f)));
    if (IconButton("##help", icon::Keyboard, Tr("단축키  (?)"), studioHelpOpen_)) studioHelpOpen_ = !studioHelpOpen_;
    sx += iconS;
    if (mcpConnected) {
        sx += Dp(8.0f);
        const ImVec2 mcpPos(sx, cy - Dp(14.0f));
        dl->AddRectFilled(mcpPos, ImVec2(mcpPos.x + Dp(64.0f), mcpPos.y + Dp(28.0f)), WithAlpha(p.accent, 0.15f),
                          Dp(14.0f));
        dl->AddRect(mcpPos, ImVec2(mcpPos.x + Dp(64.0f), mcpPos.y + Dp(28.0f)), WithAlpha(p.accent, 0.4f), Dp(14.0f));
        Icon(dl, icon::Cpu, 14.0f, ImVec2(mcpPos.x + Dp(12.0f), mcpPos.y + Dp(14.0f)), p.accentInk);
        Text(dl, Font::Bold, size::Caption, ImVec2(mcpPos.x + Dp(26.0f), mcpPos.y + Dp(7.0f)), p.accentInk, "MCP");
        ImGui::SetCursorScreenPos(mcpPos);
        ImGui::InvisibleButton("##mcpbadge", ImVec2(Dp(64.0f), Dp(28.0f)));
        if (ImGui::IsItemHovered()) {
            const std::string tip = std::string(Tr("MCP 연결됨: ")) + mcpServer_->PipeName();
            Tooltip(tip.c_str());
        }
        sx += Dp(64.0f);
    }
    ImGui::SetCursorScreenPos(ImVec2(sx, cy - Dp(18.0f)));
    if (IconButton("##rb_collapse", collapsed ? icon::CaretDown : icon::CaretUp,
                   collapsed ? Tr("리본 펼치기  (Ctrl+F1)") : Tr("리본 접기  (Ctrl+F1)")))
        collapsed = !collapsed;
    sx += iconS + Dp(8.0f);
    ImGui::SetCursorScreenPos(ImVec2(sx, cy - Dp(16.0f)));
    if (Button("##save", Tr("저장"), icon::FloppyDisk, ButtonKind::Primary, ImVec2(saveW / Dpi(), 32.0f)))
        StudioSave(false);
    Tooltip(Tr("프로젝트 저장  (Ctrl+S)"));

    // --- left: back, title, project name, dirty badge
    float x = x0 + Dp(12.0f);
    ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(18.0f)));
    if (IconButton("##back", icon::ArrowLeft, Tr("라이브러리로 돌아가기"))) {
        StudioRequest(StudioAction::Leave);
        if (!studio_) return;
    }
    x += iconS + Dp(12.0f);
    Text(dl, Font::Bold, size::Title, ImVec2(x, cy - Dp(11.0f)), p.ink, Tr("스튜디오"));
    x += TextSize(Font::Bold, size::Title, Tr("스튜디오")).x + Dp(12.0f);
    dl->AddLine(ImVec2(x, cy - Dp(9.0f)), ImVec2(x, cy + Dp(9.0f)), p.line);
    x += Dp(12.0f);
    {
        const std::string title = d.projectPath.empty() ? std::string(Tr("제목 없음")) : PathToUtf8(d.projectPath.stem());
        const ImVec2 titleSize = TextSize(Font::Semibold, size::Body, title.c_str());
        const float maxX = std::max(x + Dp(60.0f), std::min(x + Dp(240.0f), rx - Dp(320.0f)));
        TextEllipsis(dl, Font::Semibold, size::Body, ImVec2(x, cy - Dp(10.0f)), maxX, p.ink2, title.c_str());
        // the hover target is the name itself (a wider one would cover the tabs and take their clicks)
        const float nameW = std::min(titleSize.x, maxX - x);
        ImGui::SetCursorScreenPos(ImVec2(x, cy - Dp(12.0f)));
        ImGui::InvisibleButton("##rbprojname", ImVec2(std::max(1.0f, nameW), Dp(24.0f)));
        if (!d.projectPath.empty() && ImGui::IsItemHovered()) Tooltip(PathToUtf8(d.projectPath).c_str());
        x = std::min(maxX, x + titleSize.x) + Dp(12.0f);
    }
    if (d.Dirty()) {
        ImVec2 bs;
        Badge(dl, ImVec2(x, cy - Dp(10.0f)), Tr("저장 안 됨"), p.warnSoft, p.warn, &bs);
        x += bs.x + Dp(12.0f);
    }
    x += Dp(12.0f);

    // --- tabs: text only, the active one in ink; ONE accent underline slides from the old tab to the new one
    float activeX = 0.0f, activeW = 0.0f;
    for (int i = 0; i < kRibbonTabCount && x < rx - Dp(24.0f); ++i) {
        char id[48];
        std::snprintf(id, sizeof(id), "##rbtab_%s", kRibbonTabs[i].id);
        const char* label = Tr(kRibbonTabs[i].label);
        const ImVec2 ts = TextSize(Font::Semibold, size::Body, label);
        const float tw = ts.x + Dp(20.0f);
        ImGui::SetCursorScreenPos(ImVec2(x, y0));
        ImGui::InvisibleButton(id, ImVec2(tw, hh));
        uiprobe::Info(ImGui::GetItemID(), id, label);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = tab == i;
        if (ImGui::IsItemClicked()) {
            if (active && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) collapsed = !collapsed;  // fold / unfold
            else {
                tab = i;
                collapsed = false;  // a tab click while collapsed expands the ribbon
            }
        }
        const ImU32 col = active || hovered ? p.ink : p.ink2;
        Text(dl, Font::Semibold, size::Body, ImVec2(x + (tw - ts.x) * 0.5f, cy - ts.y * 0.5f), col, label);
        if (tab == i) {
            activeX = x + Dp(6.0f);
            activeW = tw - Dp(12.0f);
        }
        x += tw + Dp(2.0f);
    }
    if (activeW > 0.0f) {
        // exponential ease toward the active tab (~120 ms); the first frame snaps
        static float ux = -1.0f, uw = 0.0f;
        const float k = std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
        if (ux < 0.0f) ux = activeX, uw = activeW;
        ux += (activeX - ux) * k;
        uw += (activeW - uw) * k;
        dl->AddRectFilled(ImVec2(ux, y0 + hh - Dp(3.0f)), ImVec2(ux + uw, y0 + hh), p.accent, Dp(1.5f));
    }
    if (studio_.get() != &d) return;
    if (tab != studioRibbonTab_) {
        studioRibbonTab_ = tab;
        settings_.studioRibbonTab = tab;
        settings_.Save(settingsPath_);
    }
    if (collapsed != studioRibbonCollapsed_) {
        studioRibbonCollapsed_ = collapsed;
        settings_.studioRibbonCollapsed = collapsed;
        settings_.Save(settingsPath_);
    }
    if (collapsed) {
        dl->AddLine(ImVec2(x0, y1 - 0.5f), ImVec2(x1, y1 - 0.5f), p.line);
        return;
    }

    // --- tool row: one child so a row wider than the window scrolls with the wheel
    const float ty0 = y0 + hh;
    const float toolsH = Dp(kRibbonToolsH);
    ImGui::SetCursorScreenPos(ImVec2(x0, ty0));
    ImGui::BeginChild("##ribbontools", ImVec2(x1 - x0, toolsH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::BeginDisabled(StudioModal());
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const auto screenX = [&](float local) { return wp.x - ImGui::GetScrollX() + local; };

    const char* target = d.selectedModel < 0 ? Tr("카메라") : (d.Selected() ? d.Selected()->name.c_str() : "");
    const bool hasModel = d.selectedModel >= 0 && d.Selected() != nullptr;
    const bool isCharacter = hasModel && d.Selected()->kind == ModelKind::Character;
    const bool hasKeys = !d.selection.empty();
    const bool canPaste = !d.clipboard.empty() && d.clipboardModel == d.selectedModel;

    // Actions that can replace the project (new / open / recent) run after EndChild: returning from inside the child
    // would leave the ImGui window / disabled stacks unbalanced.
    std::function<void()> post;
    float gx = Dp(16.0f);
    // One group: its controls at the top, the caption 6 px above the row's bottom, a 1 px separator after it (12 px on
    // both sides).
    bool firstGroup = true;
    const auto group = [&](const char* caption, auto&& body) {
        if (!firstGroup) {
            tdl->AddLine(ImVec2(screenX(gx), wp.y + Dp(10.0f)), ImVec2(screenX(gx), wp.y + toolsH - Dp(10.0f)), p.line);
            gx += Dp(12.0f);
        }
        firstGroup = false;
        ImGui::SetCursorPos(ImVec2(gx, 0.0f));
        ImGui::BeginGroup();
        body();
        ImGui::EndGroup();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const ImVec2 cs = TextSize(Font::Regular, size::Caption, caption);
        Text(tdl, Font::Regular, size::Caption, ImVec2(a.x + ((b.x - a.x) - cs.x) * 0.5f, wp.y + toolsH - Dp(6.0f) - cs.y),
             p.ink2, caption);   // ink3 is under 4.5:1 at caption size
        gx = (b.x - wp.x) + ImGui::GetScrollX() + Dp(12.0f);
    };
    const auto same = [] { ImGui::SameLine(0, Dp(6.0f)); };
    // Small controls sit vertically centred in the group's control area (the 52 px above the caption).
    const float controlY = Dp(11.0f);   // (52 px control area - 30 px segmented) / 2

    switch (tab) {
    case 0: {  // 홈
        group(Tr("프로젝트"), [&] {
            if (RibbonButton("##rbnew", icon::FilePlus, Tr("새로 만들기"), Tr("새 프로젝트  (Ctrl+N)")))
                post = [this] { StudioRequest(StudioAction::New); };
            same();
            if (RibbonButton("##rbopen", icon::FolderOpen, Tr("열기"), Tr("프로젝트 열기  (Ctrl+O)")))
                post = [this] { StudioRequest(StudioAction::Open); };
            same();
            if (RibbonButton("##rbrecent", icon::ClockCounterClockwise, Tr("최근"), Tr("최근 프로젝트"), true, false, true))
                ImGui::OpenPopup("##rbrecentpop");
            const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
            ImGui::SetNextWindowPos(ImVec2(rmin.x, rmax.y + Dp(4.0f)), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(Dp(300.0f), 0));
            if (ImGui::BeginPopup("##rbrecentpop")) {
                const std::vector<std::filesystem::path> recent = StudioRecentProjects();
                if (recent.empty()) {
                    PushFont(Font::Regular, size::Small);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
                    ImGui::TextUnformatted(Tr("최근 프로젝트가 없습니다"));
                    ImGui::PopStyleColor();
                    PopFont();
                }
                for (const std::filesystem::path& rp : recent) {
                    ImGui::PushID(PathToUtf8(rp).c_str());
                    // the folder name tells same-named projects apart ("project" in several folders)
                    const std::string stem = PathToUtf8(rp.stem());
                    const std::string folder = PathToUtf8(rp.parent_path().filename());
                    if (MenuItem("##rbrecentitem", stem.c_str(), icon::FolderSimple, folder.c_str())) {
                        ImGui::CloseCurrentPopup();
                        post = [this, rp] { StudioRequest(StudioAction::OpenFile, rp); };
                    }
                    Tooltip(PathToUtf8(rp).c_str());
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
            same();
            if (RibbonButton("##rbsave", icon::FloppyDisk, Tr("저장"), Tr("프로젝트 저장  (Ctrl+S)"))) StudioSave(false);
            same();
            if (RibbonButton("##rbsaveas", icon::DownloadSimple, Tr("다른 이름으로"),
                             Tr("다른 이름으로 저장  (Ctrl+Shift+S)")))
                StudioSave(true);
        });
        group(Tr("추가"), [&] {
            if (RibbonButton("##rbaddmodel", icon::Plus, Tr("모델 추가"), Tr("캐릭터, 스테이지, 소품 추가"))) {
                studioAddPage_ = 0;
                studioReplaceUid_ = 0;
                studioAddFilter_[0] = 0;
                studioOpenAdd_ = true;
            }
            same();
            if (RibbonButton("##rbaudio", icon::MusicNotes, Tr("오디오"), Tr("음원 오프셋 / 바꾸기 / 제거"), true, false, true))
                ImGui::OpenPopup("##rbaudiopop");
            const ImVec2 amin = ImGui::GetItemRectMin(), amax = ImGui::GetItemRectMax();
            ImGui::SetNextWindowPos(ImVec2(amin.x, amax.y + Dp(4.0f)), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(Dp(300.0f), 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
            if (ImGui::BeginPopup("##rbaudiopop")) {
                DrawStudioAudioMenuItems();
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();
        });
        {
            const std::string caption = std::string(Tr("모션 파일")) + " · " + target;
            group(caption.c_str(), [&] {
                if (RibbonButton("##rbvmdin", icon::FilmStrip, Tr("VMD 불러오기"), Tr("모션 VMD 불러오기")))
                    StudioImportVmd();
                same();
                if (RibbonButton("##rbvmdout", icon::Export, Tr("VMD 내보내기"), Tr("모션 VMD 내보내기")))
                    StudioExportVmd();
                same();
                if (RibbonButton("##rbvdpin", icon::UploadSimple, Tr("포즈 불러오기"),
                                 hasModel ? Tr("VPD 포즈 불러오기") : Tr("모델을 선택하세요"), hasModel))
                    StudioImportVpd();
                same();
                if (RibbonButton("##rbvdpout", icon::DownloadSimple, Tr("포즈 내보내기"),
                                 hasModel ? Tr("VPD 포즈 내보내기") : Tr("모델을 선택하세요"), hasModel))
                    StudioExportVpd();
            });
        }
        group(Tr("클립보드"), [&] {
            if (RibbonButton("##rbcopy", icon::Copy, Tr("복사"),
                             hasKeys ? Tr("키 복사  (Ctrl+C)") : Tr("선택한 키가 없습니다"), hasKeys))
                StudioCopySelected();
            same();
            if (RibbonButton("##rbcut", icon::Scissors, Tr("잘라내기"),
                             hasKeys ? Tr("키 잘라내기  (Ctrl+X)") : Tr("선택한 키가 없습니다"), hasKeys)) {
                StudioCopySelected();
                StudioDeleteSelected(Tr("키 잘라내기"));
            }
            same();
            if (RibbonButton("##rbpaste", icon::ClipboardText, Tr("붙여넣기"),
                             canPaste ? Tr("붙여넣기  (Ctrl+V)") : Tr("클립보드가 비었습니다"), canPaste))
                StudioPaste(false);
            same();
            if (RibbonButton("##rbpasteflip", icon::FlipHorizontal, Tr("반전 붙여넣기"),
                             canPaste ? Tr("좌우 반전 붙여넣기") : Tr("클립보드가 비었습니다"), canPaste))
                StudioPaste(false, true);
        });
        break;
    }
    case 1: {  // 애니메이션
        {
            // target chip: what the model-related groups act on (the selected model, else the camera)
            const std::string chip = std::string(Tr("대상: ")) + target;
            const ImVec2 cts = TextSize(Font::Semibold, size::Caption, chip.c_str());
            const float chipH = cts.y + Dp(10.0f);
            const float chipW = cts.x + Dp(20.0f);
            const float cy0 = Dp(11.0f) + (Dp(kRibbonButtonH) - chipH) * 0.5f;
            const ImVec2 ca(screenX(gx), wp.y + cy0), cb(ca.x + chipW, ca.y + chipH);
            tdl->AddRectFilled(ca, cb, p.accentSoft, chipH * 0.5f);
            Text(tdl, Font::Semibold, size::Caption, ImVec2(ca.x + Dp(10.0f), ca.y + Dp(5.0f)), p.accentInk,
                 chip.c_str());
            ImGui::SetCursorPos(ImVec2(gx, 0.0f));
            ImGui::Dummy(ImVec2(chipW, Dp(kRibbonButtonH) + Dp(11.0f)));
            gx = (cb.x - wp.x) + ImGui::GetScrollX() + Dp(12.0f);
            firstGroup = false;   // the chip counts as the first group: a separator follows it
        }
        group(Tr("키"), [&] {
            if (RibbonButton("##rbkey", icon::Diamond, Tr("키 등록"), Tr("현재 포즈를 키로 등록  (I)")))
                StudioRegisterPose(false);
            same();
            if (RibbonButton("##rbkeyall", icon::Bone, Tr("전체 본 등록"), Tr("모든 본을 키로 등록")))
                StudioRegisterPose(true);
            same();
            if (RibbonButton("##rbautokey", icon::Lightning, Tr("자동 키"),
                             d.autoKey ? Tr("자동 키 켬: 값을 고치면 재생 헤드에 키가 생겨요")
                                       : Tr("자동 키 끔: I 키로 직접 등록해요"),
                             true, d.autoKey))
                d.autoKey = !d.autoKey;
        });
        group(Tr("프레임"), [&] {
            // the timelines' own tooltips (scope-aware: range length, selected rows or every row)
            const bool range = d.HasRange();
            const int n = range ? d.view.rangeEnd - d.view.rangeStart + 1 : 1;
            const bool rows = !d.selectedRows.empty() || !d.selection.empty();
            char tipDel[160], tipIns[160];
            std::snprintf(tipIns, sizeof(tipIns), range ? Tr("구간 앞에 %d프레임 삽입") : Tr("현재 프레임에 %d프레임 삽입"), n);
            std::snprintf(tipDel, sizeof(tipDel), range ? Tr("구간의 %d프레임 삭제") : Tr("현재 프레임에서 %d프레임 삭제"), n);
            const std::string scope = std::string("  (") + (rows ? Tr("선택한 행") : Tr("모든 행")) + ")";
            if (RibbonButton("##rbinsframes", icon::ArrowsOutLineHorizontal, Tr("프레임 삽입"),
                             (tipIns + scope).c_str()))
                StudioShiftFrames(false);
            same();
            if (RibbonButton("##rbdelframes", icon::ArrowsInLineHorizontal, Tr("프레임 삭제"),
                             (tipDel + scope).c_str()))
                StudioShiftFrames(true);
        });
        group(Tr("모션 도구"), [&] {
            const char* reason = Tr("캐릭터를 선택하세요");
            if (RibbonButton("##rbrestpose", icon::PersonSimple, Tr("팔 레스트 포즈 보정"),
                             isCharacter ? Tr("팔 레스트 포즈 보정") : reason, isCharacter))
                StudioOpenRestPoseDialog();
            same();
            if (RibbonButton("##rbbake", icon::Stack, Tr("물리 굽기"), isCharacter ? Tr("물리 결과를 본 키로 굽기") : reason,
                             isCharacter))
                StudioOpenBakeDialog();
        });
        group(Tr("물리"), [&] {
            if (RibbonButton("##rbphysics", icon::Atom, Tr("물리"),
                             d.physics ? Tr("물리 켜짐 (클릭: 끄기)") : Tr("물리 꺼짐 (클릭: 켜기)"), true, d.physics))
                StudioTogglePhysics();
            same();
            if (RibbonButton("##rbphysreset", icon::ArrowCcw, Tr("물리 초기화"), Tr("물리 초기화")))
                d.physicsFrame = -1.0f;
        });
        break;
    }
    case 2: {  // 카메라
        group(Tr("카메라"), [&] {
            if (RibbonButton("##rbcamadd", icon::Camera, Tr("새 카메라"), Tr("지금 뷰포트 시점으로 새 카메라")))
                StudioAddCameraFromView();
            same();
            if (RibbonButton("##rbcamkey", icon::Diamond, Tr("뷰를 키로"), Tr("현재 시점을 카메라 키로 등록")))
                StudioKeyCameraFromView();
            same();
            ImGui::BeginDisabled(!d.HasCameraKeys());
            if (RibbonButton("##rbmotioncam", icon::VideoCamera, Tr("카메라로 보기"),
                             d.useMotionCamera ? Tr("카메라 모션으로 보는 중 (클릭: 자유 카메라)")
                                               : Tr("자유 카메라 (클릭: 카메라 모션)"),
                             d.HasCameraKeys(), d.useMotionCamera && d.HasCameraKeys()))
                d.useMotionCamera = !d.useMotionCamera;
            ImGui::EndDisabled();
            same();
            if (RibbonButton("##rbpossess", icon::Crosshair, Tr("빙의"),
                             d.possessCamera ? Tr("카메라 빙의 해제  (Esc)")
                                             : Tr("카메라 빙의: 뷰포트 조작이 이 카메라를 움직여요"),
                             true, d.possessCamera))
                StudioPossess(!d.possessCamera);
        });
        group(Tr("프레임 비율"), [&] {
            ImGui::SetCursorPos(ImVec2(gx, controlY));
            const char* presets[kAspectPresetCount];
            for (int i = 0; i < kAspectPresetCount; ++i) presets[i] = kAspectPresets[i].label;
            int cur = d.AspectIndex();
            if (Segmented("##rbaspect", presets, kAspectPresetCount, &cur, 320.0f, 30.0f)) {
                d.SetAspectPreset(cur);
            }
        });
        group(Tr("VMD"), [&] {
            if (RibbonButton("##rbcamvmdin", icon::DownloadSimple, Tr("카메라 VMD 불러오기"), Tr("카메라 VMD 불러오기"))) {
                StudioSelectModel(-1);
                StudioImportVmd();
            }
            same();
            if (RibbonButton("##rbcamvmdout", icon::UploadSimple, Tr("카메라 VMD 내보내기"), Tr("카메라 VMD 내보내기"))) {
                StudioSelectModel(-1);
                StudioExportVmd();
            }
        });
        break;
    }
    case 3: {  // 조명
        group(Tr("메인 조명"), [&] {
            SceneLight* sun = nullptr;
            for (SceneLight& l : d.lights)
                if (l.kind == LightKind::Sun) {
                    sun = &l;
                    break;
                }
            const bool linked = sun && sun->vmdLink;
            if (RibbonButton("##rbvmdlink", icon::LinkSimple, Tr("VMD 연동"),
                             !sun ? Tr("메인 조명이 없습니다")
                                  : (linked ? Tr("카메라 VMD의 조명 키를 따릅니다 (클릭: 직접 편집)")
                                            : Tr("직접 편집 중 (클릭: 카메라 VMD의 조명 키 따르기)")),
                             sun != nullptr, linked))
                StudioSetLightSource(linked ? 2 : 0);
            same();
            if (RibbonButton("##rblightpreset", icon::Sliders, Tr("프리셋"), Tr("조명 프리셋 불러오기"), true, false, true))
                ImGui::OpenPopup("##rblightpresetpop");
            const ImVec2 pmin = ImGui::GetItemRectMin(), pmax = ImGui::GetItemRectMax();
            ImGui::SetNextWindowPos(ImVec2(pmin.x, pmax.y + Dp(4.0f)), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(Dp(240.0f), 0));
            if (ImGui::BeginPopup("##rblightpresetpop")) {
                static const char* const kPresetIds[] = {"##rbpre0", "##rbpre1", "##rbpre2", "##rbpre3"};
                static const char* const kPresetIcons[] = {icon::Sun, icon::Sun, icon::MicrophoneStage, icon::Moon};
                for (int i = 0; i < 4; ++i) {
                    if (MenuItem(kPresetIds[i], LightingPresetName((LightingPreset)i), kPresetIcons[i])) {
                        studioLightPresetPending_ = i;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }
        });
        group(Tr("라이트 추가"), [&] {
            bool hasSun = false, hasAmbient = false, hasSky = false;
            for (const SceneLight& l : d.lights) {
                hasSun |= l.kind == LightKind::Sun;
                hasAmbient |= l.kind == LightKind::Ambient;
                hasSky |= l.kind == LightKind::Sky;
            }
            if (RibbonButton("##rblightpoint", icon::Lightbulb, Tr("점광원"), Tr("점광원 추가")))
                StudioAddLight(LightKind::Point);
            same();
            if (RibbonButton("##rblightspot", icon::Aperture, Tr("스팟"), Tr("스팟 추가")))
                StudioAddLight(LightKind::Spot);
            same();
            if (RibbonButton("##rblightarea", icon::Image, Tr("면광원"), Tr("면광원 추가")))
                StudioAddLight(LightKind::Area);
            same();
            if (RibbonButton("##rblightfog", icon::Drop, Tr("안개 볼륨"), Tr("안개 볼륨 추가")))
                StudioAddLight(LightKind::FogVolume);
            if (!hasSky) {
                same();
                if (RibbonButton("##rblightsky", icon::Mountains, Tr("하늘"), Tr("하늘 조명 추가")))
                    StudioAddLight(LightKind::Sky);
            }
            if (!hasSun) {
                same();
                if (RibbonButton("##rblightsun", icon::Sun, Tr("메인 조명"), Tr("메인 조명 추가")))
                    StudioAddLight(LightKind::Sun);
            }
            if (!hasAmbient) {
                same();
                if (RibbonButton("##rblightamb", icon::CircleHalf, Tr("환경광"), Tr("환경광 추가")))
                    StudioAddLight(LightKind::Ambient);
            }
        });
        break;
    }
    case 4: {  // 렌더
        group(Tr("출력"), [&] {
            if (RibbonButton("##rbvideo", icon::FilmStrip, Tr("영상 렌더…"), Tr("영상 렌더"))) StudioRenderVideo();
            same();
            if (RibbonButton("##rbstill", icon::Image, Tr("고품질 스틸"),
                             renderer_.OfflineSupported()
                                 ? Tr("고품질 스틸 렌더")
                                 : Tr("이 그래픽 카드에서는 오프라인 GI 렌더를 사용할 수 없습니다"),
                             renderer_.OfflineSupported()))
                StudioRenderStill();
        });
        group(Tr("뷰포트 렌더러"), [&] {
            const bool rtOk = renderer_.RayTracingSupported(), giOk = renderer_.OfflineSupported();
            int cur = std::clamp(settings_.studioRenderer, 0, 3);
            if ((cur == 1 || cur == 2) && !rtOk) cur = 0;
            if (cur == 3 && !giOk) cur = 0;
            const char* names[4] = {Tr("래스터"), "RT", "PT", "GI"};
            const bool disabled[4] = {false, !rtOk, !rtOk, !giOk};
            const char* tips[4] = {nullptr, Tr("이 그래픽 카드에서는 레이 트레이싱을 사용할 수 없습니다"),
                                   Tr("이 그래픽 카드에서는 레이 트레이싱을 사용할 수 없습니다"),
                                   Tr("이 그래픽 카드에서는 오프라인 GI 렌더를 사용할 수 없습니다")};
            ImGui::SetCursorPos(ImVec2(gx, controlY));
            if (Segmented("##rbrenderer", names, 4, &cur, 280.0f, 30.0f, nullptr, disabled, tips))
                StudioSetViewportRenderer(cur);
        });
        group(Tr("효과"), [&] {
            const bool effectsActive =
                !settings_.effectStack.empty() || settings_.bloom || settings_.dof || settings_.volumetric;
            if (RibbonButton("##rbeffects", icon::Sparkle, Tr("화면 효과"),
                             Tr("화면 효과  (오른쪽 패널의 효과 탭)"), true, effectsActive))
                studioFocusEffects_ = true;
        });
        break;
    }
    default: {  // 5: 보기
        group(Tr("셰이딩"), [&] {
            const char* modes[] = {Tr("솔리드"), Tr("언릿"), Tr("와이어")};
            ImGui::SetCursorPos(ImVec2(gx, controlY));
            Segmented("##rbshading", modes, 3, &d.shading, 260.0f, 30.0f);
        });
        group(Tr("레이아웃"), [&] {
            const char* layouts[] = {Tr("단일"), Tr("4분할")};
            ImGui::SetCursorPos(ImVec2(gx, controlY));
            Segmented("##rblayout", layouts, 2, &d.viewLayout, 180.0f, 30.0f);
        });
        group(Tr("기즈모"), [&] {
            if (RibbonButton("##rbgizmomodel", icon::ArrowsOutCardinal, Tr("모델 이동"), Tr("모델 이동 기즈모  (T)"), true,
                             d.modelGizmo))
                d.modelGizmo = !d.modelGizmo;
            same();
            if (RibbonButton("##rbgizmolocal", icon::Magnet, Tr("로컬 축"), Tr("로컬 축 기준  (L)"), true, d.gizmoLocal))
                d.gizmoLocal = !d.gizmoLocal;
        });
        group(Tr("창"), [&] {
            if (RibbonButton("##rblayoutreset", icon::Monitor, Tr("패널 배치 초기화"),
                             Tr("패널 배치 초기화 (패널은 탭을 끌어서 옮기고 크기를 바꿀 수 있어요)")))
                studioResetLayout_ = true;
            same();
            if (RibbonButton("##rbhelp", icon::Keyboard, Tr("단축키"), Tr("단축키  (?)"), true, studioHelpOpen_))
                studioHelpOpen_ = !studioHelpOpen_;
        });
        break;
    }
    }

    // the wheel scrolls a tool row wider than the window
    if (ImGui::IsWindowHovered()) ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * Dp(80.0f));
    ImGui::EndDisabled();
    ImGui::EndChild();
    dl->AddLine(ImVec2(x0, y1 - 0.5f), ImVec2(x1, y1 - 0.5f), p.line);
    if (post) post();   // may replace or leave the studio: nothing of `d` is touched after it
}

} // namespace mmdx
