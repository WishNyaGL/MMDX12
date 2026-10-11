#pragma once
// CPU-side scene loading (runs on a worker thread): parses PMX/VMD, decodes textures,
// binds motions. GPU upload happens afterwards on the main thread (App).
#include "anim/Motion.h"
#include "asset/AssetLibrary.h"
#include "asset/ImageLoader.h"
#include "asset/PbrMaterial.h"
#include "asset/PmxModel.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mmdx {

struct LoadedModelCpu {
    std::shared_ptr<PmxModel> pmx;
    std::vector<ImageRGBA8> textures;  // parallel to pmx->textures; empty = missing/failed
    // The model's default shader-pack texture folder (found through its own MME files by asset/MmeHints): the
    // directory of the face texture the pack needs. Empty when none was found. Filled on the loader worker thread.
    std::filesystem::path defaultPackTextureFolder;
    // Per-material Ray-MMD PBR records + decoded map textures (work unit P2); empty when the model has none.
    PbrModelData pbr;
};

struct PerformerCast {
    CharacterAsset character;
    SongPart part;
    bool isCenter = false;
};

struct ScenePackage {
    LoadedModelCpu character;
    std::string characterId;
    std::string characterDisplayName;
    std::vector<LoadedModelCpu> stageParts;   // empty when no stage selected
    std::shared_ptr<BoundMotion> motion;      // bound to character
    std::shared_ptr<CameraMotion> camera;     // null if the song has no camera vmd
    std::vector<VmdLightKey> lightKeys;       // the camera vmd's light / self-shadow keys (may be empty)
    std::vector<VmdShadowKey> shadowKeys;
    std::filesystem::path audioPath;          // may be empty
    float endFrame = 0;                       // max(motion end, camera end)
    // Performers after `character`, each with its own bound dance
    // (parallel vectors).
    std::vector<LoadedModelCpu> extraCharacters;
    std::vector<std::string> extraCharacterIds;
    std::vector<std::string> extraCharacterDisplayNames;
    std::vector<std::shared_ptr<BoundMotion>> extraMotions;
};

struct LoadProgress {
    std::atomic<float> fraction{0};    // 0..1
    std::mutex mutex;
    std::string status;                // guarded by mutex, UTF-8, e.g. "텍스처 디코딩 12/40"
    void SetStatus(const std::string& s) { std::lock_guard<std::mutex> l(mutex); status = s; }
    std::string Status() { std::lock_guard<std::mutex> l(mutex); return status; }
};

// Decodes every texture referenced by m.pmx's materials into m.textures (parallel).
// `progress` may be null. When `rayMaterials` is set, the model's Ray-MMD PBR data is parsed and decoded too
// (asset/PbrMaterial.h) and stored in m.pbr.
void DecodeModelTextures(LoadedModelCpu& m, LoadProgress* progress, float fracBegin, float fracEnd,
                         const char* label, bool rayMaterials = false);

// The character's model without textures (material names for the library's per-material shader list); null on failure.
std::shared_ptr<PmxModel> LoadCharacterModelOnly(const std::filesystem::path& path);

// `stage` may be null. Returns false with *error on failure of the character or motion;
// a stage part or texture that fails to load only logs a warning.
// Textures are decoded in parallel (std::for_each with std::execution::par).
// If `formation` is provided and non-empty, loads the cast performers into character (center) and extraCharacters.
bool LoadScenePackage(const CharacterAsset& character, const StageAsset* stage, const SongAsset& song,
                      ScenePackage& out, LoadProgress* progress, std::string* error,
                      const std::vector<PerformerCast>* formation = nullptr, bool rayMaterials = false);

// Render benchmark scene: characters[i] posed by the dance of songs[i] (sizes equal, >= 1).
// out.character / out.motion = index 0, out.extraCharacters / out.extraMotions = the rest.
// No stage, camera or audio (audioPath empty, camera null); endFrame = 0. Fails (with *error)
// when any character, its textures' model or its dance VMD cannot be loaded.
bool LoadRenderBenchPackage(const std::vector<CharacterAsset>& characters, const std::vector<SongAsset>& songs,
                            ScenePackage& out, LoadProgress* progress, std::string* error);

} // namespace mmdx
