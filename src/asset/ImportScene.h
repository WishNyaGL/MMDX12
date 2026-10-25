#pragma once
// Format-neutral scene description shared by the glTF (cgltf) and FBX/OBJ (ufbx) front ends.
// It stays in the source convention: right-handed, +Y up, metres, row-vector matrices
// (v * M, DirectXMath). ImportConvert turns it into a PmxModel in MMD space.
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mmdx {

struct ImpNode {
    std::string name;
    int parent = -1;
    DirectX::XMFLOAT4X4 local{};  // relative to the parent
    DirectX::XMFLOAT4X4 world{};  // filled by ImpScene::UpdateWorld
};

struct ImpTexture {
    std::filesystem::path file;   // resolved file on disk (empty when embedded or missing)
    std::vector<uint8_t> bytes;   // embedded encoded image (PNG/JPG/WebP...)
    std::string label;            // name for logs
};

// S2a: one glTF KHR_texture_transform. Identity (offset 0, scale 1, rotation 0) when the map has none.
struct ImpUvTransform {
    float offsetU = 0.0f, offsetV = 0.0f;
    float scaleU = 1.0f, scaleV = 1.0f;
    float rotation = 0.0f;        // radians
};

struct ImpMaterial {
    std::string name;
    DirectX::XMFLOAT4 baseColor{1, 1, 1, 1};
    DirectX::XMFLOAT3 emissive{0, 0, 0};   // glTF emissiveFactor (linear); the strength is emissiveStrength
    float emissiveStrength = 1.0f;         // KHR_materials_emissive_strength
    int texture = -1;             // into ImpScene::textures
    bool doubleSided = false;
    bool blend = false;           // alpha blend or mask: keep texture alpha
    bool opaque = false;          // declared opaque (glTF alphaMode OPAQUE): texture alpha is not coverage
    // P6: PBR data read from the glTF/FBX material (texture indices into ImpScene::textures). `npr` marks VRM /
    // MToon / KHR_materials_unlit materials: those are cel-shaded on purpose and get no PBR record.
    bool npr = false;
    bool hasMetallicRoughness = false;
    float metallic = 1.0f, roughness = 1.0f;
    int metalRoughTex = -1;       // packed metal-roughness map: G = roughness, B = metalness (glTF)
    int normalTex = -1;
    float normalScale = 1.0f;
    int occlusionTex = -1;
    float occlusionStrength = 1.0f;
    int emissiveTex = -1;
    // S2a: KHR_materials_transmission / ior / volume / clearcoat / specular / sheen and KHR_texture_transform.
    float transmission = 0.0f;
    int transmissionTex = -1;     // R channel
    float ior = 1.5f;
    float volumeThickness = 0.0f;
    int thicknessTex = -1;        // G channel (carried for S2b)
    DirectX::XMFLOAT3 attenuationColor{1, 1, 1};
    float attenuationDistance = 0.0f;   // 0 = infinite
    float clearcoat = 0.0f;
    float clearcoatRoughness = 0.0f;
    float specularFactor = 1.0f;
    DirectX::XMFLOAT3 specularColor{1, 1, 1};
    DirectX::XMFLOAT3 sheenColor{0, 0, 0};
    float sheenRoughness = 0.0f;
    ImpUvTransform baseColorUv, metalRoughUv, normalUv, occlusionUv, emissiveUv, transmissionUv, thicknessUv;
};

// S2a: KHR_lights_punctual, in the source convention (glTF: metres, +Y up, direction along the node's -Z).
struct ImpLight {
    enum class Kind : uint8_t { Point = 0, Spot, Directional };
    Kind kind = Kind::Point;
    std::string name;
    DirectX::XMFLOAT3 position{0, 0, 0};
    DirectX::XMFLOAT3 direction{0, 0, -1};
    DirectX::XMFLOAT3 color{1, 1, 1};
    float intensity = 1.0f;       // candela (point / spot) or lux (directional), as the extension declares
    float range = 0.0f;           // metres, 0 = unlimited
    float innerCone = 0.0f, outerCone = 0.7853982f;   // radians
};

// P6 record rule: a material gets a PBR record unless it is NPR (VRM / MToon / KHR_materials_unlit), or has no PBR
// map and its factors are the glTF defaults (metallic 1, roughness 1) — many exporters leave those untouched.
inline bool ImpMaterialHasPbrData(const ImpMaterial& m) {
    if (m.npr) return false;
    const bool anyMap = m.metalRoughTex >= 0 || m.normalTex >= 0 || m.occlusionTex >= 0 || m.emissiveTex >= 0;
    if (anyMap) return true;
    if (m.emissive.x > 0 || m.emissive.y > 0 || m.emissive.z > 0) return true;
    // S2a: an extension the material carries is PBR data on its own (a glass sphere with default factors).
    if (m.transmission > 0.0f || m.clearcoat > 0.0f || m.specularFactor != 1.0f) return true;
    return m.hasMetallicRoughness && (m.metallic < 0.999f || m.roughness < 0.999f);
}

struct ImpMorphTarget {
    std::string name;
    std::vector<uint32_t> vertices;              // mesh-local vertex indices
    std::vector<DirectX::XMFLOAT3> offsets;      // position deltas in mesh space
};

// One drawable: a triangle list with a single material, instanced at `node`.
struct ImpMesh {
    std::string name;
    int node = -1;
    int skin = -1;
    int material = -1;
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<DirectX::XMFLOAT3> normals;      // may be empty (generated later)
    std::vector<DirectX::XMFLOAT2> uvs;          // may be empty
    std::vector<std::array<int, 4>> joints;      // indices into ImpSkin::joints, -1 unused
    std::vector<DirectX::XMFLOAT4> weights;
    std::vector<uint32_t> indices;
    std::vector<ImpMorphTarget> morphs;
    std::string morphGroup;                      // key shared by primitives of the same source mesh
};

struct ImpSkin {
    std::vector<int> joints;                          // node indices
    std::vector<DirectX::XMFLOAT4X4> inverseBind;     // mesh space -> joint space at bind
};

// A named expression assembled from morph targets (VRM presets).
struct ImpExpression {
    std::string preset;           // normalised VRM preset: aa ih ou ee oh blink blinkLeft blinkRight happy ...
    struct Bind { std::string morphGroup; int target = 0; float weight = 1; };
    std::vector<Bind> binds;
};

struct ImpScene {
    std::string name;             // model / scene title (may be empty)
    std::vector<ImpNode> nodes;
    std::vector<ImpMesh> meshes;
    std::vector<ImpSkin> skins;
    std::vector<ImpMaterial> materials;
    std::vector<ImpTexture> textures;
    std::vector<ImpLight> lights;            // S2a: KHR_lights_punctual (empty when the file has none)
    std::map<std::string, int> humanBones;   // VRM humanoid slot (VRM 1.0 names) -> node
    std::vector<ImpExpression> expressions;
    std::vector<std::string> warnings;
    std::string unsupported;      // non-empty: the file cannot be shown (e.g. Draco compression)

    void UpdateWorld();           // any node order
};

// Front ends. Return false only when the file cannot be parsed; `scene.unsupported` reports
// files that parse but cannot be shown.
bool ImportGltf(const std::filesystem::path& path, ImpScene& scene, std::string* error);
bool ImportUfbx(const std::filesystem::path& path, ImpScene& scene, std::string* error);

} // namespace mmdx
