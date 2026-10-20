// Shader pack choice (render/ShaderPack.h): how a choice reaches a model's GPU constants, and the picker shared by the
// library panel, the play bar and the studio inspector. The pack manager screen is UiShaders.cpp.
#include "app/App.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <climits>
#include <set>
#include <cmath>
#include <thread>

#include "app/Icons.h"
#include "app/LookApply.h"
#include "asset/MmeAssign.h"
#include "asset/PmxModel.h"
#include "asset/RayMaterial.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include "render/ShaderPack.h"
#include "render/GpuModel.h"
#include "studio/FileDialog.h"
#include "studio/StudioDoc.h"
#include "ui_probe/UiProbe.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace mmdx {

using namespace ui;

// P6: declared here rather than including asset/ModelImport.h, which also defines mmdx::ModelRole (GpuModel.h wins
// in this translation unit). Defined in asset/ModelImport.cpp.
bool ModelHasImportedPbr(const std::filesystem::path& path);

namespace {
// M6b: a wrapped paragraph in the kit's type (the shader screens' helper).
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
} // namespace

namespace {
// M6b: the model's own MME files (the Auto candidate): `.emd` / same-name `.fx` / name[fx].pmx (asset/MmeAssign.h).
bool ModelShipsMme(const std::filesystem::path& modelPath) {
    if (modelPath.empty()) return false;
    const std::vector<MmeAssignment> a = FindModelEffects(modelPath);
    for (const MmeAssignment& x : a)
        if (!x.effect.empty() || x.hidden) return true;
    return false;
}
} // namespace

// Folder picker (FOS_PICKFOLDERS): runs on its own STA thread like studio::FileDialog, keeping the
// owner's messages pumped so the enable ping-pong cannot deadlock. Declared in App.h, shared with
// the shader manager screen (UiShaders.cpp).
std::filesystem::path PickPackTextureFolder(HWND owner) {
    std::filesystem::path result;
    std::thread t([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return;
        {
            ComPtr<IFileOpenDialog> dlg;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
                DWORD opts = 0;
                dlg->GetOptions(&opts);
                dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS);
                if (SUCCEEDED(dlg->Show(owner))) {
                    ComPtr<IShellItem> item;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        result = path;
                        CoTaskMemFree(path);
                    }
                }
            }
        }
        CoUninitialize();
    });
    // Keep pumping the owner's messages while waiting (see studio/FileDialog.cpp).
    HANDLE h = (HANDLE)t.native_handle();
    while (MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    t.join();
    return result;
}

std::string PackAuthorLine(const std::vector<PackAuthor>& authors) {
    std::string s;
    for (size_t i = 0; i < authors.size() && i < 3; ++i) s += (i ? ", " : "") + authors[i].name;
    if (authors.size() > 3) s += " +" + std::to_string(authors.size() - 3);
    return s;
}

bool PackMatches(const std::string& needle, const std::string& id, const LocalizedText& name,
                 const std::vector<PackAuthor>& authors, const std::vector<std::string>& tags) {
    if (needle.empty()) return true;
    const auto has = [&](const std::string& s) { return ToLowerAscii(s).find(needle) != std::string::npos; };
    if (has(id) || has(name.Get(PackLanguage())) || has(PackAuthorLine(authors))) return true;
    for (const std::string& t : tags)
        if (has(t)) return true;
    return false;
}

bool App::ApplyLookById(const std::string& id, bool persist) {
    const ShaderPack* pack = ShaderPacks().Find(id);
    if (!pack || pack->type != PackType::Look || !pack->look || !PackUsable(pack)) {
        LOG_WARN("look: no usable look pack '%s'", id.c_str());
        return false;
    }
    const LookData& look = *pack->look;
    for (const LookPackRef& ref : look.packs) {
        if (ref.id.empty() || ShaderPacks().Find(ref.id)) continue;
        LOG_WARN("look %s: referenced pack '%s' is not installed", id.c_str(), ref.id.c_str());
    }
    std::string characterId;
    if (scene_ && !scene_->characterId.empty()) characterId = scene_->characterId;
    ShaderChoice choice = characterId.empty() ? ShaderChoice{} : settings_.CharacterShader(characterId);
    ApplyLook(look, settings_, characterId.empty() ? nullptr : &choice);
    if (!characterId.empty() && look.setsSurface) settings_.SetCharacterShader(characterId, choice);
    if (persist) settings_.Save(settingsPath_);
    LOG_INFO("LOOK applied %s", id.c_str());
    return true;
}

bool App::ExportLook(const std::string& id, const std::string& displayName) {
    std::string characterId;
    if (scene_ && !scene_->characterId.empty()) characterId = scene_->characterId;
    ShaderChoice surface = characterId.empty() ? ShaderChoice{} : settings_.CharacterShader(characterId);
    const LookData look = CaptureLook(settings_, characterId.empty() ? nullptr : &surface);
    LocalizedText name;
    name.byLang[""] = displayName.empty() ? id : displayName;
    LocalizedText display = name;
    std::string error;
    if (!ShaderPacks().WriteLookPack(ShaderPacks().UserRoot() / Utf8ToPath(id), id, display, {}, "", look, &error)) {
        LOG_WARN("look export failed: %s", error.c_str());
        return false;
    }
    LOG_INFO("LOOK exported %s", id.c_str());
    return true;
}

bool App::PackUsable(const ShaderPack* pack) const {
    return pack && pack->Selectable() && !packGuard_.Quarantined(pack->id);
}

void App::ApplyShaderChoice(GpuModel& gpu, const ShaderChoice& choice) {
    const ShaderPack* pack = choice.pack.empty() ? nullptr : ShaderPacks().Find(choice.pack);
    if (pack && !PackUsable(pack)) pack = nullptr;
    if (!pack) {
        gpu.SetShaderPack(nullptr, PackParamValues{}, {});
        return;
    }
    // per-character texture folder: the choice's folder, else the model's own default (found through its MME files),
    // else the pack-level one (registry setting)
    const std::filesystem::path folder =
        !choice.textureFolder.empty()        ? Utf8ToPath(choice.textureFolder)
        : !gpu.DefaultPackTextureFolder().empty() ? gpu.DefaultPackTextureFolder()
                                             : ShaderPacks().TextureFolder(choice.pack);
    gpu.SetShaderPack(pack, choice.params, folder, choice.materials);
}

namespace {
const char* MaterialClassName(int cls) {
    switch (cls) {
        case (int)PackClass::Body: return Tr("의상");
        case (int)PackClass::Skin: return Tr("피부");
        case (int)PackClass::Face: return Tr("얼굴");
        case (int)PackClass::Eye: return Tr("눈");
        case (int)PackClass::Hair: return Tr("머리카락");
        case (int)PackClass::Weapon: return Tr("무기");
        default: return Tr("기본 셰이딩");
    }
}
} // namespace

int App::MaterialOverrideCount(const ShaderChoice& choice) { return (int)choice.materials.size(); }

// M6a: the model's MME choice. The saved per-character / per-stage choice (unless a run-wide CLI override applies),
// defaulting to None for characters and Auto for stages. `id` may be empty (no saved choice): then the CLI overrides
// or the kind default decide. The global MME switch off wins over everything (no MME object effect anywhere); the
// Auto workflow forces the automatic choice (Auto) for every model regardless of what is saved. `modelPath` is
// reserved for the Auto workflow (an object effect needs the model's own files).
MmeChoice App::ResolveMmeChoice(const std::string& id, bool isStage, const std::filesystem::path& modelPath) const {
    (void)modelPath;
    if (!MmeGlobalEnabled()) return MmeChoice{};   // global off: nothing MME applies
    // --mme-auto forces None / Auto for every model; --stage-mme does the same for stages only (and wins for stages).
    if (isStage && options_.stageMmeSet) return {options_.stageMme != 0 ? MmeChoice::Auto : MmeChoice::None, {}};
    if (options_.mmeAutoSet) return {options_.mmeAuto != 0 ? MmeChoice::Auto : MmeChoice::None, {}};
    if (MmeAutoWorkflow()) return MmeChoice{MmeChoice::Auto, {}};   // 완전 자동: every model gets its own files
    if (!id.empty()) return isStage ? settings_.StageMme(id) : settings_.CharacterMme(id);
    return isStage ? MmeChoice{MmeChoice::Auto, {}} : MmeChoice{};
}

// The same resolution around an explicit saved choice (a studio project model's MmeChoice).
MmeChoice App::ResolveMmeChoiceFor(const MmeChoice& saved, bool isStage,
                                   const std::filesystem::path& modelPath) const {
    (void)modelPath;
    if (!MmeGlobalEnabled()) return MmeChoice{};
    if (isStage && options_.stageMmeSet) return {options_.stageMme != 0 ? MmeChoice::Auto : MmeChoice::None, {}};
    if (options_.mmeAutoSet) return {options_.mmeAuto != 0 ? MmeChoice::Auto : MmeChoice::None, {}};
    if (MmeAutoWorkflow()) return MmeChoice{MmeChoice::Auto, {}};
    return saved;
}

// Auto (완전 자동) is the decider only when no run-wide CLI override (--mme-auto / --stage-mme) did: those force the
// choice explicitly and are the user's own call (the required-controller skip is for the automatic assignment).
bool App::MmeAutoSkipApplies(const MmeChoice& mme, bool isStage) const {
    if (mme.mode != MmeChoice::Auto) return false;
    if (options_.mmeAutoSet) return false;
    if (isStage && options_.stageMmeSet) return false;
    return MmeAutoWorkflow();
}

// M6a: the per-material effect files a choice assigns. None = native shading, Auto = the model's own files
// (FindModelEffects: .emd / same-name .fx / name[fx].pmx), File = one effect for every material.
void App::ApplyMmeEffect(GpuModel& gpu, const MmeChoice& choice, const std::filesystem::path& modelPath,
                         bool autoWorkflow) {
    const size_t count = gpu.Materials().size();
    if (choice.mode == MmeChoice::None) {
        gpu.SetMmeEffects({});
        return;
    }
    if (choice.mode == MmeChoice::File) {
        gpu.SetMmeEffects(std::vector<std::filesystem::path>(count, choice.file));
        return;
    }
    // Auto: what the model's own files ask for, mapped to per-material paths (subset -1 = every material,
    // subset n = material n; later entries override earlier; hidden = hide the model, ignored for now).
    // The filesystem-dependent resolution (FindModelEffects + the IsRayMaterialFx skip) is cached per
    // (model path, material count) for 1.0 s, so a typical frame does no disk I/O here.
    const std::string cacheKey = PathToUtf8(modelPath) + "|" + std::to_string(count);
    const auto now = std::chrono::steady_clock::now();
    MmeAutoResolved& resolved = mmeAutoResolved_[cacheKey];
    if (resolved.at == std::chrono::steady_clock::time_point{} ||
        now - resolved.at >= std::chrono::duration<double>(1.0)) {
        resolved = MmeAutoResolved{};
        resolved.perMaterial.assign(count, {});
        const std::vector<MmeAssignment> assigns = FindModelEffects(modelPath);
        for (const MmeAssignment& a : assigns) {
            if (a.hidden && a.subset < 0) resolved.hidden = true;
            // Ray-MMD material files (material_2.0 / ray macros) are what the native PBR reads (asset/RayMaterial):
            // Auto leaves them to it. Run as MME objects they need ray.x's deferred G-buffer and render dark. File
            // mode still forces any effect.
            if (!a.effect.empty() && IsRayMaterialFx(a.effect)) {
                ++resolved.raySkipped;
                continue;
            }
            if (a.subset < 0) {
                for (auto& p : resolved.perMaterial) p = a.effect;
            } else if ((size_t)a.subset < resolved.perMaterial.size() && !a.hidden) {
                resolved.perMaterial[(size_t)a.subset] = a.effect;
            }
        }
        resolved.at = now;
    }
    std::vector<std::filesystem::path> perMaterial = resolved.perMaterial;
    const bool hidden = resolved.hidden;
    const size_t raySkipped = resolved.raySkipped;
    if (autoWorkflow && !modelPath.empty()) {
        // Auto (완전 자동) only: an effect that reads a required external controller object which is not in the scene
        // renders the whole model as a dark translucent silhouette (the binder uses the missing-object defaults). Such
        // a model resolves to native shading for MME: no object effect on any of its materials (partial looks broken
        // too). The decision is cached by (path, scene-names key); it re-evaluates when the scene model set changes,
        // so the effect applies again once the controller appears.
        const std::string skip = MmeControllerSkip(modelPath);
        if (!skip.empty()) {
            const std::string logKey = PathToUtf8(modelPath) + "|" + skip;
            if (mmeSkipLogged_.insert(logKey).second)
                LOG_INFO("MME auto: %s: skipped (needs controller '%s')", PathToUtf8(modelPath).c_str(),
                         skip.c_str());
            // 완전 자동, play scene: the controller's file may exist in the library — queue its automatic load
            // (it applies from the frame after PollMmeControllers loads it; the studio keeps today's skip).
            if (scene_ && !studio_) RequestMmeController(modelPath, skip);
            gpu.SetMmeEffects({});
            return;
        }
    }

    if (hidden && !modelPath.empty() && mmeHiddenWarned_.insert(PathToUtf8(modelPath)).second)
        LOG_WARN("MME: %s asks to hide the model (Obj.show = false); ignored for now",
                 PathToUtf8(modelPath).c_str());
    if (raySkipped && !modelPath.empty() && mmeHiddenWarned_.insert("ray|" + PathToUtf8(modelPath)).second)
        LOG_INFO("MME: %s: %zu Ray-MMD material assignment(s) left to the native PBR", PathToUtf8(modelPath).c_str(),
                 raySkipped);
    gpu.SetMmeEffects(std::move(perMaterial));
}

// The lower-case names a CONTROLOBJECT lookup can match (render/PassMmeOffscreen.cpp MmeSceneLookup): each loaded
// model's source file name and display name, plus the MME effect objects' and the Auto workflow's auto-loaded
// controller models' file / display names. Play: center + extras + stages + effect objects + controllers. Studio:
// the project's models. Cached per scene model set (count + pointers): the cheap key is built every call, the
// name list only when it changed.
const std::vector<std::string>& App::MmeSceneNames() {
    std::string key;
    if (studio_) {
        for (const auto& m : studio_->models) {
            key += 's';
            key += std::to_string((uintptr_t)m.get());
            key += '.';
            key += std::to_string((uintptr_t)m->gpu.get());
        }
    } else if (scene_) {
        if (scene_->character) {
            key += 'c';
            key += std::to_string((uintptr_t)scene_->characterGpu.get());
        }
        for (size_t i = 0; i < scene_->stages.size() && i < scene_->stageGpu.size(); ++i) {
            key += 't';
            key += std::to_string((uintptr_t)scene_->stageGpu[i].get());
        }
        for (const auto& e : scene_->extras) {
            key += 'e';
            key += std::to_string((uintptr_t)e.gpu.get());
        }
        for (const auto& o : scene_->effectObjects) {
            key += 'o';
            key += std::to_string((uintptr_t)o.mesh.get());
        }
        for (const auto& c : scene_->controllers) {
            key += 'k';
            key += std::to_string((uintptr_t)c.gpu.get());
        }
    }
    if (key == mmeSceneNamesKey_) return mmeSceneNames_;   // unchanged: no rebuild, no per-frame allocation
    mmeSceneNamesKey_ = key;
    std::vector<std::string> names;
    const auto add = [&](const std::filesystem::path& p, const std::string& display) {
        if (!p.empty()) names.push_back(ToLowerAscii(PathToUtf8(p.filename())));
        if (!display.empty()) names.push_back(ToLowerAscii(display));
    };
    if (studio_) {
        for (const auto& m : studio_->models)
            add(m->path, m->pmx && !m->pmx->name.empty() ? m->pmx->name : m->name);
    } else if (scene_) {
        if (scene_->character) add(scene_->character->Model().sourcePath, scene_->characterDisplayName);
        for (size_t i = 0; i < scene_->stages.size() && i < scene_->stageGpu.size(); ++i) {
            // stages carry no motion instance: AppendMmeObject fills the display name only (no file name)
            const std::string display = scene_->stageGpu[i]->Name();
            if (!display.empty()) names.push_back(ToLowerAscii(display));
        }
        for (const auto& e : scene_->extras) {
            if (!e.gpu) continue;
            add(e.instance ? e.instance->Model().sourcePath : std::filesystem::path(),
                !e.displayName.empty() ? e.displayName : e.gpu->Name());
        }
        for (const auto& o : scene_->effectObjects) {
            if (!o.fileName.empty()) names.push_back(ToLowerAscii(o.fileName));
            if (!o.displayName.empty()) names.push_back(ToLowerAscii(o.displayName));
        }
        for (const auto& c : scene_->controllers) {
            if (!c.fileName.empty()) names.push_back(ToLowerAscii(c.fileName));
            if (!c.displayName.empty()) names.push_back(ToLowerAscii(c.displayName));
        }
    }
    mmeSceneNames_ = std::move(names);
    // scene model set changed: the per-model auto decision cache no longer applies (re-evaluated, not latched)
    mmeAutoSkip_.clear();
    return mmeSceneNames_;
}

// The required controller a model's Auto assignment needs but the scene lacks (lower case), or "" when nothing is
// missing. Cached by (model path, scene-names key) in mmeAutoSkip_ so ApplyMmeEffect's per-frame call is one lookup.
std::string App::MmeControllerSkip(const std::filesystem::path& modelPath) {
    if (modelPath.empty()) return {};
    const std::vector<std::string>& names = MmeSceneNames();   // also refreshes the cache key
    const std::string cacheKey = PathToUtf8(modelPath) + "|" + mmeSceneNamesKey_;
    const auto it = mmeAutoSkip_.find(cacheKey);
    if (it != mmeAutoSkip_.end()) return it->second;
    // The Auto effects this model would use (Ray-MMD material files are left to the native PBR, as ApplyMmeEffect does).
    std::string skip;
    for (const MmeAssignment& a : FindModelEffects(modelPath)) {
        if (a.effect.empty() || IsRayMaterialFx(a.effect)) continue;
        for (const std::string& c : MmeRequiredControllers(a.effect)) {
            if (std::find(names.begin(), names.end(), c) == names.end()) { skip = c; break; }
        }
        if (!skip.empty()) break;
    }
    mmeAutoSkip_[cacheKey] = skip;
    return skip;
}

// Per-material class / default shading of a pack model. One row per material: name + a class dropdown ("자동" shows the
// pack's own pick). Ctrl / Shift + click selects several rows; a dropdown on a selected row then sets all of them.
bool App::DrawMaterialShaderList(const char* id, ShaderChoice& choice, const PmxModel& pmx, float width, float maxHeight) {
    const ShaderPack* pack = choice.pack.empty() ? nullptr : ShaderPacks().Find(choice.pack);
    if (!pack || pmx.materials.empty()) return false;
    const Palette& p = P();
    bool changed = false;
    // the selection belongs to one list (model + widget); another list starts empty
    const std::string owner = std::string(id) + "|" + std::to_string(pmx.materials.size()) + "|" + pmx.name;
    if (owner != materialSelOwner_) {
        materialSelOwner_ = owner;
        materialSel_.clear();
        materialSelAnchor_ = -1;
    }

    // header: count + reset
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        char head[96];
        const int n = MaterialOverrideCount(choice);
        if (n > 0) std::snprintf(head, sizeof(head), Tr("재질 %d개 · %d개 직접 지정"), (int)pmx.materials.size(), n);
        else std::snprintf(head, sizeof(head), Tr("재질 %d개 · 모두 자동"), (int)pmx.materials.size());
        Text(dl, Font::Regular, size::Caption, ImVec2(c.x, c.y + Dp(6.0f)), p.ink3, head);
        if (n > 0) {
            const float bw = TextSize(Font::Semibold, size::Small, Tr("모두 자동")).x / Dpi() + 24.0f;
            ImGui::SetCursorScreenPos(ImVec2(c.x + width - Dp(bw), c.y));
            if (Button((std::string(id) + "_reset").c_str(), Tr("모두 자동"), nullptr, ButtonKind::Ghost, ImVec2(bw, 26.0f))) {
                choice.materials.clear();
                changed = true;
            }
            ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(28.0f)));
        } else {
            ImGui::Dummy(ImVec2(width, Dp(26.0f)));
        }
    }

    const float rowH = Dp(32.0f), comboW = std::min(Dp(132.0f), width * 0.48f);
    const float listH = std::min(maxHeight, rowH * (float)pmx.materials.size() + Dp(4.0f));
    ImGui::BeginChild((std::string(id) + "_list").c_str(), ImVec2(width, listH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGuiIO& io = ImGui::GetIO();
    for (int i = 0; i < (int)pmx.materials.size(); ++i) {
        const PmxMaterial& m = pmx.materials[(size_t)i];
        ImGui::PushID(i);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 b(a.x + w, a.y + rowH - Dp(2.0f));
        const bool sel = materialSel_.count(i) > 0;
        // row hit area (left of the dropdown): plain click = only this row, Ctrl = toggle, Shift = range
        ImGui::InvisibleButton("##row", ImVec2(std::max(1.0f, w - comboW - Dp(6.0f)), b.y - a.y));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            if (io.KeyShift && materialSelAnchor_ >= 0) {
                if (!io.KeyCtrl) materialSel_.clear();
                for (int k = std::min(i, materialSelAnchor_); k <= std::max(i, materialSelAnchor_); ++k) materialSel_.insert(k);
            } else if (io.KeyCtrl) {
                if (!materialSel_.erase(i)) materialSel_.insert(i);
                materialSelAnchor_ = i;
            } else {
                const bool only = sel && materialSel_.size() == 1;
                materialSel_.clear();
                if (!only) materialSel_.insert(i);
                materialSelAnchor_ = i;
            }
        }
        if (sel) dl->AddRectFilled(a, b, p.accentSoft, Dp(8.0f));
        else if (hovered) dl->AddRectFilled(a, b, WithAlpha(p.ink, IsDark() ? 0.07f : 0.04f), Dp(8.0f));

        const auto it = choice.materials.find(m.name);
        const int cur = it != choice.materials.end() ? it->second : -2;   // -2 = automatic
        const bool tex = m.textureIndex >= 0 && (size_t)m.textureIndex < pmx.textures.size();
        const int autoCls = (int)pack->Classify(m.name, m.nameEn, tex ? pmx.textures[(size_t)m.textureIndex] : std::string());
        const float ty = a.y + (b.y - a.y - Dp(size::Small * 1.25f)) * 0.5f;
        TextEllipsis(dl, Font::Regular, size::Small, ImVec2(a.x + Dp(10.0f), ty), b.x - comboW - Dp(14.0f),
                     cur == -2 ? p.ink2 : p.ink, m.name.empty() ? m.nameEn.c_str() : m.name.c_str());

        // class dropdown
        std::string preview = cur == -2 ? std::string(Tr("자동")) + " · " + MaterialClassName(autoCls) : MaterialClassName(cur);
        ImGui::SetCursorScreenPos(ImVec2(b.x - comboW, a.y + (b.y - a.y - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(comboW);
        PushFont(Font::Regular, size::Small);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(cur == -2 ? p.ink3 : (cur == kMaterialPackOff ? p.warn : p.ink)));
        const bool open = ImGui::BeginCombo("##cls", preview.c_str(), ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton);
        ImGui::PopStyleColor();
        {   // a quiet caret instead of ImGui's arrow box
            const ImVec2 cmax = ImGui::GetItemRectMax(), cmin = ImGui::GetItemRectMin();
            Icon(ImGui::GetWindowDrawList(), icon::CaretDown, 12.0f, ImVec2(cmax.x - Dp(12.0f), (cmin.y + cmax.y) * 0.5f), p.ink3);
        }
        if (open) {
            int pick = INT_MIN;
            const std::string autoLabel = std::string(Tr("자동")) + "  (" + MaterialClassName(autoCls) + ")";
            if (ImGui::Selectable(autoLabel.c_str(), cur == -2)) pick = -2;
            ImGui::Separator();
            for (int c = 0; c <= (int)PackClass::Weapon; ++c)
                if (ImGui::Selectable(MaterialClassName(c), cur == c)) pick = c;
            ImGui::Separator();
            if (ImGui::Selectable(Tr("기본 셰이딩 (팩 끄기)"), cur == kMaterialPackOff)) pick = kMaterialPackOff;
            ImGui::EndCombo();
            if (pick != INT_MIN) {
                std::set<int> targets = sel && materialSel_.size() > 1 ? materialSel_ : std::set<int>{i};
                for (int t : targets) {
                    const std::string& name = pmx.materials[(size_t)t].name;
                    if (pick == -2) choice.materials.erase(name);
                    else choice.materials[name] = pick;
                }
                changed = true;
            }
        }
        PopFont();
        if (sel && materialSel_.size() > 1 && ImGui::IsItemHovered()) Tooltip(Tr("선택한 재질 모두에 적용"));
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + rowH));
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(1, 1));
    ImGui::EndChild();
    PushFont(Font::Regular, size::Caption);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink3));
    ImGui::TextWrapped("%s", Tr("Ctrl · Shift + 클릭으로 여러 재질을 골라 한 번에 바꿀 수 있어요. 기본 셰이딩은 그 재질만 팩 없이 그립니다."));
    ImGui::PopStyleColor();
    PopFont();
    return changed;
}

ShaderChoice App::PlayShaderChoice(const GpuModel& gpu, const std::string& characterId) {
    if (!scene_ || screen_ == Screen::BenchRun || screen_ == Screen::BenchRender) return {};  // fixed workload
    ShaderChoice c = characterId.empty() ? ShaderChoice{} : settings_.CharacterShader(characterId);
    if (options_.shaderPackSet) {   // --shader-pack: this run only
        if (c.pack != options_.shaderPack) {
            c.params.clear();
            c.textureFolder.clear();
        }
        c.pack = options_.shaderPack;
        return c;
    }
    if (c.pack.empty()) SuggestShaderPack(gpu, characterId);
    return c;
}

void App::SuggestShaderPack(const GpuModel& gpu, const std::string& characterId) {
    if (characterId.empty() || options_.shaderPackSet || HeadlessRun()) return;
    if (!suggestChecked_.insert(characterId).second) return;
    const ModelTraits traits = gpu.Traits();
    for (const ShaderPack& p : ShaderPacks().Packs()) {
        if (p.type != PackType::Surface || !PackUsable(&p) || !p.Suggests(traits)) continue;
        if (settings_.ShaderSuggested(p.id, characterId)) continue;
        settings_.SetShaderSuggested(p.id, characterId);
        settings_.Save(settingsPath_);
        toast_ = {Tr("표면 셰이더 추천"),
                  "'" + p.name.Get(PackLanguage()) + "' " + Tr("표면 셰이더가 이 모델에 잘 맞아 보여요 · 이펙트 메뉴에서 켜 보세요"),
                  {}, false, timeSeconds_ + 6.0};
        return;
    }
}

void App::CheckShaderPackErrors() {
    const ShaderPackRegistry& reg = ShaderPacks();
    if (reg.ErrorCount() == shaderPackErrorsSeen_) return;
    shaderPackErrorsSeen_ = reg.ErrorCount();
    const ShaderPack* bad = reg.Find(reg.LastErrorPack());
    toast_ = Toast{};
    toast_.title = Tr("표면 셰이더를 적용하지 못했습니다");
    toast_.detail = (bad ? bad->name.Get(PackLanguage()) : reg.LastErrorPack()) +
                    Tr(" · 기본 셰이딩으로 표시합니다. 이펙트 탭에서 오류를 볼 수 있습니다");
    toast_.error = true;
    toast_.until = timeSeconds_ + 8.0;
}

// ---- previews -------------------------------------------------------------------------------------------------------

static std::string PackThumbKey(const ShaderPack& pack) {
    // the registry generation is part of the key: an edited preview (hot reload) becomes a new entry
    return "p:" + pack.id + ":" + std::to_string(ShaderPacks().Generation());
}

static void CoverUv(const ThumbnailCache& thumbs, const std::string& key, ImVec2 a, ImVec2 b, ImVec2& uv0, ImVec2& uv1) {
    uint32_t w = 0, h = 0;
    thumbs.Size(key, w, h);
    uv0 = ImVec2(0, 0);
    uv1 = ImVec2(1, 1);
    if (w && h) {
        const float ra = (b.x - a.x) / std::max(1.0f, b.y - a.y), ri = (float)w / (float)h;
        if (ri > ra) {
            const float k = ra / ri;
            uv0.x = (1 - k) * 0.5f;
            uv1.x = 1 - uv0.x;
        } else {
            const float k = ri / ra;
            uv0.y = (1 - k) * 0.5f;
            uv1.y = 1 - uv0.y;
        }
    }
}

uint64_t App::PackThumb(const ShaderPack& pack) {
    if (pack.preview.empty()) return 0;
    return thumbs_.Get(PackThumbKey(pack), ThumbnailKind::Image, {pack.preview});
}

uint64_t App::PackThumbBefore(const ShaderPack& pack) {
    if (pack.previewBefore.empty()) return 0;
    return thumbs_.Get("pb:" + pack.id + ":" + std::to_string(ShaderPacks().Generation()), ThumbnailKind::Image,
                       {pack.previewBefore});
}

uint64_t App::RemotePackThumb(const RemotePack& pack) {
    if (pack.previewFile.empty()) return 0;
    return thumbs_.Get("r:" + pack.id + ":" + pack.version, ThumbnailKind::Image, {pack.previewFile});
}

uint64_t App::RemotePackThumbBefore(const RemotePack& pack) {
    if (pack.previewBeforeFile.empty()) return 0;
    return thumbs_.Get("rb:" + pack.id + ":" + pack.version, ThumbnailKind::Image, {pack.previewBeforeFile});
}

void App::DrawPackImage(ImDrawList* dl, uint64_t tex, const std::string& key, ImVec2 a, ImVec2 b, float rounding,
                        ImDrawFlags flags) {
    const Palette& p = P();
    if (!tex) {   // placeholder: a soft mint field with the pack glyph
        dl->AddRectFilled(a, b, Mix(p.surface, p.accentSoft, 0.7f), rounding, flags);
        Icon(dl, icon::Diamond, std::clamp((b.y - a.y) / Dpi() * 0.35f, 12.0f, 30.0f),
             ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), WithAlpha(p.accentInk, 0.55f));
        return;
    }
    ImVec2 uv0, uv1;
    CoverUv(thumbs_, key, a, b, uv0, uv1);
    dl->AddImageRounded((ImTextureID)tex, a, b, uv0, uv1, IM_COL32_WHITE, rounding, flags);
}

void App::DrawPackCompare(const char* id, ImDrawList* dl, uint64_t before, const std::string& beforeKey, uint64_t after,
                         const std::string& afterKey, ImVec2 a, ImVec2 b, float rounding, ImDrawFlags flags,
                         PackCompareMode mode) {
    if (!before || !after) {
        DrawPackImage(dl, after, afterKey, a, b, rounding, flags);
        return;
    }

    const Palette& p = P();
    const float width = std::max(1.0f, b.x - a.x);
    const float height = std::max(1.0f, b.y - a.y);
    float& t = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID(id), 0.5f);

    bool hovered = false;
    bool active = false;

    if (mode == PackCompareMode::Drag) {
        const ImVec2 savedPos = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton(id, ImVec2(width, height));
        hovered = ImGui::IsItemHovered();
        active = ImGui::IsItemActive();
        ImGui::SetCursorScreenPos(savedPos);

        std::string packId;
        const size_t c1 = afterKey.find(':');
        if (c1 != std::string::npos) {
            const size_t c2 = afterKey.find(':', c1 + 1);
            if (c2 != std::string::npos) packId = afterKey.substr(c1 + 1, c2 - (c1 + 1));
            else packId = afterKey.substr(c1 + 1);
        } else {
            packId = afterKey;
        }
        uiprobe::Info(ImGui::GetItemID(), ("compare:" + packId).c_str(), nullptr);

        if (hovered || active) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }

        if (active) {
            t = 1.0f - std::clamp((ImGui::GetIO().MousePos.x - a.x) / width, 0.0f, 1.0f);
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            t = 0.5f;
        }
    } else { // Hover mode
        const bool hovering = ImGui::IsMouseHoveringRect(a, b) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        const float target = hovering ? 1.0f - std::clamp((ImGui::GetIO().MousePos.x - a.x) / width, 0.0f, 1.0f) : 1.0f;
        t += (target - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
        if (std::abs(1.0f - t) < 0.002f && !hovering) t = 1.0f;
    }

    t = std::clamp(t, 0.0f, 1.0f);
    const float splitX = std::clamp(a.x + width * (1.0f - t), a.x, b.x);

    ImVec2 uv0, uv1;
    CoverUv(thumbs_, afterKey, a, b, uv0, uv1);

    const float splitU = uv0.x + (uv1.x - uv0.x) * ((splitX - a.x) / width);

    const ImDrawFlags effFlags = (flags & ImDrawFlags_RoundCornersMask_) == 0 ? ImDrawFlags_RoundCornersAll : flags;
    const ImDrawFlags flagsLeft = (effFlags & (ImDrawFlags_RoundCornersTopLeft | ImDrawFlags_RoundCornersBottomLeft)) |
                                 (flags & ~ImDrawFlags_RoundCornersMask_);
    const ImDrawFlags flagsRight = (effFlags & (ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomRight)) |
                                  (flags & ~ImDrawFlags_RoundCornersMask_);

    if (splitX > a.x) {
        dl->AddImageRounded((ImTextureID)before, a, ImVec2(splitX, b.y), uv0, ImVec2(splitU, uv1.y),
                            IM_COL32_WHITE, rounding, flagsLeft);
    }
    if (splitX < b.x) {
        dl->AddImageRounded((ImTextureID)after, ImVec2(splitX, a.y), b, ImVec2(splitU, uv0.y), uv1,
                            IM_COL32_WHITE, rounding, flagsRight);
    }

    // Divider
    const bool showDivider = (mode == PackCompareMode::Drag) || (splitX > a.x + 1.0f && splitX < b.x - 1.0f);
    if (showDivider) {
        const float lineW = Dp(2.0f);
        dl->AddRectFilled(ImVec2(splitX - lineW * 0.5f - Dp(1.5f), a.y),
                          ImVec2(splitX + lineW * 0.5f + Dp(1.5f), b.y),
                          IM_COL32(0, 0, 0, 45));
        dl->AddRectFilled(ImVec2(splitX - lineW * 0.5f - Dp(0.5f), a.y),
                          ImVec2(splitX + lineW * 0.5f + Dp(0.5f), b.y),
                          IM_COL32(0, 0, 0, 70));
        dl->AddLine(ImVec2(splitX, a.y), ImVec2(splitX, b.y), IM_COL32(255, 255, 255, 240), lineW);

        if (mode == PackCompareMode::Drag) {
            const ImVec2 knobCenter(splitX, (a.y + b.y) * 0.5f);
            const float grow = Anim(ImGui::GetItemID(), hovered || active);
            const float knobR = Dp(14.0f + 2.0f * grow);
            SoftShadow(dl, ImVec2(knobCenter.x - knobR, knobCenter.y - knobR),
                       ImVec2(knobCenter.x + knobR, knobCenter.y + knobR), knobR, Dp(6.0f), 0.20f,
                       ImVec2(0, Dp(1.0f)));
            dl->AddCircleFilled(knobCenter, knobR, p.surface);
            dl->AddCircle(knobCenter, knobR, p.lineStrong, 0, Dp(1.0f));
            Icon(dl, icon::CaretLeft, Dp(12.0f), ImVec2(knobCenter.x - Dp(4.5f), knobCenter.y), p.ink2);
            Icon(dl, icon::CaretRight, Dp(12.0f), ImVec2(knobCenter.x + Dp(4.5f), knobCenter.y), p.ink2);
        }
    }

    // Labels (Drag mode only)
    if (mode == PackCompareMode::Drag) {
        const char* leftText = Tr("적용 전");
        const char* rightText = Tr("적용 후");
        const ImVec2 leftTs = TextSize(Font::Semibold, size::Caption, leftText);
        const ImVec2 rightTs = TextSize(Font::Semibold, size::Caption, rightText);
        const float margin = Dp(10.0f);
        const float padX = Dp(8.0f), padY = Dp(4.0f);

        const float pillLeftW = leftTs.x + padX * 2.0f;
        const float pillLeftH = leftTs.y + padY * 2.0f;
        const float leftWidth = splitX - a.x;
        const float leftFade = std::clamp((leftWidth - margin) / pillLeftW, 0.0f, 1.0f);
        if (leftFade > 0.01f) {
            const ImVec2 p0(a.x + margin, b.y - margin - pillLeftH);
            const ImVec2 p1(p0.x + pillLeftW, p0.y + pillLeftH);
            dl->AddRectFilled(p0, p1, WithAlpha(IM_COL32(0, 0, 0, 255), 0.65f * leftFade), pillLeftH * 0.5f);
            Text(dl, Font::Semibold, size::Caption, ImVec2(p0.x + padX, p0.y + padY),
                 WithAlpha(IM_COL32(255, 255, 255, 255), leftFade), leftText);
        }

        const float pillRightW = rightTs.x + padX * 2.0f;
        const float pillRightH = rightTs.y + padY * 2.0f;
        const float rightWidth = b.x - splitX;
        const float rightFade = std::clamp((rightWidth - margin) / pillRightW, 0.0f, 1.0f);
        if (rightFade > 0.01f) {
            const ImVec2 p0(b.x - margin - pillRightW, b.y - margin - pillRightH);
            const ImVec2 p1(p0.x + pillRightW, p0.y + pillRightH);
            dl->AddRectFilled(p0, p1, WithAlpha(IM_COL32(0, 0, 0, 255), 0.65f * rightFade), pillRightH * 0.5f);
            Text(dl, Font::Semibold, size::Caption, ImVec2(p0.x + padX, p0.y + padY),
                 WithAlpha(IM_COL32(255, 255, 255, 255), rightFade), rightText);
        }
    }
}

// M6b: the studio project's MME post stack as RenderSettings entries (docs/mme/mme_design.md §2.10). Empty outside
// the studio / without a project: ApplyRenderSettings then falls back to the play-mode stack.
std::vector<MmeEffectEntry> App::StudioMmeEntries() const {
    std::vector<MmeEffectEntry> out;
    if (!studio_) return out;
    for (const studio::MmePostEntry& e : studio_->mmePost) {
        MmeEffectEntry m;
        m.file = e.path;
        m.enabled = e.enabled;
        out.push_back(std::move(m));
    }
    return out;
}

// Studio project effect stack: evaluated entries for the frame (empty outside the studio).
std::vector<EffectStackEntry> App::StudioEvaluatedEffects(float frame) const {
    std::vector<EffectStackEntry> out;
    if (!studio_) return out;
    out.reserve(studio_->effects.size());
    for (const studio::StudioEffect& e : studio_->effects) {
        if (!e.entry.enabled) continue;
        out.push_back(studio::EvaluateEffect(e, frame));
    }
    return out;
}

// M6b: a studio MME stack edit (add / remove / toggle) marks the project dirty and pushes the stack to the renderer.
void App::StudioMmeChanged() {
    if (studio_) ++studio_->projectVersion;
    ApplyRenderSettings();
}

// Ray-MMD main effect guard: every MME add site asks before adding ray.x / ray.fx next to the native PBR renderer.
// True = queued for the confirm modal (the caller must NOT add now); false = add as before.
bool App::GuardRayMme(const std::filesystem::path& p, std::function<void(const std::filesystem::path&)> add) {
    if (!IsRayMmdMainEffect(p)) return false;
    rayConfirmPath_ = p;
    rayConfirmAdd_ = std::move(add);
    return true;
}

// The shared Ray-MMD confirm modal. Draws only while a guarded add is pending; "취소" (default) drops it, "그래도
// MME로 추가" runs the site's continuation.
void App::DrawRayMmeConfirm() {
    using namespace ui;
    if (rayConfirmPath_.empty()) return;
    const Palette& p = P();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dp(420.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(24.0f), Dp(22.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(p.surface));

    if (!ImGui::IsPopupOpen("##raymmeconfirm")) ImGui::OpenPopup("##raymmeconfirm");
    if (ImGui::BeginPopupModal("##raymmeconfirm", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        TextEllipsis(dl, Font::Semibold, size::Title, c, c.x + Dp(372.0f), p.ink, Tr("Ray-MMD"));
        Text(dl, Font::Semibold, size::Body, ImVec2(c.x, c.y + Dp(28.0f)), p.ink,
             PathToUtf8(rayConfirmPath_.filename()).c_str());

        ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(54.0f)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Dp(372.0f));   // window-local x
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink2));
        PushFont(Font::Regular, size::Small);
        ImGui::TextWrapped("%s", Tr("이 앱은 Ray-MMD를 네이티브 PBR로 지원합니다. 캐릭터의 재질 소스를 'Ray-MMD'로 바꾸면 그대로 쓸 수 있습니다. MME로 함께 실행하면 조명과 후처리가 두 번 적용됩니다."));
        PopFont();
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        ImGui::Dummy(ImVec2(0, Dp(20.0f)));
        bool close = false;
        // default: cancel, nothing is added
        if (Button("##raycancel", Tr("취소"), nullptr, ButtonKind::Secondary, ImVec2(180.0f, 40.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            close = true;
        }
        ImGui::SameLine(0, Dp(12.0f));
        if (Button("##rayforce", Tr("그래도 MME로 추가"), nullptr, ButtonKind::Primary, ImVec2(180.0f, 40.0f))) {
            const std::filesystem::path file = rayConfirmPath_;
            std::function<void(const std::filesystem::path&)> add = std::move(rayConfirmAdd_);
            rayConfirmPath_.clear();
            rayConfirmAdd_ = nullptr;
            close = true;
            if (add) add(file);
        }
        if (close) {
            rayConfirmPath_.clear();
            rayConfirmAdd_ = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// M6b: one-time suggestion when a character with no saved MME choice ships an .emd / same-name .fx. The toast
// offers an apply button (DrawToast); never applied automatically. Remembered per character id.
void App::SuggestMme(const std::string& characterId, const std::filesystem::path& modelPath) {
    if (characterId.empty() || options_.mmeAutoSet || options_.mmeModelSet || HeadlessRun()) return;
    if (!MmeGlobalEnabled() || MmeAutoWorkflow()) return;   // no suggestions with MME off / in 완전 자동
    if (settings_.CharacterMme(characterId).mode != MmeChoice::None) return;
    if (settings_.MmeSuggested(characterId)) return;
    if (!ModelShipsMme(modelPath)) return;
    settings_.SetMmeSuggested(characterId);
    settings_.Save(settingsPath_);
    toast_ = Toast{Tr("이 모델은 MME 재질을 포함하고 있어요"), Tr("MME 메뉴에서 켤 수 있어요"), {}, false, timeSeconds_ + 6.0};
    mmeSuggestApply_ = characterId;   // the toast's apply button
}

// M6b: the per-model MME choice editor (combo 없음 / 모델 동봉 효과 / 파일 선택…). Shared by the lobby character
// panel, the play bar's shader popup and the studio inspector. `hasOwn` = the model ships an `.emd` / same-name `.fx`
// (only then is the Auto row offered). Returns true when the choice changed.
bool App::DrawMmeRow(const char* id, MmeChoice& choice, const std::string& modelId,
                     const std::filesystem::path& modelPath, float width) {
    const Palette& p = P();
    (void)modelId;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool changed = false;
    const bool hasOwn = ModelShipsMme(modelPath);

    // update the visible state if a CLI override applies this run (it is not saved)
    const bool stageOverride = options_.stageMmeSet;
    const bool autoOverride = options_.mmeAutoSet;

    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const ImVec2 a = win->DC.CursorPos;
    const ImVec2 b(a.x + width, a.y + Dp(30.0f));
    Text(dl, Font::Regular, size::Small, ImVec2(a.x, a.y + Dp(6.0f)), p.ink2, Tr("효과 파일"));
    ImGui::SetCursorScreenPos(ImVec2(b.x - Dp(190.0f), a.y + Dp(2.0f)));
    ImGui::SetNextItemWidth(Dp(190.0f));
    PushFont(Font::Regular, size::Small);

    std::string preview = choice.mode == MmeChoice::Auto    ? std::string(Tr("모델 동봉 효과"))
                          : choice.mode == MmeChoice::File  ? PathToUtf8(choice.file.filename())
                                                            : std::string(Tr("없음"));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(p.ink));
    // the app style has no window padding: give the dropdown a margin so its first / last rows are not cut
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(6.0f), Dp(6.0f)));
    const bool open = ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLargest | ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    uiprobe::Info(ImGui::GetItemID(), id, preview.c_str());   // MCP / guide scripts find the dropdown by its id
    {
        const ImVec2 cmax = ImGui::GetItemRectMax(), cmin = ImGui::GetItemRectMin();
        Icon(dl, icon::CaretDown, 12.0f, ImVec2(cmax.x - Dp(12.0f), (cmin.y + cmax.y) * 0.5f), p.ink3);
    }
    if (open) {
        if (ImGui::Selectable(Tr("없음"), choice.mode == MmeChoice::None)) {
            choice = MmeChoice{};
            changed = true;
        }
        ImGui::BeginDisabled(!hasOwn);
        if (ImGui::Selectable(Tr("모델 동봉 효과"), choice.mode == MmeChoice::Auto)) {
            choice = MmeChoice{MmeChoice::Auto, {}};
            changed = true;
        }
        ImGui::EndDisabled();
        if (!hasOwn) {
            PushFont(Font::Regular, size::Caption);
            ImGui::TextDisabled("%s", Tr("이 모델은 동봉된 MME 재질이 없어요"));
            PopFont();
        }
        // a library object effect picked for this model (File mode)
        for (const EffectAsset& e : library_.effects) {
            if (e.kind == "post" || e.kind == "pre") continue;   // object effects only
            ImGui::PushID(e.id.c_str());
            const bool sel = choice.mode == MmeChoice::File && choice.file == e.path;
            if (ImGui::Selectable(e.displayName.c_str(), sel)) {
                choice = MmeChoice{MmeChoice::File, e.path};
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::Selectable(Tr("파일 선택…"))) {
            const std::filesystem::path picked =
                studio::OpenFileDialog(hwnd_, {{L"MME 효과 (.fx)", L"*.fx"}}, modelPath.parent_path());
            if (!picked.empty()) {
                choice = MmeChoice{MmeChoice::File, picked};
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    PopFont();
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
    ImGui::Dummy(ImVec2(width, 0));

    if (stageOverride || autoOverride) {
        PushFont(Font::Regular, size::Caption);
        ImGui::TextDisabled("%s", Tr("명령줄 설정이 이번 실행 동안 우선합니다"));
        PopFont();
    }
    return changed;
}


bool App::DrawShaderSelector(const char* id, ShaderChoice& choice, float width, const ModelTraits* traits) {
    const Palette& p = P();
    ShaderPackRegistry& reg = ShaderPacks();
    const std::string lang = PackLanguage();
    const ShaderPack* cur = choice.pack.empty() ? nullptr : reg.Find(choice.pack);
    bool changed = false;

    // the row: preview, name, version / authors, caret
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = win->DrawList;
    const ImVec2 a = win->DC.CursorPos;
    const ImVec2 b(a.x + width, a.y + Dp(54.0f));
    bool hovered = false;
    const std::string hit = std::string(id) + "_row";
    const std::string popup = std::string(id) + "_popup";
    if (CardItem(hit.c_str(), a, b, &hovered)) ImGui::OpenPopup(popup.c_str());
    const float hv = Anim(ImGui::GetID(hit.c_str()), hovered);
    dl->AddRectFilled(a, b, Mix(p.surface, p.sunken, 0.4f + 0.6f * hv), Dp(12.0f));
    dl->AddRect(a, b, Mix(p.line, p.lineStrong, hv), Dp(12.0f));
    const ImVec2 ia(a.x + Dp(7.0f), a.y + Dp(7.0f)), ib(ia.x + Dp(71.0f), b.y - Dp(7.0f));
    if (cur) {
        DrawPackImage(dl, PackThumb(*cur), PackThumbKey(*cur), ia, ib, Dp(8.0f));
    } else {
        dl->AddRectFilled(ia, ib, p.sunken, Dp(8.0f));
        Icon(dl, icon::Cube, 18.0f, ImVec2((ia.x + ib.x) * 0.5f, (ia.y + ib.y) * 0.5f), p.ink2);
    }
    const float tx = ib.x + Dp(12.0f), tr = b.x - Dp(30.0f);
    const std::string title = cur ? cur->name.Get(lang) : (choice.pack.empty() ? std::string(Tr("MMD 기본")) : choice.pack);
    std::string sub = cur ? "v" + cur->version + (cur->authors.empty() ? "" : "  ·  " + PackAuthorLine(cur->authors))
                          : (choice.pack.empty() ? std::string(Tr("앱 기본 셰이딩")) : std::string(Tr("설치되어 있지 않음")));
    const bool problem = (cur && !PackUsable(cur)) || (!cur && !choice.pack.empty());
    if (cur && !PackUsable(cur)) sub = Tr("사용할 수 없음 · 이펙트 탭에서 확인");
    TextEllipsis(dl, Font::Semibold, size::Body, ImVec2(tx, a.y + Dp(9.0f)), tr, p.ink, title.c_str());
    TextEllipsis(dl, Font::Regular, size::Caption, ImVec2(tx, a.y + Dp(30.0f)), tr, problem ? p.warn : p.ink3, sub.c_str());
    Icon(dl, icon::CaretDown, 14.0f, ImVec2(b.x - Dp(16.0f), (a.y + b.y) * 0.5f), p.ink2);
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
    ImGui::Dummy(ImVec2(width, 0));

    // the list
    ImGui::SetNextWindowPos(ImVec2(a.x, b.y + Dp(6.0f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(std::max(width, Dp(340.0f)), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
    if (ImGui::BeginPopup(popup.c_str())) {
        studioThumbsShown_ = true;   // the studio / play screens only pump thumbnails while one is drawn
        static char filter[96] = {};
        if (ImGui::IsWindowAppearing()) filter[0] = 0;
        const float pw = ImGui::GetContentRegionAvail().x;
        SearchField("##packsearch", filter, sizeof(filter), Tr("이름, 작성자, 태그로 검색"), pw / Dpi());
        Gap(8.0f);
        const std::string needle = ToLowerAscii(filter);
        const float listH = std::min(Dp(372.0f), Dp(58.0f) * (float)(reg.Packs().size() + 1) + Dp(16.0f));
        ImGui::BeginChild("##packlist", ImVec2(pw, listH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        const float lw = ImGui::GetContentRegionAvail().x;
        const auto row = [&](const char* rid, const ShaderPack* pk, bool selected) -> bool {
            const ImVec2 ra = ImGui::GetCursorScreenPos(), rb(ra.x + lw, ra.y + Dp(56.0f));
            bool h = false;
            const bool selectable = !pk || PackUsable(pk);
            const bool clicked = CardItem(rid, ra, rb, &h, pk ? pk->name.Get(lang).c_str() : Tr("MMD 기본"));
            const float t = Anim(ImGui::GetID(rid), h && selectable);
            if (selected) ldl->AddRectFilled(ra, rb, p.accentSoft, Dp(10.0f));
            else if (t > 0.01f) ldl->AddRectFilled(ra, rb, WithAlpha(p.sunken, t), Dp(10.0f));
            const ImVec2 pa(ra.x + Dp(6.0f), ra.y + Dp(6.0f)), pb(pa.x + Dp(78.0f), rb.y - Dp(6.0f));
            if (pk) {
                DrawPackImage(ldl, PackThumb(*pk), PackThumbKey(*pk), pa, pb, Dp(7.0f));
            } else {
                ldl->AddRectFilled(pa, pb, p.sunken, Dp(7.0f));
                Icon(ldl, icon::Cube, 16.0f, ImVec2((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f), p.ink2);
            }
            const float x = pb.x + Dp(10.0f), r = rb.x - Dp(28.0f);
            const std::string nm = pk ? pk->name.Get(lang) : std::string(Tr("MMD 기본"));
            std::string sb = pk ? "v" + pk->version + (pk->authors.empty() ? "" : "  ·  " + PackAuthorLine(pk->authors))
                                : std::string(Tr("앱 기본 셰이딩"));
            if (pk && !selectable) sb = Tr("사용할 수 없음 · 이펙트 탭에서 확인");
            float nameR = r;
            if (pk && selectable && traits && pk->Suggests(*traits)) {   // pack.json "suggest" fits this model
                const char* tag = Tr("추천");
                const float bw = TextSize(Font::Semibold, size::Caption, tag).x + Dp(16.0f);
                const float nw = TextSize(Font::Semibold, size::Small, nm.c_str()).x;
                const float bx = std::max(x, std::min(x + nw + Dp(6.0f), r - bw));
                Badge(ldl, ImVec2(bx, ra.y + Dp(8.0f)), tag, p.accentSoft, p.accentInk);
                nameR = bx - Dp(4.0f);
            }
            TextEllipsis(ldl, Font::Semibold, size::Small, ImVec2(x, ra.y + Dp(10.0f)), nameR, selectable ? p.ink : p.ink3, nm.c_str());
            TextEllipsis(ldl, Font::Regular, size::Caption, ImVec2(x, ra.y + Dp(30.0f)), r, selectable ? p.ink3 : p.warn,
                         sb.c_str());
            if (selected) Icon(ldl, icon::Check, 14.0f, ImVec2(rb.x - Dp(14.0f), (ra.y + rb.y) * 0.5f), p.accentInk);
            if (pk && h && !pk->description.Empty()) Tooltip(pk->description.Get(lang).c_str());
            ImGui::SetCursorScreenPos(ImVec2(ra.x, rb.y + Dp(2.0f)));
            ImGui::Dummy(ImVec2(lw, 0));
            return clicked && selectable;
        };
        if (needle.empty() && row("##pk_default", nullptr, choice.pack.empty()) && !choice.pack.empty()) {
            choice.SwitchPack("");
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        int shown = 0;
        for (const ShaderPack& pk : reg.Packs()) {
            if (pk.status == PackStatus::Duplicate || pk.type != PackType::Surface ||
                !PackMatches(needle, pk.id, pk.name, pk.authors, pk.tags))
                continue;
            ++shown;
            const std::string rid = "##pk_" + pk.id;
            if (row(rid.c_str(), &pk, choice.pack == pk.id) && choice.pack != pk.id) {
                choice.SwitchPack(pk.id);
                changed = true;
                ImGui::CloseCurrentPopup();
            }
        }
        if (!needle.empty() && shown == 0) ImGui::TextDisabled("%s", Tr("검색 결과가 없습니다"));
        ImGui::EndChild();
        // the manager is a screen of its own: only the library can switch to it (inside a scene / the studio the button did nothing)
        if (screen_ == Screen::Select) {
            Gap(6.0f);
            if (Button("##packmanage", Tr("이펙트 관리 · 온라인에서 받기"), icon::Diamond, ButtonKind::Ghost)) {
                ImGui::CloseCurrentPopup();
                if (!choice.pack.empty()) shaderSelInstalled_ = choice.pack;
                shaderTab_ = 0;
                screen_ = Screen::Shaders;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    return changed;
}

// 1/2/3 columns for the parameter grid: packs with many params split the grid, small packs stay single column.
static int PackParamColumns(size_t n) {
    if (n <= 6) return 1;
    if (n <= 14) return 2;
    return 3;
}

// Popup width for the pack parameter grid (columns from the pack's param count), clamped to the viewport.
float PackParamsPopupWidth(const ShaderChoice& choice) {
    const ShaderPack* pack = choice.pack.empty() ? nullptr : ShaderPacks().Find(choice.pack);
    const float w = (float)PackParamColumns(pack ? pack->params.size() : 0) * Dp(280.0f) + Dp(32.0f);
    return std::min(w, ImGui::GetMainViewport()->WorkSize.x - Dp(24.0f));
}

bool App::DrawShaderPackParams(ShaderChoice& choice) {
    const ShaderPack* pack = choice.pack.empty() ? nullptr : ShaderPacks().Find(choice.pack);
    if (!pack) return false;
    bool changed = false;
    const std::string lang = PackLanguage();
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ShaderPackRegistry& reg = ShaderPacks();
    // per-character texture folder (pack.json "textures" packs): the folder this model's set loads from, picker +
    // clear + the set's missing count; edits the ShaderChoice (the callers save it)
    if (!pack->textures.empty()) {
        const float w = ImGui::GetContentRegionAvail().x;
        const std::filesystem::path folder =
            choice.textureFolder.empty() ? std::filesystem::path() : Utf8ToPath(choice.textureFolder);
        const std::string shown = folder.empty() ? std::string(Tr("팩 설정 사용"))
                                                 : std::string(Tr("텍스처 폴더")) + ": " + PathToUtf8(folder);
        const uint32_t missing = reg.MissingTextures(pack->id, folder);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        TextEllipsis(dl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(6.0f)), c.x + w - Dp(96.0f),
                     missing ? p.warn : p.ink2, shown.c_str());
        ImGui::SetCursorScreenPos(ImVec2(c.x + w - Dp(88.0f), c.y));
        if (IconButton("##choiceTexPick", icon::FolderOpen, Tr("텍스처 폴더 선택"))) {
            const std::filesystem::path picked = PickPackTextureFolder(hwnd_);
            if (!picked.empty()) {
                choice.textureFolder = PathToUtf8(picked);
                changed = true;
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(folder.empty());
        if (IconButton("##choiceTexClear", icon::X, Tr("텍스처 폴더 지우기"))) {
            choice.textureFolder.clear();
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + Dp(30.0f)));
        if (missing) {
            Text(dl, Font::Regular, size::Caption, ImGui::GetCursorScreenPos(), p.warn,
                 (std::to_string(missing) + Tr("개의 팩 텍스처가 없어 흰색으로 표시됩니다")).c_str());
            ImGui::Dummy(ImVec2(w, Dp(18.0f)));
        }
        ImGui::Dummy(ImVec2(w, 0));
        Gap(4.0f);
    }
    if (pack->params.empty()) return changed;
    changed |= DrawShaderPackParameters(*pack, choice.params, "##spp", true);
    Gap(6.0f);
    ImGui::BeginDisabled(choice.params.empty());
    if (Button("##sp_reset", Tr("기본값으로"), icon::ArrowCcw, ButtonKind::Ghost)) {
        choice.params.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

// A collapsible params.ext group header (full-width row, caret + label); returns whether the group is open.
// The open state is per pack + group in AppSettings::paramGroupOpen (default: the manifest's "collapsed").
bool App::DrawParamGroupHeader(const ShaderPack& pack, const ShaderPackGroup& g, const char* idPrefix) {
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool open = settings_.ParamGroupOpen(pack.id, g.id, !g.collapsed);
    const std::string label = g.label.Empty() ? g.id : g.label.Get(PackLanguage());
    const std::string hit = std::string(idPrefix) + "_grp_" + g.id;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + ImGui::GetContentRegionAvail().x, a.y + Dp(28.0f));
    bool hovered = false;
    if (CardItem(hit.c_str(), a, b, &hovered, label.c_str())) {   // advances the cursor past the row
        open = !open;
        settings_.SetParamGroupOpen(pack.id, g.id, open);
    }
    dl->AddRectFilled(a, b, hovered ? WithAlpha(p.ink, 0.05f) : p.sunken, Dp(4.0f));
    Icon(dl, open ? icon::CaretDown : icon::CaretRight, Dp(14.0f), ImVec2(a.x + Dp(12.0f), (a.y + b.y) * 0.5f), p.ink2);
    Text(dl, Font::Semibold, size::Small, ImVec2(a.x + Dp(26.0f), a.y + Dp(6.0f)), p.ink, label.c_str());
    return open;
}

bool App::DrawShaderPackParameters(const ShaderPack& pack, std::map<std::string, float>& params,
                                   const char* idPrefix, bool grid) {
    if (pack.params.empty()) return false;
    bool changed = false;
    const std::string lang = PackLanguage();
    const Palette& p = P();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto drawParamWidget = [&](const ShaderPackParam& sp, const std::string& prefix) {
        bool itemChanged = false;
        const std::string label = sp.label.Empty() ? sp.key : sp.label.Get(lang);
        const std::string sid = prefix + "_" + sp.key;

        switch (sp.type) {
        case ShaderPackParamType::Float: {
            float v = params.count(sp.key) ? params[sp.key] : sp.def;
            if (SliderRow(sid.c_str(), label.c_str(), &v, sp.min, sp.max, "%.2f")) {
                if (sp.step > 0.0f) v = sp.min + std::round((v - sp.min) / sp.step) * sp.step;
                params[sp.key] = v;
                itemChanged = true;
            }
            break;
        }
        case ShaderPackParamType::Int: {
            float v = params.count(sp.key) ? params[sp.key] : sp.def;
            if (SliderRow(sid.c_str(), label.c_str(), &v, sp.min, sp.max, "%.0f")) {
                v = std::round(v);
                params[sp.key] = v;
                itemChanged = true;
            }
            break;
        }
        case ShaderPackParamType::Bool: {
            float raw = params.count(sp.key) ? params[sp.key] : (sp.defBool ? 1.0f : 0.0f);
            bool b = (raw > 0.5f);
            if (Switch(sid.c_str(), label.c_str(), &b)) {
                params[sp.key] = b ? 1.0f : 0.0f;
                itemChanged = true;
            }
            break;
        }
        case ShaderPackParamType::Color: {
            float rgb[3] = {
                params.count(sp.key + "#0") ? params[sp.key + "#0"] : sp.defVec[0],
                params.count(sp.key + "#1") ? params[sp.key + "#1"] : sp.defVec[1],
                params.count(sp.key + "#2") ? params[sp.key + "#2"] : sp.defVec[2]
            };
            const ImVec2 c = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            TextEllipsis(dl, Font::Regular, size::Small, ImVec2(c.x, c.y + Dp(4.0f)), c.x + w * 0.4f, p.ink2, label.c_str());
            ImGui::SetCursorScreenPos(ImVec2(c.x + w * 0.4f, c.y));
            if (ColorField(sid.c_str(), rgb)) {
                params[sp.key + "#0"] = rgb[0];
                params[sp.key + "#1"] = rgb[1];
                params[sp.key + "#2"] = rgb[2];
                itemChanged = true;
            }
            if (sp.alpha) {
                float a = params.count(sp.key + "#3") ? params[sp.key + "#3"] : sp.defVec[3];
                std::string alphaLabel = label + " " + Tr("불투명도");
                if (SliderRow((sid + "_a").c_str(), alphaLabel.c_str(), &a, 0.0f, 1.0f, "%.2f")) {
                    params[sp.key + "#3"] = a;
                    itemChanged = true;
                }
            }
            break;
        }
        case ShaderPackParamType::Vec3: {
            float xyz[3] = {
                params.count(sp.key + "#0") ? params[sp.key + "#0"] : sp.defVec[0],
                params.count(sp.key + "#1") ? params[sp.key + "#1"] : sp.defVec[1],
                params.count(sp.key + "#2") ? params[sp.key + "#2"] : sp.defVec[2]
            };
            static const char* kAxes[3] = {" X", " Y", " Z"};
            for (int c = 0; c < 3; ++c) {
                std::string cid = sid + "_" + std::to_string(c);
                std::string clabel = label + kAxes[c];
                if (SliderRow(cid.c_str(), clabel.c_str(), &xyz[c], sp.min, sp.max, "%.2f")) {
                    if (sp.step > 0.0f) xyz[c] = sp.min + std::round((xyz[c] - sp.min) / sp.step) * sp.step;
                    params[sp.key + "#" + std::to_string(c)] = xyz[c];
                    itemChanged = true;
                }
            }
            break;
        }
        }
        return itemChanged;
    };

    // A run of params: up to 3 columns (column-major) when `grid` and the panel is wide enough, else one column.
    int gridIndex = 0;
    auto drawList = [&](const std::vector<const ShaderPackParam*>& list) {
        const float avail = ImGui::GetContentRegionAvail().x;
        const int cols = grid ? std::clamp(std::min((int)(avail / Dp(240.0f)), (int)list.size()), 1, 3) : 1;
        const std::string tableId = std::string(idPrefix) + "_grid" + std::to_string(gridIndex++);
        if (cols > 1 && ImGui::BeginTable(tableId.c_str(), cols, ImGuiTableFlags_SizingStretchSame)) {
            const size_t n = list.size();
            const int rows = (int)((n + (size_t)cols - 1) / (size_t)cols);   // column-major: ceil(n/cols)
            for (int row = 0; row < rows; ++row) {
                ImGui::TableNextRow();
                for (int col = 0; col < cols; ++col) {
                    const size_t i = (size_t)col * (size_t)rows + (size_t)row;
                    if (i >= n) continue;
                    ImGui::TableSetColumnIndex(col);
                    changed |= drawParamWidget(*list[i], idPrefix);
                }
            }
            ImGui::EndTable();
        } else {
            for (const ShaderPackParam* sp : list) changed |= drawParamWidget(*sp, idPrefix);
        }
    };

    if (pack.groups.empty()) {
        std::vector<const ShaderPackParam*> all;
        for (const ShaderPackParam& sp : pack.params) all.push_back(&sp);
        drawList(all);
    } else {
        std::vector<const ShaderPackParam*> ungrouped;
        std::map<std::string, std::vector<const ShaderPackParam*>> grouped;
        for (const ShaderPackParam& sp : pack.params) {
            if (sp.group.empty()) ungrouped.push_back(&sp);
            else grouped[sp.group].push_back(&sp);
        }

        if (!ungrouped.empty()) drawList(ungrouped);

        for (const ShaderPackGroup& g : pack.groups) {
            auto it = grouped.find(g.id);
            if (it == grouped.end() || it->second.empty()) continue;

            const bool open = DrawParamGroupHeader(pack, g, idPrefix);

            if (open) {
                ImGui::Indent(Dp(12.0f));
                Gap(4.0f);
                drawList(it->second);
                Gap(4.0f);
                ImGui::Unindent(Dp(12.0f));
            }
            Gap(4.0f);
        }
    }
    return changed;
}

// ---- PBR material source (P5) ---------------------------------------------------------------------------------------

MaterialChoice App::ResolveMaterialChoice(const std::string& characterId, const std::filesystem::path& modelPath) const {
    return ResolveMaterialChoiceFrom(settings_.characterMaterials, RayMmdGlobalEnabled(), MmeAutoWorkflow(), characterId,
                                     modelPath);
}

// The resolution over an explicit saved map / switch / workflow: the MME panel's draft comparison uses it. `enabled`
// is the Ray-MMD switch: --ray-global 0 wins over everything.
MaterialChoice App::ResolveMaterialChoiceFrom(const std::map<std::string, MaterialChoice>& materials, bool enabled,
                                              bool autoWorkflow, const std::string& characterId,
                                              const std::filesystem::path& modelPath) const {
    // --ray-global 0 wins over everything: Ray-MMD material data is treated as Default.
    if (!enabled) return MaterialChoice{};
    // --material-source wins over saved choices and the workflow: Ray for every model, Default for none.
    if (options_.materialSourceSet) {
        MaterialChoice c;
        c.ray = options_.materialSource == 1;
        return c;
    }
    // 완전 자동: Ray (auto candidate, empty .emd) for every model that has a Ray .emd candidate, Default otherwise.
    if (autoWorkflow) {
        if (modelPath.empty()) return MaterialChoice{};
        const std::string key = (characterId.empty() ? PathToUtf8(modelPath) : characterId) + "|auto";
        const std::vector<RayEmdCandidate>& cands = MaterialCandidates(key, modelPath, 0);
        MaterialChoice c;
        c.ray = std::any_of(cands.begin(), cands.end(),
                            [](const RayEmdCandidate& e) { return !e.readFailed && e.rayEntries > 0; });
        return c;
    }
    const auto it = materials.find(characterId);
    return (characterId.empty() || it == materials.end()) ? MaterialChoice{} : it->second;
}

const std::vector<RayEmdCandidate>& App::MaterialCandidates(const std::string& key,
                                                            const std::filesystem::path& modelPath,
                                                            int pmxMaterialCount) const {
    // Reachable from loader worker threads too (the Auto workflow calls it through the material resolver), so the
    // session cache is guarded; the returned reference outlives the lock (the map is never erased).
    std::lock_guard<std::mutex> lock(materialCandidatesMutex_);
    auto it = materialCandidates_.find(key);
    if (it != materialCandidates_.end()) return it->second;
    std::vector<RayEmdCandidate> cands = FindRayEmdCandidates(modelPath, pmxMaterialCount);  // ~10 ms, cached per session
    return materialCandidates_.emplace(key, std::move(cands)).first->second;
}

bool App::ImportedPbrCandidates(const std::string& key, const std::filesystem::path& modelPath) {
    auto it = importedPbrCandidates_.find(key);
    if (it != importedPbrCandidates_.end()) return it->second;
    const bool has = ModelHasImportedPbr(modelPath);   // parses the file, does not decode textures
    return importedPbrCandidates_.emplace(key, has).first->second;
}

int App::ModelMaterialCount(const std::string& key, const std::filesystem::path& modelPath) {
    auto it = materialCountCache_.find(key);
    if (it != materialCountCache_.end()) return it->second;
    // A cheap header read for PMX/PMD (ProbePmx/ProbePmd skip geometry); an imported model with PBR data has no PMX
    // material count, so any positive count lets the "기본 / PBR" segment show (the count is only used for Ray .emd
    // matching, which imported models do not use).
    int count = 0;
    std::string err;
    PmxProbe probe;
    if (ProbePmx(modelPath, probe, &err) || ProbePmd(modelPath, probe, &err))
        count = (int)probe.materialCount;
    else if (ImportedPbrCandidates(key, modelPath))
        count = 1;
    return materialCountCache_.emplace(key, count).first->second;
}

bool App::HasMaterialOptions(const std::string& cacheKey, const std::filesystem::path& modelPath, int pmxMaterialCount,
                             const MaterialChoice& choice) {
    if (modelPath.empty() || pmxMaterialCount <= 0) return false;
    if (ImportedPbrCandidates(cacheKey, modelPath)) return true;   // P6: an imported model with PBR data
    const std::vector<RayEmdCandidate>& cands = MaterialCandidates(cacheKey, modelPath, pmxMaterialCount);
    return std::any_of(cands.begin(), cands.end(),
                       [](const RayEmdCandidate& c) { return !c.readFailed && c.rayEntries > 0; }) ||
           !choice.emd.empty();
}

bool App::DrawMaterialSelector(const char* id, MaterialChoice& choice, const std::string& cacheKey,
                               const std::filesystem::path& modelPath, int pmxMaterialCount, float width) {
    if (modelPath.empty() || pmxMaterialCount <= 0) return false;
    bool changed = false;
    const bool importedPbr = ImportedPbrCandidates(cacheKey, modelPath);   // P6: imported model PBR data
    const std::vector<RayEmdCandidate>& cands = MaterialCandidates(cacheKey, modelPath, pmxMaterialCount);
    // The widget only exists when there is something to choose: at least one Ray candidate, imported PBR data, or
    // an explicit .emd.
    const bool hasRayCandidate = std::any_of(cands.begin(), cands.end(), [](const RayEmdCandidate& c) {
        return !c.readFailed && c.rayEntries > 0;
    });
    if (!hasRayCandidate && !importedPbr && choice.emd.empty()) return false;

    // Default / Ray-MMD selector (imitate DrawShaderSelector's option control). An imported model's second segment
    // is "PBR" (its own material data, no .emd row).
    {
        const char* labels[] = {Tr("기본"), importedPbr ? "PBR" : "Ray-MMD"};
        int mode = choice.ray ? 1 : 0;
        if (ui::Segmented((std::string(id) + "_src").c_str(), labels, 2, &mode, width / Dpi(), 34.0f)) {
            choice.ray = mode == 1;
            changed = true;
        }
    }
    if (!choice.ray || importedPbr) return changed;   // Default, or an imported model: no .emd row

    // Candidate combo: relative to the model folder; an explicit path shows its file name. "파일 선택…" opens a dialog.
    const std::filesystem::path modelDir = modelPath.parent_path();
    const auto relLabel = [&](const std::filesystem::path& emd) {
        std::error_code ec;
        const std::filesystem::path rel = emd.lexically_relative(modelDir);
        const std::string s = PathToUtf8(rel.empty() ? emd.filename() : rel);
        return s.empty() ? PathToUtf8(emd.filename()) : s;
    };
    std::filesystem::path chosen = choice.emd.empty() ? std::filesystem::path() : Utf8ToPath(choice.emd);
    Gap(6.0f);
    const std::string preview =
        chosen.empty() ? std::string(Tr("자동 (가장 알맞은 .emd)"))
                       : relLabel(chosen) + (std::filesystem::exists(chosen) ? "" : std::string("  ·  ") + Tr("파일 없음"));
    ImGui::SetNextItemWidth(width);
    PushFont(Font::Regular, size::Small);
    bool comboChanged = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(6.0f), Dp(6.0f)));
    const bool emdOpen = ImGui::BeginCombo((std::string(id) + "_emd").c_str(), preview.c_str(),
                                           ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleVar();
    {   // a quiet caret instead of ImGui's arrow box (as the material class dropdown)
        const ImVec2 cmax = ImGui::GetItemRectMax(), cmin = ImGui::GetItemRectMin();
        Icon(ImGui::GetWindowDrawList(), icon::CaretDown, 12.0f, ImVec2(cmax.x - Dp(12.0f), (cmin.y + cmax.y) * 0.5f),
             P().ink3);
    }
    if (emdOpen) {
        if (ImGui::Selectable(Tr("자동 (가장 알맞은 .emd)"), chosen.empty())) {
            choice.emd.clear();
            comboChanged = true;
        }
        for (size_t i = 0; i < cands.size(); ++i) {
            const RayEmdCandidate& c = cands[i];
            if (c.readFailed || c.rayEntries == 0) continue;
            ImGui::PushID((int)i);
            const bool sel = !chosen.empty() && c.path == chosen;
            if (ImGui::Selectable(relLabel(c.path).c_str(), sel)) {
                choice.emd = PathToUtf8(c.path);
                comboChanged = true;
            }
            if (c.rayEntries < c.objCount)
                ImGui::SetItemTooltip("%s", (std::to_string(c.rayEntries) + "/" + std::to_string(c.objCount) + " Ray").c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    PopFont();
    if (comboChanged) {
        choice.ray = true;
        changed = true;
    }
    Gap(4.0f);
    if (Button((std::string(id) + "_pick").c_str(), Tr("파일 선택…"), icon::FolderOpen, ButtonKind::Ghost)) {
        const std::filesystem::path picked =
            studio::OpenFileDialog(hwnd_, {{L"Ray-MMD material (.emd)", L"*.emd"}}, modelDir);
        if (!picked.empty()) {
            choice.ray = true;
            choice.emd = PathToUtf8(picked);
            changed = true;
        }
    }
    return changed;
}

void App::SuggestRayMaterial(const std::string& characterId, const std::filesystem::path& modelPath, int pmxMaterialCount) {
    if (characterId.empty() || options_.materialSourceSet || HeadlessRun()) return;
    if (!RayMmdGlobalEnabled() || MmeAutoWorkflow()) return;   // no suggestions with Ray-MMD off / in 완전 자동
    if (ImportedPbrCandidates(characterId, modelPath)) return;   // P6: imported models get no suggestion toast
    if (!materialSuggestChecked_.emplace(characterId, true).second) return;
    if (settings_.CharacterMaterial(characterId).ray || settings_.MaterialSuggested(characterId)) return;
    const std::vector<RayEmdCandidate>& cands = MaterialCandidates(characterId, modelPath, pmxMaterialCount);
    const bool anyRay = std::any_of(cands.begin(), cands.end(), [](const RayEmdCandidate& c) {
        return !c.readFailed && c.rayEntries > 0;
    });
    if (!anyRay) return;
    settings_.SetMaterialSuggested(characterId);
    settings_.Save(settingsPath_);
    toast_ = {Tr("Ray-MMD 재질 추천"),
              Tr("이 캐릭터에 Ray-MMD 재질 데이터가 있어요 · 셰이더 탭의 재질 데이터에서 켤 수 있어요"), {}, false, timeSeconds_ + 6.0};
}

void App::DrawStudioShaderRow(float w) {
    studio::StudioDoc& d = *studio_;
    studio::StudioModel* m = d.Selected();
    if (!m) return;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(w, Dp(10.0f)));
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, Tr("표면 셰이더"));
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    }
    ShaderChoice choice = m->shader;
    const ModelTraits studioTraits = m->gpu ? m->gpu->Traits() : ModelTraits{};
    bool changed = DrawShaderSelector("##studioshader", choice, w, m->gpu ? &studioTraits : nullptr);
    if (options_.shaderPackSet) {
        PushFont(Font::Regular, size::Caption);
        ImGui::TextDisabled("%s", Tr("--shader-pack 이 이번 실행 동안 우선합니다"));
        PopFont();
    }
    const ShaderPack* pack = choice.pack.empty() ? nullptr : ShaderPacks().Find(choice.pack);
    if (pack) {
        Gap(6.0f);
        if (!pack->params.empty()) {
            if (Button("##studioshaderparams", Tr("팩 설정"), icon::Sliders, ButtonKind::Ghost))
                ImGui::OpenPopup("##studioshaderparamspopup");
            ImGui::SameLine(0, Dp(4.0f));
        }
        const int overrides = MaterialOverrideCount(choice);
        const std::string matLabel = overrides ? std::string(Tr("재질")) + "  " + std::to_string(overrides) : std::string(Tr("재질"));
        if (Button("##studioshadermats", matLabel.c_str(), icon::Palette, ButtonKind::Ghost))
            ImGui::OpenPopup("##studioshadermatspopup");
        Tooltip(Tr("재질마다 분류를 바꾸거나 팩을 끕니다"));
        ImGui::SetNextWindowSize(ImVec2(Dp(380.0f), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(14.0f), Dp(12.0f)));
        if (ImGui::BeginPopup("##studioshadermatspopup")) {
            SectionLabel(Tr("재질별 셰이딩"));
            changed |= DrawMaterialShaderList("##studiomats", choice, *m->pmx, ImGui::GetContentRegionAvail().x,
                                              ImGui::GetMainViewport()->WorkSize.y * 0.6f);
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    if (pack && !pack->params.empty()) {
        ImGui::SetNextWindowSize(ImVec2(PackParamsPopupWidth(choice), 0));
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                            ImVec2(FLT_MAX, ImGui::GetMainViewport()->WorkSize.y - Dp(24.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(16.0f), Dp(14.0f)));
        if (ImGui::BeginPopup("##studioshaderparamspopup")) {
            SectionLabel(pack->name.Get(PackLanguage()).c_str());
            changed |= DrawShaderPackParams(choice);
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }
    if (changed) {
        m->shader = choice;
        ++d.projectVersion;   // saved with the project (no undo step)
    }
}

void App::DrawStudioMaterialRow(float w) {
    studio::StudioDoc& d = *studio_;
    studio::StudioModel* m = d.Selected();
    if (!m) return;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();

    // P5: PBR material source (Default / Ray-MMD) of the selected model (character, stage or prop). Changing it
    // reloads this model's PBR data through the replace-model path (same uid / motion / placement / shader), on a
    // worker thread. A stage's key is its library id, like a character's.
    const std::string key = m->libraryId.empty() ? PathToUtf8(m->path) : m->libraryId;
    const int materialCount = (int)m->pmx->materials.size();
    if (!HasMaterialOptions(key, m->path, materialCount, m->material)) return;
    ImGui::Dummy(ImVec2(w, Dp(10.0f)));
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, Tr("재질 데이터"));
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    }
    // The Ray-MMD switch off / the Auto workflow decide the material source: show what applies, disabled.
    if (!RayMmdGlobalEnabled() || MmeAutoWorkflow()) {
        const MaterialChoice resolved = ResolveMaterialChoice(m->libraryId, m->path);
        PushFont(Font::Regular, size::Caption);
        ImGui::TextDisabled("%s", Tr("완전 자동 / Ray-MMD 꺼짐: 재질 소스가 자동으로 정해집니다"));
        PopFont();
        const std::string label = std::string(Tr("적용: ")) + (resolved.ray ? "Ray-MMD" : Tr("기본"));
        ImGui::BeginDisabled(true);
        PushFont(Font::Regular, size::Small);
        ImGui::TextUnformatted(label.c_str());
        PopFont();
        ImGui::EndDisabled();
        return;
    }
    MaterialChoice material = m->material;
    if (options_.materialSourceSet) {
        PushFont(Font::Regular, size::Caption);
        ImGui::TextDisabled("%s", Tr("--material-source 가 이번 실행 동안 우선합니다"));
        PopFont();
    }
    if (DrawMaterialSelector("##studiomat", material, key, m->path, materialCount, w)) {
        m->material = material;
        if (!m->libraryId.empty()) {   // the same model in play mode remembers it too
            settings_.SetCharacterMaterial(m->libraryId, material);
            settings_.Save(settingsPath_);
        }
        StudioAddModelFile(m->kind, m->path, m->uid, &m->material);   // reload this model's PBR data
    }
}

// M6b: the selected studio model's MME object effect choice (없음 / 모델 동봉 효과 / 파일 선택…), saved in the project.
void App::DrawStudioMmeRow(float w) {
    studio::StudioDoc& d = *studio_;
    studio::StudioModel* m = d.Selected();
    if (!m) return;
    const Palette& p = P();
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(w, Dp(10.0f)));
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        Text(cdl, Font::Semibold, size::Caption, c, p.ink3, Tr("MME"));
        ImGui::Dummy(ImVec2(w, Dp(20.0f)));
    }
    // The global switch off / the Auto workflow override the project's own choice: show what applies, disabled.
    if (!MmeGlobalEnabled() || MmeAutoWorkflow()) {
        const MmeChoice mme = ResolveMmeChoiceFor(m->mme, m->kind == studio::ModelKind::Stage, m->path);
        PushFont(Font::Regular, size::Caption);
        ImGui::TextDisabled("%s", Tr("완전 자동 / MME 꺼짐: 저장한 설정 대신 자동 효과를 씁니다"));
        PopFont();
        const std::string label = std::string(Tr("적용: ")) +
            (mme.mode == MmeChoice::Auto ? Tr("모델 동봉 효과")
             : mme.mode == MmeChoice::File ? PathToUtf8(mme.file.filename())
                                           : Tr("없음"));
        ImGui::BeginDisabled(true);
        PushFont(Font::Regular, size::Small);
        ImGui::TextUnformatted(label.c_str());
        PopFont();
        ImGui::EndDisabled();
        return;
    }
    MmeChoice mme = m->mme;
    if (DrawMmeRow("##studiomme", mme, m->libraryId, m->path, w)) {
        m->mme = mme;
        ++d.projectVersion;   // saved with the project (no undo step)
    }
}

// M6b: the ordered MME post stack (RenderSettings::mmeEffects): per entry an enable switch, up / down / remove, and
// an "MME 효과 추가" picker over the library's post effects (plus a file dialog). Shared by the play bar / shader
// manager (play-mode settings_.mmeEffects) and the studio (the project's own stack). True = the stack changed.
bool App::DrawMmePostStackEditor(std::vector<MmeEffectEntry>& stack, bool header) {
    const Palette& p = P();
    if (header) SectionLabel(Tr("MME"));
    Para(Tr("위에서 아래로 MME 효과를 차례로 적용합니다. 영상·스틸 렌더에도 같은 스택이 쓰입니다."), p.ink3, size::Caption);
    Gap(6.0f);
    bool changed = false;
    size_t swapWith = SIZE_MAX, removeAt = SIZE_MAX, swapA = 0;
    for (size_t i = 0; i < stack.size(); ++i) {
        MmeEffectEntry& e = stack[i];
        ImGui::PushID((int)i);
        bool on = e.enabled;
        const std::string name = PathToUtf8(e.file.filename());
        std::error_code ec;
        const bool exists = std::filesystem::exists(e.file, ec);
        const std::string hint = exists ? PathToUtf8(e.file.parent_path()) : std::string(Tr("파일 없음"));
        if (Switch("##on", name.c_str(), &on, hint.c_str())) {
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

    // the add picker: library post effects (files under effects/ whose kind is post)
    if (Button("##mmeadd", Tr("MME 효과 추가"), icon::Plus, ButtonKind::Secondary)) ImGui::OpenPopup("##mmeaddpopup");
    ImGui::SetNextWindowSize(ImVec2(Dp(320.0f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Dp(12.0f), Dp(12.0f)));
    if (ImGui::BeginPopup("##mmeaddpopup")) {
        int shown = 0;
        for (const EffectAsset& e : library_.effects) {
            if (e.kind != "post") continue;   // a post stack only takes post effects
            ++shown;
            ImGui::PushID(e.id.c_str());
            if (MenuItem("##mmeadd", e.displayName.c_str())) {
                // add into whichever stack this editor instance edits (play-mode settings or the studio project)
                const auto addEntry = [this](const std::filesystem::path& file) {
                    MmeEffectEntry m;
                    m.file = file;
                    m.enabled = true;
                    if (screen_ == Screen::Studio && studio_) {
                        std::vector<MmeEffectEntry> entries = StudioMmeEntries();
                        bool dup = false;
                        for (const MmeEffectEntry& x : entries) dup |= x.file == file;
                        if (!dup) {
                            entries.push_back(std::move(m));
                            studio_->mmePost.clear();
                            for (const MmeEffectEntry& x : entries)
                                studio_->mmePost.push_back(studio::MmePostEntry{x.file, x.enabled});
                            StudioMmeChanged();
                        }
                    } else {
                        bool dup = false;
                        for (const MmeEffectEntry& x : settings_.mmeEffects) dup |= x.file == file;
                        if (!dup) {
                            settings_.mmeEffects.push_back(m);
                            settings_.Save(settingsPath_);
                            ApplyRenderSettings();
                        }
                    }
                };
                if (GuardRayMme(e.path, addEntry)) {
                    ImGui::CloseCurrentPopup();
                } else {
                    MmeEffectEntry m;
                    m.file = e.path;
                    m.enabled = true;
                    stack.push_back(std::move(m));
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::PopID();
        }
        if (!shown) {
            PushFont(Font::Regular, size::Caption);
            ImGui::TextDisabled("%s", Tr("라이브러리 effects 폴더에 post 효과가 없습니다"));
            PopFont();
        }
        ImGui::Separator();
        if (MenuItem("##mmefile", Tr("파일 선택…"))) {
            const std::filesystem::path picked =
                studio::OpenFileDialog(hwnd_, {{L"MME 효과 (.fx / .x)", L"*.fx;*.x"}, {L"MME 효과", L"*.fx"}});
            if (!picked.empty()) {
                if (GuardRayMme(picked, [this](const std::filesystem::path& file) {
                        // the file dialog adds into whichever stack this editor edits (see the library branch)
                        MmeEffectEntry m;
                        m.file = file;
                        m.enabled = true;
                        if (screen_ == Screen::Studio && studio_) {
                            std::vector<MmeEffectEntry> entries = StudioMmeEntries();
                            bool dup = false;
                            for (const MmeEffectEntry& x : entries) dup |= x.file == file;
                            if (!dup) {
                                entries.push_back(std::move(m));
                                studio_->mmePost.clear();
                                for (const MmeEffectEntry& x : entries)
                                    studio_->mmePost.push_back(studio::MmePostEntry{x.file, x.enabled});
                                StudioMmeChanged();
                            }
                        } else {
                            bool dup = false;
                            for (const MmeEffectEntry& x : settings_.mmeEffects) dup |= x.file == file;
                            if (!dup) {
                                settings_.mmeEffects.push_back(m);
                                settings_.Save(settingsPath_);
                                ApplyRenderSettings();
                            }
                        }
                    })) {
                    // the confirm modal collects the add
                } else {
                    MmeEffectEntry m;
                    m.file = picked;
                    m.enabled = true;
                    stack.push_back(std::move(m));
                    changed = true;
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    if (stack.empty()) Para(Tr("MME 효과가 없습니다."), p.ink3, size::Caption);
    return changed;
}

// M6b: the studio's project MME post stack. Mirrors DrawMmePostStackEditor over studio_->mmePost.
bool App::DrawStudioMmeStackEditor() {
    if (!studio_) return false;
    std::vector<MmeEffectEntry> entries = StudioMmeEntries();
    const bool changed = DrawMmePostStackEditor(entries);
    if (changed) {
        studio_->mmePost.clear();
        for (MmeEffectEntry& e : entries) studio_->mmePost.push_back(studio::MmePostEntry{e.file, e.enabled});
        StudioMmeChanged();
    }
    return changed;
}

} // namespace mmdx
