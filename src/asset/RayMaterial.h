#pragma once
// Ray-MMD material data, read from a model's MikuMikuEffect ".emd" (.fx assignments per PMX material) and the
// per-material ".fx" (a #define table of map slots and values that feeds Ray-MMD's G-buffer encoder). This is the
// asset layer of the 2.0.0 PBR work: it produces raw descs, it never renders and never interprets Ray-MMD's
// shading semantics (the RayMaterialDesc stores the raw FROM / TYPE / ... integers).
//
// It follows only the references the asset files themselves carry; it never guesses from folder or file names.
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mmdx {

// The Ray-MMD map slots (material_common_2.0.fxsub). MATCAP is deliberately not a slot here (it is outside the
// 2.0.0 PBR block); it survives in RayMaterialDesc::defines like any other unknown key.
enum class RaySlot : int {
    Albedo, AlbedoSub, Alpha, Normal, NormalSub, Smoothness, Metalness, Specular, Occlusion, Parallax,
    Emissive, CustomA, CustomB, Count
};
constexpr int kRaySlotCount = (int)RaySlot::Count;
const char* RaySlotName(RaySlot slot);

// One map slot as declared. All integer fields are the raw Ray-MMD values; `value` is the slot's declared scalar
// (SMOOTHNESS_VALUE, ...) or float3 (ALBEDO, SPECULAR, EMISSIVE, CUSTOM_B, ALBEDO_SUB) as written.
struct RayMapSlot {
    int from = 0;              // <PREFIX>_MAP_FROM: 0 none (value), 1 file, 2 animated file, 3 PMX material tex,
                               // 4 sphere map, 5 toon map, 6 screen dummy, 7/8 material-tex variants
    int type = 0;              // <PREFIX>_MAP_TYPE
    int swizzle = 0;           // <PREFIX>_MAP_SWIZZLE
    int uvFlip = 0;            // <PREFIX>_MAP_UV_FLIP
    int applyScale = 0;        // <PREFIX>_MAP_APPLY_SCALE
    int colorFlip = 0;         // <PREFIX>_MAP_COLOR_FLIP (where present)
    bool fileDeclared = false; // <PREFIX>_MAP_FILE was present and non-empty
    std::filesystem::path file; // resolved existing file (only for from == 1 / 2); empty otherwise
    bool fileMissing = false;  // a real file was declared for from == 1 / 2 but did not resolve
    bool hasUvScale = false;   // <PREFIX>_MAP_UV_SCALE present
    float uvScaleU = 1, uvScaleV = 1;   // <PREFIX>_MAP_UV_SCALE (broadcast when scalar)
    float loopU = 1, loopV = 1;         // const float2 <name>MapLoopNum (texture repeat count)
    bool hasScale = false;              // a *MapScale float exists for this slot
    float scale = 0;                    // normalMapScale / parallaxMapScale / ...
    bool hasValue = false;              // the slot's const float / float3 value was parsed
    int valueComponents = 0;            // 0 unparsed, 1 scalar, 3 float3
    float value[3] = {0, 0, 0};         // value1 / float3 as declared
    bool hasEnable = false;             // <PREFIX>_ENABLE was defined
    bool enabled = false;               // its value != 0
    int applyMorphColor = 0;            // <PREFIX>_MAP_APPLY_MORPH_COLOR
    int applyDiffuse = 0;               // ALBEDO_MAP_APPLY_DIFFUSE
    int applyMorphIntensity = 0;        // EMISSIVE_MAP_APPLY_MORPH_INTENSITY
    int applyBlink = 0;                 // EMISSIVE_MAP_APPLY_BLINK
};

// One PMX material's Ray data. `valid` means a Ray .fx was found and parsed; `ray` is the content rule result for
// the material's own .fx. A material whose entry is "none" / hidden / unassigned / non-Ray keeps valid = false.
struct RayMaterialDesc {
    bool valid = false;
    bool ray = false;
    std::filesystem::path fx;                 // the material .fx the .emd assigned (may be empty)
    int shadingModel = 0;                     // CUSTOM_ENABLE
    RayMapSlot slots[kRaySlotCount];
    std::map<std::string, std::string> defines;   // the raw #define table (post-preprocess), unknown keys survive
};

// Aggregate parse statistics. The per-.fx vectors collect distinct files so a probe can merge them across models
// and report library-unique counts; the integer counters are summed per call.
struct RayParseStats {
    int emdRead = 0;
    int fxRead = 0, fxFailed = 0, fxRay = 0;   // distinct material .fx parsed, unreadable, Ray (this call)
    int missingFiles = 0;                      // from == 1 / 2 files that did not resolve
    std::vector<std::filesystem::path> parsedFx;   // distinct parsed material .fx
    std::vector<std::filesystem::path> failedFx;   // distinct unreadable material .fx
    std::vector<std::filesystem::path> rayFx;      // distinct Ray material .fx (subset of parsedFx)
    std::map<std::string, int> unknownDirectives;  // unknown / unparsed preprocessor directive -> count
};

// A candidate ".emd" for a model, ranked. Never selected by name.
struct RayEmdCandidate {
    std::filesystem::path path;
    int objCount = 0;          // non-none Obj / Obj[n] entries
    int rayEntries = 0;        // entries whose .fx is a Ray material (content rule)
    int inRangeEntries = 0;    // indexed entries whose material index < pmxMaterialCount
    int raySharePct = 0;       // rayEntries * 100 / objCount
    bool readFailed = false;
};

// Every ".emd" in the model's folder and its subfolders (bounded depth, default 3) whose Obj entries are readable.
// Sorted best-first: Ray-material share, then in-range Obj indices, then path (stable tie-break).
std::vector<RayEmdCandidate> FindRayEmdCandidates(const std::filesystem::path& modelFile, int pmxMaterialCount,
                                                  int maxDepth = 3);

// Parses one .emd + PMX material count into per-material descs (resized to pmxMaterialCount). Index n takes Obj[n]
// when present, else the default Obj, else nothing. Returns false only when the .emd itself is unreadable.
bool ParseRayEmd(const std::filesystem::path& emd, int pmxMaterialCount, std::vector<RayMaterialDesc>& out,
                 RayParseStats* stats = nullptr, std::string* error = nullptr);

// Picks the best candidate (FindRayEmdCandidates) and parses it. `chosenEmd` receives the .emd used, if any.
bool FindAndParseRayMaterials(const std::filesystem::path& modelFile, int pmxMaterialCount,
                              std::vector<RayMaterialDesc>& out, std::filesystem::path* chosenEmd = nullptr,
                              RayParseStats* stats = nullptr, std::string* error = nullptr);

// Content rule: true when the .fx (or, transitively, a file it #includes) defines Ray-MMD material macros
// (*_MAP_FROM / CUSTOM_ENABLE) or contains MaterialParam / EncodeGbuffer. No name is consulted.
bool IsRayMaterialFx(const std::filesystem::path& fx);

} // namespace mmdx
