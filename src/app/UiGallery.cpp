// Stage gallery (Screen::Gallery, the "갤러리" top tab): online MMDX stages from stages/index.json — install / update /
// uninstall with SHA-256 verification and resumable downloads (app/StageStore), an installed / revoked badge and the
// detail pane. Layout follows UiShaders.cpp's online tab (card grid + right detail panel).
// Online stages only for now: effects / looks stay in the 이펙트 tab (a later milestone adds them here).
#include "app/App.h"
#include "ui_probe/UiProbe.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>

#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/NetUtil.h"
#include "core/TextUtil.h"
#include "imgui.h"

namespace mmdx {

using namespace ui;

namespace {

constexpr float kAppBarH = 64.0f;   // as UiSelect.cpp
constexpr float kPad = 28.0f;
constexpr float kPanelW = 460.0f;

// Wrapped paragraph in the kit's type (as UiShaders.cpp).
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

void Caption(const char* label) {
    Gap(14.0f);
    Para(label, P().ink3, size::Caption, Font::Semibold);
    Gap(4.0f);
}

// A row of small badges that wraps (tags).
void BadgeFlow(const std::vector<std::string>& items, float width) {
    if (items.empty()) return;
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    float x = 0, y = 0, rowH = 0;
    for (const std::string& t : items) {
        const ImVec2 ts = TextSize(Font::Semibold, size::Caption, t.c_str());
        const float w = ts.x + Dp(16.0f);
        if (x > 0 && x + w > width) {
            x = 0;
            y += rowH + Dp(6.0f);
        }
        ImVec2 bs;
        Badge(dl, ImVec2(o.x + x, o.y + y), t.c_str(), WithAlpha(p.ink, 0.06f), p.ink2, &bs);
        rowH = std::max(rowH, bs.y);
        x += bs.x + Dp(6.0f);
    }
    ImGui::Dummy(ImVec2(width, y + rowH));
}

void OpenFolder(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// "1.4 MB" / "820 KB" / "12.3 MB".
std::string SizeText(uint64_t bytes) {
    if (bytes == 0) return "-";
    char buf[64];
    if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (double)(1ull << 20));
    else std::snprintf(buf, sizeof(buf), "%.0f KB", (double)bytes / 1024.0);
    return buf;
}

// The installed stage's folder name from a library id ("stages:cyclorama" -> "cyclorama").
std::string FolderIdOf(const std::string& libraryId) {
    constexpr const char* kPrefix = "stages:";
    return libraryId.rfind(kPrefix, 0) == 0 ? libraryId.substr(std::string(kPrefix).size()) : libraryId;
}

} // namespace

// ---- per frame ------------------------------------------------------------------------------------------------------

void App::PollStageStore() {
    for (const StageStore::Event& e : stageStore_.Poll()) {
        toast_ = Toast{};
        toast_.title = e.ok ? Tr("스테이지") : Tr("스테이지를 설치하지 못했습니다");
        toast_.detail = e.text;
        toast_.error = !e.ok;
        toast_.until = timeSeconds_ + (e.ok ? 4.0 : 8.0);
        // the new stage must show up in the lobby's stage list without a restart
        if (e.ok) RefreshLibraryForStage();
    }
}

void App::RefreshLibraryForStage() {
    // The same background rescan the lobby runs for library changes (App::StartRevalidation); PollScan applies it as
    // soon as the current screen is safe, so the freshly installed / removed stage appears in library_.stages.
    StartRevalidation();
}

void App::RefreshStageGallery() {
    galleryIndexApplied_ = true;
    stageStore_.Refresh(options_.galleryIndex.empty() ? StageStore::kDefaultIndexUrl : options_.galleryIndex);
}

uint64_t App::RemoteStageThumb(const RemoteStage& stage) {
    if (stage.previewFile.empty()) return 0;
    return thumbs_.Get("rs:" + stage.id + ":" + stage.version, ThumbnailKind::Image, {stage.previewFile});
}

bool App::StageInUse(const std::string& id) const {
    if (id.empty()) return false;
    const std::string folder = FolderIdOf(id);
    const auto sameId = [&](const std::string& libraryId) {
        return !libraryId.empty() && FolderIdOf(libraryId) == folder;
    };
    if (scene_ && sameId(scene_->stageId)) return true;
    if (studio_) {
        for (const studio::StudioStage& st : studio_->stages)
            if (sameId(st.stageId)) return true;
    }
    return false;
}

// ---- screen ---------------------------------------------------------------------------------------------------------

void App::DrawGallery() {
    BeginScreen("##gallery");
    DrawAppBar(2);
    const Palette& p = P();
    const std::string lang = StageLanguage();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float pad = Dp(kPad), gap = Dp(24.0f);
    const float top = Dp(kAppBarH) + pad;
    const float panelX = ds.x - pad - Dp(kPanelW);
    const float leftX = pad, leftR = panelX - gap;
    if (!galleryIndexApplied_) RefreshStageGallery();

    // ---- header: counts, refresh, folder, search
    {
        const std::string l0 = stageStore_.State() == StageStore::IndexState::Ready
                                   ? Tr("온라인  ") + std::to_string(stageStore_.Stages().size())
                                   : std::string(Tr("온라인"));
        const std::string l1 = Tr("설치됨  ") + std::to_string(stageStore_.Installed().size());
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 ls = TextSize(Font::Semibold, size::Body, l0.c_str());
            const float lh = Dp(40.0f);
            const ImVec2 la(leftX, top + (lh - ls.y) * 0.5f);
            Icon(dl, icon::Globe, 16.0f, ImVec2(la.x + Dp(8.0f), top + lh * 0.5f), p.accentInk);
            Text(dl, Font::Semibold, size::Body, ImVec2(la.x + Dp(22.0f), la.y), p.ink, l0.c_str());
            const float l1x = la.x + Dp(22.0f) + ls.x + Dp(16.0f);
            Text(dl, Font::Regular, size::Caption, ImVec2(l1x, top + Dp(12.0f)), p.ink3, l1.c_str());
            ImGui::SetCursorScreenPos(ImVec2(leftX, top));
            ImGui::Dummy(ImVec2(l1x + TextSize(Font::Regular, size::Caption, l1.c_str()).x - leftX, lh));
        }
        const float tabsR = ImGui::GetItemRectMax().x;

        float x = leftR;
        ImGui::SetCursorScreenPos(ImVec2(x - Dp(36.0f), top + Dp(2.0f)));
        ImGui::BeginDisabled(stageStore_.State() == StageStore::IndexState::Loading);
        if (IconButton("##galleryrefresh", icon::Refresh, Tr("목록 새로 고침"))) RefreshStageGallery();
        ImGui::EndDisabled();
        x -= Dp(44.0f);
        ImGui::SetCursorScreenPos(ImVec2(x - Dp(36.0f), top + Dp(2.0f)));
        if (IconButton("##galleryfolder", icon::FolderOpen, Tr("설치된 스테이지 폴더 열기")))
            OpenFolder(StageStore::StagesRoot());
        x -= Dp(44.0f);

        // search + result count on the right of the tab row
        const std::string needle = ToLowerAscii(galleryFilter_);
        int matchCount = 0;
        for (const RemoteStage& s : stageStore_.Stages())
            if (StageMatches(needle, s)) ++matchCount;
        const std::string countStr = std::to_string(matchCount) + Tr("개");
        const ImVec2 countSz = TextSize(Font::Regular, size::Caption, countStr.c_str());
        const float countX = x - countSz.x - Dp(4.0f);
        Text(ImGui::GetWindowDrawList(), Font::Regular, size::Caption, ImVec2(countX, top + Dp(12.0f)), p.ink3,
             countStr.c_str());
        const float searchRight = countX - Dp(12.0f);
        const float availSearch = (searchRight - (tabsR + Dp(90.0f))) / Dpi();
        if (availSearch > 50.0f) {
            const float sw = std::min(240.0f, availSearch);
            ImGui::SetCursorScreenPos(ImVec2(searchRight - Dp(sw), top + Dp(1.0f)));
            SearchField("##galleryfilter", galleryFilter_, sizeof(galleryFilter_), Tr("이름, 작성자, 태그"), sw);
        }
    }

    // ---- grid
    const float gridTop = top + Dp(40.0f) + Dp(20.0f);
    ImGui::SetCursorScreenPos(ImVec2(leftX - Dp(6.0f), gridTop - Dp(6.0f)));
    ImGui::BeginChild("##stagegrid", ImVec2(leftR - leftX + Dp(12.0f), ds.y - gridTop - pad + Dp(12.0f)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float inset = Dp(6.0f);
        const float availW = ImGui::GetContentRegionAvail().x - inset * 2.0f - Dp(10.0f);
        const ImVec2 origin(ImGui::GetCursorScreenPos().x + inset, ImGui::GetCursorScreenPos().y + inset);
        const float cgap = Dp(16.0f);
        const float minW = Dp(240.0f);
        const int cols = std::max(1, (int)((availW + cgap) / (minW + cgap)));
        const float cw = (availW - cgap * (cols - 1)) / cols;
        const float imgH = cw * 9.0f / 16.0f, chH = imgH + Dp(92.0f);
        const std::string needle = ToLowerAscii(galleryFilter_);
        float contentH = 0;
        int n = 0;

        const auto card = [&](const char* id, bool selected, uint64_t tex, const std::string& key,
                              const std::string& name,
                              const std::string& sub,
                              const std::vector<std::pair<std::string, std::pair<ImU32, ImU32>>>& badges,
                              float progress) {
            const int col = n % cols, row = n / cols;
            ++n;
            const ImVec2 ca(origin.x + col * (cw + cgap), origin.y + row * (chH + cgap));
            const ImVec2 cb(ca.x + cw, ca.y + chH);
            contentH = std::max(contentH, cb.y - origin.y);
            if (!ImGui::IsRectVisible(ImVec2(ca.x, ca.y - Dp(20)), ImVec2(cb.x, cb.y + Dp(20)))) return false;
            bool hovered = false;
            const bool clicked = CardItem(id, ca, cb, &hovered, name.c_str());
            uiprobe::Info(ImGui::GetItemID(), key.c_str(), nullptr);   // "rs:<id>:<version>": by stage id
            const float hv = Anim(ImGui::GetID(id), hovered);
            const float sv = Anim(ImGui::GetID((std::string(id) + "s").c_str()), selected, 14.0f);
            const float lift = -Dp(3.0f) * hv;
            const ImVec2 a0(ca.x, ca.y + lift), b0(cb.x, cb.y + lift);
            const float r = Dp(14.0f);
            SoftShadow(dl, a0, b0, r, Dp(10.0f + 8.0f * hv), 0.07f + 0.07f * hv, ImVec2(0, Dp(3.0f + 3.0f * hv)));
            dl->AddRectFilled(a0, b0, p.surface, r);
            DrawPackImage(dl, tex, key, a0, ImVec2(b0.x, a0.y + imgH), r, ImDrawFlags_RoundCornersTop);
            if (sv > 0.01f) dl->AddRect(a0, b0, WithAlpha(p.accent, sv), r, 0, Dp(2.0f));
            const float tx = a0.x + Dp(14.0f), tr = b0.x - Dp(14.0f);
            float ty = a0.y + imgH + Dp(12.0f);
            TextEllipsis(dl, Font::Semibold, size::Body, ImVec2(tx, ty), tr, p.ink, name.c_str());
            ty += Dp(22.0f);
            TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(tx, ty), tr, p.ink3, sub.c_str());
            ty += Dp(24.0f);
            float bx = tx;
            for (const auto& [text, col2] : badges) {
                ImVec2 bs;
                Badge(dl, ImVec2(bx, ty), text.c_str(), col2.first, col2.second, &bs);
                bx += bs.x + Dp(6.0f);
            }
            if (progress >= 0.0f) ProgressBar(dl, ImVec2(tx, b0.y - Dp(10.0f)), ImVec2(tr, b0.y - Dp(6.0f)), progress);
            return clicked;
        };

        const auto state = stageStore_.State();
        if (state == StageStore::IndexState::Loading || state == StageStore::IndexState::Idle) {
            Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3,
                 Tr("스테이지 목록을 불러오는 중..."));
        } else if (state == StageStore::IndexState::Failed) {
            Para(stageStore_.Error(), p.warn, size::Body);
            Gap(10.0f);
            if (Button("##galleryretry", Tr("다시 시도"), icon::Refresh)) RefreshStageGallery();
        } else {
            for (const RemoteStage& s : stageStore_.Stages()) {
                if (!StageMatches(needle, s)) continue;
                std::vector<std::pair<std::string, std::pair<ImU32, ImU32>>> badges;
                const InstalledStage* inst = stageStore_.FindInstalled(s.id);
                if (s.revoked) badges.push_back({Tr("차단됨"), {p.dangerSoft, p.danger}});
                else if (!s.Compatible()) badges.push_back({Tr("호환되지 않음"), {p.warnSoft, p.warn}});
                else if (inst && StageUpdateAvailable(*inst, s))
                    badges.push_back({Tr("업데이트 있음"), {p.accentSoft, p.accentInk}});
                else if (inst) badges.push_back({Tr("설치됨"), {WithAlpha(p.ink, 0.06f), p.ink2}});
                for (const std::string& t : s.tags) {
                    if (badges.size() >= 3) break;
                    badges.push_back({t, {WithAlpha(p.ink, 0.05f), p.ink2}});
                }
                std::string sub = "v" + s.version;
                if (!s.author.empty()) sub += "  ·  " + s.author;
                if (s.size) sub += "  ·  " + SizeText(s.size);
                ImGui::PushID(s.id.c_str());
                if (card("##sc", gallerySel_ == s.id, RemoteStageThumb(s), "rs:" + s.id + ":" + s.version,
                         s.name.Get(lang), sub, badges,
                         stageStore_.Installing(s.id) ? stageStore_.Progress(s.id) : -1.0f))
                    gallerySel_ = s.id;
                ImGui::PopID();
            }
            if (n == 0) {
                if (stageStore_.Stages().empty()) {
                    Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3,
                         Tr("아직 갤러리에 스테이지가 없습니다"));
                } else {
                    Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3,
                         Tr("조건에 맞는 스테이지가 없습니다"));
                    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + Dp(38.0f)));
                    if (Button("##galleryclear", Tr("필터 지우기"), nullptr, ButtonKind::Ghost)) galleryFilter_[0] = '\0';
                    contentH = std::max(contentH, Dp(80.0f));
                }
            }
        }
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(availW, contentH + inset));
    }
    ImGui::EndChild();

    // ---- detail panel
    {
        const ImVec2 pa(panelX, top), pb(ds.x - pad, ds.y - pad);
        Panel(ImGui::GetWindowDrawList(), pa, pb, Dp(22.0f), 1.0f);
        DrawGalleryDetail(pa.x, pa.y, pb.x, pb.y);
    }
    DrawToast();
    EndScreen();
}

// ---- detail ---------------------------------------------------------------------------------------------------------

void App::DrawGalleryDetail(float x0, float y0, float x1, float y1) {
    const Palette& p = P();
    const std::string lang = StageLanguage();
    const RemoteStage* rs = nullptr;
    for (const RemoteStage& s : stageStore_.Stages())
        if (s.id == gallerySel_) rs = &s;
    if (!rs && !stageStore_.Stages().empty()) {
        rs = &stageStore_.Stages().front();
        gallerySel_ = rs->id;
    }
    const float ip = Dp(22.0f);
    ImGui::SetCursorScreenPos(ImVec2(x0 + ip, y0 + ip));
    ImGui::BeginChild("##stagedetail", ImVec2(x1 - x0 - ip * 2.0f + Dp(8.0f), y1 - y0 - ip * 2.0f),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    const float w = x1 - x0 - ip * 2.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    if (!rs) {
        Para(Tr("온라인 갤러리의 MMDX 스테이지는 mmdx.codingbot.kr 에 올라온 것들입니다. 받은 파일은 크기와 SHA-256 을 "
                "확인한 뒤 <exe>/stages 에 설치합니다."),
             p.ink2);
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + w * 9.0f / 16.0f);
        DrawPackImage(dl, RemoteStageThumb(*rs), "rs:" + rs->id + ":" + rs->version, a, b, Dp(12.0f));
        ImGui::Dummy(ImVec2(w, b.y - a.y));
    }
    Gap(14.0f);
    Para(rs->name.Get(lang), p.ink, size::Heading, Font::Bold);
    std::string meta = "v" + rs->version + "  ·  " + rs->id;
    if (rs->size) meta += "  ·  " + SizeText(rs->size);
    Para(meta, p.ink3, size::Caption);
    if (!rs->author.empty()) Para(std::string(Tr("만든 사람")) + ": " + rs->author, p.ink3, size::Caption);

    if (rs->revoked) {
        Gap(10.0f);
        Para(Tr("갤러리에서 차단된 스테이지입니다"), p.danger, size::Small, Font::Semibold);
        Para(rs->revokedReason.empty() ? std::string(Tr("이 스테이지는 배포가 중단되었습니다.")) : rs->revokedReason,
             p.ink2, size::Caption);
    } else if (!rs->Compatible()) {
        Gap(10.0f);
        Para(Tr("이 버전의 MMDX12 에서는 쓸 수 없는 스테이지입니다. 앱을 업데이트하세요."), p.warn);
    }

    // install / update / uninstall
    Gap(14.0f);
    const InstalledStage* inst = stageStore_.FindInstalled(rs->id);
    if (stageStore_.Installing(rs->id)) {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ProgressBar(dl, a, ImVec2(a.x + w, a.y + Dp(6.0f)), stageStore_.Progress(rs->id));
        ImGui::Dummy(ImVec2(w, Dp(12.0f)));
        Para(Tr("받는 중..."), p.ink3, size::Caption);
        if (Button("##stagecancel", Tr("취소"), icon::X, ButtonKind::Secondary)) stageStore_.Cancel(rs->id);
    } else if (!rs->revoked && !inst) {
        ImGui::BeginDisabled(!rs->Compatible());
        if (Button("##stageinstall", Tr("설치"), icon::DownloadSimple, ButtonKind::Primary, ImVec2(w / Dpi(), 44.0f)))
            stageStore_.Install(*rs);
        ImGui::EndDisabled();
    } else if (!rs->revoked && inst && !StageUpdateAvailable(*inst, *rs)) {
        Para(Tr("최신 버전이 설치되어 있음"), p.ink3);
        Para(Tr("설치된 버전: v") + inst->version, p.ink3, size::Caption);
    } else if (!rs->revoked && inst) {
        if (Button("##stageupdate", Tr("업데이트"), icon::DownloadSimple, ButtonKind::Primary, ImVec2(w / Dpi(), 44.0f)))
            stageStore_.Install(*rs);
        Para(Tr("설치된 버전: v") + inst->version, p.ink3, size::Caption);
    }
    // an installed copy stays listed (never auto-deleted), even when the entry was revoked
    if (inst && !stageStore_.Installing(rs->id)) {
        const bool inUse = StageInUse(inst->id);
        const std::string installDir = PathToUtf8(inst->dir);
        const std::string installedId = inst->id;   // Uninstall() rebuilds the store's list: copy before calling
        Gap(8.0f);
        ImGui::BeginDisabled(inUse);
        if (Button("##stageuninstall", Tr("삭제"), icon::Trash, ButtonKind::Danger, ImVec2(w / Dpi(), 38.0f))) {
            std::string err;
            if (!stageStore_.Uninstall(installedId, err)) {
                toast_ = Toast{};
                toast_.title = Tr("스테이지를 삭제하지 못했습니다");
                toast_.detail = err;
                toast_.error = true;
                toast_.until = timeSeconds_ + 6.0;
            } else {
                toast_ = Toast{};
                toast_.title = Tr("스테이지");
                toast_.detail = rs->name.Get(lang) + Tr(" 삭제됨");
                toast_.until = timeSeconds_ + 4.0;
                RefreshLibraryForStage();
            }
        }
        ImGui::EndDisabled();
        if (inUse) Tooltip(Tr("이 스테이지를 쓰는 장면 / 스튜디오를 닫은 뒤 삭제할 수 있습니다"));
        Para(std::string(Tr("설치 위치")) + ": " + installDir, p.ink3, size::Caption);
        if (rs->revoked) {
            Gap(6.0f);
            Para(Tr("설치된 버전이 차단되었습니다. 새 장면에서 쓰지 않는 것을 권장합니다."), p.danger, size::Caption);
        }
    }

    if (!rs->description.Empty()) {
        Gap(12.0f);
        Para(rs->description.Get(lang), p.ink2);
    }
    if (!rs->license.empty()) {
        Caption(Tr("라이선스"));
        Para(rs->license, p.ink2);
    }
    if (!rs->tags.empty()) {
        Caption(Tr("태그"));
        BadgeFlow(rs->tags, w);
    }
    Caption(Tr("무결성"));
    Para("SHA-256  " + rs->sha256, p.ink3, size::Caption);
    if (!rs->minEngine.empty()) Para(std::string(Tr("필요한 앱 버전")) + ": v" + rs->minEngine, p.ink3, size::Caption);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

} // namespace mmdx
