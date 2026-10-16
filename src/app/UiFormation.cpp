// Unit dance formation modal: casting library characters into song parts.
#include "app/App.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app/Icons.h"
#include "app/UiHelpers.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui_probe/UiProbe.h"

namespace mmdx {

namespace {

// Lowercased tokens split at ASCII separators; bytes >= 0x80 (UTF-8 kana / kanji) stay inside tokens.
std::vector<std::string> ExtractTokens(const std::string& str) {
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : str) {
        const unsigned char u = (unsigned char)c;
        if (u >= 0x80 || std::isalnum(u)) {
            cur.push_back((char)std::tolower(u));
        } else if (!cur.empty()) {
            tokens.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
    return tokens;
}

// How well a part label ("Darko", "Emu / Felix") names a character: the best share of one alternative's tokens found
// as whole tokens in the character's name or folder name. 0 = no match.
float PartMatchScore(const std::string& label, const CharacterAsset& c) {
    std::vector<std::string> charTokens = ExtractTokens(c.displayName);
    for (auto& t : ExtractTokens(c.folderName)) charTokens.push_back(std::move(t));
    float best = 0.0f;
    size_t start = 0;
    while (start <= label.size()) {
        size_t sep = label.find(" / ", start);
        if (sep == std::string::npos) sep = label.size();
        const std::vector<std::string> alt = ExtractTokens(label.substr(start, sep - start));
        int hit = 0, counted = 0;
        for (const auto& t : alt) {
            if (t.size() < 2) continue;
            ++counted;
            if (std::find(charTokens.begin(), charTokens.end(), t) != charTokens.end()) ++hit;
        }
        if (counted > 0 && hit > 0) best = std::max(best, (float)hit / (float)counted);
        start = sep + 3;
    }
    return best;
}

// The center: the part the library measured in the middle of the stage, else the song's primary dance.
int DefaultCenterPart(const SongAsset& song) {
    if (song.centerPart >= 0 && (size_t)song.centerPart < song.parts.size()) return song.centerPart;
    for (size_t i = 0; i < song.parts.size(); ++i)
        if (song.parts[i].danceVmd == song.danceVmd) return (int)i;
    return 0;
}

}  // namespace

std::vector<PerformerCast> App::ParseFormationSpec(const std::string& spec, const SongAsset& song) {
    std::vector<PerformerCast> cast;
    if (spec.empty() || song.parts.empty()) return cast;

    std::vector<std::string> entries;
    size_t start = 0;
    while (start <= spec.size()) {
        size_t comma = spec.find(',', start);
        if (comma == std::string::npos) comma = spec.size();
        std::string entry = spec.substr(start, comma - start);
        while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) entry.erase(entry.begin());
        while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) entry.pop_back();
        entries.push_back(std::move(entry));
        start = comma + 1;
    }

    int centerIdx = -1;
    for (size_t i = 0; i < song.parts.size(); ++i) {
        if (i >= entries.size()) break;
        std::string entry = entries[i];
        bool isCenter = false;
        if (!entry.empty() && entry.back() == '*') {
            isCenter = true;
            entry.pop_back();
        }
        if (entry == "-" || entry.empty()) continue;
        const CharacterAsset* found = FindCharacter(entry);
        if (found) {
            PerformerCast p;
            p.character = *found;
            p.part = song.parts[i];
            p.isCenter = isCenter;
            if (isCenter) centerIdx = (int)cast.size();
            cast.push_back(p);
        }
    }
    if (!cast.empty() && centerIdx < 0) {
        cast[0].isCenter = true;
    }
    return cast;
}

SongAsset App::SongAsChosen(const SongAsset& song) const {
    SongAsset s = song;
    const auto it = settings_.songCameras.find(song.id);
    if (it != settings_.songCameras.end())
        for (const auto& c : song.cameraVmds)
            if (PathToUtf8(c.filename()) == it->second) { s.cameraVmd = c; break; }
    // M8a: the chosen audio version (songs with several), else the default (audioPath, already in `s`).
    // --song-audio <substring> overrides for this run (case-insensitive file-name substring).
    if (!options_.songAudio.empty()) {
        const std::string needle = ToLowerAscii(options_.songAudio);
        for (const auto& a : song.audioPaths)
            if (ToLowerAscii(PathToUtf8(a.filename())).find(needle) != std::string::npos) { s.audioPath = a; break; }
    } else if (const auto ai = settings_.songAudios.find(song.id); ai != settings_.songAudios.end()) {
        for (const auto& a : song.audioPaths)
            if (PathToUtf8(a.filename()) == ai->second) { s.audioPath = a; break; }
    }
    return s;
}

// M8b: the song's video layers (docs/mme/mme_design.md §5b). The scan classified overlayVideo / backgroundVideo;
// the per-song ini override `songVideo=<overlay|background|off>[:<blend>]|<file>|<song id>` replaces the file and/or
// the blend for the named kind. The overlay time is the song time (background loops in the pass / offline renderer).
std::vector<VideoLayerDesc> App::ResolveSongVideos(const SongAsset& song) const {
    VideoLayerDesc overlay, background;
    overlay.path = song.overlayVideo;
    overlay.kind = VideoLayerKind::Overlay;
    overlay.blend = VideoBlend::BlackKey;
    background.path = song.backgroundVideo;
    background.kind = VideoLayerKind::Background;
    background.screen = true;   // the song's background video also feeds screen.bmp materials

    const auto it = settings_.songVideos.find(song.id);
    if (it != settings_.songVideos.end() && !it->second.empty()) {
        // spec = "<kind>[:<blend>]|<file name>" (the song id is not stored in the value)
        const std::string& spec = it->second;
        const size_t sep = spec.find('|');
        const std::string head = sep == std::string::npos ? spec : spec.substr(0, sep);
        const std::string file = sep == std::string::npos ? std::string() : spec.substr(sep + 1);
        const size_t colon = head.find(':');
        const std::string kind = colon == std::string::npos ? head : head.substr(0, colon);
        const std::string blend = colon == std::string::npos ? std::string() : head.substr(colon + 1);
        VideoBlend b = VideoBlend::BlackKey;
        if (blend == "screen") b = VideoBlend::Screen;
        else if (blend == "add") b = VideoBlend::Add;
        else if (blend == "normal") b = VideoBlend::Normal;
        // A named file (relative to the song folder or an absolute path).
        std::filesystem::path chosen;
        if (!file.empty()) {
            chosen = Utf8ToPath(file);
            if (!std::filesystem::exists(chosen)) {
                std::filesystem::path cand = song.danceVmd.parent_path() / chosen;
                if (std::filesystem::exists(cand)) chosen = cand;
            }
        }
        if (kind == "off") {
            return {};
        } else if (kind == "overlay") {
            if (!chosen.empty()) overlay.path = chosen;
            overlay.blend = b;
        } else if (kind == "background") {
            if (!chosen.empty()) background.path = chosen;
        }
    }
    std::vector<VideoLayerDesc> out;
    if (!background.path.empty()) out.push_back(background);
    if (!overlay.path.empty()) out.push_back(overlay);
    return out;
}

// M8a: the stage to load for a song (see the header comment).
const StageAsset* App::ResolveSongStage(const SongAsset& song) const {
    const auto libStage = [&](const std::string& id) -> const StageAsset* {
        for (const auto& st : library_.stages)
            if (st.id == id) return &st;
        return nullptr;
    };
    // --stage on the command line wins (already applied to selStage_; "-"/"none" lands as -1).
    if (!options_.stage.empty())
        return selStage_ >= 0 && (size_t)selStage_ < library_.stages.size() ? &library_.stages[(size_t)selStage_] : nullptr;
    // An explicit per-song stage choice ("-" = none).
    if (auto it = settings_.songStages.find(song.id); it != settings_.songStages.end())
        return it->second == "-" ? nullptr : libStage(it->second);
    // The song's bundled stage (the default for an MV package).
    if (!song.bundledStageId.empty())
        if (const StageAsset* st = libStage(song.bundledStageId)) return st;
    return selStage_ >= 0 && (size_t)selStage_ < library_.stages.size() ? &library_.stages[(size_t)selStage_] : nullptr;
}

// Play / Enter / the lobby's video render: a song with several parts asks for the formation first; --formation
// casts it directly, and `allowModal` false (CLI autoplay) keeps the single-character load.
void App::StartLobbyLoad(LoadTarget target, bool allowModal) {
    if (selCharacter_ < 0 || (size_t)selCharacter_ >= library_.characters.size() || selSong_ < 0 ||
        (size_t)selSong_ >= library_.songs.size())
        return;
    const SongAsset& song = library_.songs[(size_t)selSong_];
    const StageAsset* st = ResolveSongStage(song);
    if (song.parts.size() >= 2) {
        if (!options_.formation.empty()) {
            const std::vector<PerformerCast> cast = ParseFormationSpec(options_.formation, song);
            if (!cast.empty()) {
                StartLoad(target, &cast[0].character, st, &song, &cast);
                return;
            }
            LOG_WARN("--formation '%s' cast nobody: loading the selected character alone", options_.formation.c_str());
        } else if (allowModal) {
            OpenFormationModal(target);
            return;
        }
    }
    StartLoad(target, &library_.characters[(size_t)selCharacter_], st, &song);
}

void App::OpenFormationModal(LoadTarget target) {
    if (selSong_ < 0 || (size_t)selSong_ >= library_.songs.size()) return;
    const SongAsset& song = library_.songs[(size_t)selSong_];
    if (song.parts.size() < 2) return;

    formationModal_.open = true;
    formationModal_.justOpened = true;
    formationModal_.target = target;
    formationModal_.songIndex = selSong_;
    formationModal_.pickerSlot = -1;
    formationModal_.pickerSearch[0] = '\0';
    formationModal_.slots.clear();
    formationModal_.slots.resize(song.parts.size());

    // Restore last formation used for this song if valid
    bool restored = false;
    auto it = settings_.songFormations.find(song.id);
    if (it != settings_.songFormations.end()) {
        const AppSettings::SavedFormation& saved = it->second;
        formationModal_.centerIndex = DefaultCenterPart(song);
        for (size_t i = 0; i < song.parts.size(); ++i) {
            const std::string dance = PathToUtf8(song.parts[i].danceVmd.filename());
            const auto c = saved.byDance.find(dance);
            if (c != saved.byDance.end() && FindCharacter(c->second)) {
                formationModal_.slots[i].characterId = c->second;
                restored = true;
            }
            if (dance == saved.centerDance) formationModal_.centerIndex = (int)i;
        }
    }

    if (!restored) {
        AutoFormation();
    }
}

void App::AutoFormation() {
    if (formationModal_.songIndex < 0 || (size_t)formationModal_.songIndex >= library_.songs.size()) return;
    const SongAsset& song = library_.songs[(size_t)formationModal_.songIndex];
    formationModal_.slots.assign(song.parts.size(), {});
    const int center = DefaultCenterPart(song);
    formationModal_.centerIndex = center;

    // each part takes its best-named character (at least half the label's words); a character is cast once
    std::vector<char> used(library_.characters.size(), 0);
    for (size_t i = 0; i < song.parts.size(); ++i) {
        float bestScore = 0.0f;
        int best = -1;
        for (size_t c = 0; c < library_.characters.size(); ++c) {
            if (used[c]) continue;
            const float sc = PartMatchScore(song.parts[i].label, library_.characters[c]);
            // ties: the shorter name is the closer one ("Darko" over "Darko (summer)")
            if (sc > bestScore || (sc > 0.0f && sc == bestScore && best >= 0 &&
                                   library_.characters[c].displayName.size() <
                                       library_.characters[(size_t)best].displayName.size())) {
                bestScore = sc;
                best = (int)c;
            }
        }
        if (best >= 0 && bestScore >= 0.5f) {
            formationModal_.slots[i].characterId = library_.characters[(size_t)best].id;
            used[(size_t)best] = 1;
        }
    }
    // the lobby's character (unless a part already named it): the free part nearest the center
    if (selCharacter_ >= 0 && (size_t)selCharacter_ < library_.characters.size() && !used[(size_t)selCharacter_]) {
        int best = -1;
        for (int i = 0; i < (int)formationModal_.slots.size(); ++i)
            if (formationModal_.slots[(size_t)i].characterId.empty() &&
                (best < 0 || std::abs(i - center) < std::abs(best - center)))
                best = i;
        if (best >= 0) formationModal_.slots[(size_t)best].characterId = library_.characters[(size_t)selCharacter_].id;
    }
}

void App::StartFormationLoad() {
    if (formationModal_.songIndex < 0 || (size_t)formationModal_.songIndex >= library_.songs.size()) return;
    const SongAsset& song = library_.songs[(size_t)formationModal_.songIndex];

    std::vector<PerformerCast> cast;
    for (size_t i = 0; i < formationModal_.slots.size(); ++i) {
        const std::string& cid = formationModal_.slots[i].characterId;
        if (cid.empty()) continue;
        const CharacterAsset* c = FindCharacter(cid);
        if (c) {
            PerformerCast p;
            p.character = *c;
            p.part = song.parts[i];
            p.isCenter = ((int)i == formationModal_.centerIndex);
            cast.push_back(p);
        }
    }

    if (cast.empty()) return;

    // Ensure at least one center performer
    bool hasCenter = false;
    for (const auto& p : cast) {
        if (p.isCenter) {
            hasCenter = true;
            break;
        }
    }
    if (!hasCenter) {
        cast[0].isCenter = true;
    }

    // Save formation to settings
    AppSettings::SavedFormation sf;
    for (size_t i = 0; i < formationModal_.slots.size() && i < song.parts.size(); ++i) {
        const std::string dance = PathToUtf8(song.parts[i].danceVmd.filename());
        if (!formationModal_.slots[i].characterId.empty()) sf.byDance[dance] = formationModal_.slots[i].characterId;
        if ((int)i == formationModal_.centerIndex) sf.centerDance = dance;
    }
    settings_.songFormations[song.id] = sf;
    settings_.Save(settingsPath_);

    formationModal_.open = false;

    StartLoad(formationModal_.target, &cast[0].character,
              ResolveSongStage(song), &song, &cast);
}

void App::DrawFormationModal() {
    using namespace ui;
    if (!formationModal_.open) return;
    if (formationModal_.songIndex < 0 || (size_t)formationModal_.songIndex >= library_.songs.size()) {
        formationModal_.open = false;
        return;
    }
    const SongAsset& song = library_.songs[(size_t)formationModal_.songIndex];
    if (formationModal_.slots.size() != song.parts.size()) formationModal_.slots.resize(song.parts.size());
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ds = io.DisplaySize;
    const Palette& p = P();
    const bool pickerWasOpen = formationModal_.pickerSlot >= 0;   // at frame start: Esc belongs to the picker then
    const bool keysLive = !formationModal_.justOpened;   // the Enter that opened it must not start it
    formationModal_.justOpened = false;

    // ---- the formation window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ds);
    if (!pickerWasOpen) ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(16, 24, 32, 120));
    ImGui::Begin("##formationmodal", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse |
                     (pickerWasOpen ? ImGuiWindowFlags_NoInputs : 0));
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // slots: one row of 3:4 portrait cards (the library's thumbnail shape), scrolling sideways when they do not fit
    const size_t n = formationModal_.slots.size();
    const float pad = Dp(28.0f), gap = Dp(14.0f);
    const float maxCardW = Dp(176.0f), minCardW = Dp(128.0f);
    const float panelW = std::min(ds.x - Dp(40.0f),
                                  std::max(Dp(640.0f), (float)n * maxCardW + (float)(n > 0 ? n - 1 : 0) * gap + pad * 2.0f));
    const float innerW = panelW - pad * 2.0f;
    const float fitW = n > 0 ? (innerW - gap * (float)(n - 1)) / (float)n : maxCardW;
    const float cardW = std::clamp(fitW, minCardW, maxCardW);
    const float thumbH = std::floor(cardW * 4.0f / 3.0f);
    const float cardH = thumbH + Dp(70.0f);
    const float rowW = (float)n * cardW + (float)(n > 0 ? n - 1 : 0) * gap;
    const bool scrolls = rowW > innerW + 0.5f;
    const float slotsH = cardH + Dp(scrolls ? 30.0f : 16.0f);
    const float headerH = Dp(92.0f), footerH = Dp(84.0f);
    const float panelH = std::min(ds.y - Dp(24.0f), headerH + slotsH + footerH);
    const ImVec2 a((ds.x - panelW) * 0.5f, (ds.y - panelH) * 0.5f), b(a.x + panelW, a.y + panelH);
    Panel(dl, a, b, Dp(20.0f));
    const float x0 = a.x + pad, x1 = b.x - pad;

    Text(dl, Font::Bold, size::Heading, ImVec2(x0, a.y + Dp(22.0f)), p.ink, Tr("유닛 편성"));
    char sub[256];
    std::snprintf(sub, sizeof(sub), Tr("%s · %d개 파트"), song.displayName.c_str(), (int)n);
    TextEllipsis(dl, Font::Regular, size::Small, ImVec2(x0, a.y + Dp(54.0f)), x1 - Dp(260.0f), p.ink2, sub);
    {
        const char* hint = Tr("카드를 눌러 캐릭터를 고르고, 별로 센터를 정하세요");
        const ImVec2 hs = TextSize(Font::Regular, size::Caption, hint);
        Text(dl, Font::Regular, size::Caption, ImVec2(x1 - hs.x, a.y + Dp(57.0f)), p.ink3, hint);
    }

    const float slotsTop = a.y + headerH;
    ImGui::SetCursorScreenPos(ImVec2(x0, slotsTop));
    ImGui::BeginChild("##formationslots", ImVec2(innerW, slotsH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | (scrolls ? ImGuiWindowFlags_HorizontalScrollbar : 0));
    {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float startX = origin.x + (scrolls ? 0.0f : (innerW - rowW) * 0.5f);
        const float y0 = origin.y + Dp(8.0f);
        for (size_t i = 0; i < n; ++i) {
            ImGui::PushID((int)i);
            const bool isCenter = (int)i == formationModal_.centerIndex;
            const std::string& cid = formationModal_.slots[i].characterId;
            const CharacterAsset* ch = cid.empty() ? nullptr : FindCharacter(cid);
            const ImVec2 ca(startX + (float)i * (cardW + gap), y0), cb(ca.x + cardW, ca.y + cardH);
            const std::string probeLabel = song.parts[i].label + (ch ? " · " + ch->displayName : std::string());
            bool hovered = false;
            ImGui::SetNextItemAllowOverlap();   // the center star on top of the card stays clickable
            const bool clicked = CardItem("##slot", ca, cb, &hovered, probeLabel.c_str());
            const float hv = Anim(ImGui::GetID("##hv"), hovered);
            const float cv = Anim(ImGui::GetID("##cv"), isCenter, 14.0f);
            const float lift = -Dp(3.0f) * std::max(hv, cv);
            const ImVec2 a0(ca.x, ca.y + lift), b0(cb.x, cb.y + lift);
            const float r = Dp(14.0f);
            SoftShadow(cdl, a0, b0, r, Dp(10.0f + 8.0f * hv), 0.07f + 0.07f * hv, ImVec2(0, Dp(3.0f + 3.0f * hv)));
            cdl->AddRectFilled(a0, b0, p.surface, r);
            const ImVec2 ta(a0.x, a0.y), tb(b0.x, a0.y + thumbH);
            cdl->AddRectFilled(ta, tb, Mix(p.surface, p.accentSoft, ch ? 0.85f : 0.35f), r, ImDrawFlags_RoundCornersTop);
            if (ch) {
                cdl->AddRectFilledMultiColor(ImVec2(ta.x, tb.y - thumbH * 0.35f), tb, WithAlpha(p.surface, 0.0f),
                                             WithAlpha(p.surface, 0.0f), WithAlpha(p.surface, 0.55f),
                                             WithAlpha(p.surface, 0.55f));
                if (const uint64_t tex = CharacterThumb((int)(ch - library_.characters.data())))
                    cdl->AddImageRounded(ImTextureRef(tex), ta, tb, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r,
                                         ImDrawFlags_RoundCornersTop);
                else
                    Skeleton(cdl, ImVec2(ta.x + Dp(10), ta.y + Dp(10)), ImVec2(tb.x - Dp(10), tb.y - Dp(10)), Dp(10.0f));
            } else {
                const ImVec2 mid((ta.x + tb.x) * 0.5f, (ta.y + tb.y) * 0.5f);
                cdl->AddCircle(mid, Dp(24.0f), WithAlpha(p.ink3, 0.45f + 0.35f * hv), 40, Dp(1.5f));
                Icon(cdl, icon::Plus, 22.0f, mid, WithAlpha(p.ink2, 0.6f + 0.4f * hv));
            }
            // the part this slot dances, then who dances it
            {
                const char* part = song.parts[i].label.c_str();
                const ImVec2 ps = TextSize(Font::Semibold, size::Caption, part);
                const float pw = std::min(ps.x + Dp(14.0f), cardW - Dp(20.0f));
                const ImVec2 pa(a0.x + Dp(10.0f), tb.y + Dp(10.0f)), pb(pa.x + pw, pa.y + ps.y + Dp(6.0f));
                cdl->AddRectFilled(pa, pb, p.accentSoft, (pb.y - pa.y) * 0.5f);
                TextEllipsis(cdl, Font::Semibold, size::Caption, ImVec2(pa.x + Dp(7.0f), pa.y + Dp(3.0f)), pb.x - Dp(7.0f),
                             p.accentInk, part);
                TextEllipsis(cdl, ch ? Font::Semibold : Font::Regular, size::Body, ImVec2(a0.x + Dp(12.0f), pb.y + Dp(8.0f)),
                             b0.x - Dp(10.0f), ch ? p.ink : p.ink3, ch ? ch->displayName.c_str() : Tr("비어 있음"));
            }
            // center: a badge on the center card, a star on the others
            if (isCenter) {
                const char* label = Tr("센터");
                const ImVec2 ls = TextSize(Font::Bold, size::Caption, label);
                const ImVec2 ba(a0.x + Dp(8.0f), a0.y + Dp(8.0f)), bb(ba.x + ls.x + Dp(32.0f), ba.y + ls.y + Dp(8.0f));
                cdl->AddRectFilled(ba, bb, p.accent, (bb.y - ba.y) * 0.5f);
                Icon(cdl, icon::Star, 12.0f, ImVec2(ba.x + Dp(13.0f), (ba.y + bb.y) * 0.5f), p.onAccent);
                Text(cdl, Font::Bold, size::Caption, ImVec2(ba.x + Dp(23.0f), ba.y + Dp(4.0f)), p.onAccent, label);
                cdl->AddRect(a0, b0, p.accent, r, 0, Dp(2.0f));
            } else {
                cdl->AddRect(a0, b0, hovered ? p.lineStrong : p.line, r);
                ImGui::SetCursorScreenPos(ImVec2(b0.x - Dp(36.0f), a0.y + Dp(6.0f)));
                if (IconButton("##center", icon::Star, Tr("센터로 지정"), false, 30.0f)) formationModal_.centerIndex = (int)i;
            }
            if (clicked) {
                formationModal_.pickerSlot = (int)i;
                formationModal_.pickerSearch[0] = '\0';
            }
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(ImVec2(startX, origin.y));
        ImGui::Dummy(ImVec2(rowW, cardH + Dp(12.0f)));
    }
    ImGui::EndChild();

    // ---- footer
    int cast = 0;
    for (const auto& s : formationModal_.slots) cast += s.characterId.empty() ? 0 : 1;
    const float fy = b.y - footerH + Dp(20.0f);
    dl->AddLine(ImVec2(a.x, b.y - footerH), ImVec2(b.x, b.y - footerH), p.line);
    ImGui::SetCursorScreenPos(ImVec2(x0, fy));
    if (Button("##formauto", Tr("자동 편성"), icon::Sparkle, ButtonKind::Secondary, ImVec2(120.0f, 42.0f))) AutoFormation();
    Tooltip(Tr("파트 이름과 같은 이름의 캐릭터를 배치합니다"));
    ImGui::SameLine(0.0f, Dp(8.0f));
    if (Button("##formclear", Tr("비우기"), icon::Trash, ButtonKind::Ghost, ImVec2(96.0f, 42.0f)))
        for (auto& s : formationModal_.slots) s.characterId.clear();
    const float startW = 170.0f, cancelW = 90.0f, soloW = 150.0f;   // dp
    ImGui::SetCursorScreenPos(ImVec2(x1 - Dp(startW + cancelW + soloW + 20.0f), fy));
    const bool solo = Button("##formsolo", Tr("혼자 플레이"), icon::User, ButtonKind::Ghost, ImVec2(soloW, 42.0f));
    Tooltip(Tr("라이브러리에서 고른 캐릭터 혼자 대표 파트를 춥니다"));
    ImGui::SameLine(0.0f, Dp(10.0f));
    const bool cancel = Button("##formcancel", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(cancelW, 42.0f));
    ImGui::SameLine(0.0f, Dp(10.0f));
    char startLabel[64];
    std::snprintf(startLabel, sizeof(startLabel), Tr("%d명으로 시작"), cast);
    ImGui::BeginDisabled(cast == 0);
    const bool start = Button("##formstart", startLabel, icon::Play, ButtonKind::Primary, ImVec2(startW, 42.0f));
    ImGui::EndDisabled();
    ImGui::End();

    // ---- character picker (its own window above the formation)
    if (formationModal_.pickerSlot >= 0 && (size_t)formationModal_.pickerSlot < n) {
        const size_t slot = (size_t)formationModal_.pickerSlot;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ds);
        ImGui::SetNextWindowFocus();
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(10, 15, 20, 110));
        ImGui::Begin("##formationpicker", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        const float pw = std::min(ds.x - Dp(60.0f), Dp(820.0f)), ph = std::min(ds.y - Dp(60.0f), Dp(680.0f));
        const ImVec2 pa((ds.x - pw) * 0.5f, (ds.y - ph) * 0.5f), pb(pa.x + pw, pa.y + ph);
        Panel(pdl, pa, pb, Dp(20.0f));
        const float px0 = pa.x + Dp(24.0f), px1 = pb.x - Dp(24.0f);
        Text(pdl, Font::Bold, size::Heading, ImVec2(px0, pa.y + Dp(20.0f)), p.ink, Tr("캐릭터 선택"));
        const std::string partLine = std::string(Tr("파트")) + "  " + song.parts[slot].label;
        TextEllipsis(pdl, Font::Regular, size::Small, ImVec2(px0, pa.y + Dp(52.0f)), px1 - Dp(300.0f), p.ink2, partLine.c_str());
        bool closePicker = false;
        ImGui::SetCursorScreenPos(ImVec2(px1 - Dp(36.0f), pa.y + Dp(18.0f)));
        if (IconButton("##pickerclose", icon::X, Tr("닫기 (Esc)"), false, 36.0f)) closePicker = true;
        ImGui::SetCursorScreenPos(ImVec2(px1 - Dp(48.0f) - Dp(240.0f), pa.y + Dp(20.0f)));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        SearchField("##pickersearch", formationModal_.pickerSearch, sizeof(formationModal_.pickerSearch), Tr("캐릭터 검색"),
                    240.0f);

        const float gridTop = pa.y + Dp(86.0f);
        ImGui::SetCursorScreenPos(ImVec2(px0 - Dp(6.0f), gridTop));
        ImGui::BeginChild("##pickergrid", ImVec2(px1 - px0 + Dp(12.0f), pb.y - gridTop - Dp(18.0f)), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground);
        {
            ImDrawList* gdl = ImGui::GetWindowDrawList();
            const float inset = Dp(6.0f);
            const float availW = ImGui::GetContentRegionAvail().x - inset * 2.0f - Dp(10.0f);
            const ImVec2 origin(ImGui::GetCursorScreenPos().x + inset, ImGui::GetCursorScreenPos().y + inset);
            const float cg = Dp(12.0f);
            const int cols = std::max(1, (int)((availW + cg) / (Dp(118.0f) + cg)));
            const float cw = (availW - cg * (float)(cols - 1)) / (float)cols;
            const float th = std::floor(cw * 4.0f / 3.0f), chH = th + Dp(36.0f);
            const std::string needle = ToLowerAscii(formationModal_.pickerSearch);
            float contentH = 0.0f;
            int k = 0;
            const auto tile = [&](int index) -> ImVec2 {
                const int col = index % cols, row = index / cols;
                return ImVec2(origin.x + (float)col * (cw + cg), origin.y + (float)row * (chH + cg));
            };
            // first tile: leave the part empty
            {
                const ImVec2 ta0 = tile(k++), tb0(ta0.x + cw, ta0.y + chH);
                bool hov = false;
                if (CardItem("##pickempty", ta0, tb0, &hov, Tr("비워두기"))) {
                    formationModal_.slots[slot].characterId.clear();
                    closePicker = true;
                }
                const float r = Dp(10.0f);
                gdl->AddRectFilled(ta0, tb0, hov ? Mix(p.surface, p.sunken, 0.5f) : p.surface, r);
                gdl->AddRect(ta0, tb0, hov ? p.lineStrong : p.line, r);
                Icon(gdl, icon::X, 22.0f, ImVec2((ta0.x + tb0.x) * 0.5f, ta0.y + th * 0.5f), p.ink3);
                const char* el = Tr("비워두기");
                const ImVec2 es = TextSize(Font::Semibold, size::Small, el);
                Text(gdl, Font::Semibold, size::Small, ImVec2((ta0.x + tb0.x - es.x) * 0.5f, ta0.y + th + Dp(9.0f)), p.ink2, el);
                contentH = std::max(contentH, tb0.y - origin.y);
            }
            for (size_t ci = 0; ci < library_.characters.size(); ++ci) {
                const CharacterAsset& c = library_.characters[ci];
                if (!needle.empty() && ToLowerAscii(c.displayName).find(needle) == std::string::npos &&
                    ToLowerAscii(c.folderName).find(needle) == std::string::npos)
                    continue;
                const ImVec2 ta0 = tile(k++), tb0(ta0.x + cw, ta0.y + chH);
                contentH = std::max(contentH, tb0.y - origin.y);
                if (!ImGui::IsRectVisible(ImVec2(ta0.x, ta0.y - Dp(20)), ImVec2(tb0.x, tb0.y + Dp(20)))) continue;
                ImGui::PushID((int)ci);
                bool hov = false;
                if (CardItem("##pick", ta0, tb0, &hov, c.displayName.c_str())) {
                    formationModal_.slots[slot].characterId = c.id;
                    closePicker = true;
                }
                const float r = Dp(10.0f);
                const float ihv = Anim(ImGui::GetID("##hv"), hov);
                const bool current = formationModal_.slots[slot].characterId == c.id;
                const ImVec2 a0(ta0.x, ta0.y - Dp(2.0f) * ihv), b0(tb0.x, tb0.y - Dp(2.0f) * ihv);
                gdl->AddRectFilled(a0, b0, p.surface, r);
                const ImVec2 ia(a0.x, a0.y), ib(b0.x, a0.y + th);
                gdl->AddRectFilled(ia, ib, Mix(p.surface, p.accentSoft, 0.85f), r, ImDrawFlags_RoundCornersTop);
                if (const uint64_t tex = CharacterThumb((int)ci))
                    gdl->AddImageRounded(ImTextureRef(tex), ia, ib, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r,
                                         ImDrawFlags_RoundCornersTop);
                else
                    Skeleton(gdl, ImVec2(ia.x + Dp(8), ia.y + Dp(8)), ImVec2(ib.x - Dp(8), ib.y - Dp(8)), Dp(8.0f));
                TextEllipsis(gdl, Font::Semibold, size::Small, ImVec2(a0.x + Dp(8.0f), ib.y + Dp(9.0f)), b0.x - Dp(8.0f),
                             p.ink, c.displayName.c_str());
                gdl->AddRect(a0, b0, current ? p.accent : (hov ? p.lineStrong : p.line), r, 0, current ? Dp(2.0f) : 1.0f);
                ImGui::PopID();
            }
            ImGui::SetCursorScreenPos(origin);
            ImGui::Dummy(ImVec2(availW, contentH + Dp(8.0f)));
        }
        ImGui::EndChild();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) closePicker = true;
        ImGui::End();
        if (closePicker) formationModal_.pickerSlot = -1;
    }

    if (pickerWasOpen) return;   // this frame's Esc / Enter went to the picker
    if (cancel || (keysLive && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        formationModal_.open = false;
        return;
    }
    if (solo) {
        formationModal_.open = false;
        if (selCharacter_ >= 0 && (size_t)selCharacter_ < library_.characters.size())
            StartLoad(formationModal_.target, &library_.characters[(size_t)selCharacter_],
                      ResolveSongStage(song), &song);
        return;
    }
    if (cast > 0 && (start || (keysLive && ImGui::IsKeyPressed(ImGuiKey_Enter, false)))) StartFormationLoad();
}

}  // namespace mmdx
