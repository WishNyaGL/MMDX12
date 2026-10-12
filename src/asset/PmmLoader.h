#pragma once
// Read-only MMD project (.pmm, "Polygon Movie maker 0002", plus the 0001 layout) importer.
// Independent of the App and the renderer: bounds-checked reads only, never crashes on a truncated or
// hostile file (a structural problem returns false with *error). Shift-JIS names are decoded to UTF-8.
//
// Format reference (checked against a real 0002 file, Camera_PMM.pmm):
//   - MMD's project container; the layout here follows the reverse-engineering write-up at
//     tracebox.blog.fc2.com/blog-entry-35.html (via the Wayback Machine) and the nanoem reference
//     reader (nanoem/ext/document.c, the "PMMv2" parser the mmd-anim-format crate also cites).
//   - Keys are stored as doubly-linked lists with one "initial" key per track (no object index) and
//     N additional keys (with an object index); the chains are rebuilt into per-track arrays here.
//   - PMM stores explicit Bezier interpolation tables (bone: 4 channels x {x1,y1,x2,y2}; camera:
//     6 params x {x1,y1,x2,y2}), the same control points MMD writes into a VMD.
#include <DirectXMath.h>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mmdx {

using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;

// A bone key. `interp` is PMM's own Bezier table: channel c (0 X, 1 Y, 2 Z, 3 rotation) at
// interp[c*4 + {x1,y1,x2,y2}], values 0..127. The conversion to a VMD interpolation block is
// studio::SetBoneCurve (see src/studio/StudioLoader.cpp, LoadStudioPmmPackage).
struct PmmBoneKey {
    int frame = 0;
    uint8_t interp[16] = {};
    XMFLOAT3 translation{};
    XMFLOAT4 rotation{0, 0, 0, 1};
    bool physicsDisabled = false;
};
struct PmmMorphKey {
    int frame = 0;
    float weight = 0.0f;
};

// A model's visibility / IK / outside-parent key. `ikEnabled[i]` is the enable state of the i-th
// constraint (IK) bone (`PmmModel::ikBones[i]`); `outsideParent[i]` is (parent model index, parent
// bone index) for the i-th outside-parent subject bone (-1 = none).
struct PmmModelKey {
    int frame = 0;
    bool visible = true;
    std::vector<uint8_t> ikEnabled;
    std::vector<std::pair<int, int>> outsideParent;
    bool selfShadow = true;
};

struct PmmCameraKey {
    int frame = 0;
    float distance = 0.0f;
    XMFLOAT3 target{};
    XMFLOAT3 rotation{};        // radians (x pitch, y yaw, z roll)
    int followModel = -1;       // bone-follow target model index (-1 = none)
    int followBone = -1;
    // VMD camera layout (see asset/VmdMotion.h): param p (0 X,1 Y,2 Z,3 rotation,4 distance,5 fov)
    // at p*4 = {x1,x2,y1,y2}, from PMM's 6 x {x1,y1,x2,y2}.
    uint8_t interp[24] = {};
    bool perspective = true;
    uint32_t fovDeg = 30;
};
struct PmmLightKey {
    int frame = 0;
    XMFLOAT3 color{};
    XMFLOAT3 direction{};
};
struct PmmShadowKey {
    int frame = 0;
    uint8_t mode = 0;
    float distance = 0.0f;
};
struct PmmAccessoryKey {
    int frame = 0;
    float opacity = 1.0f;
    bool visible = true;
    int parentModel = -1;       // accessory's parent model index (-1 = world)
    int parentBone = -1;        // parent bone index (-1 = the model's origin)
    XMFLOAT3 translation{};
    XMFLOAT3 rotationDeg{};     // Euler degrees (MMD accessory convention)
    float scale = 1.0f;
};

struct PmmModel {
    int index = -1;             // ModelNo (the value MMD stores, usually 1 + slot)
    std::string nameJa, nameEn;
    std::filesystem::path path;
    bool visible = true;
    int lastFrame = 0;
    std::vector<std::string> boneNames, morphNames;
    std::vector<int> ikBones;             // constraint bone indices
    std::vector<int> outsideParentBones;  // outside-parent subject bone indices
    std::map<std::string, std::vector<PmmBoneKey>> bones;    // bone name -> keys (frame sorted)
    std::map<std::string, std::vector<PmmMorphKey>> morphs;  // morph name -> keys
    std::vector<PmmModelKey> modelKeys;   // visibility / IK / outside parent (frame sorted)
};

struct PmmAccessory {
    std::string name;
    std::filesystem::path path;
    std::vector<PmmAccessoryKey> keys;    // frame sorted (the initial key is included)
};

// The native MMDX12-ish settings a PMM's MME effect controllers map onto. This struct is deliberately
// independent of the renderer / App: the App applies it to AppSettings + ApplyRenderSettings.
//
// Mapping table (approximate: the exact per-controller parameter meaning is undocumented and varies
// between controller versions; presence enables the native effect and, where the value has an obvious
// equivalent, its first key is used):
//   AutoLuminous*.x            -> autoLuminous on
//   Diffusion*.x / o_Diffusion -> bloom on; scale (Si) ~ 1 -> bloomIntensity = clamp(scale, 0..4) * 0.5
//   ikBokeh / DoF / SvDOF/HgDOF-> dof on; aperture from scale (Si), focus from translation.z (MMD units)
//   SSAO*.x / HgSAO*.x         -> ssao on
//   o_Tonemap*.x / Tonemap*.x  -> lutIntensity from opacity (Tr); lutStrength = clamp(Tr, 0..1)
//   ExcellentShadow*.x/HgShadow-> shadowDistance = clamp(scale * 160, 5, 1000) (MMD units)
//   o_LightBloom*.x / o_Glow*.x-> bloom on
//   o_PostEffect / o_SelfOverlay / o_Surplus / o_ObjectLuminous / ray*.x -> recognised, no native mapping
struct PmmEffectSettings {
    bool autoLuminous = false;
    bool bloom = false;
    bool ssao = false;
    bool dof = false;
    float bloomIntensity = -1.0f;   // < 0 = not set
    float dofFocusDistance = -1.0f; // < 0 = not set (MMD units, view-space z)
    float dofAperture = -1.0f;      // < 0 = not set
    float shadowDistance = -1.0f;   // < 0 = not set (MMD units)
    float lutIntensity = -1.0f;     // < 0 = not set
    std::vector<std::string> recognised;  // "AutoLuminous.x -> autoLuminous on" notes
    std::vector<std::string> unsupported; // controller file names that have no native mapping
};

struct PmmDocument {
    std::string version;        // "0002" or "0001"
    int outputWidth = 0, outputHeight = 0;
    float timelineFov = 30.0f;
    float preferredFps = 30.0f;
    int maxFrame = 0;
    bool hasRange = false;
    int rangeStart = 0, rangeEnd = 0;
    bool loop = false;
    std::vector<PmmModel> models;
    std::vector<PmmCameraKey> cameraKeys;
    std::vector<PmmLightKey> lightKeys;
    std::vector<PmmShadowKey> shadowKeys;
    std::vector<PmmAccessory> accessories;
    std::filesystem::path audioPath;   // as stored (usually absolute); empty = none
    bool audioEnabled = false;
    std::vector<std::string> warnings; // non-fatal notes (unparsable tail, unreachable keys, ...)
};

// Parses `path`. Returns false with *error only when the file is missing / empty / not a PMM
// (signature) / structurally broken before the first model. A damaged tail keeps what was parsed and
// adds a warning (return value still true for a readable header). Never throws.
bool LoadPmm(const std::filesystem::path& path, PmmDocument& out, std::string* error = nullptr);

// MME effect-controller classification (used to keep controllers out of the visible props and to map
// their Si / Tr / position values onto native settings). The ~10 most common controller families,
// matched case-insensitively on the file name (one table, see PmmLoader.cpp).
enum class MmeController : uint8_t {
    None = 0,
    AutoLuminous,   // AutoLuminous*.x: emissive glow -> autoLuminous
    Diffusion,      // Diffusion*.x (o_Diffusion): soft bloom -> bloom
    Dof,            // ikBokeh.x / Dof*.x / SvDOF*.x / HgDOF*.x: depth of field -> dof + aperture
    Ssao,           // SSAO*.x / HgSAO*.x: screen-space AO -> ssao
    Tone,           // o_Tonemap*.x / HgSpotLight: tone mapping -> lutIntensity
    Shadow,         // ExcellentShadow*.x / HgShadow: soft shadow -> shadowDistance
    LightBloom,     // o_LightBloom*.x / o_Glow*.x
    PostEffect,     // o_PostEffect*.x / o_SelfOverlay.x / o_Surplus.x / o_ObjectLuminous.x
    Ray,            // ray.x / ray_controller.pmx / Ray-MMD controllers
};
// Classifies by the accessory's file name. Returns MmeController::None for a normal accessory.
MmeController ClassifyMmeController(const std::string& fileName, std::string* matchedName = nullptr);
// Human-readable family name (for the warning toast / pmm_probe). Never null.
const char* MmeControllerName(MmeController c);
// Builds the native-settings mapping from the document's accessories: known controllers are kept out
// of the visible props and their first key's Si / Tr / position values fill `out`. Unknown controllers
// (LooksLikeUnknownMmeController) go to `out.unsupported`.
PmmEffectSettings MapMmeControllers(const PmmDocument& doc);
// True when `path`'s file name looks like an MME controller but is not in the known table (an MME
// folder or a .x/.pmx whose name follows the sr/controller conventions). Used for the "unsupported"
// warning list.
bool LooksLikeUnknownMmeController(const std::filesystem::path& path);

} // namespace mmdx
