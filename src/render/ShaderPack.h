#pragma once
// Shader packs: opt-in, per-model replacements of the scene pass's material shading ("type": "surface", default) or
// whole-screen post effects ("type": "effect", one entry of the user's effect stack), made by anyone.
// A pack is a folder with pack.json (manifest: metadata, material class rules / injection stage, parameters),
// surface.hlsl (PackShade) or effect.hlsl (PackEffect, the contracts in pack_api.hlsli / effect_api.hlsli, versioned
// by kPackApiVersion) and an optional preview.png.
//   built-in   <exe>/shaders/packs/<id>      shipped with the app, read-only
//   installed  <exe>/shader_packs/<id>       user / downloaded packs (online installs carry .mmdx_install.json)
// The renderer compiles a surface pack's PSOs on first use (ScenePass) and an effect pack's PSO on first frame
// (PackEffectPass); a model opts in with GpuModel::SetShaderPack, the effect stack is RenderSettings::packEffects.
// The registry is used from the main thread only.
#include "asset/ModelTraits.h"
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mmdx {

struct LookData;

// The PackShade / PackEffect contract version (pack_api.hlsli / effect_api.hlsli PACK_API_VERSION). A pack declares
// the version it was written for; packs with a higher version are listed as incompatible and never compiled.
inline constexpr int kPackApiVersion = 6;
// The PackScreen contract version (screen_api.hlsli SCREEN_API_VERSION): a stage-bundled "screen" pack draws a stage
// screen's content (render/StageScreens.h, docs/shader_screen_api.md).
inline constexpr int kScreenApiVersion = 1;
inline constexpr uint32_t kPackMaxParams = 16;
inline constexpr uint32_t kPackMaxParamsExt = 128;
inline constexpr uint32_t kPackMaxTextures = 16;   // pack.json "textures" entries (one SRV each)
inline constexpr uint32_t kPackMaxTextureSize = 4096;   // per-texture width / height cap
inline constexpr uint32_t kPackMaxPasses = 8;           // effect packs: pack.json "passes" (API v5)
using PackParamValues = std::array<float, kPackMaxParams>;

// What a pack does: replace the character / stage material shading (surface.hlsl, default; every v1/v2 pack), a
// whole-screen effect (effect.hlsl, PackEffect in effect_api.hlsli, one entry of the user's effect stack) or the content
// of an MMDX stage screen (screen.hlsl, PackScreen in screen_api.hlsli; docs/shader_screen_api.md).
enum class PackType : uint8_t { Surface = 0, Effect, Look, World, Screen };

inline constexpr uint32_t kPackMaxTexturesWorld = 32;
inline constexpr uint32_t kPackWorldMaxParticles = 8;
inline constexpr uint32_t kPackWorldMaxFields = 4;
inline constexpr uint32_t kPackWorldMaxSurfaces = 2;
inline constexpr uint32_t kPackWorldMaxParticleCount = 262144;

struct PackWorldParticleSystem {
    std::string name;
    uint32_t count = 0;   // 1 .. 262144
    enum class Blend : uint8_t { Alpha = 0, Add };
    Blend blend = Blend::Alpha;
    // "space": "local" (default; the particles live in the entity's world space) or "camera" (the host wraps them
    // into a box of "wrap" world units around the camera every frame; needs the "world.camera" capability).
    enum class Space : uint8_t { Local = 0, Camera };
    Space space = Space::Local;
    float wrap = 120.0f;   // the camera-space tile size (world units), 1 .. 4096
    // "wrapParam" (optional): the tile size follows that float param (value x "wrapScale"), so a pack's range slider
    // sizes the tile; empty = the fixed "wrap".
    std::string wrapParam;
    float wrapScale = 1.0f;
    std::string simulate; // stateful: void <fn>(inout PackParticle p, PackSimInput s)
    std::string spawn;    // stateless: PackParticle <fn>(uint id, float t)
    std::string shade;    // float4 <fn>(PackParticleShadeIn i)
    float sizeMin = 0.0f;
    float sizeMax = 1.0f;
};

struct PackWorldField {
    std::string name;
    uint32_t size = 256;  // 64 .. 2048 texels
    float extent = 10.0f; // world units, centred on entity origin
    enum class Format : uint8_t { R16F = 0, RG16F, RGBA16F };
    Format format = Format::RGBA16F;
    bool persistent = false;
    float rate = 10.0f;   // steps per second (default 10 for persistent)
    // "anchor": "transform" (default; the field is centred on the entity's origin) or "performers" (the field follows
    // the performers: its centre is the mean xz of the foot colliders; needs the "world.anchor" capability).
    enum class Anchor : uint8_t { Transform = 0, Performers };
    Anchor anchor = Anchor::Transform;
    std::string update;   // float4 <fn>(PackFieldInput f)
};

struct PackWorldSurface {
    std::string field;    // name of field
    uint32_t grid = 64;   // 16 .. 512 quads per side
    std::string height;   // float <fn>(PackSurfaceIn i)
    std::string shade;    // PackSurfaceOut <fn>(PackSurfaceShadeIn i)
    // "extent": "field" (default; the grid covers the field's extent) or "camera" (a camera-centred grid of
    // nested LOD levels; needs the "world.ground" capability).
    enum class Extent : uint8_t { Field = 0, Camera };
    Extent extent = Extent::Field;
    float radius = 400.0f;   // "radius": the camera extent's half size (world units), 10 .. 4096
    bool ground = false;      // "ground": true: the height function returns the height ABOVE the ground
    // "blend": "opaque" (default) or "overlay" (premultiplied alpha over the lit scene; needs "world.overlay").
    enum class Blend : uint8_t { Opaque = 0, Overlay };
    Blend blend = Blend::Opaque;
};

struct PackWorldData {
    float step = 1.0f / 60.0f; // 1/240 .. 1/15, default 1/60
    float rewind = 4.0f;       // 0 .. 20, default 4
    std::vector<PackWorldParticleSystem> particles; // up to 8
    std::vector<PackWorldField> fields;             // up to 4
    std::vector<PackWorldSurface> surfaces;         // up to 2
};

// Where an effect pack injects ("stage"): "pre-bloom" (linear HDR before BloomPass) or "post" (default; after
// PostPass / tonemap, display-referred sRGB into the LDR target).
enum class PackEffectStage : uint8_t { Post = 0, PreBloom };

// One entry of the user's effect stack: an effect pack, whether it runs, its parameter values (key -> value, the
// rest use the pack defaults) and an optional texture folder (empty = the pack-level folder). Carried by
// RenderSettings::packEffects, persisted as AppSettings::effectStack (ini) and edited in the shader screens.
struct EffectStackEntry {
    std::string pack;
    bool enabled = true;
    std::map<std::string, float> params;
    std::string textureFolder;
    bool operator==(const EffectStackEntry&) const = default;
};

// Material classes, as the PACK_* ids in pack_api.hlsli.
enum class PackClass : uint32_t { Body = 0, Skin = 1, Face = 2, Eye = 3, Hair = 4, Weapon = 5 };
// MaterialConstants::packClass of a material the user switched off in a pack model: every path (raster, edge, RT,
// PT, offline GI) draws it with the default shading, as if the model had no pack. Never reaches pack code.
inline constexpr uint32_t kPackClassOff = 0xFFu;
// Per-material overrides of a pack model (ShaderChoice::materials): material name (PmxMaterial::name) -> a PackClass
// value 0..5, or kMaterialPackOff. Absent = automatic (the pack's class rules).
inline constexpr int kMaterialPackOff = -1;
using MaterialClassOverrides = std::map<std::string, int>;

// S6 (docs/stage_format.md `packs`): the bundled surface pack one stage part's materials are shaded with, plus the
// per-material class overrides that keep the pack off every material its `materials` globs do not claim. Resolved once
// per part (App::StageRuntime ResolveStageSurfacePack); `id` empty = the part keeps the default shading.
struct StageSurfacePack {
    std::string id;                     // pack registry id, empty = no surface pack
    MaterialClassOverrides materials;   // GpuModel::SetShaderPack's per-material map
};

// A model's shader pack choice: pack id (empty = the default shading), the parameter values the user changed
// (key -> value; the rest use the pack's defaults) and an optional per-character texture folder (UTF-8, empty =
// the pack-level folder). Saved per character (settings) and per studio model (project).
struct ShaderMemo {   // what was set for a pack that is not the current one
    std::map<std::string, float> params;
    std::string textureFolder;
    bool operator==(const ShaderMemo&) const = default;
};
// `remembered` holds the parameters and texture folder of the packs this model used before, so going back to one
// (after trying another pack or the default shading) restores them instead of starting from scratch.
// M6a: a model's MME object effect choice (docs/mme/mme_design.md §2.12). None = native / pack shading (the default
// for characters: users' characters keep their look), Auto = what the model's files ask for (its `.emd` / same-name
// `.fx` / `name[fx].pmx`, mapped to per-material paths), File = one effect for every material (the CLI `--mme-model`).
// Stages default to Auto. Saved per character / stage (settings) and per studio model (project).
struct MmeChoice {
    enum Mode { None = 0, Auto, File };
    Mode mode = None;
    std::filesystem::path file;   // File mode only
    bool operator==(const MmeChoice&) const = default;
};

struct ShaderChoice {
    std::string pack;
    std::map<std::string, float> params;
    std::string textureFolder;
    std::map<std::string, ShaderMemo> remembered;   // pack id -> its last params / texture folder (not the current pack)
    // Per-material class overrides / "default shading" (see MaterialClassOverrides). Class semantics are shared by
    // every pack, so they survive SwitchPack.
    MaterialClassOverrides materials;
    // Selects `to` ("" = the default shading): the current pack's settings are remembered, `to`'s come back.
    void SwitchPack(const std::string& to) {
        if (to == pack) return;
        if (!pack.empty()) {
            if (params.empty() && textureFolder.empty()) remembered.erase(pack);
            else remembered[pack] = ShaderMemo{params, textureFolder};
        }
        pack = to;
        params.clear();
        textureFolder.clear();
        if (const auto it = remembered.find(to); !to.empty() && it != remembered.end()) {
            params = it->second.params;
            textureFolder = it->second.textureFolder;
            remembered.erase(it);
        }
    }
    bool operator==(const ShaderChoice&) const = default;
};

// A manifest string: either a plain JSON string (stored under "") or an object {"ko": .., "en": .., "ja": .., "zh": ..}.
struct LocalizedText {
    std::map<std::string, std::string> byLang;
    // `lang` = "ko" / "en" / "ja" / "zh"; falls back to "en", then "", then "ko", then any non-empty entry.
    const std::string& Get(const std::string& lang) const;
    bool Empty() const;
};
// The UI language as a manifest key ("ko" / "en" / "ja" / "zh"), from core/I18n.h's ActiveLanguage().
std::string PackLanguage();

struct PackAuthor {
    std::string name;   // required
    std::string role;   // free text ("셰이더", "translation", ...), may be empty
    std::string url;    // homepage / profile, may be empty (http/https only, anything else is dropped)
};

// Typed scene inputs an effect pack can read (pack.json "inputs"). The JSON names are PackInputName(): performer.head,
// performer.center, performer.hand_l, performer.hand_r, performer.foot_l, performer.foot_r, light.key, camera.focus,
// audio. Every performer.* / light.key / camera.focus input requires the "inputs.scene" capability; audio requires
// "inputs.audio".
enum class PackInput : uint8_t {
    PerformerHead = 0,
    PerformerCenter,
    PerformerHandL,
    PerformerHandR,
    PerformerFootL,
    PerformerFootR,
    LightKey,
    CameraFocus,
    Audio,
    Count
};
const char* PackInputName(PackInput input);

// The capability table: `apiVersion` 0 = never implied by the version (must be declared in "requires").
struct PackCapability {
    std::string_view name;
    int apiVersion;
};
inline constexpr PackCapability kPackCapabilities[] = {
    {"adduv", 6},      {"pt.lighting", 6}, {"state", 4},        {"passes", 5},
    {"replaces.dof", 5}, {"textures", 2},  {"effect", 3},       {"inputs.scene", 0},
    {"inputs.audio", 0}, {"surface.output", 0}, {"inputs.id", 0}, {"passes.history", 0},
    {"passes.mips", 0}, {"params.ext", 0},
    // World packs (docs/shader_world_api.md): the camera, the performer anchor, the ground map, the overlay blend.
    // apiVersion 0: never implied by the version, the pack must declare them in "requires".
    {"world.camera", 0}, {"world.anchor", 0}, {"world.ground", 0}, {"world.overlay", 0}, {"world.body", 0},
    {"world.lights", 0},
};

enum class ShaderPackParamType : uint8_t {
    Float = 0,
    Int,
    Bool,
    Color,
    Vec3
};

struct ShaderPackGroup {
    std::string id;
    LocalizedText label;          // localized like param labels
    bool collapsed = false;
};

struct ShaderPackParam {
    std::string key;
    LocalizedText label;          // "label": string or localized object; empty -> key
    ShaderPackParamType type = ShaderPackParamType::Float;
    std::string group;            // group id from pack.json "groups"
    bool alpha = false;           // color only: 4 components if true (alpha included)
    float def = 0.0f;             // float default, or x component
    float defVec[4] = {0, 0, 0, 0}; // default values for color (sRGB 0..1), vec3, or float/int/bool in [0]
    bool defBool = false;
    float min = 0.0f, max = 1.0f, step = 0.0f;
};

// A pack.json "textures" entry: an extra texture the pack's shading samples (light maps, ramps,
// face SDFs, matcaps, LUTs). `file` is relative to the pack folder (png/jpg/jpeg). Address mode and
// the sampled space: sRGB textures (default) get an _SRGB SRV and sample as LINEAR values, others
// are UNORM (raw values; data maps).
struct PackTexture {
    enum class Origin : uint8_t { Bundled, User };
    std::string file;    // required, relative to the pack, no "..", no drive / root
    bool clamp = false;  // "address": "clamp" (else "wrap")
    bool srgb = true;    // "srgb"
    Origin origin = Origin::Bundled;   // "origin": bundled (ships with the pack) or user (user-supplied)
};

enum class PackSource : uint8_t {
    BuiltIn,
    User,
    Online,
    // S6: a pack bundled in an MMDX stage folder ("bundles": a stage's packs are registered as
    // "stage:<stage id>/<folder>"). Never installed, never listed in the manager / pickers / gallery and never hot
    // reloaded; the stage owns its lifetime.
    Stage
};
enum class PackStatus : uint8_t {
    Ready,            // manifest fine; not compiled yet or compiled fine
    Compiled,         // its PSOs were built in this run
    CompileError,     // statusMessage = the compiler output (ScenePass)
    Incompatible,     // apiVersion > kPackApiVersion, or minAppVersion newer than this build
    InvalidManifest,  // listed so authors see why; statusMessage says what is wrong; never selectable
    Duplicate,        // same id as an earlier pack (built-in wins, then folder order); never selectable
    Revoked,          // a zip on the official index's revocation list; never selectable
};

struct ShaderPack {
    // identity
    std::string id;               // [a-z0-9_-]{1,64}; also the folder name for installed packs
    std::string version;          // "1.2.0" (dotted numbers; compared with Updater's CompareVersions rules)
    int apiVersion = 1;           // "apiVersion", default 1
    std::string minAppVersion;    // optional
    // presentation
    LocalizedText name, description;
    std::vector<PackAuthor> authors;
    std::string license;          // SPDX-like free text ("MIT", "CC-BY-4.0", ...)
    std::string homepage, repository;   // http/https only
    std::vector<std::string> tags;      // lower-case words ("toon", "anime", "hoyoverse", ...)
    LocalizedText recommendedFor;       // which models it is made for (free text)
    // "suggest": model-data traits the pack fits (all given conditions must hold). The app only SUGGESTS such a pack
    // (one-time toast, badge in the pickers); it never applies it by itself.
    struct Suggest {
        bool present = false;
        int additionalUvsMin = 0;       // "additionalUvsMin"
        int toonMapMaterialsMin = 0;    // "toonMapMaterialsMin"
    };
    Suggest suggest;
    std::filesystem::path preview;      // dir/preview.png (or .jpg) when present, else empty
    std::filesystem::path previewBefore; // dir/preview_before.png (or .jpg) when present, else empty
    // files
    std::filesystem::path dir;          // holds pack.json and surface.hlsl
    PackSource source = PackSource::User;
    std::string installedFrom;          // online installs: the index URL they came from (.mmdx_install.json)
    std::string installedSha;           // online installs: the installed zip's SHA-256 (.mmdx_install.json)
    // "type": "surface" (default; material shading) or "effect" (PackEffect; pack.json "stage" = pre-bloom / post)
    PackType type = PackType::Surface;
    std::shared_ptr<const LookData> look;            // look packs: the parsed "look" block
    std::shared_ptr<const PackWorldData> world;      // world packs: the parsed "world" block
    PackEffectStage stage = PackEffectStage::Post;   // effect packs only
    uint32_t stateFloats = 0;                        // effect packs only: "state": { "floats": N } (1..16)
    // effect packs only (API v5): "passes" = intermediate passes that run before PackEffect, in order. Each draws
    // `entry` (float4 entry(PackPassInput)) into its own RGBA16F target of output size x `scale`; later passes and
    // PackEffect read it as PackPassSample(index, uv).
    struct Pass {
        std::string entry;    // HLSL identifier
        float scale = 1.0f;   // 1/64 .. 1 of the output size
        bool history = false; // "history": true = its previous-frame result stays readable (PackPassSampleHistory);
                              // needs the "passes.history" capability
        bool mips = false;    // "mips": true = a mip chain is built after the pass (PackPassSampleLevel); "passes.mips"
    };
    std::vector<Pass> passes;   // at most kPackMaxPasses
    bool replacesDof = false;   // "replaces": ["dof"]: while enabled, the engine's depth of field (DofPass, the GI
                                // thin lens) stays off and the pack draws it (PackFocusZ / PackFocusAperture)
    // shading
    struct Rule {
        PackClass cls = PackClass::Body;
        std::vector<std::string> match;     // UTF-8 substrings of the PMX material name (or its English name)
        std::vector<std::string> texture;   // "texture": UTF-8 substrings of the material's diffuse texture path
    };
    std::vector<Rule> rules;              // first match wins; no match: Body
    // Per-class alpha key (pack.json class rule "alphaKey", 0..1; 0 = off), indexed by PackClass: cut out every pixel
    // whose texture red is below the key (additive-key sprites such as MME eye highlights) in every render path.
    // A rule's key belongs to the CLASS it assigns; when several rules of one class give different keys the FIRST
    // rule's value wins (a later rule of the same class does not overwrite it).
    float alphaKey[6] = {0, 0, 0, 0, 0, 0};
    std::vector<ShaderPackGroup> groups;  // pack.json "groups" (params.ext)
    std::vector<ShaderPackParam> params;  // at most kPackMaxParams (or kPackMaxParamsExt with params.ext)
    std::vector<PackTexture> textures;    // at most kPackMaxTextures; optional extra textures (PackSampleTex)
    bool hasEdge = false;                 // surface.hlsl sets PACK_HAS_EDGE: the pack draws the outlines too (PackEdge)
    bool hasPtSurface = false;            // pt_surface.hlsl exists next to surface.hlsl (offline GI stills)
    // Declared capabilities ("requires": ["state", ...]) and typed scene inputs ("inputs"): see kPackCapabilities.
    std::vector<std::string> requiresCaps;   // "requires": declared capabilities (the spec's name is a C++20 keyword)
    uint32_t inputMask = 0;               // bit per PackInput
    // True when `cap` is declared in "requires" or implied by the pack's apiVersion (kPackCapabilities).
    bool HasCap(std::string_view cap) const;
    // state
    PackStatus status = PackStatus::Ready;
    std::string statusMessage;            // English, for the log / the pack details (compiler output may be long)

    // True when pack.json declares "suggest" and `t` meets every condition; false without "suggest".
    bool Suggests(const ModelTraits& t) const {
        return suggest.present && t.additionalUvCount >= suggest.additionalUvsMin &&
               t.toonMapMaterials >= suggest.toonMapMaterialsMin;
    }
    bool Selectable() const { return status == PackStatus::Ready || status == PackStatus::Compiled; }
    // `texture` = the material's diffuse texture path as stored in the PMX (empty without one).
    PackClass Classify(const std::string& name, const std::string& nameEn, const std::string& texture) const;
    // S6: the class a STAGE material gets (docs/stage_format.md `packs`): the first class rule that matches, else the
    // pack's first class rule's class (character packs rarely name stage materials, and the class would otherwise fall
    // back to Body for every stage surface), else Body. The stage's material is always shaded by the pack, never
    // switched off: the pack owns it the way it owns a character's materials.
    PackClass ClassifyStage(const std::string& name, const std::string& nameEn, const std::string& texture) const;
    // Parameter values in manifest order: `values` (key -> value) over the defaults, clamped to the ranges.
    PackParamValues Resolve(const std::map<std::string, float>& values) const;
    void ResolveSlots(const std::map<std::string, float>& values, std::vector<DirectX::XMFLOAT4>& slots) const;
};

// Builds GPU float4 slots for declared parameters (float/int/bool in .x, color in .rgba converted sRGB -> linear, vec3 in .xyz).
void BuildPackParamSlots(const ShaderPack& pack, const std::map<std::string, float>& values,
                         std::vector<DirectX::XMFLOAT4>& slots);

// Parses a manifest. `dir` is the pack folder (for preview / surface checks). Never throws. On failure returns false
// with `error` set, and `out` still holds whatever could be read (at least id from the folder name) so the UI can list it.
bool ParseShaderPackManifest(const std::string& json, const std::filesystem::path& dir, ShaderPack& out, std::string& error);

// pack.json "textures" lookup: the user texture folder first (same relative path, then the same file
// name, then a file whose name ends with "_" + the declared name, case-insensitive, shortest wins),
// then the pack folder (exact path). False = missing (white is used). Used from the scene pass and
// the effect pass (Passes.cpp, shared helper).
bool ResolvePackTextureFile(const ShaderPack& pack, const std::string& file, const std::filesystem::path& userFolder,
                            std::filesystem::path& out);

class ShaderPackRegistry {
public:
    std::filesystem::path BuiltInRoot() const;   // <exe>/shaders/packs
    std::filesystem::path UserRoot() const;      // <exe>/shader_packs (created on demand)

    // (Re)reads every pack folder (built-in first, then installed, each sorted by folder name). Keeps the compile
    // status of packs whose files did not change. Bumps Generation(), so the scene pass drops its compiled PSOs and
    // models re-resolve their packs.
    void Scan();
    // Hot reload for authors: call every frame; checks the pack folders' file times at most twice a second and calls
    // Scan() when a pack.json / *.hlsl / *.hlsli / preview changed, a folder appeared or disappeared. True if it rescanned.
    bool PollChanges(double nowSeconds);

    const std::vector<ShaderPack>& Packs() const { return packs_; }
    const ShaderPack* Find(const std::string& id) const;      // any status, installed or stage-bundled
    uint32_t Generation() const { return generation_; }

    // S6: registers a pack bundled in an MMDX stage folder under `id` (StagePackId), parsed exactly like an installed
    // pack and reachable through Find(), but never listed in the manager / pickers / gallery and never hot reloaded.
    // Idempotent: an already registered pack with the same id and folder is returned as is. A broken pack.json is one
    // [W] (logged here, once) and returns null: the stage keeps loading. Bumps Generation() on change.
    const ShaderPack* RegisterStagePack(const std::string& id, const std::filesystem::path& dir);
    // The stage-bundled packs (PackSource::Stage), e.g. for the studio's cleanup on stage removal.
    const std::vector<ShaderPack>& StagePacks() const { return stagePacks_; }
    void UnregisterStagePacks(const std::string& stageId);   // drops "stage:<stageId>/..." entries

    // Installed packs whose zip SHA-256 is listed get PackStatus::Revoked (never selectable). Bumps Generation().
    void ApplyRevocations(const std::vector<std::string>& hashes);

    // ScenePass reports compile results; the status survives Scan() while the pack's files are unchanged.
    void ReportCompiled(const std::string& id);
    void ReportError(const std::string& id, const std::string& message);
    // Grows with every ReportError; the UI shows a toast when it changes.
    uint32_t ErrorCount() const { return errorCount_; }
    const std::string& LastErrorPack() const { return lastErrorPack_; }

    // Installs a pack folder (a dir with pack.json at its root, or one level below, as zips usually wrap it) into
    // UserRoot()/<id>, replacing an installed pack with the same id (never a built-in one). Validates the manifest and
    // the files first: only pack.json, *.hlsl, *.hlsli, *.png, *.jpg, *.md, *.txt, LICENSE*; no links; total <= 32 MB.
    // `installedFrom` non-empty writes .mmdx_install.json. Rescans. Returns false with a user-facing (Korean) error.
    bool InstallFolder(const std::filesystem::path& src, const std::string& installedFrom, std::string& id,
                       std::string& error, const std::string& installedSha = {});
    // Extracts a .zip into a temp folder (Windows tar.exe, as the updater) and installs it with InstallFolder.
    bool InstallZip(const std::filesystem::path& zip, const std::string& installedFrom, std::string& id, std::string& error);
    // Deletes an installed (User / Online) pack's folder. Built-in packs cannot be removed. Rescans.
    bool Uninstall(const std::string& id, std::string& error);
    // New pack for authors: copies <exe>/shaders/pack_template (effect: pack_template_effect) into UserRoot()/<id> and sets its id / name / author
    // in pack.json. `id` must be valid and unused. Rescans. Returns the new folder.
    bool CreateFromTemplate(const std::string& id, const std::string& name, const std::string& author,
                            std::filesystem::path& dir, std::string& error, bool effect = false);
    // Writes a look pack (apiVersion 6, "type": "look", the LookToJson block) into <folder>/pack.json, creating the
    // folder. Atomic tmp + rename like the other writes. Rescans afterwards.
    bool WriteLookPack(const std::filesystem::path& folder, const std::string& id, const LocalizedText& name,
                       const std::vector<PackAuthor>& authors, const std::string& license, const LookData& look,
                       std::string* error);

    // Per-pack user texture folder: game textures that can't be redistributed live outside the pack. Set from the
    // shader manager screen and saved in the ini (AppSettings::packTextureFolders); empty `dir` = clear. Changing it
    // bumps Generation(), so the pack's textures reload on the next draw. Used from the renderer.
    void SetTextureFolder(const std::string& id, const std::filesystem::path& dir);
    std::filesystem::path TextureFolder(const std::string& id) const;
    // Missing-texture count per (pack id, texture folder) set: ScenePass reports how many of the pack's textures
    // were missing on the last load of that set (white is used); the manager keeps showing the pack-level folder's
    // count. Survives Scan().
    void ReportMissingTextures(const std::string& id, const std::filesystem::path& folder, uint32_t missing);
    uint32_t MissingTextures(const std::string& id, const std::filesystem::path& folder) const;

private:
    std::vector<ShaderPack> packs_;
    // S6: stage-bundled packs ("stage:<stage id>/<folder>"), kept apart so Scan() / PollChanges / the manager never
    // see them. Find() searches both lists.
    std::vector<ShaderPack> stagePacks_;
    uint32_t generation_ = 0;
    uint32_t errorCount_ = 0;
    std::string lastErrorPack_;
    std::map<std::string, std::filesystem::path> textureFolders_;   // pack id -> user texture folder
    std::map<std::string, uint32_t> missingTexById_;                // pack id -> missing texture count (last load)
    std::map<std::string, uint32_t> missingTexBySet_;               // pack id + "\n" + folder -> count per set
    // PollChanges / status carry-over state (implementation detail)
    double nextPoll_ = 0;
    uint64_t stamp_ = 0;
    std::map<std::string, std::pair<uint64_t, PackStatus>> statusById_;   // files stamp -> status
    std::map<std::string, std::string> messageById_;
    std::vector<std::string> revokedHashes_;
};

// True when `stack` has an enabled, selectable effect pack that declares "replaces": ["dof"]: the engine's depth of field
// (DofPass, the offline GI thin lens) is skipped because the pack draws its own.
bool EffectStackReplacesDof(const std::vector<EffectStackEntry>& stack);

// The process-wide registry (scanned on first use).
ShaderPackRegistry& ShaderPacks();

// Valid pack id: [a-z0-9_-]{1,64}.
bool ValidShaderPackId(const std::string& id);

// S6: the registry id of a pack bundled in a stage folder: "stage:<stage id>/<pack folder as given in stage.json>".
// Not a [a-z0-9_-] id (it carries ':' and '/'), which is exactly why it can never collide with an installed pack id
// and is never written into a ShaderChoice / the project: only a manifest's `packs[]` entry names one.
std::string StagePackId(const std::string& stageId, const std::string& folder);

} // namespace mmdx
