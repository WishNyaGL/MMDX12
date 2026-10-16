#include "core/I18n.h"
#include "app/SceneLoader.h"

#include <algorithm>
#include <execution>
#include <mutex>
#include <set>

#include "anim/ModelInstance.h"
#include "anim/Motion.h"
#include "asset/ImageLoader.h"
#include "asset/ModelImport.h"
#include "asset/MmeHints.h"
#include "asset/VmdMotion.h"
#include "core/Log.h"
#include "core/TextUtil.h"

namespace mmdx {

// Decodes all textures referenced by the model's materials in parallel.
void DecodeModelTextures(LoadedModelCpu& m, LoadProgress* progress, float fracBegin, float fracEnd,
                         const char* label, const MaterialChoice& choice) {
    m.textures.resize(m.pmx->textures.size());

    // Referenced texture indices; only indices within range.
    std::set<int32_t> referenced;
    for (const PmxMaterial& mat : m.pmx->materials) {
        if (mat.textureIndex >= 0 && (size_t)mat.textureIndex < m.pmx->textures.size())
            referenced.insert(mat.textureIndex);
        if ((mat.sphereMode == PmxSphereMode::Multiply || mat.sphereMode == PmxSphereMode::Add) &&
            mat.sphereTextureIndex >= 0 && (size_t)mat.sphereTextureIndex < m.pmx->textures.size())
            referenced.insert(mat.sphereTextureIndex);
        if (!mat.sharedToon && mat.toonIndex >= 0 && (size_t)mat.toonIndex < m.pmx->textures.size())
            referenced.insert(mat.toonIndex);
    }

    if (progress) progress->SetStatus(std::string(Tr("텍스처 디코딩: ")) + label);

    std::atomic<int> done{0};
    const int total = (int)referenced.size();
    // Imported textures of opaque materials (PmxModel::opaqueTextures): alpha is not coverage.
    auto forceOpaque = [&](int32_t index) {
        if ((size_t)index >= m.pmx->opaqueTextures.size() || !m.pmx->opaqueTextures[index]) return;
        ImageRGBA8& img = m.textures[index];
        if (!img.hasAlpha) return;
        for (ImageRGBA8::Level& level : img.mips)
            for (size_t i = 3; i < level.pixels.size(); i += 4) level.pixels[i] = 255;
        img.hasAlpha = false;
    };
    std::for_each(std::execution::par, referenced.begin(), referenced.end(),
                  [&](int32_t index) {
                      if ((size_t)index < m.pmx->embeddedTextures.size() && !m.pmx->embeddedTextures[index].empty()) {
                          const auto& bytes = m.pmx->embeddedTextures[index];
                          std::string err;
                          if (!LoadImageRGBA8FromMemory(bytes.data(), bytes.size(), m.textures[index], &err))
                              LOG_WARN("embedded texture decode failed: %s (%s): %s", m.pmx->textures[index].c_str(), label, err.c_str());
                          forceOpaque(index);
                          int d = ++done;
                          if (progress && total > 0)
                              progress->fraction.store(fracBegin + (fracEnd - fracBegin) * (float)d / (float)total,
                                                       std::memory_order_relaxed);
                          return;
                      }
                      const std::filesystem::path p = m.pmx->ResolveTexturePath(index);
                      if (p.empty()) {
                          LOG_WARN("texture not found: %s (%s)", m.pmx->textures[index].c_str(), label);
                      } else {
                          std::string err;
                          if (!LoadImageRGBA8(p, m.textures[index], &err))
                              LOG_WARN("texture decode failed: %s: %s", PathToUtf8(p).c_str(), err.c_str());
                          forceOpaque(index);
                      }
                      int d = ++done;
                      if (progress && total > 0)
                          progress->fraction.store(fracBegin + (fracEnd - fracBegin) * (float)d / (float)total,
                                                   std::memory_order_relaxed);
                  });
    if (choice.ray) {
        // P6: an imported model (glTF/GLB/VRM/FBX) carries its PBR data on PmxModel::importedPbr; PMX/PMD keep the
        // Ray .emd path. `choice.ray` means "use the model's own PBR material data".
        if (!m.pmx->importedPbr.empty())
            BuildImportedPbrModelData(*m.pmx, m.pbr);
        else
            BuildPbrModelData(m.pmx->sourcePath, m.pmx->materials, m.pmx->name, m.pbr,
                              choice.emd.empty() ? std::filesystem::path() : Utf8ToPath(choice.emd));
    }
}

// The model's MME hints: the default shader-pack texture folder (the directory of the face texture its own MME
// files reference) and the per-material blend hints from its .emd. Dependency-free: only the references the asset
// files carry are followed.
struct ModelMmeHints {
    std::filesystem::path packTextureFolder;
    std::vector<uint8_t> materialBlend;
    uint8_t materialBlendDefault = 0;
};
static ModelMmeHints ModelHints(const std::filesystem::path& modelPath) {
    const MmeHints hints = ReadMmeHints(modelPath);
    ModelMmeHints out;
    for (const MmeTexRef& t : hints.textures)
        if (ToLowerAscii(t.variable).find("face") != std::string::npos) {
            out.packTextureFolder = t.file.parent_path();
            break;
        }
    out.materialBlend = hints.materialBlend;
    out.materialBlendDefault = hints.materialBlendDefault;
    return out;
}

// M8a: binds a song's stage motion VMDs (song.stageVmds, falling back to song.stageVmd) to the loaded stage parts
// whose model bones / morphs they match, one VMD per part. The stage models animate on the song clock like a
// character (no physics, no IK surprises: the normal pose path).
static bool StageBoneNameMatches(const std::string& keyed, const std::set<std::string>& modelBones) {
    if (modelBones.count(keyed)) return true;
    if (keyed.size() < 15) return false;   // VMD bone names are truncated to 15 bytes
    for (const auto& b : modelBones)
        if (b.size() > keyed.size() && b.compare(0, keyed.size(), keyed) == 0) return true;
    return false;
}

// The trailing digit run of `s` ("Stage12" -> "12", "Stage.pmx" -> "").
static std::string TrailingDigits(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && s[end - 1] >= '0' && s[end - 1] <= '9') --end;
    return s.substr(end);
}

static std::string LowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static void LoadStageMotion(const SongAsset& song, ScenePackage& out) {
    if (out.stageParts.empty()) return;
    std::vector<std::filesystem::path> vmds = song.stageVmds;
    if (vmds.empty() && !song.stageVmd.empty()) vmds.push_back(song.stageVmd);
    if (vmds.empty()) return;

    // One score per (vmd, part): the share of the VMD's keyed bone/morph names that exist on the part.
    struct Candidate {
        int vmd, part;
        double score;
        int nameRank;  // 2 = header model name equals the part stem, 1 = shares digits / prefix, 0 = none
    };
    std::vector<VmdMotion> loaded(vmds.size());
    std::vector<std::string> headerNames(vmds.size());
    for (size_t v = 0; v < vmds.size(); ++v) {
        std::string err;
        if (!LoadVmd(vmds[v], loaded[v], &err)) {
            LOG_WARN("stage motion load failed: %s: %s", PathToUtf8(vmds[v]).c_str(), err.c_str());
        } else {
            headerNames[v] = loaded[v].modelName;
        }
    }
    std::vector<Candidate> candidates;
    for (size_t v = 0; v < vmds.size(); ++v) {
        if (loaded[v].sourcePath.empty() && loaded[v].boneKeys.empty() && loaded[v].morphKeys.empty() &&
            loaded[v].ikKeys.empty())
            continue;   // failed to load
        std::set<std::string> keyedBones, keyedMorphs;
        for (const auto& bf : loaded[v].boneKeys) keyedBones.insert(bf.boneName);
        for (const auto& mf : loaded[v].morphKeys) keyedMorphs.insert(mf.morphName);
        const size_t keyed = keyedBones.size() + keyedMorphs.size();
        for (size_t i = 0; i < out.stageParts.size(); ++i) {
            const PmxModel& pmx = *out.stageParts[i].pmx;
            if (pmx.bones.empty() && pmx.morphs.empty()) continue;
            std::set<std::string> bones, morphs;
            for (const PmxBone& b : pmx.bones) bones.insert(b.name);
            for (const PmxMorph& mo : pmx.morphs) morphs.insert(mo.name);
            if (keyed == 0) continue;
            size_t matched = 0;
            for (const auto& n : keyedBones) if (StageBoneNameMatches(n, bones)) ++matched;
            for (const auto& n : keyedMorphs) if (StageBoneNameMatches(n, morphs)) ++matched;
            const double score = (double)matched / (double)keyed;
            int nameRank = 0;
            if (!headerNames[v].empty() && !pmx.sourcePath.empty()) {
                const std::string model = LowerAscii(headerNames[v]);
                const std::string stem = LowerAscii(PathToUtf8(pmx.sourcePath.stem()));
                const std::string digits = TrailingDigits(stem);
                if (model == stem) nameRank = 2;
                else if ((!digits.empty() && TrailingDigits(model) == digits) ||
                         model.rfind(stem, 0) == 0 || stem.rfind(model, 0) == 0)
                    nameRank = 1;
            }
            candidates.push_back({(int)v, (int)i, score, nameRank});
        }
    }
    // Best score first; ties by the header-name match, then by VMD file order, then part order. A part and a VMD each
    // take at most one binding.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.nameRank != b.nameRank) return a.nameRank > b.nameRank;
        if (a.vmd != b.vmd) return a.vmd < b.vmd;
        return a.part < b.part;
    });
    std::vector<bool> vmdUsed(vmds.size(), false), partUsed(out.stageParts.size(), false), vmdReported(vmds.size(), false);
    for (const Candidate& c : candidates) {
        if (vmdUsed[(size_t)c.vmd] || partUsed[(size_t)c.part]) continue;
        if (c.score < 0.5) {
            // Best candidate for this VMD falls below the threshold: report once, never bind.
            if (!vmdReported[(size_t)c.vmd]) {
                LOG_INFO("stage motion not bound: %s (best score %.2f below 0.5)",
                         PathToUtf8(vmds[(size_t)c.vmd]).c_str(), c.score);
                vmdReported[(size_t)c.vmd] = true;
            }
            continue;
        }
        const std::vector<const VmdMotion*> layers = {&loaded[(size_t)c.vmd]};
        StageMotionBinding binding;
        binding.motion = BoundMotion::Bind(*out.stageParts[(size_t)c.part].pmx, layers);
        binding.part = c.part;
        out.stageMotions.push_back(std::move(binding));
        vmdUsed[(size_t)c.vmd] = true;
        partUsed[(size_t)c.part] = true;
        LOG_INFO("stage motion bound: %s part %d (score %.2f)", PathToUtf8(vmds[(size_t)c.vmd]).c_str(), c.part,
                 c.score);
    }
}

bool LoadScenePackage(const CharacterAsset& character, const StageAsset* stage, const SongAsset& song,
                      ScenePackage& out, LoadProgress* progress, std::string* error,
                      const std::vector<PerformerCast>* formation, const MaterialChoiceFn& materialChoice) {
    const auto choiceFor = [&](const std::string& id, const std::filesystem::path& path) {
        return materialChoice ? materialChoice(id, path) : MaterialChoice{};
    };
    try {
        out = ScenePackage{};
        out.stageId = stage ? stage->id : std::string();   // M8a: the loaded stage's id (bundled stage keys)

        if (formation && !formation->empty()) {
            // Find center performer and extra performers
            PerformerCast centerPerf;
            bool foundCenter = false;
            std::vector<PerformerCast> extraPerfs;
            for (const auto& p : *formation) {
                if (p.isCenter && !foundCenter) {
                    centerPerf = p;
                    foundCenter = true;
                } else {
                    extraPerfs.push_back(p);
                }
            }
            if (!foundCenter && !extraPerfs.empty()) {
                centerPerf = extraPerfs.front();
                extraPerfs.erase(extraPerfs.begin());
            }

            const size_t totalPerformers = 1 + extraPerfs.size();
            const size_t stageCount = stage ? stage->parts.size() : 0;
            const float totalUnits = (float)(totalPerformers + (stageCount > 0 ? 1 : 0));
            float curUnit = 0.0f;

            // 1. Center performer
            {
                std::string err;
                if (progress) {
                    progress->fraction.store(curUnit / totalUnits, std::memory_order_relaxed);
                    progress->SetStatus(Tr("캐릭터 로드: ") + centerPerf.character.displayName);
                }
                out.character.pmx = std::make_shared<PmxModel>();
                if (!LoadModelFile(centerPerf.character.modelPath, ModelRole::Character, *out.character.pmx, &err)) {
                    if (error) *error = centerPerf.character.displayName + ": " + Tr("캐릭터를 불러오지 못했습니다: ") + err;
                    return false;
                }
                out.characterId = centerPerf.character.id;
                out.characterDisplayName = centerPerf.character.displayName;
                const ModelMmeHints chHints = ModelHints(centerPerf.character.modelPath);
                out.character.defaultPackTextureFolder = chHints.packTextureFolder;
                out.character.materialBlend = chHints.materialBlend;
                out.character.materialBlendDefault = chHints.materialBlendDefault;

                float frac0 = curUnit / totalUnits;
                float frac1 = (curUnit + 0.8f) / totalUnits;
                DecodeModelTextures(out.character, progress, frac0, frac1, centerPerf.character.displayName.c_str(),
                                    choiceFor(centerPerf.character.id, centerPerf.character.modelPath));

                if (progress) progress->SetStatus(Tr("모션 로드: ") + centerPerf.character.displayName);
                VmdMotion dance;
                if (!LoadVmd(centerPerf.part.danceVmd, dance, &err)) {
                    if (error) *error = centerPerf.character.displayName + ": " + Tr("모션을 불러오지 못했습니다: ") + err;
                    return false;
                }
                std::vector<std::unique_ptr<VmdMotion>> extraStorage;
                std::vector<const VmdMotion*> layers = {&dance};
                for (const std::filesystem::path& extra : centerPerf.part.extraVmds) {
                    auto vmd = std::make_unique<VmdMotion>();
                    std::string extraErr;
                    if (!LoadVmd(extra, *vmd, &extraErr)) {
                        LOG_WARN("extra vmd load skipped: %s: %s", PathToUtf8(extra).c_str(), extraErr.c_str());
                        continue;
                    }
                    layers.push_back(vmd.get());
                    extraStorage.push_back(std::move(vmd));
                }
                out.motion = BoundMotion::Bind(*out.character.pmx, layers);
                curUnit += 1.0f;
            }

            // 2. Extra performers
            for (const auto& ep : extraPerfs) {
                std::string err;
                if (progress) {
                    progress->fraction.store(curUnit / totalUnits, std::memory_order_relaxed);
                    progress->SetStatus(Tr("캐릭터 로드: ") + ep.character.displayName);
                }
                LoadedModelCpu extraM;
                extraM.pmx = std::make_shared<PmxModel>();
                if (!LoadModelFile(ep.character.modelPath, ModelRole::Character, *extraM.pmx, &err)) {
                    if (error) *error = ep.character.displayName + ": " + Tr("캐릭터를 불러오지 못했습니다: ") + err;
                    return false;
                }

                float frac0 = curUnit / totalUnits;
                float frac1 = (curUnit + 0.8f) / totalUnits;
                DecodeModelTextures(extraM, progress, frac0, frac1, ep.character.displayName.c_str(),
                                    choiceFor(ep.character.id, ep.character.modelPath));

                if (progress) progress->SetStatus(Tr("모션 로드: ") + ep.character.displayName);
                VmdMotion extraDance;
                if (!LoadVmd(ep.part.danceVmd, extraDance, &err)) {
                    if (error) *error = ep.character.displayName + ": " + Tr("모션을 불러오지 못했습니다: ") + err;
                    return false;
                }
                std::vector<std::unique_ptr<VmdMotion>> epStorage;
                std::vector<const VmdMotion*> epLayers = {&extraDance};
                for (const std::filesystem::path& extra : ep.part.extraVmds) {
                    auto vmd = std::make_unique<VmdMotion>();
                    std::string extraErr;
                    if (!LoadVmd(extra, *vmd, &extraErr)) {
                        LOG_WARN("extra vmd load skipped: %s: %s", PathToUtf8(extra).c_str(), extraErr.c_str());
                        continue;
                    }
                    epLayers.push_back(vmd.get());
                    epStorage.push_back(std::move(vmd));
                }
                auto epMotion = BoundMotion::Bind(*extraM.pmx, epLayers);
                {
                    const ModelMmeHints eh = ModelHints(ep.character.modelPath);
                    extraM.defaultPackTextureFolder = eh.packTextureFolder;
                    extraM.materialBlend = eh.materialBlend;
                    extraM.materialBlendDefault = eh.materialBlendDefault;
                }
                out.extraCharacters.push_back(std::move(extraM));
                out.extraCharacterIds.push_back(ep.character.id);
                out.extraCharacterDisplayNames.push_back(ep.character.displayName);
                out.extraMotions.push_back(epMotion);
                curUnit += 1.0f;
            }

            // 3. Stage parts
            if (stage) {
                const size_t n = stage->parts.size();
                for (size_t k = 0; k < n; ++k) {
                    const std::filesystem::path& partPath = stage->parts[k];
                    std::string label = stage->displayName +
                                        (n > 1 ? " " + std::to_string(k + 1) + "/" + std::to_string(n) : "");
                    if (progress) progress->SetStatus(Tr("스테이지 로드: ") + label);

                    LoadedModelCpu part;
                    part.pmx = std::make_shared<PmxModel>();
                    std::string err;
                    if (!LoadModelFile(partPath, ModelRole::Stage, *part.pmx, &err)) {
                        LOG_WARN("stage part load failed: %s: %s", PathToUtf8(partPath).c_str(), err.c_str());
                        continue;
                    }
                    float begin = (curUnit + (float)k / (float)std::max<size_t>(n, 1)) / totalUnits;
                    float end = (curUnit + (float)(k + 1) / (float)std::max<size_t>(n, 1)) / totalUnits;
                    DecodeModelTextures(part, progress, begin, end, label.c_str(), choiceFor(stage->id, partPath));
                    out.stageParts.push_back(std::move(part));
                }
            }

            // 4. Camera
            if (!song.cameraVmd.empty()) {
                VmdMotion camVmd;
                std::string camErr;
                if (LoadVmd(song.cameraVmd, camVmd, &camErr)) {
                    out.camera = CameraMotion::Create(camVmd);
                    out.lightKeys = camVmd.lightKeys;
                    out.shadowKeys = camVmd.shadowKeys;
                    if (!out.camera) LOG_WARN("no camera keys in %s", PathToUtf8(song.cameraVmd).c_str());
                } else {
                    LOG_WARN("camera vmd load failed: %s: %s", PathToUtf8(song.cameraVmd).c_str(), camErr.c_str());
                }
            }

            // 5. Footer data
            out.audioPath = song.audioPath;
            float endFrame = out.motion ? out.motion->EndFrame() : 0.0f;
            for (const auto& m : out.extraMotions) {
                if (m) endFrame = std::max(endFrame, m->EndFrame());
            }
            if (out.camera) endFrame = std::max(endFrame, out.camera->EndFrame());
            out.endFrame = endFrame;
            out.motionFps = song.motionFps;
            // M8a: the song-bundled stage's motion (bound to the part whose model its bones match).
            LoadStageMotion(song, out);
            for (const StageMotionBinding& b : out.stageMotions)
                if (b.motion) out.endFrame = std::max(out.endFrame, b.motion->EndFrame());

            if (progress) {
                progress->fraction.store(1.0f, std::memory_order_relaxed);
                progress->SetStatus(Tr("GPU 업로드"));
            }
            return true;
        }

        // Single character (default path)
        // 1. Character.
        {
            std::string err;
            if (progress) {
                progress->fraction.store(0.0f, std::memory_order_relaxed);
                progress->SetStatus(Tr("캐릭터 로드: ") + character.displayName);
            }
            out.character.pmx = std::make_shared<PmxModel>();
            if (!LoadModelFile(character.modelPath, ModelRole::Character, *out.character.pmx, &err)) {
                if (error) *error = Tr("캐릭터를 불러오지 못했습니다: ") + err;
                return false;
            }
            out.characterId = character.id;
            out.characterDisplayName = character.displayName;
            const ModelMmeHints chHints = ModelHints(character.modelPath);
            out.character.defaultPackTextureFolder = chHints.packTextureFolder;
            out.character.materialBlend = chHints.materialBlend;
            out.character.materialBlendDefault = chHints.materialBlendDefault;
        }

        // 2. Character textures.
        DecodeModelTextures(out.character, progress, 0.05f, 0.45f, character.displayName.c_str(),
                            choiceFor(character.id, character.modelPath));

        // 3. Stage parts.
        if (stage) {
            const size_t n = stage->parts.size();
            for (size_t k = 0; k < n; ++k) {
                const std::filesystem::path& partPath = stage->parts[k];
                std::string label = stage->displayName +
                                    (n > 1 ? " " + std::to_string(k + 1) + "/" + std::to_string(n) : "");
                if (progress) progress->SetStatus(Tr("스테이지 로드: ") + label);

                LoadedModelCpu part;
                part.pmx = std::make_shared<PmxModel>();
                std::string err;
                if (!LoadModelFile(partPath, ModelRole::Stage, *part.pmx, &err)) {
                    LOG_WARN("stage part load failed: %s: %s", PathToUtf8(partPath).c_str(), err.c_str());
                    continue;
                }
                float begin = 0.45f + 0.35f * (float)k / (float)std::max<size_t>(n, 1);
                float end = 0.45f + 0.35f * (float)(k + 1) / (float)std::max<size_t>(n, 1);
                DecodeModelTextures(part, progress, begin, end, label.c_str(), choiceFor(stage->id, partPath));
                out.stageParts.push_back(std::move(part));
            }
        }

        // 4. Motions.
        if (progress) progress->SetStatus(Tr("모션 로드"));
        VmdMotion dance;
        std::string err;
        if (!LoadVmd(song.danceVmd, dance, &err)) {
            if (error) *error = Tr("모션을 불러오지 못했습니다: ") + err;
            return false;
        }
        std::vector<std::unique_ptr<VmdMotion>> extraStorage;
        std::vector<const VmdMotion*> layers = {&dance};
        for (const std::filesystem::path& extra : song.extraVmds) {
            auto vmd = std::make_unique<VmdMotion>();
            std::string extraErr;
            if (!LoadVmd(extra, *vmd, &extraErr)) {
                LOG_WARN("extra vmd load skipped: %s: %s", PathToUtf8(extra).c_str(), extraErr.c_str());
                continue;
            }
            layers.push_back(vmd.get());
            extraStorage.push_back(std::move(vmd));
        }
        out.motion = BoundMotion::Bind(*out.character.pmx, layers);
        LOG_INFO("motion bound: %d bone tracks, %d morph tracks, end frame %.0f",
                 out.motion->BoundBoneCount(), out.motion->BoundMorphCount(), out.motion->EndFrame());

        // 5. Camera.
        if (!song.cameraVmd.empty()) {
            VmdMotion camVmd;
            std::string camErr;
            if (LoadVmd(song.cameraVmd, camVmd, &camErr)) {
                out.camera = CameraMotion::Create(camVmd);
                out.lightKeys = camVmd.lightKeys;
                out.shadowKeys = camVmd.shadowKeys;
                if (!out.camera) LOG_WARN("no camera keys in %s", PathToUtf8(song.cameraVmd).c_str());
            } else {
                LOG_WARN("camera vmd load failed: %s: %s", PathToUtf8(song.cameraVmd).c_str(), camErr.c_str());
            }
        }

        // 6. Footer data.
        out.audioPath = song.audioPath;
        float endFrame = out.motion->EndFrame();
        if (out.camera) endFrame = std::max(endFrame, out.camera->EndFrame());
        out.endFrame = endFrame;
        out.motionFps = song.motionFps;
        // M8a: the song-bundled stage's motion (bound to the part whose model its bones match).
        LoadStageMotion(song, out);
        for (const StageMotionBinding& b : out.stageMotions)
            if (b.motion) out.endFrame = std::max(out.endFrame, b.motion->EndFrame());

        // 7. Done (GPU upload happens on the main thread).
        if (progress) {
            progress->fraction.store(1.0f, std::memory_order_relaxed);
            progress->SetStatus(Tr("GPU 업로드"));
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string(Tr("씬 로드 중 오류: ")) + e.what();
        return false;
    } catch (...) {
        if (error) *error = Tr("씬 로드 중 알 수 없는 오류");
        return false;
    }
}

bool LoadRenderBenchPackage(const std::vector<CharacterAsset>& characters, const std::vector<SongAsset>& songs,
                            ScenePackage& out, LoadProgress* progress, std::string* error) {
    try {
        if (characters.empty() || characters.size() != songs.size()) {
            if (error) *error = Tr("렌더 벤치마크 장면 구성이 올바르지 않습니다");
            return false;
        }
        out = ScenePackage{};

        const size_t n = characters.size();
        for (size_t i = 0; i < n; ++i) {
            const float fraction = (float)i / (float)n;

            // 1. Character.
            LoadedModelCpu m;
            if (progress) progress->SetStatus(Tr("캐릭터 로드: ") + characters[i].displayName);
            m.pmx = std::make_shared<PmxModel>();
            std::string err;
            if (!LoadModelFile(characters[i].modelPath, ModelRole::Character, *m.pmx, &err)) {
                if (error) *error = Tr("캐릭터를 불러오지 못했습니다: ") + err;
                return false;
            }
            {
                const ModelMmeHints mh = ModelHints(characters[i].modelPath);
                m.defaultPackTextureFolder = mh.packTextureFolder;
                m.materialBlend = mh.materialBlend;
                m.materialBlendDefault = mh.materialBlendDefault;
            }

            // 2. Character textures.
            DecodeModelTextures(m, progress, fraction, (i + 0.8f) / (float)n, characters[i].displayName.c_str());

            // 3. Dance motion.
            VmdMotion dance;
            if (!LoadVmd(songs[i].danceVmd, dance, &err)) {
                if (error) *error = Tr("모션을 불러오지 못했습니다: ") + err;
                return false;
            }
            std::vector<std::unique_ptr<VmdMotion>> extraStorage;
            std::vector<const VmdMotion*> layers = {&dance};
            for (const std::filesystem::path& extra : songs[i].extraVmds) {
                auto vmd = std::make_unique<VmdMotion>();
                std::string extraErr;
                if (!LoadVmd(extra, *vmd, &extraErr)) {
                    LOG_WARN("extra vmd load skipped: %s: %s", PathToUtf8(extra).c_str(), extraErr.c_str());
                    continue;
                }
                layers.push_back(vmd.get());
                extraStorage.push_back(std::move(vmd));
            }
            auto motion = BoundMotion::Bind(*m.pmx, layers);

            if (i == 0) {
                out.character = std::move(m);
                out.motion = motion;
            } else {
                out.extraCharacters.push_back(std::move(m));
                out.extraMotions.push_back(motion);
            }
        }

        // No stage, camera or audio: endFrame stays 0.
        // Done (GPU upload happens on the main thread).
        if (progress) {
            progress->fraction.store(1.0f, std::memory_order_relaxed);
            progress->SetStatus(Tr("GPU 업로드"));
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string(Tr("씬 로드 중 오류: ")) + e.what();
        return false;
    } catch (...) {
        if (error) *error = Tr("씬 로드 중 알 수 없는 오류");
        return false;
    }
}

std::shared_ptr<PmxModel> LoadCharacterModelOnly(const std::filesystem::path& path) {
    auto pmx = std::make_shared<PmxModel>();
    std::string err;
    if (!LoadModelFile(path, ModelRole::Character, *pmx, &err)) return nullptr;
    return pmx;
}

} // namespace mmdx
