// Rig tab (step 1): skeleton analysis + diagnostics. A placeholder until the controller system lands.
#include "app/App.h"

#include "studio/RigGenerator.h"
#include "studio/StudioDoc.h"

#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "imgui.h"

namespace mmdx {

using namespace studio;

void App::DrawStudioRigTab(float w) {
    using namespace ui;
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m || !m->pmx) return;
    const Palette& p = P();

    // Generate lazily on first view, or on demand. Never keep a reference across regeneration (the shared_ptr is
    // replaced, which would leave the old object dangling -> use-after-free).
    auto ensureRig = [&]() {
        if (!m->rig) {
            studio::RigGenerateResult res = studio::GenerateRig(*m->pmx);
            m->rig = std::make_shared<studio::RigDefinition>(std::move(res.rig));
            m->rigDiag = std::move(res.diag);
        }
    };
    ensureRig();

    // Header (fixed): a Generate button that does not overflow the panel (UI widgets scale by Dpi internally).
    const float btnW = std::max(w / Dpi() - 0.0f, 60.0f);
    if (Button("##riggen", Tr("리그 생성"), icon::Refresh, ButtonKind::Primary, ImVec2(btnW, 34.0f))) {
        studio::RigGenerateResult res = studio::GenerateRig(*m->pmx);
        m->rig = std::make_shared<studio::RigDefinition>(std::move(res.rig));
        m->rigDiag = std::move(res.diag);
        ensureRig();  // no-op: already present; keeps the shared_ptr alive for the rest of this frame
    }
    Tooltip(Tr("현재 모델의 골격을 분석해 기본 리그(컨트롤러 뼈)를 생성합니다. 아직 컨트롤러 조작은 미구현입니다."));
    Gap(8.0f);

    const studio::RigDefinition& rig = *m->rig;  // safe: taken after any regeneration above
    ui::Tooltip(Tr("리그 컨트롤러는 기존 이동/회전 도구(W/E)를 따릅니다. 이동: 손/발을 움직여 IK로 해결. 회전: 손/발 뼈를 선택해 기즈모로 회전."));
    Gap(6.0f);
    int resolved = 0;
    for (const studio::RigBoneBinding& b : rig.bindings) if (b.boneIndex >= 0) ++resolved;
    char summary[96];
    std::snprintf(summary, sizeof(summary), Tr("역할 %d / %d 연결됨"), resolved, (int)rig.bindings.size());
    Text(ImGui::GetWindowDrawList(), Font::Semibold, size::Small, ImGui::GetCursorScreenPos(), p.ink, summary);
    ImGui::Dummy(ImVec2(w, Dp(6.0f)));

    // Save control: independent of the project (a dialog), plus a saved/unsaved indicator.
    {
        const bool saved = !m->rigFile.empty();
        ImGui::SetNextItemWidth(w / Dpi());
        if (Button("##rigsave", saved ? Tr("리그 저장됨 · 다시 저장…") : Tr("리그 저장…"), icon::FloppyDisk,
                   saved ? ButtonKind::Secondary : ButtonKind::Primary, ImVec2(w / Dpi(), 30.0f)))
            StudioSaveRigDialog();
        Tooltip(Tr("리그를 별도 파일(.mmdxrig)로 저장합니다. 프로젝트와 무관하게 어디든 저장할 수 있어요."));
        ImGui::Dummy(ImVec2(w, Dp(10.0f)));
    }

    // Role list: a scrollable child that only fills the remaining tab height, so it never escapes the panel or
    // overlaps adjacent UI. The header stays pinned above it.
    const float availY = ImGui::GetContentRegionAvail().y;
    const float listH = std::max(availY, Dp(80.0f));
    if (ImGui::BeginChild("##riglist", ImVec2(w, listH), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove)) {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        for (int r = 0; r < (int)studio::RigRole::Count; ++r) {
            const studio::RigBoneBinding& b = rig.bindings[(size_t)r];
            char role[32];
            std::snprintf(role, sizeof(role), "%s", studio::RigRoleName(b.role));
            const std::string value = b.boneIndex >= 0
                                          ? std::string(DisplayName(m->pmx->bones[(size_t)b.boneIndex].name,
                                                                m->pmx->bones[(size_t)b.boneIndex].nameEn))
                                          : std::string(Tr("연결 안 됨"));
            const ImVec2 c = ImGui::GetCursorScreenPos();
            Text(cdl, Font::Regular, size::Small, c, b.boneIndex >= 0 ? p.ink2 : p.warn, role);
            TextEllipsis(cdl, Font::Regular, size::Small, ImVec2(c.x + Dp(96.0f), c.y), c.x + w, p.ink, value.c_str());
            ImGui::Dummy(ImVec2(w, Dp(20.0f)));
        }
        if (!m->rigDiag.warnings.empty()) {
            ImGui::Dummy(ImVec2(w, Dp(8.0f)));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.warn));
            PushFont(Font::Regular, size::Small);
            for (const std::string& ww : m->rigDiag.warnings) ImGui::TextUnformatted(ww.c_str());
            PopFont();
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::EndChild();
}

} // namespace mmdx
