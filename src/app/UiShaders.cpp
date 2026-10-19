// Effect pack manager (Screen::Shaders, the "이펙트" tab): installed packs and the online gallery, pack details,
// installing (zip / folder / drop / download), removing, creating a pack from the template.
#include "app/App.h"
#include "ui_probe/UiProbe.h"

#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <thread>

#include "app/Icons.h"
#include "app/UiKit.h"
#include "asset/RayMaterial.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/NetUtil.h"
#include "core/TextUtil.h"
#include "render/RenderPass.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "studio/FileDialog.h"

namespace mmdx {

using namespace ui;

namespace {
// The gallery's verified badge (official index only): a blue disc with a white check, the same in both themes.
constexpr ImU32 kVerifiedBlue = IM_COL32(29, 155, 240, 255);
void VerifiedBadge(ImDrawList* dl, ImVec2 centre, float sizeDp) {
    dl->AddCircleFilled(centre, ui::Dp(sizeDp * 0.5f), kVerifiedBlue, 20);
    ui::Icon(dl, icon::Check, sizeDp * 0.7f, centre, IM_COL32(255, 255, 255, 255));
}


constexpr float kAppBarH = 64.0f;   // as UiSelect.cpp
constexpr float kPad = 28.0f;
constexpr float kPanelW = 460.0f;

void OpenUrl(const std::string& url) {
    if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0)
        ShellExecuteW(nullptr, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenFolder(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The host shown for a third-party gallery source ("example.com" for https://example.com/x.json; a local path
// stays as it is).
std::string SourceHost(const std::string& url) {
    std::string s = url;
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    else return s;
    size_t end = s.find_first_of("/?#");
    if (end == std::string::npos) end = s.size();
    return s.substr(0, end);
}

// Trims spaces / tabs (the gallery URL field).
std::string TrimUrl(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

const char* SourceLabel(PackSource s) {
    switch (s) {
    case PackSource::BuiltIn: return Tr("기본 제공");
    case PackSource::Online: return Tr("온라인");
    default: return Tr("직접 설치");
    }
}

// status badge text + colours; nullptr = nothing to say (ready / compiled)
const char* StatusLabel(PackStatus s, ImU32& bg, ImU32& fg) {
    const Palette& p = P();
    switch (s) {
    case PackStatus::CompileError: bg = p.dangerSoft; fg = p.danger; return Tr("컴파일 오류");
    case PackStatus::Incompatible: bg = p.warnSoft; fg = p.warn; return Tr("호환되지 않음");
    case PackStatus::InvalidManifest: bg = p.dangerSoft; fg = p.danger; return Tr("매니페스트 오류");
    case PackStatus::Duplicate: bg = p.warnSoft; fg = p.warn; return Tr("중복된 id");
    case PackStatus::Revoked: bg = p.dangerSoft; fg = p.danger; return Tr("갤러리에서 차단됨");
    default: return nullptr;
    }
}

// Wrapped paragraph in the kit's type.
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

const ShaderPack* InstalledVersionOf(const std::string& id) {
    const ShaderPack* p = ShaderPacks().Find(id);
    return p && p->status != PackStatus::Duplicate ? p : nullptr;
}

// "화면 효과" / "표면 셰이더" / "룩" type badge text for a card.
const char* TypeLabel(PackType t) {
    if (t == PackType::Effect) return Tr("화면 효과");
    if (t == PackType::Look) return Tr("룩");
    if (t == PackType::World) return Tr("월드 이펙트");
    return Tr("표면 셰이더");
}

} // namespace

// ---- per frame ------------------------------------------------------------------------------------------------------

void App::PollShaderPacks() {
    ShaderPackRegistry& reg = ShaderPacks();
    reg.PollChanges(timeSeconds_);   // hot reload: edited packs recompile on their next draw
    for (const ShaderPackStore::Event& e : shaderStore_.Poll(reg)) {
        toast_ = Toast{};
        toast_.title = e.ok ? Tr("이펙트 팩") : Tr("이펙트 팩을 설치하지 못했습니다");
        toast_.detail = e.text;
        toast_.error = !e.ok;
        toast_.until = timeSeconds_ + (e.ok ? 4.0 : 8.0);
    }
    if (!droppedFiles_.empty()) {
        const std::vector<std::filesystem::path> files = std::move(droppedFiles_);
        droppedFiles_.clear();
        // packs can be dropped on the library and the shader screens (not into a scene / the studio)
        if (screen_ == Screen::Select || screen_ == Screen::Shaders || screen_ == Screen::BenchLobby)
            for (const auto& f : files) InstallPackPath(f);
    }
}

void App::InstallPackPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::string id, err;
    bool ok = false;
    if (std::filesystem::is_directory(path, ec)) {
        ok = ShaderPacks().InstallFolder(path, "", id, err);
    } else if (ToLowerAscii(PathToUtf8(path.extension())) == ".zip") {
        ok = ShaderPacks().InstallZip(path, "", id, err);
    } else {
        return;   // not a pack; ignore silently (people drop all sorts of things on windows)
    }
    toast_ = Toast{};
    if (ok) {
        const ShaderPack* p = ShaderPacks().Find(id);
        toast_.title = Tr("이펙트 팩을 설치했습니다");
        toast_.detail = p ? p->name.Get(PackLanguage()) + " v" + p->version : id;
        shaderSelInstalled_ = id;
        shaderTab_ = 0;
        screen_ = Screen::Shaders;
        LOG_INFO("shader pack installed from %s: %s", PathToUtf8(path).c_str(), id.c_str());
    } else {
        toast_.title = Tr("이펙트 팩을 설치하지 못했습니다");
        toast_.detail = err;
        toast_.error = true;
        LOG_WARN("shader pack install from %s failed: %s", PathToUtf8(path).c_str(), err.c_str());
    }
    toast_.until = timeSeconds_ + (ok ? 4.0 : 8.0);
}

// ---- screen ---------------------------------------------------------------------------------------------------------

void App::DrawShaders() {
    BeginScreen("##shaders");
    DrawAppBar(1);
    const Palette& p = P();
    ShaderPackRegistry& reg = ShaderPacks();
    const std::string lang = PackLanguage();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float pad = Dp(kPad), gap = Dp(24.0f);
    const float top = Dp(kAppBarH) + pad;
    const float panelX = ds.x - pad - Dp(kPanelW);
    const float leftX = pad, leftR = panelX - gap;
    if (shaderTab_ == 1 && shaderStore_.State() == ShaderPackStore::IndexState::Idle) RefreshGallery();

    // ---- header: tabs, search, actions
    {
        if (shaderPackWriteTimesGen_ != reg.Generation()) {
            shaderPackWriteTimesGen_ = reg.Generation();
            shaderPackWriteTimes_.clear();
            for (const ShaderPack& pk : reg.Packs()) {
                std::error_code ec;
                const auto t = std::filesystem::last_write_time(pk.dir / L"pack.json", ec);
                shaderPackWriteTimes_[pk.id] = !ec ? t : std::filesystem::file_time_type::min();
            }
        }

        int installedCount = 0;
        for (const ShaderPack& pk : reg.Packs()) installedCount += pk.status != PackStatus::Duplicate;
        const std::string l0 = Tr("설치됨  ") + std::to_string(installedCount);
        const std::string l1 = shaderStore_.State() == ShaderPackStore::IndexState::Ready
                                   ? Tr("온라인  ") + std::to_string(shaderStore_.Packs().size())
                                   : std::string(Tr("온라인"));
        const std::string l2 = std::string(Tr("MME")) +
                               (library_.effects.empty() ? "" : "  " + std::to_string(library_.effects.size()));
        const char* labels[] = {l0.c_str(), l1.c_str(), l2.c_str()};
        const char* icons[] = {icon::Stack, icon::Globe, icon::Sparkle};
        ImGui::SetCursorScreenPos(ImVec2(leftX, top));
        Segmented("##shadertabs", labels, 3, &shaderTab_, 0.0f, 40.0f, icons);
        const float tabsR = ImGui::GetItemRectMax().x;

        // actions, right-aligned against the panel
        float x = leftR;
        const auto rightButton = [&](const char* id, const char* label, const char* ic, ButtonKind kind) {
            const float w = TextSize(Font::Semibold, size::Small, label).x + Dp(ic ? 52.0f : 32.0f);
            x -= w;
            ImGui::SetCursorScreenPos(ImVec2(x, top + Dp(2.0f)));
            const bool r = Button(id, label, ic, kind, ImVec2(w / Dpi(), 36.0f));
            x -= Dp(8.0f);
            return r;
        };
        if (shaderTab_ == 0) {
            if (rightButton("##newpack", Tr("새 팩 만들기"), icon::Plus, ButtonKind::Secondary)) {
                newPackOpen_ = true;
                newPackId_[0] = newPackName_[0] = 0;
                std::snprintf(newPackAuthor_, sizeof(newPackAuthor_), "%s", settings_.nickname.c_str());
            }
            if (rightButton("##installzip", Tr("zip 설치"), icon::DownloadSimple, ButtonKind::Secondary)) {
                const std::filesystem::path f = studio::OpenFileDialog(hwnd_, {{L"Shader pack (zip)", L"*.zip"}});
                if (!f.empty()) InstallPackPath(f);
            }
            ImGui::SetCursorScreenPos(ImVec2(x - Dp(36.0f), top + Dp(2.0f)));
            if (IconButton("##packfolder", icon::FolderOpen, Tr("설치된 팩 폴더 열기"))) OpenFolder(reg.UserRoot());
            x -= Dp(44.0f);
            ImGui::SetCursorScreenPos(ImVec2(x - Dp(36.0f), top + Dp(2.0f)));
            if (IconButton("##packreload", icon::Refresh, Tr("팩 다시 불러오기 (파일을 저장하면 자동으로 다시 불러옵니다)"))) {
                ResetShaderSourceHashes();
                reg.Scan();
            }
            x -= Dp(44.0f);
        } else {
            ImGui::SetCursorScreenPos(ImVec2(x - Dp(36.0f), top + Dp(2.0f)));
            ImGui::BeginDisabled(shaderStore_.State() == ShaderPackStore::IndexState::Loading);
            if (IconButton("##storerefresh", icon::Refresh, Tr("목록 새로 고침"))) RefreshGallery();
            ImGui::EndDisabled();
            x -= Dp(44.0f);
            ImGui::SetCursorScreenPos(ImVec2(x - Dp(36.0f), top + Dp(2.0f)));
            if (IconButton("##gallerysources", icon::Stack, Tr("갤러리 출처: 공식 갤러리 외에 다른 팩 목록(index.json) 추가"))) {
                gallerySourcesOpen_ = true;
                galleryUrl_[0] = 0;
                galleryUrlError_.clear();
            }
            x -= Dp(44.0f);
            if (rightButton("##storesite", Tr("갤러리 웹사이트"), icon::Globe, ButtonKind::Ghost))
                OpenUrl("https://mmdx.codingbot.kr/" + lang + "/shader-packs/");
        }

        float curX = tabsR + Dp(12.0f);

        // type filter: Segmented (전체 / 표면 셰이더 / 화면 효과 / 룩) — the installed / online tabs only
        if (shaderTab_ < 2) {
            const char* typeLabels[] = {Tr("전체"), Tr("표면 셰이더"), Tr("화면 효과"), Tr("룩"), Tr("월드 이펙트")};
            int curType = std::clamp(settings_.shaderType[shaderTab_], 0, 4);
            ImGui::SetCursorScreenPos(ImVec2(curX, top + Dp(2.0f)));
            if (Segmented("##packtype", typeLabels, 5, &curType, 0.0f, 36.0f)) {
                settings_.shaderType[shaderTab_] = curType;
                settings_.Save(settingsPath_);
            }
            curX = ImGui::GetItemRectMax().x + Dp(8.0f);

            // sort combo: compact combo (추천순 / 이름순 / 최근 업데이트 / 파라미터 많은 순)
            const char* sortLabels[] = {Tr("추천순"), Tr("이름순"), Tr("최근 업데이트"), Tr("파라미터 많은 순")};
            const float comboW = Dp(120.0f);
            const char* sortPreview = sortLabels[std::clamp(settings_.shaderSort[shaderTab_], 0, 3)];
            ImGui::SetCursorScreenPos(ImVec2(curX, top + Dp(2.0f)));
            ImGui::SetNextItemWidth(comboW);
            PushFont(Font::Regular, size::Small);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink));
            const bool openSort = ImGui::BeginCombo("##packsort", sortPreview, ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton);
            ImGui::PopStyleColor();
            {
                const ImVec2 cmax = ImGui::GetItemRectMax(), cmin = ImGui::GetItemRectMin();
                Icon(ImGui::GetWindowDrawList(), icon::CaretDown, 12.0f, ImVec2(cmax.x - Dp(12.0f), (cmin.y + cmax.y) * 0.5f), p.ink3);
            }
            if (openSort) {
                for (int k = 0; k < 4; ++k) {
                    const bool sel = settings_.shaderSort[shaderTab_] == k;
                    if (ImGui::Selectable(sortLabels[k], sel)) {
                        settings_.shaderSort[shaderTab_] = k;
                        settings_.Save(settingsPath_);
                    }
                }
                ImGui::EndCombo();
            }
            PopFont();
            curX = ImGui::GetItemRectMax().x + Dp(12.0f);
        }

        // Compute match count for result count and search field layout
        const std::string needle = ToLowerAscii(shaderFilter_);
        const int curType0 = std::clamp(settings_.shaderType[0], 0, 4);
        const int curType1 = std::clamp(settings_.shaderType[1], 0, 4);
        int matchCount = 0;
        if (shaderTab_ == 0) {
            for (const ShaderPack& pk : reg.Packs()) {
                if (curType0 == 1 && pk.type != PackType::Surface) continue;
                if (curType0 == 2 && pk.type != PackType::Effect) continue;
                if (curType0 == 3 && pk.type != PackType::Look) continue;
                if (curType0 == 4 && pk.type != PackType::World) continue;
                if (!PackMatches(needle, pk.id, pk.name, pk.authors, pk.tags)) continue;
                ++matchCount;
            }
        } else if (shaderTab_ == 1) {
            for (const RemotePack& rp : shaderStore_.Packs()) {
                if (curType1 == 1 && rp.type != "surface") continue;
                if (curType1 == 2 && rp.type != "effect") continue;
                if (curType1 == 3 && rp.type != "look") continue;
                if (curType1 == 4 && rp.type != "world") continue;
                if (!PackMatches(needle, rp.id, rp.name, rp.authors, rp.tags)) continue;
                ++matchCount;
            }
        } else {
            for (const EffectAsset& e : library_.effects) {
                if (!needle.empty() && ToLowerAscii(e.displayName).find(needle) == std::string::npos &&
                    ToLowerAscii(e.id).find(needle) == std::string::npos && ToLowerAscii(e.kind).find(needle) == std::string::npos)
                    continue;
                ++matchCount;
            }
        }

        // Result count on the right of the toolbar ("N개", caption, ink3)
        const std::string countStr = std::to_string(matchCount) + Tr("개");
        const ImVec2 countSz = TextSize(Font::Regular, size::Caption, countStr.c_str());
        const float countX = x - countSz.x - Dp(4.0f);
        Text(ImGui::GetWindowDrawList(), Font::Regular, size::Caption, ImVec2(countX, top + Dp(12.0f)), p.ink3, countStr.c_str());

        // Search field shrinks to make room
        const float searchRight = countX - Dp(12.0f);
        const float availSearch = (searchRight - curX) / Dpi();
        if (availSearch > 50.0f) {
            const float sw = std::min(240.0f, availSearch);
            ImGui::SetCursorScreenPos(ImVec2(searchRight - Dp(sw), top + Dp(1.0f)));
            SearchField("##packfilter", shaderFilter_, sizeof(shaderFilter_), Tr("이름, 작성자, 태그"), sw);
        }
    }

    // ---- grid
    const float gridTop = top + Dp(40.0f) + Dp(20.0f);
    ImGui::SetCursorScreenPos(ImVec2(leftX - Dp(6.0f), gridTop - Dp(6.0f)));
    ImGui::BeginChild("##packgrid", ImVec2(leftR - leftX + Dp(12.0f), ds.y - gridTop - pad + Dp(12.0f)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
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
        const std::string needle = ToLowerAscii(shaderFilter_);
        const int curType = shaderTab_ < 2 ? std::clamp(settings_.shaderType[shaderTab_], 0, 4) : 0;
        float contentH = 0;
        int n = 0;

        // one card; returns true when clicked
        const auto card = [&](const char* id, bool selected, uint64_t beforeTex, const std::string& beforeKey,
                              uint64_t tex, const std::string& key, const std::string& name,
                              const std::string& sub, const std::vector<std::pair<std::string, std::pair<ImU32, ImU32>>>& badges,
                              float progress, bool verified = false) {
            const int col = n % cols, row = n / cols;
            ++n;
            const ImVec2 ca(origin.x + col * (cw + cgap), origin.y + row * (chH + cgap));
            const ImVec2 cb(ca.x + cw, ca.y + chH);
            contentH = std::max(contentH, cb.y - origin.y);
            if (!ImGui::IsRectVisible(ImVec2(ca.x, ca.y - Dp(20)), ImVec2(cb.x, cb.y + Dp(20)))) return false;
            bool hovered = false;
            const bool clicked = CardItem(id, ca, cb, &hovered, name.c_str());
            uiprobe::Info(ImGui::GetItemID(), key.c_str(), nullptr);   // "p:<id>:..." / "r:<id>:<version>": by pack id
            const float hv = Anim(ImGui::GetID(id), hovered);
            const float sv = Anim(ImGui::GetID((std::string(id) + "s").c_str()), selected, 14.0f);
            const float lift = -Dp(3.0f) * hv;
            const ImVec2 a0(ca.x, ca.y + lift), b0(cb.x, cb.y + lift);
            const float r = Dp(14.0f);
            SoftShadow(dl, a0, b0, r, Dp(10.0f + 8.0f * hv), 0.07f + 0.07f * hv, ImVec2(0, Dp(3.0f + 3.0f * hv)));
            dl->AddRectFilled(a0, b0, p.surface, r);
            DrawPackCompare(id, dl, beforeTex, beforeKey, tex, key, a0, ImVec2(b0.x, a0.y + imgH), r,
                            ImDrawFlags_RoundCornersTop, PackCompareMode::Hover);
            if (sv > 0.01f) dl->AddRect(a0, b0, WithAlpha(p.accent, sv), r, 0, Dp(2.0f));
            const float tx = a0.x + Dp(14.0f), tr = b0.x - Dp(14.0f);
            float ty = a0.y + imgH + Dp(12.0f);
            TextEllipsis(dl, Font::Semibold, size::Body, ImVec2(tx, ty), tr, p.ink, name.c_str());
            if (verified) {
                const float nx = std::min(tx + TextSize(Font::Semibold, size::Body, name.c_str()).x + Dp(6.0f), tr - Dp(14.0f));
                VerifiedBadge(dl, ImVec2(nx + Dp(8.0f), ty + Dp(9.0f)), 15.0f);
                if (hovered) Tooltip(Tr("검수된 팩"));
            }
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

        // verified / community information comes from the online entry of the same id (when listed)
        const auto remoteOf = [&](const std::string& id) -> const RemotePack* {
            for (const RemotePack& r : shaderStore_.Packs())
                if (r.id == id) return &r;
            return nullptr;
        };
        if (shaderTab_ == 0) {
            std::vector<const ShaderPack*> installedList;
            for (const ShaderPack& pk : reg.Packs()) {
                if (curType == 1 && pk.type != PackType::Surface) continue;
                if (curType == 2 && pk.type != PackType::Effect) continue;
                if (curType == 3 && pk.type != PackType::Look) continue;
                if (curType == 4 && pk.type != PackType::World) continue;
                if (!PackMatches(needle, pk.id, pk.name, pk.authors, pk.tags)) continue;
                installedList.push_back(&pk);
            }
            const int sortMode = std::clamp(settings_.shaderSort[0], 0, 3);
            std::stable_sort(installedList.begin(), installedList.end(), [&](const ShaderPack* a, const ShaderPack* b) {
                if (sortMode == 0) {
                    const bool aBuiltIn = a->source == PackSource::BuiltIn;
                    const bool bBuiltIn = b->source == PackSource::BuiltIn;
                    if (aBuiltIn != bBuiltIn) return aBuiltIn > bBuiltIn;
                    return false;
                }
                if (sortMode == 1) {
                    const std::string na = ToLowerAscii(a->name.Get(lang));
                    const std::string nb = ToLowerAscii(b->name.Get(lang));
                    if (na != nb) return na < nb;
                    return false;
                }
                if (sortMode == 2) {
                    const auto itA = shaderPackWriteTimes_.find(a->id);
                    const auto itB = shaderPackWriteTimes_.find(b->id);
                    const auto tA = itA != shaderPackWriteTimes_.end() ? itA->second : std::filesystem::file_time_type::min();
                    const auto tB = itB != shaderPackWriteTimes_.end() ? itB->second : std::filesystem::file_time_type::min();
                    if (tA != tB) return tA > tB;
                    return false;
                }
                if (sortMode == 3) {
                    if (a->params.size() != b->params.size()) return a->params.size() > b->params.size();
                    return false;
                }
                return false;
            });

            for (const ShaderPack* pk : installedList) {
                std::vector<std::pair<std::string, std::pair<ImU32, ImU32>>> badges;
                const bool isEffect = pk->type == PackType::Effect;
                const char* typeText = TypeLabel(pk->type);
                const std::pair<ImU32, ImU32> typeCol = isEffect ? std::make_pair(WithAlpha(p.ink, 0.12f), p.ink)
                                                                 : std::make_pair(WithAlpha(p.ink, 0.05f), p.ink2);
                badges.push_back({typeText, typeCol});
                badges.push_back({SourceLabel(pk->source), {WithAlpha(p.ink, 0.06f), p.ink2}});
                const RemotePack* rp = remoteOf(pk->id);
                if (rp && !rp->verified) badges.push_back({Tr("미검수"), {WithAlpha(p.ink, 0.06f), p.ink3}});
                ImU32 bg = 0, fg = 0;
                if (const char* st = StatusLabel(pk->status, bg, fg)) badges.push_back({st, {bg, fg}});
                if (packGuard_.Quarantined(pk->id)) badges.push_back({Tr("충돌로 비활성화됨"), {p.dangerSoft, p.danger}});
                if (!pk->previewBefore.empty())
                    badges.push_back({Tr("전/후 비교"), {WithAlpha(p.ink, 0.06f), p.ink2}});
                const std::string sub = "v" + pk->version + (pk->authors.empty() ? "" : "  ·  " + PackAuthorLine(pk->authors));
                ImGui::PushID(PathToUtf8(pk->dir).c_str());
                if (card("##pc", shaderSelInstalled_ == pk->id, PackThumbBefore(*pk),
                         "pb:" + pk->id + ":" + std::to_string(reg.Generation()), PackThumb(*pk),
                         "p:" + pk->id + ":" + std::to_string(reg.Generation()), pk->name.Get(lang), sub, badges, -1.0f,
                         rp && rp->verified))
                    shaderSelInstalled_ = pk->id;
                ImGui::PopID();
            }
            if (n == 0) {
                if (reg.Packs().empty()) {
                    Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3,
                         Tr("설치된 이펙트 팩이 없습니다. 온라인 탭에서 받거나 zip을 창에 끌어 놓으세요"));
                } else {
                    Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3,
                         Tr("조건에 맞는 팩이 없습니다"));
                    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + Dp(38.0f)));
                    if (Button("##packclear", Tr("필터 지우기"), nullptr, ButtonKind::Ghost)) {
                        shaderFilter_[0] = '\0';
                        settings_.shaderType[0] = 0;
                        settings_.Save(settingsPath_);
                    }
                    contentH = std::max(contentH, Dp(80.0f));
                }
            }
        } else if (shaderTab_ == 1) {
            const auto st = shaderStore_.State();
            if (st == ShaderPackStore::IndexState::Loading) {
                Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3, Tr("온라인 목록을 불러오는 중..."));
            } else if (st == ShaderPackStore::IndexState::Failed) {
                Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.warn, shaderStore_.Error().c_str());
                ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + Dp(40.0f)));
                if (Button("##storeretry", Tr("다시 시도"), icon::Refresh)) RefreshGallery();
            } else {
                std::vector<const RemotePack*> onlineList;
                for (const RemotePack& rp : shaderStore_.Packs()) {
                    if (curType == 1 && rp.type != "surface") continue;
                    if (curType == 2 && rp.type != "effect") continue;
                    if (curType == 3 && rp.type != "look") continue;
                    if (curType == 4 && rp.type != "world") continue;
                    if (!PackMatches(needle, rp.id, rp.name, rp.authors, rp.tags)) continue;
                    onlineList.push_back(&rp);
                }
                const int sortMode = std::clamp(settings_.shaderSort[1], 0, 3);
                std::stable_sort(onlineList.begin(), onlineList.end(), [&](const RemotePack* a, const RemotePack* b) {
                    if (sortMode == 0) {
                        if (a->builtIn != b->builtIn) return a->builtIn > b->builtIn;
                        if (a->updated != b->updated) return a->updated > b->updated;
                        return false;
                    }
                    if (sortMode == 1) {
                        const std::string na = ToLowerAscii(a->name.Get(lang));
                        const std::string nb = ToLowerAscii(b->name.Get(lang));
                        if (na != nb) return na < nb;
                        return false;
                    }
                    if (sortMode == 2) {
                        if (a->updated != b->updated) return a->updated > b->updated;
                        return false;
                    }
                    if (sortMode == 3) {
                        if (a->paramCount != b->paramCount) return a->paramCount > b->paramCount;
                        return false;
                    }
                    return false;
                });

                for (const RemotePack* rp : onlineList) {
                    std::vector<std::pair<std::string, std::pair<ImU32, ImU32>>> badges;
                    const bool isEffect = rp->type == "effect";
                    const char* typeText = isEffect              ? Tr("화면 효과")
                                         : rp->type == "world"   ? Tr("월드 이펙트")
                                         : rp->type == "look"    ? Tr("룩")
                                                                 : Tr("표면 셰이더");
                    const std::pair<ImU32, ImU32> typeCol = isEffect ? std::make_pair(WithAlpha(p.ink, 0.12f), p.ink)
                                                                     : std::make_pair(WithAlpha(p.ink, 0.05f), p.ink2);
                    badges.push_back({typeText, typeCol});
                    const ShaderPack* inst = InstalledVersionOf(rp->id);
                    if (!rp->verified) badges.push_back({Tr("미검수"), {WithAlpha(p.ink, 0.06f), p.ink3}});
                    if (!rp->Compatible()) badges.push_back({Tr("호환되지 않음"), {p.warnSoft, p.warn}});
                    else if (inst && UpdateAllowed(*inst, *rp))
                        badges.push_back({Tr("업데이트 있음"), {p.accentSoft, p.accentInk}});
                    else if (inst) badges.push_back({Tr("설치됨"), {WithAlpha(p.ink, 0.06f), p.ink2}});
                    if (rp->builtIn) badges.push_back({Tr("앱에 포함"), {WithAlpha(p.ink, 0.06f), p.ink2}});
                    if (!rp->previewBeforeUrl.empty())
                        badges.push_back({Tr("전/후 비교"), {WithAlpha(p.ink, 0.06f), p.ink2}});
                    std::string sub = "v" + rp->version + (rp->authors.empty() ? "" : "  ·  " + PackAuthorLine(rp->authors));
                    if (!rp->verified) {
                        if (!rp->owner.empty()) sub += "  ·  " + rp->owner;
                        if (!IsOfficialIndexUrl(rp->source)) sub += "  ·  " + SourceHost(rp->source);
                    }
                    ImGui::PushID(rp->id.c_str());
                    if (card("##rc", shaderSelRemote_ == rp->id, RemotePackThumbBefore(*rp),
                             "rb:" + rp->id + ":" + rp->version, RemotePackThumb(*rp), "r:" + rp->id + ":" + rp->version,
                             rp->name.Get(lang), sub, badges, shaderStore_.Installing(rp->id) ? shaderStore_.Progress(rp->id) : -1.0f,
                             rp->verified))
                        shaderSelRemote_ = rp->id;
                    ImGui::PopID();
                }
                if (n == 0) {
                    if (shaderStore_.Packs().empty()) {
                        Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3,
                             Tr("아직 갤러리에 팩이 없습니다"));
                    } else {
                        Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3,
                             Tr("조건에 맞는 팩이 없습니다"));
                        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + Dp(38.0f)));
                        if (Button("##packclear", Tr("필터 지우기"), nullptr, ButtonKind::Ghost)) {
                            shaderFilter_[0] = '\0';
                            settings_.shaderType[1] = 0;
                            settings_.Save(settingsPath_);
                        }
                        contentH = std::max(contentH, Dp(80.0f));
                    }
                }
            }
        } else {
            // M6b: the MME effects tab — the library's effects/ entries.
            std::vector<const EffectAsset*> list;
            for (const EffectAsset& e : library_.effects) {
                if (!needle.empty() && ToLowerAscii(e.displayName).find(needle) == std::string::npos &&
                    ToLowerAscii(e.id).find(needle) == std::string::npos && ToLowerAscii(e.kind).find(needle) == std::string::npos)
                    continue;
                list.push_back(&e);
            }
            std::stable_sort(list.begin(), list.end(), [](const EffectAsset* a, const EffectAsset* b) {
                return ToLowerAscii(a->displayName) < ToLowerAscii(b->displayName);
            });
            for (const EffectAsset* e : list) {
                std::vector<std::pair<std::string, std::pair<ImU32, ImU32>>> badges;
                const std::pair<ImU32, ImU32> kindCol =
                    e->kind == "post"   ? std::make_pair(p.accentSoft, p.accentInk)
                    : e->kind == "pre"  ? std::make_pair(WithAlpha(p.ink, 0.12f), p.ink)
                                        : std::make_pair(WithAlpha(p.ink, 0.05f), p.ink2);
                badges.push_back({e->kind == "post" ? Tr("화면 효과") : e->kind == "pre" ? Tr("사전 효과") : Tr("오브젝트 효과"),
                                  kindCol});
                ImGui::PushID(e->id.c_str());
                if (card("##mec", shaderSelMme_ == e->id, 0, {}, 0, "mme:" + e->id, e->displayName, e->id, badges, -1.0f))
                    shaderSelMme_ = e->id;
                ImGui::PopID();
            }
            if (n == 0) {
                if (library_.effects.empty()) {
                    Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3,
                         Tr("라이브러리 effects 폴더에 MME 효과가 없습니다"));
                } else {
                    Text(dl, Font::Regular, size::Body, ImVec2(origin.x, origin.y + Dp(8.0f)), p.ink3,
                         Tr("조건에 맞는 효과가 없습니다"));
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
        if (shaderTab_ == 0) DrawShaderPackDetail(pa.x, pa.y, pb.x, pb.y);
        else if (shaderTab_ == 1) DrawRemotePackDetail(pa.x, pa.y, pb.x, pb.y);
        else DrawMmeDetail(pa.x, pa.y, pb.x, pb.y);
    }
    DrawNewPackDialog();
    DrawGallerySourcesDialog();
    DrawUnverifiedInstallDialog();
    DrawToast();
    EndScreen();
}

void App::DrawShaderPackDetail(float x0, float y0, float x1, float y1) {
    const Palette& p = P();
    ShaderPackRegistry& reg = ShaderPacks();
    const std::string lang = PackLanguage();
    const ShaderPack* pk = reg.Find(shaderSelInstalled_);
    if (!pk && !reg.Packs().empty()) {
        shaderSelInstalled_ = reg.Packs().front().id;
        pk = &reg.Packs().front();
    }
    const float ip = Dp(22.0f);
    ImGui::SetCursorScreenPos(ImVec2(x0 + ip, y0 + ip));
    ImGui::BeginChild("##packdetail", ImVec2(x1 - x0 - ip * 2.0f + Dp(8.0f), y1 - y0 - ip * 2.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    const float w = x1 - x0 - ip * 2.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    if (!pk) {
        Para(Tr("MMDX 이펙트는 표면 셰이더 · 화면 효과 · 월드 이펙트 · 룩으로 장면을 꾸미는 확장입니다. 누구나 만들어 공유할 수 있습니다."), p.ink2);
        Gap(10.0f);
        if (Button("##docs0", Tr("만드는 법 보기"), icon::Info, ButtonKind::Ghost))
            OpenUrl("https://mmdx.codingbot.kr/" + lang + "/docs/shader-packs/");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + w * 9.0f / 16.0f);
        const std::string afterKey = "p:" + pk->id + ":" + std::to_string(reg.Generation());
        const std::string beforeKey = "pb:" + pk->id + ":" + std::to_string(reg.Generation());
        DrawPackCompare("##cmp_installed", dl, PackThumbBefore(*pk), beforeKey, PackThumb(*pk), afterKey, a, b,
                        Dp(12.0f), 0, PackCompareMode::Drag);
        ImGui::Dummy(ImVec2(w, b.y - a.y));
        if (!pk->previewBefore.empty()) {
            Gap(6.0f);
            Text(dl, Font::Regular, size::Caption, ImGui::GetCursorScreenPos(), p.ink3,
                 Tr("가운데 선을 끌어 적용 전후를 비교하세요"));
            ImGui::Dummy(ImVec2(w, Dp(16.0f)));
        }
    }
    Gap(14.0f);
    Para(pk->name.Get(lang), p.ink, size::Heading, Font::Bold);
    Para("v" + pk->version + "  ·  " + pk->id + "  ·  " + SourceLabel(pk->source) +
             (pk->stateFloats > 0 ? "  ·  " + std::to_string(pk->stateFloats) + " floats" : ""),
         p.ink3, size::Caption);
    {
        const RemotePack* listing = nullptr;
        for (const RemotePack& r : shaderStore_.Packs())
            if (r.id == pk->id) listing = &r;
        if (listing) {
            if (listing->verified) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                VerifiedBadge(dl, ImVec2(at.x + Dp(8.0f), at.y + Dp(9.0f)), 15.0f);
                Text(dl, Font::Semibold, size::Caption, ImVec2(at.x + Dp(22.0f), at.y), p.ink, Tr("검수된 팩"));
                ImGui::Dummy(ImVec2(w, Dp(18.0f)));
            } else {
                std::string line = std::string(Tr("미검수"));
                if (!listing->owner.empty()) line += "  ·  " + listing->owner;
                if (!IsOfficialIndexUrl(listing->source)) line += "  ·  " + SourceHost(listing->source);
                Para(line, p.ink3, size::Caption);
            }
        }
    }

    // problems first: authors need them, users need to know why it is not selectable
    if (packGuard_.Quarantined(pk->id)) {
        Gap(12.0f);
        Para(Tr("충돌로 비활성화됨"), p.danger, size::Small, Font::Semibold);
        Para(Tr("이 팩을 쓰는 동안 앱이 비정상 종료되어 다음 실행부터 꺼 두었습니다."), p.ink2, size::Caption);
        if (Button("##packrelease", Tr("다시 사용"), icon::Refresh, ButtonKind::Secondary)) {
            packGuard_.Release(pk->id);
            ApplyRenderSettings();
        }
    }
    if (!pk->Selectable() || pk->status == PackStatus::CompileError || packGuard_.Quarantined(pk->id)) {
        Gap(12.0f);
        ImU32 bg = 0, fg = 0;
        const char* st = StatusLabel(pk->status, bg, fg);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(ImVec2(a.x + Dp(12.0f), a.y + Dp(10.0f)));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - Dp(24.0f));
        Para(st ? st : "", fg, size::Small, Font::Semibold);
        Gap(4.0f);
        std::string msg = pk->statusMessage;
        if (msg.size() > 1500) msg = msg.substr(0, 1500) + "\n...";
        Para(msg.empty() ? std::string(Tr("mmdx12.log 에 자세한 내용이 있습니다")) : msg, p.ink2, size::Caption);
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        const ImVec2 gb = ImGui::GetItemRectMax();
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(a, ImVec2(a.x + w, gb.y + Dp(10.0f)), bg ? bg : p.sunken, Dp(10.0f));
        dl->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(a.x, gb.y + Dp(10.0f)));
        ImGui::Dummy(ImVec2(w, 0));
    }

    Gap(14.0f);
    if (shaderDetailTab_ < 0 || shaderDetailTab_ > 2) shaderDetailTab_ = 0;
    const char* tabLabels[] = {Tr("정보"), Tr("설정"), Tr("관리")};
    Segmented("##packdetailtab", tabLabels, 3, &shaderDetailTab_, w / Dpi(), 36.0f);

    if (shaderDetailTab_ == 0) {
        if (!pk->description.Empty()) {
            Gap(12.0f);
            Para(pk->description.Get(lang), p.ink2);
        }
        if (!pk->recommendedFor.Empty()) {
            Caption(Tr("이런 모델에 맞춰 만들었어요"));
            Para(pk->recommendedFor.Get(lang), p.ink2);
        }
        if (pk->type == PackType::Surface) {
            Caption(Tr("지원하는 렌더 경로"));
            Para(pk->hasPtSurface ? Tr("래스터 · 레이 트레이싱 · 패스 트레이싱 · 오프라인 GI에 모두 적용") : Tr("래스터 · 레이 트레이싱에 적용 (이 팩은 패스 트레이싱 · 오프라인 GI용 셰이더가 없어 기본 셰이딩)"), p.ink2);
        }
        if (!pk->authors.empty()) {
            Caption(Tr("만든 사람"));
            for (size_t i = 0; i < pk->authors.size(); ++i) {
                const PackAuthor& a = pk->authors[i];
                const ImVec2 c = ImGui::GetCursorScreenPos();
                Text(dl, Font::Semibold, size::Small, ImVec2(c.x, c.y + Dp(6.0f)), p.ink, a.name.c_str());
                if (!a.role.empty())
                    Text(dl, Font::Regular, size::Caption,
                         ImVec2(c.x + TextSize(Font::Semibold, size::Small, a.name.c_str()).x + Dp(8.0f), c.y + Dp(7.0f)), p.ink3,
                         a.role.c_str());
                if (!a.url.empty()) {
                    ImGui::SetCursorScreenPos(ImVec2(c.x + w - Dp(32.0f), c.y));
                    ImGui::PushID((int)i);
                    if (IconButton("##authorlink", icon::LinkSimple, (std::string(Tr("웹사이트 열기: ")) + a.url).c_str(), false, 28.0f)) OpenUrl(a.url);
                    ImGui::PopID();
                }
                ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(30.0f)));
                ImGui::Dummy(ImVec2(w, 0));
            }
        }
        if (!pk->license.empty() || !pk->homepage.empty() || !pk->repository.empty()) {
            Caption(Tr("라이선스 · 링크"));
            if (!pk->license.empty()) Para(pk->license, p.ink2);
            if (!pk->homepage.empty()) {
                Gap(4.0f);
                if (Button("##home", Tr("홈페이지"), icon::Globe, ButtonKind::Ghost)) OpenUrl(pk->homepage);
                if (!pk->repository.empty()) ImGui::SameLine();
            }
            if (!pk->repository.empty() && Button("##repo", Tr("소스 저장소"), icon::LinkSimple, ButtonKind::Ghost))
                OpenUrl(pk->repository);
        }
        if (!pk->tags.empty()) {
            Caption(Tr("태그"));
            BadgeFlow(pk->tags, w);
        }
    } else if (shaderDetailTab_ == 1) {
        if (!pk->params.empty()) {
            Caption(Tr("조절할 수 있는 값"));
            for (const ShaderPackParam& sp : pk->params) {
                const ImVec2 c = ImGui::GetCursorScreenPos();
                std::string label = sp.label.Empty() ? sp.key : sp.label.Get(lang);
                if (!sp.group.empty()) label = "[" + sp.group + "] " + label;
                char range[64];
                if (sp.type == ShaderPackParamType::Float) {
                    std::snprintf(range, sizeof(range), "%.2f  (%.2f – %.2f)", sp.def, sp.min, sp.max);
                } else if (sp.type == ShaderPackParamType::Int) {
                    std::snprintf(range, sizeof(range), "%.0f  (%.0f – %.0f)", sp.def, sp.min, sp.max);
                } else if (sp.type == ShaderPackParamType::Bool) {
                    std::snprintf(range, sizeof(range), "%s", sp.defBool ? "true" : "false");
                } else if (sp.type == ShaderPackParamType::Color) {
                    if (sp.alpha) {
                        std::snprintf(range, sizeof(range), "[%.2f, %.2f, %.2f, %.2f]", sp.defVec[0], sp.defVec[1], sp.defVec[2], sp.defVec[3]);
                    } else {
                        std::snprintf(range, sizeof(range), "[%.2f, %.2f, %.2f]", sp.defVec[0], sp.defVec[1], sp.defVec[2]);
                    }
                } else if (sp.type == ShaderPackParamType::Vec3) {
                    std::snprintf(range, sizeof(range), "[%.2f, %.2f, %.2f]", sp.defVec[0], sp.defVec[1], sp.defVec[2]);
                }
                TextEllipsis(dl, Font::Regular, size::Small, c, c.x + w * 0.6f, p.ink2, label.c_str());
                const ImVec2 rs = TextSize(Font::Regular, size::Caption, range);
                Text(dl, Font::Regular, size::Caption, ImVec2(c.x + w - rs.x, c.y + Dp(1.0f)), p.ink3, range);
                ImGui::Dummy(ImVec2(w, Dp(22.0f)));
            }
        }
        if (!pk->textures.empty()) {
            Caption(Tr("팩 텍스처"));
            uint32_t missingUser = 0, missingBundled = 0;
            std::filesystem::path probe;
            for (const PackTexture& t : pk->textures)
                if (!ResolvePackTextureFile(*pk, t.file, reg.TextureFolder(pk->id), probe)) {
                    if (t.origin == PackTexture::Origin::User) ++missingUser;
                    else ++missingBundled;
                }
            for (const PackTexture& t : pk->textures) {
                const ImVec2 c = ImGui::GetCursorScreenPos();
                const std::string origin = t.origin == PackTexture::Origin::User ? Tr("사용자") : Tr("팩 포함");
                std::string line = t.file + (t.clamp ? " (clamp)" : "") + "  ·  " + origin;
                TextEllipsis(dl, Font::Regular, size::Caption, c, c.x + w, p.ink3, line.c_str());
                ImGui::Dummy(ImVec2(w, Dp(16.0f)));
            }
            const uint32_t missing = reg.MissingTextures(pk->id, reg.TextureFolder(pk->id));
            if (missing)
                Para(std::to_string(missing) +
                         Tr("개의 팩 텍스처가 없어 흰색으로 표시됩니다 - 텍스처 폴더를 지정하세요"),
                     p.warn, size::Caption);
            if (missingBundled)
                Para(std::string(Tr("팩 파일 누락")) + " (" + std::to_string(missingBundled) + ")", p.danger, size::Caption);
            // the user texture folder: game textures that can't be redistributed (picker + clear)
            const std::filesystem::path folder = reg.TextureFolder(pk->id);
            ImGui::PushID("##packtexfolder");
            {
                const ImVec2 c = ImGui::GetCursorScreenPos();
                const std::string shown =
                    std::string(Tr("텍스처 폴더")) + ": " + (folder.empty() ? std::string(Tr("없음")) : PathToUtf8(folder));
                TextEllipsis(dl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(6.0f)), c.x + w - Dp(96.0f), p.ink2,
                             shown.c_str());
                ImGui::SetCursorScreenPos(ImVec2(c.x + w - Dp(88.0f), c.y));
                if (IconButton("##pick", icon::FolderOpen, Tr("텍스처 폴더 선택"))) {
                    const std::filesystem::path picked = PickPackTextureFolder(hwnd_);
                    if (!picked.empty()) {
                        reg.SetTextureFolder(pk->id, picked);
                        settings_.packTextureFolders[pk->id] = PathToUtf8(picked);
                        settings_.Save(settingsPath_);
                    }
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(folder.empty());
                if (IconButton("##clear", icon::X, Tr("텍스처 폴더 지우기"))) {
                    reg.SetTextureFolder(pk->id, {});
                    settings_.packTextureFolders.erase(pk->id);
                    settings_.Save(settingsPath_);
                }
                ImGui::EndDisabled();
                if (missingUser)
                    Text(dl, Font::Regular, size::Caption, ImVec2(c.x, c.y + Dp(34.0f)), p.warn,
                         (std::string(Tr("사용자 텍스처 폴더 연결 필요")) + " (" + std::to_string(missingUser) + ")").c_str());
                ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(missingUser ? 54.0f : 30.0f)));
                ImGui::Dummy(ImVec2(w, 0));
            }
            ImGui::PopID();
        }
        if (pk->params.empty() && pk->textures.empty()) {
            Gap(12.0f);
            Para(Tr("조절할 수 있는 항목이 없습니다"), p.ink3, size::Caption);
        }
        if (pk->type == PackType::World) {
            // installing a world does not place it: add / remove it from the user's world stack right here
            Gap(12.0f);
            auto& stack = settings_.worldStack;
            const auto it = std::find_if(stack.begin(), stack.end(), [&](const EffectStackEntry& e) { return e.pack == pk->id; });
            const bool inStack = it != stack.end();
            if (!inStack ? Button("##worldstackadd", Tr("월드에 추가"), icon::Plus, ButtonKind::Primary)
                         : Button("##worldstackremove", Tr("월드에서 빼기"), icon::X, ButtonKind::Secondary)) {
                if (inStack) stack.erase(it);
                else stack.push_back({pk->id, true, {}, {}});
                settings_.Save(settingsPath_);
                ApplyRenderSettings();
            }
            Gap(8.0f);
            Para(Tr("켠 월드는 로비의 세부 탭(\"월드 이펙트\")과 재생 바의 화면 효과 버튼에서 끄거나 순서를 바꿀 수 있습니다."), p.ink3,
                 size::Caption);
        }
        if (pk->type == PackType::Effect) {
            // installing an effect does not turn it on: add / remove it from the user's stack right here
            Gap(12.0f);
            auto& stack = settings_.effectStack;
            const auto it = std::find_if(stack.begin(), stack.end(), [&](const EffectStackEntry& e) { return e.pack == pk->id; });
            const bool inStack = it != stack.end();
            if (!inStack ? Button("##fxstackadd", Tr("화면 효과에 추가"), icon::Plus, ButtonKind::Primary)
                         : Button("##fxstackremove", Tr("화면 효과에서 빼기"), icon::X, ButtonKind::Secondary)) {
                if (inStack) stack.erase(it);
                else stack.push_back({pk->id, true, {}, {}});
                settings_.Save(settingsPath_);
                ApplyRenderSettings();
            }
            Gap(8.0f);
            Para(Tr("켠 효과는 로비의 이펙트 탭(\"화면 효과\")과 재생 바의 화면 효과 버튼에서 끄거나 순서를 바꿀 수 있습니다."), p.ink3,
                 size::Caption);
        }
    } else if (shaderDetailTab_ == 2) {
        Gap(12.0f);
        if (Button("##openpack", Tr("폴더 열기"), icon::FolderOpen, ButtonKind::Secondary)) OpenFolder(pk->dir);
        if (pk->source != PackSource::BuiltIn) {
            ImGui::SameLine();
            if (Button("##removepack", Tr("삭제"), icon::Trash, ButtonKind::Danger)) ImGui::OpenPopup("##removeconfirm");
        }
        Gap(8.0f);
        if (Button("##packdocs", Tr("이펙트 팩 만드는 법"), icon::Info, ButtonKind::Ghost))
            OpenUrl("https://mmdx.codingbot.kr/" + lang + "/docs/shader-packs/");
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(18.0f), Dp(16.0f)));
        // Para wraps at the window edge, so the auto-sized popup needs a width (else one word per line).
        ImGui::SetNextWindowSize(ImVec2(Dp(340.0f), 0.0f));
        if (ImGui::BeginPopup("##removeconfirm")) {
            const std::string q = pk->name.Get(lang) + Tr(" 팩을 삭제할까요? 이 팩을 쓰던 캐릭터는 기본 셰이딩으로 돌아갑니다.");
            Para(q, p.ink);
            Gap(10.0f);
            if (Button("##removeyes", Tr("삭제"), icon::Trash, ButtonKind::Danger)) {
                std::string err;
                const std::string id = pk->id;
                if (!reg.Uninstall(id, err)) {
                    toast_ = Toast{};
                    toast_.title = Tr("삭제하지 못했습니다");
                    toast_.detail = err;
                    toast_.error = true;
                    toast_.until = timeSeconds_ + 6.0;
                }
                shaderSelInstalled_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Button("##removeno", Tr("취소"), nullptr, ButtonKind::Ghost)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// ---- effect stack editor ------------------------------------------------------------------------------------------------------

// M6b: the MME tab's detail pane (docs/mme/mme_design.md §2.13): compile status per pass, unsupported features and
// missing textures (the parsed diagnostics), and the post-effect "효과 스택에 추가" action. Compiling for the
// diagnostics runs on a worker thread (a spinner while it runs).
void App::DrawMmeDetail(float x0, float y0, float x1, float y1) {
    const Palette& p = P();
    const EffectAsset* sel = nullptr;
    for (const EffectAsset& e : library_.effects)
        if (e.id == shaderSelMme_) sel = &e;
    if (!sel && !library_.effects.empty()) {
        sel = &library_.effects.front();
        shaderSelMme_ = sel->id;
    }
    const float ip = Dp(22.0f);
    ImGui::SetCursorScreenPos(ImVec2(x0 + ip, y0 + ip));
    ImGui::BeginChild("##mmedetail", ImVec2(x1 - x0 - ip * 2.0f + Dp(8.0f), y1 - y0 - ip * 2.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    const float w = x1 - x0 - ip * 2.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    if (!sel) {
        Para(Tr("MME 효과는 MikuMikuEffect(.fx) 파일입니다. 라이브러리 effects 폴더에 넣으면 여기에 나타납니다."), p.ink2);
        Gap(10.0f);
        Para(Tr("화면 효과는 재생 중 화면 전체에, 오브젝트 효과는 캐릭터·스테이지 재질에 적용됩니다."), p.ink3, size::Caption);
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Para(sel->displayName, p.ink, size::Heading, Font::Bold);
    const char* kindLabel = sel->kind == "post" ? Tr("화면 효과") : sel->kind == "pre" ? Tr("사전 효과") : Tr("오브젝트 효과");
    Para(std::string(kindLabel) + "  ·  " + sel->id, p.ink3, size::Caption);

    // Ray-MMD diagnostics note: the native PBR serves Ray-MMD; running it through MME is not recommended.
    if (IsRayMmdMainEffect(sel->path) || IsRayMaterialFx(sel->path))
        Para(Tr("Ray-MMD: 네이티브 PBR로 지원 (MME 실행은 비권장)"), p.warn, size::Caption);

    // Compile for diagnostics when the selection changes (async, one at a time). The spinner shows while running.
    if (mmeDiagFile_ != sel->id) {
        mmeDiagFile_ = sel->id;
        mmeDiagJob_.Start(sel->path);
        mmeDiag_ = MmeEffectDiag{};
    }
    const bool busy = !mmeDiagJob_.Busy() && mmeDiag_.passes.empty() && mmeDiag_.kind.empty() && mmeDiagFile_ == sel->id;
    // poll on every frame (the future is ready a few frames after selection)
    if (mmeDiagJob_.Busy() && mmeDiagJob_.Poll(mmeDiag_)) { /* done */ }
    const bool compiling = mmeDiagJob_.Busy() && mmeDiag_.kind.empty();
    (void)busy;

    Gap(12.0f);
    // post effects can be added to the play-mode MME post stack right here
    if (sel->kind == "post") {
        auto& stack = settings_.mmeEffects;
        const auto it = std::find_if(stack.begin(), stack.end(),
                                     [&](const MmeEffectEntry& e) { return e.file == sel->path; });
        const bool inStack = it != stack.end();
        if (!inStack ? Button("##mestackadd", Tr("효과 스택에 추가"), icon::Plus, ButtonKind::Primary)
                     : Button("##mestackremove", Tr("효과 스택에서 빼기"), icon::X, ButtonKind::Secondary)) {
            if (inStack) stack.erase(it);
            else if (GuardRayMme(sel->path, [this](const std::filesystem::path& file) {
                         bool dup = false;
                         for (const MmeEffectEntry& x : settings_.mmeEffects) dup |= x.file == file;
                         if (!dup) {
                             MmeEffectEntry m;
                             m.file = file;
                             m.enabled = true;
                             settings_.mmeEffects.push_back(std::move(m));
                             settings_.Save(settingsPath_);
                             ApplyRenderSettings();
                         }
                     })) {
                // the confirm modal collects the add
            }
            else {
                MmeEffectEntry m;
                m.file = sel->path;
                m.enabled = true;
                stack.push_back(std::move(m));
            }
            settings_.Save(settingsPath_);
            ApplyRenderSettings();
        }
        Gap(8.0f);
        Para(Tr("켠 효과는 재생 바의 화면 효과 버튼에서 끄거나 순서를 바꿀 수 있습니다."), p.ink3, size::Caption);
        Gap(12.0f);
    } else {
        Para(Tr("오브젝트 효과는 로비 캐릭터 패널이나 스튜디오에서 모델에 적용합니다."), p.ink3, size::Caption);
        Gap(12.0f);
    }

    if (compiling) {
        Skeleton(dl, ImGui::GetCursorScreenPos(),
                 ImVec2(ImGui::GetCursorScreenPos().x + w, ImGui::GetCursorScreenPos().y + Dp(28.0f)), Dp(8.0f));
        ImGui::Dummy(ImVec2(w, Dp(32.0f)));
        Para(Tr("효과를 컴파일하는 중..."), p.ink3, size::Caption);
    } else {
        // compile status per pass
        SectionLabel(Tr("패스 컴파일"));
        if (mmeDiag_.passes.empty()) {
            Para(Tr("이 파일에는 컴파일할 패스가 없습니다"), p.ink3, size::Caption);
        } else {
            for (const MmeEffectDiag::Pass& pp : mmeDiag_.passes) {
                const ImVec2 c = ImGui::GetCursorScreenPos();
                Icon(dl, pp.ok ? icon::Check : icon::Warning, 14.0f, ImVec2(c.x + Dp(7.0f), c.y + Dp(9.0f)),
                     pp.ok ? p.accent : p.danger);
                Text(dl, Font::Regular, size::Small, ImVec2(c.x + Dp(22.0f), c.y + Dp(2.0f)), pp.ok ? p.ink : p.danger,
                     (pp.technique + " / " + pp.pass).c_str());
                if (!pp.ok && !pp.error.empty())
                    TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(c.x + Dp(22.0f), c.y + Dp(20.0f)),
                                 c.x + w, p.ink3, pp.error.c_str());
                ImGui::Dummy(ImVec2(w, pp.ok ? Dp(26.0f) : Dp(44.0f)));
            }
        }
        if (!mmeDiag_.unsupported.empty()) {
            SectionLabel(Tr("지원하지 않는 기능"));
            for (const std::string& s : mmeDiag_.unsupported) Para("· " + s, p.warn, size::Caption);
        }
        if (!mmeDiag_.missingTextures.empty()) {
            SectionLabel(Tr("없는 텍스처"));
            for (const std::string& s : mmeDiag_.missingTextures)
                Para("· " + s + "  " + Tr("(흰색으로 표시)"), p.ink3, size::Caption);
        }
        if (mmeDiag_.ok && mmeDiag_.unsupported.empty() && mmeDiag_.missingTextures.empty()) {
            Gap(12.0f);
            Para(Tr("이 효과는 모든 패스가 정상적으로 컴파일됩니다."), p.accentInk, size::Small, Font::Semibold);
        }
        if (!mmeDiag_.kind.empty()) {
            Gap(8.0f);
            char meta[96];
            std::snprintf(meta, sizeof(meta), "%d/%d %s", (int)(mmeDiag_.passes.size() - mmeDiag_.unsupported.size()),
                          (int)mmeDiag_.passes.size(), Tr("패스"));
            Para(meta, p.ink3, size::Caption);
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

// The ordered effect stack (RenderSettings::packEffects): per entry an enable switch, up / down / remove and the pack's
// sliders; "효과 추가" appends an installed effect pack. True = the stack changed this frame (the caller saves + applies).
bool App::DrawEffectStackEditor(std::vector<EffectStackEntry>& stack, bool header) {
    const Palette& p = P();
    ShaderPackRegistry& reg = ShaderPacks();
    const std::string lang = PackLanguage();

    if (header) SectionLabel(Tr("효과 스택"));
    Para(Tr("위에서 아래로 화면 효과를 차례로 적용합니다. 영상·스틸 렌더에도 같은 스택이 쓰입니다."), p.ink3, size::Caption);
    Gap(6.0f);

    // Structural edits (move / remove) are applied after the loop so every entry keeps drawing on the frame a slider
    // changes; breaking out early shrank the layout mid-drag and made the panel jitter.
    bool changed = false;
    size_t swapWith = SIZE_MAX, removeAt = SIZE_MAX, swapA = 0;
    for (size_t i = 0; i < stack.size(); ++i) {
        EffectStackEntry& e = stack[i];
        const ShaderPack* pk = reg.Find(e.pack);
        const bool usable = pk && PackUsable(pk) && pk->type == PackType::Effect;
        const std::string name = pk ? pk->name.Get(lang) : e.pack;
        const char* where = !pk ? Tr("설치되어 있지 않음")
                            : !usable ? Tr("사용할 수 없음 · 이펙트 탭에서 확인")
                            : pk->stage == PackEffectStage::PreBloom ? Tr("블룸 앞 (HDR)") : Tr("톤맵 뒤");
        ImGui::PushID((int)i);
        bool on = e.enabled;
        if (Switch("##on", name.c_str(), &on, where)) {
            e.enabled = on;
            changed = true;
        }
        if (IconButton("##up", icon::CaretUp, Tr("위로"), false, 28.0f) && i > 0) {
            swapA = i - 1;
            swapWith = i;
        }
        ImGui::SameLine();
        if (IconButton("##down", icon::CaretDown, Tr("아래로"), false, 28.0f) && i + 1 < stack.size()) {
            swapA = i;
            swapWith = i + 1;
        }
        ImGui::SameLine();
        if (IconButton("##remove", icon::X, Tr("스택에서 제거"), false, 28.0f)) removeAt = i;
        if (usable && e.enabled && !pk->params.empty()) {
            changed |= DrawShaderPackParameters(*pk, e.params, "##fx");
        }
        ImGui::PopID();
        Gap(8.0f);
    }
    if (removeAt != SIZE_MAX) {
        stack.erase(stack.begin() + (long)removeAt);
        changed = true;
    } else if (swapWith != SIZE_MAX) {
        std::swap(stack[swapA], stack[swapWith]);
        changed = true;
    }

    std::vector<const ShaderPack*> addable;
    for (const ShaderPack& pk : reg.Packs()) {
        if (pk.type != PackType::Effect || !PackUsable(&pk)) continue;
        bool in = false;
        for (const EffectStackEntry& e : stack) in |= e.pack == pk.id;
        if (!in) addable.push_back(&pk);
    }
    if (!addable.empty()) {
        if (Button("##fxadd", Tr("효과 추가"), icon::Plus, ButtonKind::Secondary)) ImGui::OpenPopup("##fxaddpopup");
        ImGui::SetNextWindowSize(ImVec2(Dp(300.0f), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
        if (ImGui::BeginPopup("##fxaddpopup")) {
            for (const ShaderPack* pk : addable) {
                const std::string rid = "##fxadd_" + pk->id;
                if (MenuItem(rid.c_str(), pk->name.Get(lang).c_str())) {
                    stack.push_back({pk->id, true, {}, {}});
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    if (stack.empty()) {
        Para(Tr("효과가 없습니다. 화면 효과를 추가하면 위에서 아래로 차례로 적용됩니다."), p.ink3, size::Caption);
        if (addable.empty()) {   // nothing installed yet: say where effects come from
            Gap(6.0f);
            if (screen_ == Screen::Select) {   // the lobby can switch screens; play / studio would lose the scene
                if (Button("##fxgetonline", Tr("온라인에서 효과 받기"), icon::Globe, ButtonKind::Secondary)) {
                    shaderTab_ = 1;
                    screen_ = Screen::Shaders;
                }
            } else {
                Para(Tr("화면 효과는 라이브러리 화면의 이펙트 탭 > 온라인에서 받을 수 있습니다."), p.ink3, size::Caption);
            }
        }
    }
    return changed;
}

void App::DrawRemotePackDetail(float x0, float y0, float x1, float y1) {
    const Palette& p = P();
    const std::string lang = PackLanguage();
    const RemotePack* rp = nullptr;
    for (const RemotePack& r : shaderStore_.Packs())
        if (r.id == shaderSelRemote_) rp = &r;
    if (!rp && !shaderStore_.Packs().empty()) {
        rp = &shaderStore_.Packs().front();
        shaderSelRemote_ = rp->id;
    }
    const float ip = Dp(22.0f);
    ImGui::SetCursorScreenPos(ImVec2(x0 + ip, y0 + ip));
    ImGui::BeginChild("##remotedetail", ImVec2(x1 - x0 - ip * 2.0f + Dp(8.0f), y1 - y0 - ip * 2.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    const float w = x1 - x0 - ip * 2.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    if (!rp) {
        Para(Tr("온라인 갤러리의 팩은 mmdx.codingbot.kr 에 올라온 것들입니다. 받은 파일은 크기와 SHA-256 을 확인한 뒤 설치합니다."), p.ink2);
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    {
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + w * 9.0f / 16.0f);
        const std::string afterKey = "r:" + rp->id + ":" + rp->version;
        const std::string beforeKey = "rb:" + rp->id + ":" + rp->version;
        DrawPackCompare("##cmp_remote", dl, RemotePackThumbBefore(*rp), beforeKey, RemotePackThumb(*rp), afterKey,
                        a, b, Dp(12.0f), 0, PackCompareMode::Drag);
        ImGui::Dummy(ImVec2(w, b.y - a.y));
        if (!rp->previewBeforeFile.empty() || !rp->previewBeforeUrl.empty()) {
            Gap(6.0f);
            Text(dl, Font::Regular, size::Caption, ImGui::GetCursorScreenPos(), p.ink3,
                 Tr("가운데 선을 끌어 적용 전후를 비교하세요"));
            ImGui::Dummy(ImVec2(w, Dp(16.0f)));
        }
    }
    Gap(14.0f);
    Para(rp->name.Get(lang), p.ink, size::Heading, Font::Bold);
    char meta[160];
    std::snprintf(meta, sizeof(meta), "v%s  ·  %s  ·  %.1f KB", rp->version.c_str(), rp->id.c_str(), rp->size / 1024.0);
    Para(meta, p.ink3, size::Caption);
    if (rp->verified) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        VerifiedBadge(ImGui::GetWindowDrawList(), ImVec2(at.x + Dp(8.0f), at.y + Dp(9.0f)), 15.0f);
        Text(ImGui::GetWindowDrawList(), Font::Semibold, size::Caption, ImVec2(at.x + Dp(22.0f), at.y), p.ink,
             Tr("검수된 팩"));
        ImGui::Dummy(ImVec2(1.0f, Dp(20.0f)));
    } else {
        std::string line = std::string(Tr("미검수"));
        if (!rp->owner.empty()) line += "  ·  " + rp->owner;
        if (!IsOfficialIndexUrl(rp->source)) line += "  ·  " + SourceHost(rp->source);
        Para(line, p.ink3, size::Caption);
    }

    // install / update
    Gap(14.0f);
    const ShaderPack* inst = InstalledVersionOf(rp->id);
    if (shaderStore_.Installing(rp->id)) {
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ProgressBar(dl, a, ImVec2(a.x + w, a.y + Dp(6.0f)), shaderStore_.Progress(rp->id));
        ImGui::Dummy(ImVec2(w, Dp(12.0f)));
        Para(Tr("받는 중..."), p.ink3, size::Caption);
    } else if (!rp->Compatible()) {
        Para(Tr("이 버전의 MMDX12 에서는 쓸 수 없는 팩입니다. 앱을 업데이트하세요."), p.warn);
    } else if (inst && inst->source == PackSource::BuiltIn && net::CompareVersions(rp->version, inst->version) <= 0) {
        Para(Tr("앱에 이미 포함되어 있습니다."), p.ink3);
    } else if (inst && !UpdateAllowed(*inst, *rp)) {
        Para(Tr("최신 버전이 설치되어 있음"), p.ink3);
        Para(Tr("설치된 버전: v") + inst->version, p.ink3, size::Caption);
    } else {
        const bool newer = inst != nullptr;
        const char* label = !inst ? Tr("설치") : Tr("업데이트");
        if (Button("##storeinstall", label, icon::DownloadSimple, ButtonKind::Primary, ImVec2(w / Dpi(), 44.0f))) {
            if (!rp->verified) pendingUnverifiedInstall_ = rp->id;
            else shaderStore_.Install(*rp);
        }
        if (newer) Para(Tr("설치된 버전: v") + inst->version, p.ink3, size::Caption);
    }

    if (!rp->description.Empty()) {
        Gap(12.0f);
        Para(rp->description.Get(lang), p.ink2);
    }
    if (!rp->recommendedFor.Empty()) {
        Caption(Tr("이런 모델에 맞춰 만들었어요"));
        Para(rp->recommendedFor.Get(lang), p.ink2);
    }
    if (!rp->authors.empty()) {
        Caption(Tr("만든 사람"));
        for (size_t i = 0; i < rp->authors.size(); ++i) {
            const PackAuthor& a = rp->authors[i];
            const ImVec2 c = ImGui::GetCursorScreenPos();
            Text(dl, Font::Semibold, size::Small, ImVec2(c.x, c.y + Dp(6.0f)), p.ink, a.name.c_str());
            if (!a.role.empty())
                Text(dl, Font::Regular, size::Caption,
                     ImVec2(c.x + TextSize(Font::Semibold, size::Small, a.name.c_str()).x + Dp(8.0f), c.y + Dp(7.0f)), p.ink3,
                     a.role.c_str());
            if (!a.url.empty()) {
                ImGui::SetCursorScreenPos(ImVec2(c.x + w - Dp(32.0f), c.y));
                ImGui::PushID((int)i);
                if (IconButton("##authorlink", icon::LinkSimple, (std::string(Tr("웹사이트 열기: ")) + a.url).c_str(), false, 28.0f)) OpenUrl(a.url);
                ImGui::PopID();
            }
            ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(30.0f)));
            ImGui::Dummy(ImVec2(w, 0));
        }
    }
    if (!rp->license.empty() || !rp->homepage.empty() || !rp->repository.empty()) {
        Caption(Tr("라이선스 · 링크"));
        if (!rp->license.empty()) Para(rp->license, p.ink2);
        if (!rp->homepage.empty()) {
            Gap(4.0f);
            if (Button("##rhome", Tr("홈페이지"), icon::Globe, ButtonKind::Ghost)) OpenUrl(rp->homepage);
            if (!rp->repository.empty()) ImGui::SameLine();
        }
        if (!rp->repository.empty() && Button("##rrepo", Tr("소스 저장소"), icon::LinkSimple, ButtonKind::Ghost))
            OpenUrl(rp->repository);
    }
    if (!rp->tags.empty()) {
        Caption(Tr("태그"));
        BadgeFlow(rp->tags, w);
    }
    Caption(Tr("무결성"));
    Para("SHA-256  " + rp->sha256, p.ink3, size::Caption);
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void App::DrawNewPackDialog() {
    if (!newPackOpen_) return;
    const Palette& p = P();
    ImGui::OpenPopup("##newpackdlg");
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(440.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    if (ImGui::BeginPopupModal("##newpackdlg", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        const float w = ImGui::GetContentRegionAvail().x;
        Para(Tr("새 이펙트 팩"), p.ink, size::Title, Font::Bold);
        Gap(4.0f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        Para(Tr("주석이 달린 템플릿으로 shader_packs 폴더에 팩을 만듭니다. 앱을 켜 둔 채로 surface.hlsl 을 고치면 저장할 때마다 바로 반영됩니다."),
             p.ink2);
        ImGui::PopTextWrapPos();
        Gap(10.0f);
        if (Chip("##npsurface", Tr("표면 셰이더"), nullptr, !newPackEffect_)) newPackEffect_ = false;
        ImGui::SameLine();
        if (Chip("##npeffect", Tr("화면 효과"), nullptr, newPackEffect_)) newPackEffect_ = true;
        Gap(10.0f);
        TextField("##npid", Tr("id (영문 소문자, 숫자, _ -)"), newPackId_, sizeof(newPackId_), w / Dpi(), "my_toon");
        Gap(8.0f);
        TextField("##npname", Tr("이름"), newPackName_, sizeof(newPackName_), w / Dpi(), Tr("내 툰 표면 셰이더"));
        Gap(8.0f);
        TextField("##npauthor", Tr("만든 사람"), newPackAuthor_, sizeof(newPackAuthor_), w / Dpi());
        const std::string id = newPackId_;
        const char* problem = nullptr;
        if (!id.empty() && !ValidShaderPackId(id)) problem = Tr("id 는 영문 소문자, 숫자, _, - 만 쓸 수 있습니다");
        else if (!id.empty() && ShaderPacks().Find(id)) problem = Tr("이미 있는 id 입니다");
        if (problem) {
            Gap(6.0f);
            Para(problem, p.danger, size::Caption);
        }
        Gap(16.0f);
        ImGui::BeginDisabled(id.empty() || problem || !newPackName_[0]);
        if (Button("##npcreate", Tr("만들고 폴더 열기"), icon::Plus, ButtonKind::Primary)) {
            std::filesystem::path dir;
            std::string err;
            if (ShaderPacks().CreateFromTemplate(id, newPackName_, newPackAuthor_, dir, err, newPackEffect_)) {
                shaderSelInstalled_ = id;
                OpenFolder(dir);
                newPackOpen_ = false;
                ImGui::CloseCurrentPopup();
            } else {
                toast_ = Toast{};
                toast_.title = Tr("팩을 만들지 못했습니다");
                toast_.detail = err;
                toast_.error = true;
                toast_.until = timeSeconds_ + 6.0;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (Button("##npcancel", Tr("취소"), nullptr, ButtonKind::Ghost)) {
            newPackOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void App::CollectFrameActivePacks(std::set<std::string>& out) const {
    out.clear();
    const auto add = [&](const std::string& id) {
        if (id.empty()) return;
        const ShaderPack* pk = ShaderPacks().Find(id);
        if (pk && pk->source == PackSource::BuiltIn) return;   // built-ins are known good
        out.insert(id);
    };
    // enabled effect-stack entries for this frame (studio: the project's stack is what the renderer holds)
    for (const EffectStackEntry& e : renderer_.Settings().packEffects)
        if (e.enabled) add(e.pack);
    // surface packs of drawn models
    if (studio_ && (screen_ == Screen::Studio || screen_ == Screen::Offline)) {
        for (const auto& m : studio_->models)
            if (m && m->gpu && m->visible) add(m->gpu->ShaderPackId());
    } else if (scene_) {
        if (scene_->characterGpu) add(scene_->characterGpu->ShaderPackId());
        for (const auto& g : scene_->stageGpu)
            if (g) add(g->ShaderPackId());
        for (const auto& e : scene_->extras)
            if (e.gpu) add(e.gpu->ShaderPackId());
    }
}

// ---- gallery sources -------------------------------------------------------------------------------------------------

// Official first (implicit), then the user's galleryIndex URLs in saved order.
std::vector<std::string> App::GalleryIndexUrls() const {
    // --pack-index is the testing override for the official index (the store still puts official-URL sources first)
    std::vector<std::string> urls{options_.packIndex.empty() ? ShaderPackStore::kDefaultIndexUrl : options_.packIndex};
    for (const std::string& u : settings_.galleryIndexes)
        if (!IsOfficialIndexUrl(u)) urls.push_back(u);
    return urls;
}

void App::RefreshGallery() {
    shaderStore_.Refresh(GalleryIndexUrls());
}

void App::DrawGallerySourcesDialog() {
    if (!gallerySourcesOpen_) return;
    const Palette& p = P();
    ImGui::OpenPopup("##gallerysourcesdlg");
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(560.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    bool close = false;
    if (ImGui::BeginPopupModal("##gallerysourcesdlg", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        const float w = ImGui::GetContentRegionAvail().x;
        Para(Tr("갤러리 출처"), p.ink, size::Title, Font::Bold);
        Gap(4.0f);
        Para(Tr("공식 갤러리는 항상 먼저 불러옵니다. 추가한 출처의 팩은 검수되지 않은 것으로 표시됩니다."), p.ink2,
             size::Caption);

        Gap(12.0f);
        // official source: fixed row with its state
        {
            const auto state = shaderStore_.SourceState(ShaderPackStore::kDefaultIndexUrl);
            const char* stateText = state == ShaderPackStore::IndexState::Loading ? Tr("불러오는 중...")
                                    : state == ShaderPackStore::IndexState::Failed ? Tr("불러오지 못했습니다")
                                    : state == ShaderPackStore::IndexState::Ready ? Tr("준비됨")
                                                                                  : Tr("대기 중");
            const ImVec2 c = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(c, ImVec2(c.x + w, c.y + Dp(52.0f)), p.sunken, Dp(10.0f));
            Text(dl, Font::Semibold, size::Small, ImVec2(c.x + Dp(12.0f), c.y + Dp(9.0f)), p.ink, Tr("공식 갤러리"));
            TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(c.x + Dp(12.0f), c.y + Dp(29.0f)), c.x + w - Dp(110.0f),
                         p.ink3, ShaderPackStore::kDefaultIndexUrl);
            Text(dl, Font::Regular, size::Caption, ImVec2(c.x + w - Dp(100.0f), c.y + Dp(9.0f)),
                 state == ShaderPackStore::IndexState::Failed ? p.danger : p.ink2, stateText);
            if (state == ShaderPackStore::IndexState::Failed) {
                TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(c.x + w - Dp(100.0f), c.y + Dp(29.0f)),
                             c.x + w - Dp(10.0f), p.ink3, shaderStore_.SourceError(ShaderPackStore::kDefaultIndexUrl).c_str());
            }
            ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(60.0f)));
        }
        // the user's sources
        for (size_t i = 0; i < settings_.galleryIndexes.size(); ++i) {
            const std::string url = settings_.galleryIndexes[i];
            if (IsOfficialIndexUrl(url)) continue;   // never stored, never listed twice
            const auto state = shaderStore_.SourceState(url);
            const char* stateText = state == ShaderPackStore::IndexState::Loading ? Tr("불러오는 중...")
                                    : state == ShaderPackStore::IndexState::Failed ? Tr("불러오지 못했습니다")
                                    : state == ShaderPackStore::IndexState::Ready ? Tr("준비됨")
                                                                                  : Tr("대기 중");
            const ImVec2 c = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(c, ImVec2(c.x + w, c.y + Dp(52.0f)), p.sunken, Dp(10.0f));
            TextEllipsis(dl, Font::Regular, size::Small, ImVec2(c.x + Dp(12.0f), c.y + Dp(9.0f)), c.x + w - Dp(150.0f), p.ink,
                         url.c_str());
            TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(c.x + Dp(12.0f), c.y + Dp(29.0f)), c.x + w - Dp(150.0f),
                         p.ink3, state == ShaderPackStore::IndexState::Failed
                                     ? shaderStore_.SourceError(url).c_str()
                                     : SourceHost(url).c_str());
            Text(dl, Font::Regular, size::Caption, ImVec2(c.x + w - Dp(140.0f), c.y + Dp(9.0f)),
                 state == ShaderPackStore::IndexState::Failed ? p.danger : p.ink2, stateText);
            ImGui::SetCursorScreenPos(ImVec2(c.x + w - Dp(36.0f), c.y + Dp(8.0f)));
            ImGui::PushID((int)i);
            if (IconButton("##removesource", icon::X, Tr("출처 제거"))) {
                settings_.galleryIndexes.erase(settings_.galleryIndexes.begin() + (long)i);
                settings_.Save(settingsPath_);
                RefreshGallery();
                ImGui::PopID();
                break;   // the list changed: the loop's indices are stale
            }
            ImGui::PopID();
            ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(60.0f)));
        }

        Gap(4.0f);
        TextField("##galleryurl", Tr("출처 주소 (http(s) 또는 로컬 경로)"), galleryUrl_, sizeof(galleryUrl_),
                  std::max(120.0f, (w - Dp(80.0f)) / Dpi()), "https://example.com/shader-packs/index.json");
        if (!galleryUrlError_.empty()) Para(galleryUrlError_, p.danger, size::Caption);
        Gap(8.0f);
        ImGui::BeginDisabled(galleryUrl_[0] == '\0');
        if (Button("##galleryadd", Tr("추가"), icon::Plus, ButtonKind::Primary)) {
            std::string url = TrimUrl(galleryUrl_);
            galleryUrlError_.clear();
            const bool http = url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
            if (url.empty()) {
                galleryUrlError_ = Tr("주소를 입력하세요");
            } else if (!http && url.find("file:") != 0 && url.find("://") != std::string::npos) {
                galleryUrlError_ = Tr("http(s) 주소 또는 로컬 경로만 쓸 수 있습니다");
            } else if (IsOfficialIndexUrl(url) ||
                       std::find(settings_.galleryIndexes.begin(), settings_.galleryIndexes.end(), url) !=
                           settings_.galleryIndexes.end()) {
                galleryUrlError_ = Tr("이미 있는 출처입니다");
            } else {
                settings_.galleryIndexes.push_back(url);
                settings_.Save(settingsPath_);
                galleryUrl_[0] = 0;
                RefreshGallery();
            }
        }
        ImGui::EndDisabled();
        Gap(16.0f);
        if (Button("##gallerysourcesclose", Tr("닫기"), nullptr, ButtonKind::Ghost)) close = true;
        if (close) {
            gallerySourcesOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    if (close && !ImGui::IsPopupOpen("##gallerysourcesdlg")) gallerySourcesOpen_ = false;
}

// An unverified (community / third-party) entry asks once before installing (docs/effect_ecosystem.md §3).
void App::DrawUnverifiedInstallDialog() {
    if (pendingUnverifiedInstall_.empty()) return;
    const Palette& p = P();
    const RemotePack* rp = nullptr;
    for (const RemotePack& r : shaderStore_.Packs())
        if (r.id == pendingUnverifiedInstall_) rp = &r;
    if (!rp) {
        pendingUnverifiedInstall_.clear();
        return;
    }
    const std::string lang = PackLanguage();
    if (!ImGui::IsPopupOpen("##unverifiedinstall")) ImGui::OpenPopup("##unverifiedinstall");
    ImGui::SetNextWindowSize(ImVec2(Dp(440.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    if (ImGui::BeginPopupModal("##unverifiedinstall", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
        const float w = ImGui::GetContentRegionAvail().x;
        Para(rp->name.Get(lang), p.ink, size::Title, Font::Bold);
        Gap(4.0f);
        Para(rp->owner.empty() ? Tr("미검수") : rp->owner, p.ink3, size::Caption);
        if (!IsOfficialIndexUrl(rp->source)) Para(SourceHost(rp->source), p.ink3, size::Caption);
        Gap(10.0f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
        Para(Tr("검수되지 않은 팩은 앱이 직접 확인하지 않았습니다. 설치할까요?"), p.ink2);
        ImGui::PopTextWrapPos();
        Gap(16.0f);
        if (Button("##unverifiedyes", Tr("설치"), icon::DownloadSimple, ButtonKind::Primary)) {
            shaderStore_.Install(*rp);
            pendingUnverifiedInstall_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (Button("##unverifiedno", Tr("취소"), nullptr, ButtonKind::Ghost)) {
            pendingUnverifiedInstall_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

} // namespace mmdx
