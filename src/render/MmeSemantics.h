#pragma once
// M3b: MME semantic binder (CPU only). Resolves every `$Globals` variable of a compiled stage against the effect's
// M1 parameter semantics (spec draft §4.1, §4.6, §4.8; Appendix B matrix spellings) and writes a fixed
// MmeFrameValues into a staging buffer initialised from the variables' reflection default values. No D3D12 objects
// live here, so the binder is testable without a GPU.
#include "asset/MmeEffect.h"
#include "render/MmeCompiler.h"
#include "render/RenderTypes.h"

#include <DirectXMath.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mmdx {

class GpuModel;
struct MmeSceneObject;

// The controller accessory panel of the effect (M3_behaviour "CONTROLOBJECT"): CONTROLOBJECT `(self)` reads these.
// Defined in render/RenderTypes.h (structural type for the public FrameView API).

// Everything the binder needs from one frame. Filled by the pass from FrameView / RenderTargets; no clocks in here.
struct MmeFrameValues {
    DirectX::XMFLOAT4X4 view{}, proj{};       // camera, no jitter
    DirectX::XMFLOAT3 cameraPos{}, cameraDir{};
    DirectX::XMFLOAT3 lightDir{0, -1, 0};     // from light toward scene, normalised
    DirectX::XMFLOAT3 lightColor{1, 1, 1};    // MMD gamma space
    // M5a fix 2: the MMD self-shadow light camera, computed once per frame and passed in so the zplot pass and
    // every object_ss draw of one frame use the same matrices. Orthographic, centred on the scene's characters,
    // depth range 2000 units (near = 1, far = 2001). Filled by the caller via BuildLightCamera (below).
    DirectX::XMFLOAT4X4 lightView{}, lightProj{};
    float viewportW = 0, viewportH = 0;
    // TIME / ELAPSEDTIME: (time, elapsed) for SyncInEditMode=false, (sceneTime, sceneElapsed) for true. The caller
    // fills all four; the binder just picks.
    float time = 0, elapsed = 0, sceneTime = 0, sceneElapsed = 0;
    MmeControllerPanel self;                  // the effect's controller accessory
    // M5a: the owner effect's controller panel for `(OffscreenOwner)` inside an offscreen sub-effect draw.
    MmeControllerPanel offscreenOwner;
    // Other objects for CONTROLOBJECT: name (file name, lower case) -> lookup. May be empty.
    // `kind`: 0 = bool, 1 = scalar float, 2 = vector (float3/float4), 3 = matrix (float4x4). `out` holds up to 16
    // floats (a float4x4 fills all 16); `outCount` receives the number of floats written. Returns false when the
    // object / item could not be resolved (the binder then uses the spec's missing-object defaults).
    std::function<bool(const std::string& name, const std::string& item, int kind, float out[16], int& outCount)>
        lookup;
    // M7 fix 5: TEXTUREVALUE provider. Called with the parameter name; returns a pointer to `width*height` float4s
    // (packed RGBA8, no mip) for the named texture, or null when unavailable (the binder then keeps the default).
    // `outCount` receives the number of float4s. The array may be longer than the parameter's element count.
    std::function<const float*(const std::string& name, uint32_t& outCount)> textureValue;
};

// M5a fix 2: builds the MMD self-shadow light camera used for every `<Object="Light">` matrix and by the shadow
// map pass. Orthographic, centred on the scene's characters (non-stage `models`): their POSED bone positions from
// `objects` (FrameView::mmeObjects; bind-pose bounds do not follow the dance and outlier vertices blow them up),
// else their bind-pose bounds, else the origin with 25 units half-size; xy half-extent = the light-space extent + a
// skin margin, depth range 2000 (near = 1, far = 2001, the characters around depth ~1000). Computed once per frame
// and stored in MmeFrameValues::lightView/lightProj.
void BuildLightCamera(const DirectX::XMFLOAT3& lightDir, const std::vector<const GpuModel*>& models,
                      const std::vector<MmeSceneObject>& objects, DirectX::XMFLOAT4X4& view,
                      DirectX::XMFLOAT4X4& proj);

// M5c: the accessory world matrix of a controller panel: Scaling(si * 10) * RotationRollPitchYaw(rx, ry, rz) *
// Translation(x, y, z), row-vector (v * M). Shared by the binder's WORLD / CONTROLOBJECT values and the effect
// object's mesh draw, so the effect's WORLD matrix and the mesh it draws agree (docs/mme/mme_design.md §2.9).
DirectX::XMMATRIX MmeAccessoryWorld(const MmeControllerPanel& panel);

// M4b: per-draw object values for an MME object effect (docs/mme/mme_design.md §2.6, spec draft §4.2, §5). Passed
// alongside MmeFrameValues so the binder's material / by-name providers read the current material instead of the
// controller accessory. Fill it per material; the matrices (WORLD = identity, WORLDVIEWPROJECTION = the scene's
// jittered viewProj) come through MmeFrameValues (view / proj) as the caller sets them for the object draw.
struct MmeObjectValues {
    DirectX::XMFLOAT4 diffuse{1, 1, 1, 1};      // Geometry DIFFUSE = material diffuse rgba
    DirectX::XMFLOAT4 ambient{0, 0, 0, 1};      // Geometry AMBIENT = material diffuse rgb (MMD quirk)
    DirectX::XMFLOAT4 specular{0, 0, 0, 1};     // material specular
    float specularPower = 0.0f;                 // material specular power
    DirectX::XMFLOAT4 emissive{0, 0, 0, 1};     // Geometry EMISSIVE = material ambient rgb (MMD quirk)
    DirectX::XMFLOAT4 toonColor{1, 1, 1, 1};    // bottom-left texel of the material's toon texture
    DirectX::XMFLOAT4 edgeColor{0, 0, 0, 1};    // material edge colour
    // ADDINGTEXTURE / MULTIPLYINGTEXTURE / spheres, from MaterialConstants::texAdd/texMul/sphereAdd/sphereMul.
    DirectX::XMFLOAT4 texAdd{0, 0, 0, 0}, texMul{1, 1, 1, 1}, sphereAdd{0, 0, 0, 0}, sphereMul{1, 1, 1, 1};
    // By-name uniforms (spec draft §5).
    bool useTexture = false, useSpheremap = false, useSubtexture = false, useToon = true;
    bool spadd = false, parthf = false, transp = false, opadd = false;
};

// One cbuffer variable resolved by MmeConstantWriter::Bind, kept for tracing / tests.
struct MmeBoundVar {
    std::string name;
    std::string semantic;      // UPPER case, empty when the parameter has no semantic
    uint32_t offset = 0, size = 0;
    uint32_t rows = 0, columns = 0, elements = 0;
    bool columnMajor = false;
    MmeCbVar::Kind kind = MmeCbVar::Other;
    bool hasSemantic = false;
    // Internal provider description (public so the test can inspect it; not part of the stable contract).
    int provider = 0;
    int matrixBase = 0, matrixSuffix = 0;
    bool objectLight = false;
    bool objectGeometry = false;   // M4b: Material="Geometry" colour / power (object effects)
    int colorKind = 0;
    int posDirKind = 0;
    int byName = 0;                // M4b: spec §5 by-name uniform (Provider == PByName)
    bool syncInEditMode = false;
    std::string coName, coItem;
};

// One stage's constant staging buffer: initialised from MmeCbVar::defaultValue, then semantics overwrite.
class MmeConstantWriter {
public:
    // Resolves every cbuffer variable of `stage` against `fx` params (by name) once: semantic, annotations
    // (Object, name, item, SyncInEditMode), type -> a provider id. Unknown semantics keep the default (logged once
    // into `diags`).
    void Bind(const MmeEffectDesc& fx, const MmeStage& stage, std::vector<MmeDiag>& diags);
    void Write(const MmeFrameValues& v);                  // all frame-level providers
    // M4b: writes the per-material / by-name providers from `o` on top of the frame values (object effects only).
    void WriteObject(const MmeObjectValues& o);

    // Script-driven overrides by param name (LoopGetIndex): set a float/int value; returns false if not in this stage.
    bool SetScalar(const std::string& param, float value);

    // Reads a float4/float param's current staging value by name (ClearSetColor / ClearSetDepth / LoopByCount);
    // falls back to the M1 initializer when the variable is not in this stage's cbuffer (it may be unused by the shader).
    static bool ReadParam(const MmeEffectDesc& fx, const std::vector<MmeConstantWriter*>& writers,
                          const std::string& param, float out[4]);

    const std::vector<uint8_t>& Bytes() const { return bytes_; }
    const std::vector<MmeBoundVar>& Vars() const { return vars_; }
    // Reads up to 4 floats of a bound variable's current staging value (for tracing). False when absent.
    bool ReadValue(const std::string& name, float out[4]) const;

private:
    void WriteControlObject(const MmeBoundVar& bv, const MmeFrameValues& v);

    std::vector<uint8_t> bytes_;
    std::vector<MmeBoundVar> vars_;   // parallel to the stage cbuffer order; bytes_ empty when the stage has no $Globals
};

} // namespace mmdx
