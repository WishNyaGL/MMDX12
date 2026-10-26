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
    ImDrawList* cdl = ImGui::GetWindowDrawList();

    if (!m->rig) {
        studio::RigGenerateResult res = studio::GenerateRig(*m->pmx);
        m->rig = std::make_shared<studio::RigDefinition>(std::move(res.rig));
        m->rigDiag = std::move(res.diag);
    }
    studio::RigDefinition& rig = *m->rig;

    ImGui::Dummy(ImVec2(w, Dp(4.0f)));
    if (Button("##riggen", Tr("리그 생성"), icon::Refresh, ButtonKind::Primary, ImVec2(w, 34.0f))) {
        studio::RigGenerateResult res = studio::GenerateRig(*m->pmx);
        m->rig = std::make_shared<studio::RigDefinition>(std::move(res.rig));
        m->rigDiag = std::move(res.diag);
    }
    Tooltip(Tr("현재 모델의 골격을 분석해 기본 리그(컨트롤러 뼈)를 생성합니다. 아직 컨트롤러 조작은 미구현입니다."));
    Gap(8.0f);

    int resolved = 0;
    for (const studio::RigBoneBinding& b : rig.bindings) if (b.boneIndex >= 0) ++resolved;
    char summary[96];
    std::snprintf(summary, sizeof(summary), Tr("역할 %d / %d 연결됨"), resolved, (int)rig.bindings.size());
    Text(cdl, Font::Semibold, size::Small, ImGui::GetCursorScreenPos(), p.ink, summary);
    ImGui::Dummy(ImVec2(w, Dp(22.0f)));

    ImGui::BeginChild("##riglist", ImVec2(w, Dp(320.0f)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
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
        TextEllipsis(cdl, Font::Regular, size::Small, ImVec2(c.x + Dp(110.0f), c.y), c.x + w, p.ink, value.c_str());
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    }
    ImGui::EndChild();

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

} // namespace mmdx
