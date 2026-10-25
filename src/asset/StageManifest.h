#pragma once
// An MMDX stage: a folder with stage.json ("format": "mmdx-stage", "formatVersion": 1) plus its model parts
// (glTF / GLB / PMX). This header is pure data (no renderer / app includes): StageManifest is the parsed file,
// the evaluator (StageEval.h) turns it into resolved target values, AssetLibrary detects the folder, and
// tools/stage_check validates it. The format reference is docs/stage_format.md.
//
// Everything relative in stage.json is relative to the stage folder. Colours are LINEAR 0..1 (glTF / light space),
// positions and sizes are MMD world units, frames are song frames at 30 fps.
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "studio/SceneLight.h"

namespace mmdx {

inline constexpr const char* kStageFormatName = "mmdx-stage";
inline constexpr uint32_t kStageFormatVersion = 1;
inline constexpr const char* kStageManifestName = "stage.json";

// The UI language as a stage.json key ("ko" / "en" / "ja" / "zh"), from core/I18n.h's ActiveLanguage().
std::string StageLanguage();

// A localized string: either a plain JSON string (stored under "") or {"ko": .., "en": .., ..}. Same shape and
// fallback as render/ShaderPack.h's LocalizedText (the asset layer may not include the render layer, so the type is
// duplicated here; the JSON and the fallback order are identical).
struct StageText {
    std::map<std::string, std::string> byLang;
    // `lang` first, then "en", then "", then "ko", then any non-empty entry.
    const std::string& Get(const std::string& lang) const;
    bool Empty() const;
};

struct StageAuthor {
    std::string name;   // required
    std::string role;   // free text, may be empty
    std::string url;    // http/https only, anything else is dropped
};

// "role": "main" (default), "floor" (the part the stage is moved onto / players stand on) or "props".
enum class StagePartRole : uint8_t { Main = 0, Floor, Props };
const char* StagePartRoleName(StagePartRole role);
bool ParseStagePartRole(const std::string& s, StagePartRole& out);

struct StagePart {
    std::string file;   // relative to the stage folder, required
    std::string name;   // optional; defaults to the file stem
    StagePartRole role = StagePartRole::Main;
};

// One "materials" entry: a material-name pattern (exact or * / ? glob, case-insensitive) and the PBR values it
// overrides. Absent members leave the model's own value alone.
struct StageMaterialRule {
    std::string match;                    // required
    std::optional<DirectX::XMFLOAT4> baseColor;
    std::optional<float> roughness;
    std::optional<float> metallic;
    std::optional<DirectX::XMFLOAT3> emissive;
    std::optional<float> emissiveStrength;
    std::optional<float> transmission;
    std::optional<float> ior;
    std::optional<float> alpha;
    std::optional<bool> mirror;           // planar mirror surface (milestone S10)
    std::optional<std::string> screen;    // a screen id from "screens"
    std::optional<bool> castShadow;
    std::optional<bool> receiveShadow;
};

enum class StageScreenSource : uint8_t { None = 0, Image, Video, Camera, Program };
const char* StageScreenSourceName(StageScreenSource s);
bool ParseStageScreenSource(const std::string& s, StageScreenSource& out);

struct StageScreen {
    std::string id;                      // required, unique
    std::vector<std::string> materials;  // material names / globs of this surface, at least one
    StageScreenSource source = StageScreenSource::None;
    std::string file;                    // image / video file, or program pack folder ("program")
    std::string camera;                  // camera name ("camera")
    uint32_t width = 1920, height = 1080;  // the source resolution (LED pixel grid)
    float brightness = 1.0f;
    bool emitLight = true;               // consumed by S5 / S8; parsed and validated only for now
};

struct StageSocket {
    std::string name;                    // required, unique
    std::string node;                    // glTF node / PMX bone name; empty = the explicit transform below
    DirectX::XMFLOAT3 position{0, 0, 0};
    DirectX::XMFLOAT3 rotationDeg{0, 0, 0};
    DirectX::XMFLOAT3 scale{1, 1, 1};
};

struct StageCamera {
    std::string name;                    // required, unique
    DirectX::XMFLOAT3 position{0, 12, -30};
    DirectX::XMFLOAT3 target{0, 10, 0};
    float fov = 45.0f;                   // vertical, degrees
};

struct StageFog {
    bool present = false;
    float density = 0.02f;
    DirectX::XMFLOAT3 color{0.6f, 0.65f, 0.75f};
    float anisotropy = 0.4f;
    float heightFalloff = 0.0f;
};

struct StageAmbient {
    bool present = false;
    std::optional<DirectX::XMFLOAT3> skyZenith, skyHorizon, ground;
    std::optional<float> intensity;
};

struct StageEnvironment {
    StageFog fog;
    StageAmbient ambient;
};

// A bundled pack folder ("surface" + material globs, "world" + socket, "screen" + screen id) and its initial
// parameters. Parsed and validated only in S1a.
enum class StagePackRole : uint8_t { Surface = 0, World, Screen };
const char* StagePackRoleName(StagePackRole role);
bool ParseStagePackRole(const std::string& s, StagePackRole& out);

struct StagePackRef {
    std::string folder;                  // required, relative to the stage folder, holds pack.json
    StagePackRole role = StagePackRole::Surface;
    std::vector<std::string> materials;  // surface: material names / globs
    std::string socket;                  // world: socket name
    std::string screen;                  // screen: screen id
    std::map<std::string, float> params; // initial parameter values (key -> value)
};

enum class StageParamType : uint8_t { Float = 0, Int, Bool, Color, Enum };
const char* StageParamTypeName(StageParamType t);
bool ParseStageParamType(const std::string& s, StageParamType& out);

// A resolved (or authored) value. Number covers float / int / bool (0 or 1) / enum (the option index);
// Color is rgba; Text is an enum option's "value" string (only produced by the evaluator).
struct StageValue {
    enum class Type : uint8_t { Number = 0, Color, Text };
    Type type = Type::Number;
    float x = 0, y = 0, z = 0, w = 1;   // Number: x; Color: rgba
    std::string text;

    static StageValue Number(float v) { StageValue s; s.x = v; return s; }
    static StageValue Color(const DirectX::XMFLOAT4& c) { StageValue s; s.type = Type::Color; s.x = c.x; s.y = c.y; s.z = c.z; s.w = c.w; return s; }
    static StageValue Text(std::string t) { StageValue s; s.type = Type::Text; s.text = std::move(t); return s; }
    float AsNumber() const { return x; }
    DirectX::XMFLOAT4 AsColor() const { return {x, y, z, w}; }   // Number -> (x, x, x, 1)
    bool operator==(const StageValue&) const = default;
};
using StageParamValues = std::map<std::string, StageValue>;

struct StageParamOption {
    std::string value;      // the JSON / binding string of the option (required, unique per param)
    StageText label;        // empty -> value
};

// A typed, localized, exposed stage parameter. The evaluator resolves one number (or colour) per param; an enum's
// number is its option index.
struct StageParam {
    std::string key;                     // required, unique
    StageParamType type = StageParamType::Float;
    StageText label;                     // empty -> key
    std::string group;                   // optional group id (see StageParamGroup)
    float def = 0.0f;                    // float / int / bool (0|1) / enum (option index)
    float min = 0.0f, max = 1.0f;        // float / int
    DirectX::XMFLOAT4 defColor{1, 1, 1, 1};
    std::vector<StageParamOption> options;  // enum only
    // The default value in StageValue form (Color for colors, Number otherwise).
    StageValue DefaultValue() const;
};

struct StageParamGroup {
    std::string id;
    StageText label;
    bool collapsed = false;
};

struct StageVariant {
    std::string id;                      // required, unique
    StageText label;                     // empty -> id
    StageParamValues values;             // param key -> value
};

// param -> target with an optional scale / offset / remap: out = remap(value) * scale + offset, where remap maps
// [inMin, inMax] onto [outMin, outMax] before scale / offset. Several bindings may hit one target: the evaluator
// emits them in list order and a consumer applies them in that order.
struct StageBinding {
    std::string param;                   // required, a known param key
    std::string target;                  // required, see docs/stage_format.md ("lights.<name>.<field>" ...)
    float scale = 1.0f;
    float offset = 0.0f;
    std::optional<std::array<float, 4>> remap;  // inMin, inMax, outMin, outMax
};

// L0 timeline: one keyed param track on the song timeline (frames at 30 fps).
struct StageCueKey {
    int frame = 0;
    StageValue value;
};
struct StageCue {
    std::string param;                   // required, a known param key
    bool stepped = false;                // "stepped": the last key at or before the frame wins (else linear)
    std::vector<StageCueKey> keys;       // sorted by frame, one key per frame
};

struct StageManifest {
    // identity / presentation
    std::string id;                      // required, [a-z0-9_-]{1,64}
    std::string version;                 // "1.0.0"
    StageText name, description;
    std::vector<StageAuthor> authors;
    std::string license;
    std::string minAppVersion;
    std::vector<std::string> tags;       // lower-case words
    std::vector<std::filesystem::path> previews;   // existing preview images / videos, in declared order
    // content
    std::vector<StagePart> models;
    std::vector<StageMaterialRule> materials;
    std::vector<studio::SceneLight> lights;        // the .mmdxproj "lights" schema; names are the binding keys
    std::vector<StageSocket> sockets;
    std::vector<StageCamera> cameras;
    std::vector<StageScreen> screens;
    StageEnvironment environment;
    std::vector<StagePackRef> packs;
    std::vector<StageParamGroup> groups;
    std::vector<StageParam> params;
    std::vector<StageVariant> variants;
    std::string defaultVariant;          // empty: the first variant (or none)
    std::vector<StageBinding> bindings;
    std::vector<StageCue> cues;
    // where it came from
    std::filesystem::path dir;
    std::filesystem::path manifestPath;

    const StageParam* FindParam(const std::string& key) const;
    const StageVariant* FindVariant(const std::string& id) const;
    const StageScreen* FindScreen(const std::string& id) const;
    const studio::SceneLight* FindLight(const std::string& name) const;
    // The variant the evaluator picks when none is requested (defaultVariant, else the first, else nullptr).
    const StageVariant* DefaultVariant() const;
    // displayName source: the localized name, else the folder name.
    std::string DisplayName(const std::string& lang) const;
};

// ---- parsing ---------------------------------------------------------------------------------------------------------
// Never throws. On failure returns false with `error` set (one line) and `out` holding whatever could be read.
// `dir` is the stage folder (relative paths in the JSON resolve against it); `dir` / `manifestPath` are set on `out`.
bool ParseStageManifest(const std::string& json, const std::filesystem::path& dir, StageManifest& out, std::string& error);
// Reads <dir>/stage.json. False (with `error`) when it is missing or unreadable.
bool LoadStageManifest(const std::filesystem::path& stageJson, StageManifest& out, std::string& error);

// ---- semantic validation (no file-system access) ---------------------------------------------------------------------
// Everything that can be checked from the parsed JSON alone: required / unique ids and names, param and variant
// references, binding target paths and their field types, cue tracks, pack roles, screen sources, glob syntax.
// Returns human-readable English errors (empty = valid). The tool adds the file / model checks.
std::vector<std::string> ValidateStageManifest(const StageManifest& manifest);

// ---- shared helpers --------------------------------------------------------------------------------------------------
// Simple wildcard match: '*' = any run of characters, '?' = one character, ASCII-case-insensitive.
bool StageGlobMatch(const std::string& pattern, const std::string& text);
// True when `pattern` holds a wildcard ('*' or '?').
bool StageGlobIsPattern(const std::string& pattern);
// "path/to/x.glb" -> "x"; empty for an empty path.
std::string StageFileStem(const std::string& file);

// A binding target, split and typed. `ok` false = unknown target (the message says why); used by validation and by
// the evaluator's assignment building.
struct StageTargetSpec {
    enum class Kind : uint8_t { Lights, Materials, Screens, PackParams, Environment };
    enum class ValueType : uint8_t { Float = 0, Vec3, Color, Bool, String, Int };
    Kind kind = Kind::Lights;
    ValueType type = ValueType::Float;
    std::string owner;   // light name / material pattern / screen id / pack index
    std::string field;   // "intensity", "baseColor", "fog.density", ...
    int packIndex = -1;
    bool ok = false;
    std::string error;
};
// Resolves one target path against `manifest` (owner existence included).
StageTargetSpec ResolveStageTarget(const StageManifest& manifest, const std::string& target);

} // namespace mmdx
