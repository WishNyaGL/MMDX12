// Worker-thread part of opening a scene in the Studio. Separate from StudioDoc.cpp because
// asset/ModelImport.h and render/GpuModel.h both declare mmdx::ModelRole.
#include "studio/StudioDoc.h"
#include "app/SceneLoader.h"
#include "asset/AssetLibrary.h"
#include "asset/MmeHints.h"
#include "asset/ModelImport.h"
#include "asset/VmdMotion.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/TextUtil.h"

namespace mmdx::studio {

namespace {
// The model's default shader-pack texture folder: the directory of the face texture the model's own MME files
// reference (e.g. a face SDF a pack samples). Never guesses names: only the asset's references are followed.
std::filesystem::path ModelPackTextureFolder(const std::filesystem::path& modelPath) {
    const MmeHints hints = ReadMmeHints(modelPath);
    for (const MmeTexRef& t : hints.textures)
        if (ToLowerAscii(t.variable).find("face") != std::string::npos) return t.file.parent_path();
    return {};
}

bool LoadModel(const std::filesystem::path& path, ModelRole role, const std::string& label, LoadProgress* progress,
               float f0, float f1, StudioPackageModel& out, std::string* error, const MaterialChoice& choice = {}) {
    LoadedModelCpu cpu;
    cpu.pmx = std::make_shared<PmxModel>();
    if (!LoadModelFile(path, role, *cpu.pmx, error)) return false;
    DecodeModelTextures(cpu, progress, f0, f1, label.c_str(), choice);
    out.pmx = cpu.pmx;
    out.textures = std::move(cpu.textures);
    out.pbr = std::move(cpu.pbr);
    out.material = choice;   // the choice the PBR data was built with (the inspector / project show it)
    out.name = label;
    if (role == ModelRole::Character) out.defaultPackTextureFolder = ModelPackTextureFolder(path);
    return true;
}
} // namespace

bool LoadStudioPackage(const CharacterAsset& character, const StageAsset* stage, const SongAsset* song,
                       StudioPackage& out, LoadProgress* progress, std::string* error,
                       const MaterialChoiceFn& materialChoice) {
    const auto choiceFor = [&](const std::string& id) {
        return materialChoice ? materialChoice(id) : MaterialChoice{};
    };
    try {
        out = StudioPackage{};
        if (progress) progress->fraction.store(0.0f, std::memory_order_relaxed);

        if (stage) {
            const size_t n = stage->parts.size();
            for (size_t k = 0; k < n; ++k) {
                const std::string label =
                    stage->displayName + (n > 1 ? " " + std::to_string(k + 1) + "/" + std::to_string(n) : "");
                if (progress) progress->SetStatus(Tr("스테이지 로드: ") + label);
                StudioPackageModel part;
                part.kind = ModelKind::Stage;
                part.libraryId = stage->id;   // P5: a stage's material choice is keyed by its library id
                std::string err;
                const float f0 = 0.05f + 0.35f * (float)k / (float)n, f1 = 0.05f + 0.35f * (float)(k + 1) / (float)n;
                if (!LoadModel(stage->parts[k], ModelRole::Stage, label, progress, f0, f1, part, &err, choiceFor(stage->id))) {
                    LOG_WARN("studio: stage part load failed: %s: %s", PathToUtf8(stage->parts[k]).c_str(), err.c_str());
                    continue;
                }
                out.models.push_back(std::move(part));
            }
        }

        if (progress) progress->SetStatus(Tr("캐릭터 로드: ") + character.displayName);
        StudioPackageModel ch;
        ch.libraryId = character.id;
        std::string err;
        if (!LoadModel(character.modelPath, ModelRole::Character, character.displayName, progress, 0.4f, 0.85f, ch, &err,
                       choiceFor(character.id))) {
            if (error) *error = Tr("캐릭터를 불러오지 못했습니다: ") + err;
            return false;
        }

        if (song) {
            if (progress) progress->SetStatus(Tr("모션 로드"));
            MotionData dance, camera;
            LoadStudioSong(*song, dance, camera, nullptr);
            ch.motion = std::move(dance);
            if (!song->cameraVmd.empty()) {
                out.camera.camera = std::move(camera.camera);
                out.camera.light = std::move(camera.light);
                out.camera.shadow = std::move(camera.shadow);
                out.camera.modelName = camera.modelName;
            }
            out.audioPath = song->audioPath;
        }
        ch.motion.CanonicalizeNames(*ch.pmx);
        if (ch.motion.modelName.empty()) ch.motion.modelName = ch.pmx->name;
        if (progress) progress->fraction.store(1.0f, std::memory_order_relaxed);
        out.models.push_back(std::move(ch));
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}



bool LoadStudioModel(const std::filesystem::path& path, ModelKind kind, const std::string& label,
                     StudioPackageModel& out, LoadProgress* progress, std::string* error, const MaterialChoice& choice) {
    try {
        out = StudioPackageModel{};
        if (progress) progress->SetStatus(Tr("모델 로드: ") + label);
        if (!std::filesystem::exists(path)) {
            if (error) *error = Tr("모델 파일이 없습니다: ") + PathToUtf8(path);
            return false;
        }
        const ModelRole role = kind == ModelKind::Character ? ModelRole::Character
                               : kind == ModelKind::Prop    ? ModelRole::Prop
                                                            : ModelRole::Stage;
        std::string err;
        if (!LoadModel(path, role, label, progress, 0.05f, 0.95f, out, &err, choice)) {
            if (error) *error = Tr("모델을 불러오지 못했습니다: ") + err;
            return false;
        }
        out.kind = kind;
        out.name = label;
        if (progress) progress->fraction.store(1.0f, std::memory_order_relaxed);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool LoadStudioStage(const StageAsset& stage, std::vector<StudioPackageModel>& out, LoadProgress* progress,
                     std::string* error, const MaterialChoice& choice) {
    try {
        out.clear();
        const size_t n = stage.parts.size();
        std::string lastErr;
        for (size_t k = 0; k < n; ++k) {
            const std::string label =
                stage.displayName + (n > 1 ? " " + std::to_string(k + 1) + "/" + std::to_string(n) : "");
            if (progress) progress->SetStatus(Tr("스테이지 로드: ") + label);
            StudioPackageModel part;
            part.kind = ModelKind::Stage;
            part.libraryId = stage.id;   // P5: a stage's material choice is keyed by its library id
            std::string err;
            const float f0 = 0.05f + 0.9f * (float)k / (float)n;
            const float f1 = 0.05f + 0.9f * (float)(k + 1) / (float)n;
            if (!LoadModel(stage.parts[k], ModelRole::Stage, label, progress, f0, f1, part, &err, choice)) {
                LOG_WARN("studio: stage part load failed: %s: %s", PathToUtf8(stage.parts[k]).c_str(), err.c_str());
                lastErr = err;
                continue;
            }
            out.push_back(std::move(part));
        }
        if (out.empty() && n > 0) {
            if (error) *error = Tr("모델을 불러오지 못했습니다: ") + lastErr;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool LoadStudioSong(const SongAsset& song, MotionData& dance, MotionData& camera, std::string* error) {
    try {
        dance = {};
        camera = {};
        std::vector<std::filesystem::path> files;
        if (!song.danceVmd.empty()) files.push_back(song.danceVmd);
        files.insert(files.end(), song.extraVmds.begin(), song.extraVmds.end());
        std::string lastErr;
        int loaded = 0;
        int attempted = 0;
        for (const auto& f : files) {
            attempted++;
            VmdMotion vmd;
            std::string vmdErr;
            if (!LoadVmd(f, vmd, &vmdErr)) {
                LOG_WARN("studio: motion skipped: %s: %s", PathToUtf8(f).c_str(), vmdErr.c_str());
                lastErr = vmdErr;
                continue;
            }
            loaded++;
            MotionData d = MotionData::FromVmd(vmd);
            d.camera.clear();
            d.light.clear();
            d.shadow.clear();
            if (dance.Empty()) dance.modelName = d.modelName;
            dance.Merge(d);
        }
        if (!song.cameraVmd.empty()) {
            attempted++;
            VmdMotion cam;
            std::string camErr;
            if (LoadVmd(song.cameraVmd, cam, &camErr)) {
                loaded++;
                MotionData d = MotionData::FromVmd(cam);
                camera.camera = std::move(d.camera);
                camera.light = std::move(d.light);
                // a light track that only repeats MMD's defaults says nothing (and would override the lighting)
                if (IsDefaultLightTrack(camera.light)) camera.light.clear();
                camera.shadow = std::move(d.shadow);
                camera.modelName = d.modelName;
            } else {
                LOG_WARN("studio: camera skipped: %s: %s", PathToUtf8(song.cameraVmd).c_str(), camErr.c_str());
                lastErr = camErr;
            }
        }
        if (loaded == 0 && attempted > 0) {
            if (error) *error = std::string(Tr("모션 로드")) + ": " + lastErr;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool LoadStudioProjectPackage(const std::filesystem::path& file, bool recovery, StudioPackage& out,
                              LoadProgress* progress, std::string* error) {
    try {
        out = StudioPackage{};
        if (progress) progress->SetStatus(Tr("프로젝트 로드"));
        ProjectData pd;
        std::vector<std::string> warn;
        std::string err;
        if (!LoadProject(file, pd, &err, &warn)) {
            if (error) *error = Tr("프로젝트를 열 수 없습니다: ") + err;
            return false;
        }
        out.fromProject = true;
        out.editor = pd.editor;
        out.camera = pd.camera;
        out.audioPath = pd.audioPath;
        out.audioOffset = pd.audioOffset;
        out.warnings = warn;
        out.recovered = recovery;
        out.projectPath = recovery ? pd.recoveryOf : file;

        const size_t n = pd.models.size();
        std::vector<int> remap(n, -1);
        for (size_t i = 0; i < n; ++i) {
            auto& pm = pd.models[i];
            const float f0 = 0.05f + 0.9f * (float)i / (float)n;
            const float f1 = 0.05f + 0.9f * (float)(i + 1) / (float)n;
            if (progress) progress->SetStatus(Tr("모델 로드: ") + pm.name);
            
            StudioPackageModel m;
            const ModelRole role = pm.kind == ModelKind::Character ? ModelRole::Character
                                  : pm.kind == ModelKind::Prop    ? ModelRole::Prop
                                                                  : ModelRole::Stage;
            std::string merr;
            if (!LoadModel(pm.path, role, pm.name, progress, f0, f1, m, &merr, pm.material)) {
                out.warnings.push_back(Tr("모델을 불러오지 못했습니다: ") + pm.name + " (" + PathToUtf8(pm.path) + ")");
                continue;
            }
            m.name = pm.name;
            m.libraryId = pm.libraryId;
            m.kind = pm.kind;
            m.visible = pm.visible;
            m.physicsOff = pm.physicsOff;
            m.attach = pm.attach;
            m.place = pm.place;
            m.shader = pm.shader;
            m.material = pm.material;
            m.motion = std::move(pm.motion);
            m.motion.CanonicalizeNames(*m.pmx);
            if (m.motion.modelName.empty()) m.motion.modelName = m.pmx->name;
            
            remap[i] = (int)out.models.size();
            out.models.push_back(std::move(m));
        }

        for (auto& m : out.models) {
            if (m.kind == ModelKind::Prop) {
                int parent = m.attach.parent;
                m.attach.parent = (parent >= 0 && parent < (int)remap.size()) ? remap[parent] : -1;
                // accessory keys keep "1 + the package model index" (0 = the world), like the project's attach
                for (AccessoryKf& k : m.motion.accessory) {
                    if (k.parentUid == 0) continue;
                    const size_t pi = k.parentUid - 1;
                    k.parentUid = pi < remap.size() && remap[pi] >= 0 ? (uint32_t)remap[pi] + 1 : 0;
                }
            } else {
                m.attach = PropAttach{};
            }
        }

        if (out.editor.selectedModel >= 0 && out.editor.selectedModel < (int)remap.size()) {
            out.editor.selectedModel = remap[out.editor.selectedModel];
        } else {
            out.editor.selectedModel = -1;
        }

        // spot targets: 1 + the project's model index -> 1 + the package's (a model that failed to load: the performer)
        for (SceneLight& l : out.editor.lights) {
            if (l.targetUid == 0) continue;
            const size_t pi = l.targetUid - 1;
            l.targetUid = pi < remap.size() && remap[pi] >= 0 ? (uint32_t)remap[pi] + 1 : 0;
        }

        LOG_INFO("studio: project %s: %d models, %d warnings", PathToUtf8(file).c_str(), (int)out.models.size(), (int)out.warnings.size());
        if (progress) progress->fraction.store(1.0f, std::memory_order_relaxed);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// PMM (MMD project) import
// ---------------------------------------------------------------------------

namespace {

// Splits a PMM document into one VMD per model (bones / morphs / IK) written next to the .pmm, plus
// the camera track (camera / light / self-shadow). Returns the per-model VMD paths (empty for a model
// with no keys). The VMD round trip is how PMM keys reach the studio, so the interpolation block, the
// physics flag and the IK keys follow the exact semantics asset/VmdWriter.cpp implements.
std::vector<std::filesystem::path> WritePmmVmdFiles(const std::filesystem::path& pmmFile, const PmmDocument& doc,
                                                    const std::filesystem::path& scratchDir, MotionData& cameraOut,
                                                    std::string* error) {
    std::vector<std::filesystem::path> paths;
    paths.reserve(doc.models.size());
    const auto dir = scratchDir;
    const auto stem = pmmFile.stem();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const auto boneInterp = [](const uint8_t pmm[16], uint8_t vmd[64]) {
        // PMM channel c {x1,y1,x2,y2} -> VMD channel table {x1,x2,y1,y2} (SetBoneCurve rewrites all rows).
        FillLinearInterp(vmd);
        for (int c = 0; c < 4; ++c) {
            const uint8_t curve[4] = {pmm[c * 4 + 0], pmm[c * 4 + 2], pmm[c * 4 + 1], pmm[c * 4 + 3]};
            SetBoneCurve(vmd, c, curve);
        }
    };
    const auto cameraInterp = [](const uint8_t pmm[24], uint8_t vmd[24]) {
        FillLinearCameraInterp(vmd);
        for (int p = 0; p < 6; ++p) {
            const uint8_t curve[4] = {pmm[p * 4 + 0], pmm[p * 4 + 2], pmm[p * 4 + 1], pmm[p * 4 + 3]};
            SetCameraCurve(vmd, p, curve);
        }
    };

    for (size_t mi = 0; mi < doc.models.size(); ++mi) {
        const PmmModel& m = doc.models[mi];
        VmdMotion vmd;
        vmd.modelName = m.nameJa.empty() ? m.nameEn : m.nameJa;
        for (const auto& [name, keys] : m.bones) {
            for (const PmmBoneKey& k : keys) {
                VmdBoneKey o;
                o.boneName = name;
                o.frame = (uint32_t)std::max(0, k.frame);
                o.translation = k.translation;
                o.rotation = k.rotation;
                boneInterp(k.interp, o.interp);
                vmd.boneKeys.push_back(std::move(o));
                vmd.maxFrame = std::max(vmd.maxFrame, o.frame);
            }
        }
        for (const auto& [name, keys] : m.morphs) {
            for (const PmmMorphKey& k : keys) {
                VmdMorphKey o;
                o.morphName = name;
                o.frame = (uint32_t)std::max(0, k.frame);
                o.weight = k.weight;
                vmd.morphKeys.push_back(std::move(o));
                vmd.maxFrame = std::max(vmd.maxFrame, o.frame);
            }
        }
        // IK keys: one VMD record per frame that changes any IK bone state, listing every IK bone.
        {
            std::vector<std::pair<std::string, std::vector<IkKf>>> tracks;
            for (size_t bi = 0; bi < m.ikBones.size() && bi < m.boneNames.size(); ++bi) {
                const int bone = m.ikBones[bi];
                if (bone < 0 || bone >= (int)m.boneNames.size()) continue;
                std::vector<IkKf> keys;
                for (const PmmModelKey& mk : m.modelKeys)
                    if (bi < mk.ikEnabled.size()) keys.push_back(IkKf{mk.frame, mk.ikEnabled[bi] != 0});
                if (!keys.empty()) tracks.emplace_back(m.boneNames[(size_t)bone], std::move(keys));
            }
            for (const auto& [name, keys] : tracks) {
                for (const IkKf& k : keys) {
                    VmdIkKey o;
                    o.frame = (uint32_t)std::max(0, k.frame);
                    o.visible = true;
                    for (const auto& [n2, ks] : tracks) {
                        bool enabled = true;
                        for (const IkKf& kk : ks) { if (kk.frame <= k.frame) enabled = kk.enabled; else break; }
                        o.ikStates.emplace_back(n2, enabled);
                    }
                    vmd.ikKeys.push_back(std::move(o));
                    vmd.maxFrame = std::max(vmd.maxFrame, o.frame);
                }
            }
        }
        std::filesystem::path p;
        if (!vmd.boneKeys.empty() || !vmd.morphKeys.empty() || !vmd.ikKeys.empty()) {
            p = dir / (stem.string() + " - " + SanitizeFileName(vmd.modelName) + ".vmd");
            if (!SaveVmd(p, vmd)) {   // SaveVmd returns void; report on reload below
                LOG_WARN("studio: pmm model motion write failed: %s", PathToUtf8(p).c_str());
            }
        }
        paths.push_back(p);
    }

    // Camera / light / self-shadow.
    {
        VmdMotion cam;
        cam.modelName = "カメラ・照明";
        for (const PmmCameraKey& k : doc.cameraKeys) {
            VmdCameraKey o;
            o.frame = (uint32_t)std::max(0, k.frame);
            o.distance = k.distance;
            o.target = k.target;
            o.rotation = k.rotation;
            cameraInterp(k.interp, o.interp);
            o.fovDeg = k.fovDeg;
            o.perspective = k.perspective;
            cam.cameraKeys.push_back(std::move(o));
            cam.maxFrame = std::max(cam.maxFrame, o.frame);
        }
        for (const PmmLightKey& k : doc.lightKeys)
            cam.lightKeys.push_back({(uint32_t)std::max(0, k.frame), k.color, k.direction});
        for (const PmmShadowKey& k : doc.shadowKeys)
            cam.shadowKeys.push_back({(uint32_t)std::max(0, k.frame), k.mode, k.distance});
        cameraOut = MotionData::FromVmd(cam);
    }
    (void)error;
    return paths;
}

// A short label ("stem") for an accessory / its file.
std::string AccessoryLabel(const std::string& name, const std::filesystem::path& path) {
    if (!name.empty()) return name;
    return PathToUtf8(path.stem());
}

} // namespace

bool LoadStudioPmmPackage(const std::filesystem::path& file, const std::vector<CharacterAsset>& characters,
                          const std::vector<StageAsset>& stages, StudioPackage& out, LoadProgress* progress,
                          std::string* error) {
    try {
        out = StudioPackage{};
        out.fromProject = false;
        out.fromPmm = true;
        if (progress) progress->SetStatus(Tr("PMM 프로젝트 로드"));

        PmmDocument doc;
        std::string err;
        if (!LoadPmm(file, doc, &err)) {
            if (error) *error = Tr("PMM 파일을 열 수 없습니다: ") + err;
            return false;
        }

        MotionData camera;
        // PMM keys are split into temporary VMDs (never into the PMM's own folder: library/ is read-only
        // and a PMM may live anywhere) and re-read, so their semantics go through the VMD code.
        const std::filesystem::path scratch = std::filesystem::temp_directory_path() / "mmdx12_pmm_import";
        std::error_code ecScratch;
        std::filesystem::remove_all(scratch, ecScratch);
        const std::vector<std::filesystem::path> vmdPaths = WritePmmVmdFiles(file, doc, scratch, camera, nullptr);
        out.camera = std::move(camera);
        if (IsDefaultLightTrack(out.camera.light)) out.camera.light.clear();
        if (IsDefaultShadowTrack(out.camera.shadow)) out.camera.shadow.clear();

        const auto pmmDir = file.parent_path();
        const auto resolve = [&](const std::filesystem::path& stored) -> std::filesystem::path {
            std::error_code ec;
            if (stored.empty()) return {};
            if (stored.is_absolute()) {
                if (std::filesystem::exists(stored, ec)) return stored;
            } else {
                const std::filesystem::path rel = pmmDir / stored;
                if (std::filesystem::exists(rel, ec)) return std::filesystem::absolute(rel);
            }
            const std::string want = ToLowerAscii(PathToUtf8(stored.filename()));
            for (const CharacterAsset& c : characters)
                if (ToLowerAscii(PathToUtf8(c.modelPath.filename())) == want) return c.modelPath;
            for (const StageAsset& s : stages)
                for (const auto& p : s.parts)
                    if (ToLowerAscii(PathToUtf8(p.filename())) == want) return p;
            return {};
        };

        const PmmEffectSettings effects = MapMmeControllers(doc);
        out.pmmEffects = effects;

        const size_t modelCount = doc.models.size();
        // The PMM model index -> package index (a model that fails to load is skipped; prop parents remap).
        std::vector<int> remap(modelCount, -1);
        for (size_t i = 0; i < modelCount; ++i) {
            const PmmModel& m = doc.models[i];
            const float f0 = 0.05f + 0.9f * (float)i / (float)std::max<size_t>(1, modelCount);
            const float f1 = 0.05f + 0.9f * (float)(i + 1) / (float)std::max<size_t>(1, modelCount);
            if (progress) progress->SetStatus(Tr("모델 로드: ") + (m.nameJa.empty() ? m.nameEn : m.nameJa));

            const std::filesystem::path resolved = resolve(m.path);
            if (resolved.empty()) {
                out.warnings.push_back(Tr("모델을 찾을 수 없습니다: ") + PathToUtf8(m.path));
                continue;
            }
            StudioPackageModel pm;
            const ModelRole role = ModelRole::Character;   // PMM models are characters
            std::string merr;
            const MaterialChoice choice;                   // PMM says nothing about PBR material source
            if (!LoadModel(resolved, role, m.nameJa.empty() ? m.nameEn : m.nameJa, progress, f0, f1, pm, &merr, choice)) {
                out.warnings.push_back(Tr("모델을 불러오지 못했습니다: ") + PathToUtf8(resolved));
                continue;
            }
            pm.name = m.nameJa.empty() ? PathToUtf8(resolved.stem()) : m.nameJa;
            pm.visible = m.visible;
            // The model's motion: re-read the VMD written next to the .pmm (the studio VMD path).
            if (i < vmdPaths.size() && !vmdPaths[i].empty()) {
                VmdMotion vmd;
                std::string verr;
                if (LoadVmd(vmdPaths[i], vmd, &verr)) {
                    MotionData d = MotionData::FromVmd(vmd);
                    d.camera.clear();
                    d.light.clear();
                    d.shadow.clear();
                    d.modelName = pm.pmx->name;
                    pm.motion = std::move(d);
                }
            }
            remap[i] = (int)out.models.size();
            out.models.push_back(std::move(pm));
        }

        // Accessories -> props (accessory key track). Controllers stay out.
        for (const PmmAccessory& acc : doc.accessories) {
            std::string matched;
            const MmeController c = ClassifyMmeController(PathToUtf8(acc.path.filename()), &matched);
            if (c != MmeController::None) continue;
            if (LooksLikeUnknownMmeController(acc.path)) continue;   // hidden, listed below
            const std::filesystem::path resolved = resolve(acc.path);
            if (resolved.empty()) {
                out.warnings.push_back(Tr("소품을 찾을 수 없습니다: ") + PathToUtf8(acc.path));
                continue;
            }
            StudioPackageModel pm;
            std::string merr;
            if (!LoadStudioModel(resolved, ModelKind::Prop, AccessoryLabel(acc.name, acc.path), pm, progress, &merr)) {
                out.warnings.push_back(Tr("소품을 불러오지 못했습니다: ") + PathToUtf8(resolved));
                continue;
            }
            // PMM accessory parent is a model index; the package wants 1 + the package model index (0 = world).
            const auto parentToPackage = [&](int pmmParent) -> uint32_t {
                if (pmmParent < 0 || pmmParent >= (int)remap.size()) return 0;
                return remap[(size_t)pmmParent] >= 0 ? (uint32_t)remap[(size_t)pmmParent] + 1 : 0;
            };
            const auto boneNameOf = [&](int pmmParent, int bone) -> std::string {
                if (pmmParent < 0 || pmmParent >= (int)doc.models.size()) return {};
                if (bone < 0 || bone >= (int)doc.models[(size_t)pmmParent].boneNames.size()) return {};
                return doc.models[(size_t)pmmParent].boneNames[(size_t)bone];
            };
            for (const PmmAccessoryKey& k : acc.keys) {
                AccessoryKf o;
                o.frame = std::max(0, k.frame);
                o.translation = k.translation;
                o.rotationDeg = k.rotationDeg;
                o.scale = k.scale;
                o.opacity = std::clamp(k.opacity, 0.0f, 1.0f);
                o.visible = k.visible;
                o.parentUid = parentToPackage(k.parentModel);
                o.bone = boneNameOf(k.parentModel, k.parentBone);
                pm.motion.accessory.push_back(std::move(o));
            }
            // The static attach fallback (no-key placement / outliner preview) = the first key.
            if (!pm.motion.accessory.empty()) {
                const AccessoryKf& k0 = pm.motion.accessory.front();
                pm.attach.parent = k0.parentUid == 0 ? -1 : (int)k0.parentUid - 1;
                pm.attach.bone = k0.bone;
                pm.attach.translation = k0.translation;
                pm.attach.rotationDeg = k0.rotationDeg;
                pm.attach.scale = k0.scale;
            }
            out.models.push_back(std::move(pm));
        }

        for (const std::string& c : effects.unsupported)
            out.warnings.push_back(Tr("MME 효과(지원 안 함): ") + c);

        // Audio: PMM stores an absolute path; resolve it the same way (PMM folder first).
        if (!doc.audioPath.empty()) {
            std::error_code ec;
            std::filesystem::path audio = doc.audioPath;
            if (!audio.is_absolute() || !std::filesystem::exists(audio, ec)) {
                const std::filesystem::path rel = pmmDir / audio.filename();
                if (std::filesystem::exists(rel, ec)) audio = std::filesystem::absolute(rel);
            }
            if (std::filesystem::exists(audio, ec)) out.audioPath = audio;
            else out.warnings.push_back(Tr("음원을 찾을 수 없습니다: ") + PathToUtf8(doc.audioPath));
        }

        // Editor defaults: frame range (from PMM if present); the aspect / fps come from the PMM header.
        if (doc.hasRange) {
            out.editor.rangeStart = doc.rangeStart;
            out.editor.rangeEnd = doc.rangeEnd;
        }
        out.pmmWidth = doc.outputWidth;
        out.pmmHeight = doc.outputHeight;
        out.pmmFps = doc.preferredFps;
        out.pmmSourcePath = std::filesystem::absolute(file);
        out.editor.useMotionCamera = !out.camera.camera.empty();
        for (const std::string& w : doc.warnings) out.warnings.push_back(w);

        if (out.models.empty() && out.camera.camera.empty()) {
            if (error) *error = Tr("PMM에서 불러올 수 있는 장면이 없습니다");
            return false;
        }
        // Every model unresolved: keep the camera scene, but say so.
        LOG_INFO("studio: pmm %s: %d models, %d accessories, %d warnings", PathToUtf8(file).c_str(),
                 (int)out.models.size(), (int)doc.accessories.size(), (int)out.warnings.size());
        if (progress) progress->fraction.store(1.0f, std::memory_order_relaxed);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

} // namespace mmdx::studio
