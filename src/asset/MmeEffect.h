#pragma once
// M1: MikuMikuEffect (.fx) IR. Parses an effect file (with its #include chain and macro expansion) into a pure-data
// description: global parameters with semantics and annotations, samplers, techniques, passes (render states +
// shader refs), scripts and STANDARDSGLOBAL metadata. No D3D, no rendering policy: only what a later compiler /
// runtime needs. The format itself is documented in docs/mme/research/mme_spec_draft.md (§2 assignment, §3
// techniques, §4 semantics, §6 textures, §7 script, §8 states).
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mmdx {

// A diagnostic attached to a file:line. Parser and preprocessor never abort; they record and continue.
struct MmeDiag {
    enum Severity { Info, Warning, Error };
    Severity sev = Info;
    std::filesystem::path file;
    int line = 0;              // 1-based physical line in `file`
    std::string message;
};

// A typed annotation / initializer value. Vectors and arrays are flattened into `nums`; the element kind is kept
// for the common scalar cases (a float4 initialised from {1,2,3,4} is Float with four numbers).
struct MmeValue {
    enum Kind { Bool, Int, Float, String };
    Kind kind = Float;
    std::vector<float> nums;   // Bool/Int stored as floats too
    std::string str;           // for Kind==String (and raw-text fallbacks)
};

struct MmeAnnotation {
    std::string type;          // as written, e.g. "string", "bool", "float2"
    std::string name;          // as written, e.g. "MMDPass", "ResourceName"
    MmeValue value;
};

// Case-insensitive annotation lookup (MME annotation names are case-insensitive).
const MmeAnnotation* FindAnnotation(const std::vector<MmeAnnotation>& a, std::string_view name);

// One global declaration in the effect file.
struct MmeParam {
    std::string type;          // HLSL spelling, keywords normalised to lower case (float4x4, texture2d, int, ...)
    std::string name;
    std::string semantic;      // UPPER case, empty when the parameter has no semantic
    std::vector<MmeAnnotation> annotations;
    MmeValue init;
    bool hasInit = false;
    int arraySize = 0;         // 0 = scalar
    bool isShared = false;
    bool isStatic = false;
    bool isUniform = false;
    bool isConst = false;
    bool rowMajor = false;      // declared with the `row_major` type modifier
    bool columnMajor = false;   // declared with the `column_major` type modifier
    int line = 0;
    std::filesystem::path file;
};

// A sampler declaration: `sampler2D Name = sampler_state { texture = <T>; ADDRESSU = WRAP; ... };`
struct MmeSampler {
    std::string name;
    std::string type;          // sampler, sampler2d, samplercube, sampler3d
    std::string texture;       // texture name inside <> / () / bare
    std::map<std::string, std::string> states;   // UPPER state name -> raw value text
    std::string registerBinding;   // "s0" when declared `: register(s0)`
    int line = 0;
    std::filesystem::path file;
};

// VertexShader / PixelShader assignment in a pass.
struct MmeShaderRef {
    std::string profile;       // "vs_3_0", "ps_2_0", ...; empty when NULL
    std::string function;      // target function name
    std::vector<std::string> args;   // literal argument texts
    bool isNull = false;
    bool isAsm = false;
};

struct MmePass {
    std::string name;
    std::vector<MmeAnnotation> annotations;
    std::vector<std::pair<std::string, std::string>> states;   // UPPER name, raw value, file order
    MmeShaderRef vs, ps;
    int line = 0;
};

struct MmeTechnique {
    std::string name;
    std::vector<MmeAnnotation> annotations;
    std::vector<MmePass> passes;
    int line = 0;
    // Derived from the annotations (kept so the runtime does not re-scan the annotation list per draw).
    std::string mmdPass;       // lower case; "object" when the annotation is absent
    std::string subset;        // raw Subset text; empty = all subsets
    int useTexture = -1;       // -1 absent, 0 false, 1 true
    int useSphereMap = -1;
    int useToon = -1;
};

// One command of a technique / pass script.
struct MmeScriptCommand {
    std::string command;       // canonical case for the known commands, else the raw text
    std::string arg;
    bool known = false;
};

// Splits an MME script ("A=B; C; ..."): trims, skips empty entries, canonicalises command names case-insensitively.
std::vector<MmeScriptCommand> ParseMmeScript(std::string_view script);

struct MmeEffectDesc {
    std::filesystem::path file;
    std::vector<std::filesystem::path> includes;   // resolved, in first-seen order
    std::vector<MmeParam> params;
    std::vector<MmeSampler> samplers;
    std::vector<MmeTechnique> techniques;
    // STANDARDSGLOBAL, taken from the float parameter carrying that semantic.
    std::string scriptClass;   // lower case, default "object"
    std::string scriptOrder;   // lower case, default "standard"
    std::string scriptOutput;
    std::string globalScript;
    bool hasStandardsGlobal = false;
    std::map<std::string, std::string> defines;   // final macro table: name -> replacement text
    std::string expandedSource;                   // fully preprocessed text (for M2 and debugging)
    std::vector<MmeDiag> diags;
    bool ok = false;           // false only when the file could not be read

    // "post" (scene / sceneorobject postprocess), "pre" (preprocess), "object" otherwise.
    std::string kind() const;
    // True when any parameter carries the OFFSCREENRENDERTARGET semantic.
    bool UsesOffscreen() const;
};

struct MmeLoadOptions {
    std::map<std::string, std::string> predefines;   // e.g. { "MMDX12", "1" }
    int maxIncludeDepth = 16;
};

// Loads and parses an effect file. Never throws; unreadable files leave ok=false and no diags of their own.
MmeEffectDesc LoadMmeEffect(const std::filesystem::path& fx, const MmeLoadOptions& opt = {});

} // namespace mmdx
