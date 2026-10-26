// glTF 2.0 / GLB / VRM (0.x and 1.0) front end: cgltf -> ImpScene.
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "asset/BinaryReader.h"
#include "asset/ImportScene.h"
#include "core/TextUtil.h"

#include <json.hpp>

#include <cstdlib>
#include <cstring>

namespace mmdx {

using namespace DirectX;

namespace {

// cgltf opens files with fopen(char*), which cannot reach non-ASCII paths on Windows.
cgltf_result ReadFileUtf8(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size,
                          void** data) {
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(Utf8ToPath(path), bytes, nullptr)) return cgltf_result_file_not_found;
    void* mem = std::malloc(bytes.empty() ? 1 : bytes.size());
    if (!mem) return cgltf_result_out_of_memory;
    if (!bytes.empty()) std::memcpy(mem, bytes.data(), bytes.size());
    *size = bytes.size();
    *data = mem;
    return cgltf_result_success;
}

void ReleaseFile(const cgltf_memory_options*, const cgltf_file_options*, void* data) { std::free(data); }

// S11: MSFT_texture_dds (Blender's "Microsoft DDS" export) replaces a texture's image with a DDS one: the extension
// on the texture names the index of another image ("source"), which carries the .dds path / bytes. cgltf exposes it
// as raw JSON in cgltf_texture::extensions.
const cgltf_image* TextureDdsImage(const cgltf_data* data, const cgltf_texture* tex) {
    if (!tex || !tex->extensions) return nullptr;
    for (cgltf_size e = 0; e < tex->extensions_count; ++e) {
        const cgltf_extension& ext = tex->extensions[e];
        if (!ext.name || !ext.data) continue;
        if (std::strcmp(ext.name, "MSFT_texture_dds") != 0) continue;
        nlohmann::json j = nlohmann::json::parse(ext.data, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return nullptr;
        auto it = j.find("source");
        if (it == j.end() || !it->is_number_integer()) return nullptr;
        const int64_t index = it->get<int64_t>();
        if (index < 0 || (cgltf_size)index >= data->images_count) return nullptr;
        return &data->images[index];
    }
    return nullptr;
}

// The image a texture samples: its DDS replacement when MSFT_texture_dds names one, else the plain / WebP image.
const cgltf_image* TextureImage(const cgltf_data* data, const cgltf_texture* tex) {
    if (!tex) return nullptr;
    if (const cgltf_image* dds = TextureDdsImage(data, tex)) return dds;
    return tex->image ? tex->image : (tex->has_webp ? tex->webp_image : nullptr);
}

XMFLOAT4X4 ToRowMatrix(const float m[16]) {
    // glTF stores column-major matrices for column vectors; read in order, that is exactly the
    // row-major matrix for row vectors (the transpose).
    XMFLOAT4X4 r;
    std::memcpy(&r, m, sizeof(float) * 16);
    return r;
}

const char* ResultName(cgltf_result r) {
    switch (r) {
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "unknown format";
    case cgltf_result_invalid_json: return "invalid json";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found";
    case cgltf_result_io_error: return "io error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "glTF 1.0 is not supported";
    default: return "error";
    }
}

std::vector<float> Unpack(const cgltf_accessor* a) {
    std::vector<float> v;
    if (!a) return v;
    v.resize(cgltf_accessor_unpack_floats(a, nullptr, 0));
    cgltf_accessor_unpack_floats(a, v.data(), v.size());
    return v;
}

// S2a: a texture view's KHR_texture_transform (identity when the map has none).
ImpUvTransform UvTransformOf(const cgltf_texture_view& v) {
    ImpUvTransform out;
    if (!v.has_transform) return out;
    out.offsetU = v.transform.offset[0];
    out.offsetV = v.transform.offset[1];
    out.scaleU = v.transform.scale[0];
    out.scaleV = v.transform.scale[1];
    out.rotation = v.transform.rotation;
    return out;
}

// VRM 0.x humanoid slot and expression names -> VRM 1.0 names.
std::string Vrm0Bone(const std::string& b) {
    static const std::pair<const char*, const char*> kThumb[] = {
        {"leftThumbProximal", "leftThumbMetacarpal"}, {"leftThumbIntermediate", "leftThumbProximal"},
        {"rightThumbProximal", "rightThumbMetacarpal"}, {"rightThumbIntermediate", "rightThumbProximal"},
    };
    for (auto& [from, to] : kThumb)
        if (b == from) return to;
    return b;
}

std::string Vrm0Preset(std::string p) {
    p = ToLowerAscii(p);
    static const std::pair<const char*, const char*> kMap[] = {
        {"a", "aa"}, {"i", "ih"}, {"u", "ou"}, {"e", "ee"}, {"o", "oh"}, {"blink", "blink"},
        {"blink_l", "blinkLeft"}, {"blink_r", "blinkRight"}, {"joy", "happy"}, {"angry", "angry"},
        {"sorrow", "sad"}, {"fun", "relaxed"},
    };
    for (auto& [from, to] : kMap)
        if (p == from) return to;
    return {};
}

void ReadVrm(const cgltf_data* data, ImpScene& scene) {
    const auto meshIndexOfNode = [&](int node) -> int {
        if (node < 0 || (size_t)node >= data->nodes_count || !data->nodes[node].mesh) return -1;
        return (int)(data->nodes[node].mesh - data->meshes);
    };
    for (cgltf_size i = 0; i < data->data_extensions_count; ++i) {
        const cgltf_extension& ext = data->data_extensions[i];
        if (!ext.name || !ext.data) continue;
        const std::string name = ext.name;
        if (name != "VRMC_vrm" && name != "VRM") continue;
        nlohmann::json j = nlohmann::json::parse(ext.data, nullptr, false);
        if (j.is_discarded()) {
            scene.warnings.push_back("unreadable " + name + " extension");
            continue;
        }
        if (name == "VRMC_vrm") {
            if (j.contains("meta") && j["meta"].contains("name") && j["meta"]["name"].is_string())
                scene.name = j["meta"]["name"].get<std::string>();
            if (j.contains("humanoid") && j["humanoid"].contains("humanBones") && j["humanoid"]["humanBones"].is_object())
                for (auto& [slot, v] : j["humanoid"]["humanBones"].items())
                    if (v.is_object() && v.contains("node") && v["node"].is_number_integer())
                        scene.humanBones[slot] = v["node"].get<int>();
            if (j.contains("expressions") && j["expressions"].contains("preset") && j["expressions"]["preset"].is_object())
                for (auto& [preset, v] : j["expressions"]["preset"].items()) {
                    ImpExpression e;
                    e.preset = preset;
                    if (v.contains("morphTargetBinds") && v["morphTargetBinds"].is_array())
                        for (auto& b : v["morphTargetBinds"]) {
                            const int mesh = meshIndexOfNode(b.value("node", -1));
                            if (mesh < 0) continue;
                            e.binds.push_back({"m" + std::to_string(mesh), b.value("index", 0), b.value("weight", 1.0f)});
                        }
                    if (!e.binds.empty()) scene.expressions.push_back(std::move(e));
                }
        } else {
            if (j.contains("meta") && j["meta"].contains("title") && j["meta"]["title"].is_string())
                scene.name = j["meta"]["title"].get<std::string>();
            if (j.contains("humanoid") && j["humanoid"].contains("humanBones") && j["humanoid"]["humanBones"].is_array())
                for (auto& b : j["humanoid"]["humanBones"])
                    if (b.contains("bone") && b["bone"].is_string() && b.contains("node") && b["node"].is_number_integer())
                        scene.humanBones[Vrm0Bone(b["bone"].get<std::string>())] = b["node"].get<int>();
            if (j.contains("blendShapeMaster") && j["blendShapeMaster"].contains("blendShapeGroups") &&
                j["blendShapeMaster"]["blendShapeGroups"].is_array())
                for (auto& g : j["blendShapeMaster"]["blendShapeGroups"]) {
                    ImpExpression e;
                    e.preset = Vrm0Preset(g.value("presetName", std::string()));
                    if (e.preset.empty()) continue;
                    if (g.contains("binds") && g["binds"].is_array())
                        for (auto& b : g["binds"]) {
                            const int mesh = b.value("mesh", -1);
                            if (mesh < 0) continue;
                            e.binds.push_back({"m" + std::to_string(mesh), b.value("index", 0), b.value("weight", 100.0f) / 100.0f});
                        }
                    if (!e.binds.empty()) scene.expressions.push_back(std::move(e));
                }
        }
    }
}

int ImportImage(const cgltf_data* data, const cgltf_image* img, const std::filesystem::path& dir, ImpScene& scene,
                std::map<const cgltf_image*, int>& cache) {
    if (!img) return -1;
    if (auto it = cache.find(img); it != cache.end()) return it->second;
    ImpTexture t;
    t.label = img->name ? img->name : (img->uri && std::strncmp(img->uri, "data:", 5) != 0 ? img->uri : "embedded");
    if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data) {
        const uint8_t* p = (const uint8_t*)img->buffer_view->buffer->data + img->buffer_view->offset;
        t.bytes.assign(p, p + img->buffer_view->size);
    } else if (img->uri && std::strncmp(img->uri, "data:", 5) == 0) {
        const char* comma = std::strchr(img->uri, ',');
        if (comma && std::strstr(img->uri, ";base64")) {
            size_t len = std::strlen(comma + 1);
            size_t padding = 0;
            while (len > 0 && comma[len] == '=') --len, ++padding;
            const size_t bytes = (len + padding) / 4 * 3 - padding;
            void* out = nullptr;
            cgltf_options opts{};
            if (bytes > 0 && cgltf_load_buffer_base64(&opts, bytes, comma + 1, &out) == cgltf_result_success && out) {
                t.bytes.assign((uint8_t*)out, (uint8_t*)out + bytes);
                std::free(out);
            }
        }
    } else if (img->uri) {
        std::string uri = img->uri;
        uri.resize(cgltf_decode_uri(uri.data()));
        t.file = dir / Utf8ToPath(uri);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(t.file, ec)) {
            scene.warnings.push_back("texture not found: " + uri);
            t.file.clear();
        }
    }
    (void)data;
    const int index = (int)scene.textures.size();
    scene.textures.push_back(std::move(t));
    cache[img] = index;
    return index;
}

} // namespace

bool ImportGltf(const std::filesystem::path& path, ImpScene& scene, std::string* error) {
    std::vector<uint8_t> fileData;
    std::string readErr;
    if (!ReadWholeFile(path, fileData, &readErr)) {
        if (error) *error = readErr;
        return false;
    }
    cgltf_options opts{};
    opts.file.read = ReadFileUtf8;
    opts.file.release = ReleaseFile;
    cgltf_data* data = nullptr;
    cgltf_result r = cgltf_parse(&opts, fileData.data(), fileData.size(), &data);
    if (r != cgltf_result_success) {
        if (error) *error = ResultName(r);
        return false;
    }
    struct Guard {
        cgltf_data* d;
        ~Guard() { cgltf_free(d); }
    } guard{data};

    for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
        const std::string ext = data->extensions_required[i];
        if (ext == "KHR_draco_mesh_compression" || ext == "EXT_meshopt_compression" || ext == "KHR_mesh_quantization_unsupported") {
            scene.unsupported = ext + " is not supported";
            return true;
        }
    }

    r = cgltf_load_buffers(&opts, data, PathToUtf8(path).c_str());
    if (r != cgltf_result_success) {
        if (error) *error = std::string("buffers: ") + ResultName(r);
        return false;
    }

    // Nodes (index order; UpdateWorld handles any parent order).
    scene.nodes.resize(data->nodes_count);
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& n = data->nodes[i];
        ImpNode& out = scene.nodes[i];
        out.name = n.name ? n.name : "node" + std::to_string(i);
        out.parent = n.parent ? (int)(n.parent - data->nodes) : -1;
        float m[16];
        cgltf_node_transform_local(&n, m);
        out.local = ToRowMatrix(m);
    }
    // glTF scene names ("Scene", "Root Scene") say nothing; only VRM meta titles are used.

    // Skins.
    for (cgltf_size i = 0; i < data->skins_count; ++i) {
        const cgltf_skin& s = data->skins[i];
        ImpSkin out;
        for (cgltf_size j = 0; j < s.joints_count; ++j) out.joints.push_back((int)(s.joints[j] - data->nodes));
        std::vector<float> ibm = Unpack(s.inverse_bind_matrices);
        out.inverseBind.resize(s.joints_count);
        for (cgltf_size j = 0; j < s.joints_count; ++j) {
            if (ibm.size() >= (j + 1) * 16) out.inverseBind[j] = ToRowMatrix(&ibm[j * 16]);
            else XMStoreFloat4x4(&out.inverseBind[j], XMMatrixIdentity());
        }
        scene.skins.push_back(std::move(out));
    }

    // S2a: KHR_lights_punctual. One entry per node that references a light; the position / direction come from the
    // node's world transform (the source convention: metres, +Y up, the light shines along its node's -Z). The asset
    // layer converts metres / axes to engine units later (asset/StageLights.h), so this stays format-neutral.
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& node = data->nodes[i];
        if (!node.light) continue;
        const cgltf_light& l = *node.light;
        ImpLight out;
        out.name = node.name ? node.name : (l.name ? l.name : "light" + std::to_string(i));
        switch (l.type) {
        case cgltf_light_type_directional: out.kind = ImpLight::Kind::Directional; break;
        case cgltf_light_type_spot: out.kind = ImpLight::Kind::Spot; break;
        default: out.kind = ImpLight::Kind::Point; break;
        }
        float m[16];
        cgltf_node_transform_world(&node, m);
        const XMFLOAT4X4 world = ToRowMatrix(m);
        XMFLOAT3 origin, zero;
        XMStoreFloat3(&origin, XMVector3TransformCoord(XMVectorZero(), XMLoadFloat4x4(&world)));
        XMStoreFloat3(&zero, XMVector3TransformNormal(XMVectorSet(0, 0, -1, 0), XMLoadFloat4x4(&world)));
        out.position = origin;
        const XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&zero));
        XMStoreFloat3(&out.direction, d);
        out.color = {l.color[0], l.color[1], l.color[2]};
        out.intensity = l.intensity;
        out.range = l.range;
        out.innerCone = l.spot_inner_cone_angle;
        out.outerCone = l.spot_outer_cone_angle;
        scene.lights.push_back(std::move(out));
    }

    // Materials and textures.
    const std::filesystem::path dir = path.parent_path();
    std::map<const cgltf_image*, int> imageCache;
    const bool isVrm = ToLowerAscii(PathToUtf8(path.extension())) == ".vrm";
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& m = data->materials[i];
        ImpMaterial out;
        out.name = m.name ? m.name : "material" + std::to_string(i);
        const cgltf_texture* tex = nullptr;
        if (m.has_pbr_metallic_roughness) {
            const float* c = m.pbr_metallic_roughness.base_color_factor;
            out.baseColor = {c[0], c[1], c[2], c[3]};
            tex = m.pbr_metallic_roughness.base_color_texture.texture;
            out.baseColorUv = UvTransformOf(m.pbr_metallic_roughness.base_color_texture);
        } else if (m.has_pbr_specular_glossiness) {
            const float* c = m.pbr_specular_glossiness.diffuse_factor;
            out.baseColor = {c[0], c[1], c[2], c[3]};
            tex = m.pbr_specular_glossiness.diffuse_texture.texture;
            out.baseColorUv = UvTransformOf(m.pbr_specular_glossiness.diffuse_texture);
        }
        if (tex) out.texture = ImportImage(data, TextureImage(data, tex), dir, scene, imageCache);
        out.emissive = {m.emissive_factor[0], m.emissive_factor[1], m.emissive_factor[2]};
        if (m.has_emissive_strength) out.emissiveStrength = m.emissive_strength.emissive_strength;
        out.doubleSided = m.double_sided;
        out.blend = m.alpha_mode != cgltf_alpha_mode_opaque;
        out.opaque = !out.blend;

        // P6: metallic-roughness / normal / occlusion / emissive maps. These are the SAME cgltf images as the base
        // colour, so the shared image cache dedupes them into ImpScene::textures.
        auto importView = [&](const cgltf_texture_view& v, ImpUvTransform* uvOut = nullptr) {
            if (uvOut) *uvOut = UvTransformOf(v);
            const cgltf_texture* t = v.texture;
            if (!t) return -1;
            if (v.texcoord != 0) {   // only UV0 is supported
                scene.warnings.push_back("ignored a PBR map on UV" + std::to_string((int)v.texcoord) + " of " + out.name);
                return -1;
            }
            return ImportImage(data, TextureImage(data, t), dir, scene, imageCache);
        };
        // VRM / MToon / KHR_materials_unlit are NPR on purpose: never give them glossy PBR data.
        if (isVrm || m.unlit) out.npr = true;
        for (cgltf_size e = 0; e < m.extensions_count; ++e)
            if (m.extensions[e].name && std::string(m.extensions[e].name).find("mtoon") != std::string::npos) out.npr = true;
        if (!out.npr) {
            if (m.has_pbr_metallic_roughness) {
                out.hasMetallicRoughness = true;
                out.metallic = m.pbr_metallic_roughness.metallic_factor;
                out.roughness = m.pbr_metallic_roughness.roughness_factor;
                out.metalRoughTex = importView(m.pbr_metallic_roughness.metallic_roughness_texture, &out.metalRoughUv);
            }
            out.normalTex = importView(m.normal_texture, &out.normalUv);
            out.normalScale = m.normal_texture.scale;
            out.occlusionTex = importView(m.occlusion_texture, &out.occlusionUv);
            out.occlusionStrength = m.occlusion_texture.scale;   // cgltf's `scale` is the occlusion strength
            out.emissiveTex = importView(m.emissive_texture, &out.emissiveUv);
            // S2a: the glTF 2.0 material extensions (cgltf parses them; the values are the extension defaults when a
            // material does not use one).
            if (m.has_transmission) {
                out.transmission = m.transmission.transmission_factor;
                out.transmissionTex = importView(m.transmission.transmission_texture, &out.transmissionUv);
            }
            if (m.has_ior) out.ior = m.ior.ior;
            if (m.has_volume) {
                out.volumeThickness = m.volume.thickness_factor;
                out.thicknessTex = importView(m.volume.thickness_texture, &out.thicknessUv);
                out.attenuationColor = {m.volume.attenuation_color[0], m.volume.attenuation_color[1],
                                        m.volume.attenuation_color[2]};
                out.attenuationDistance = m.volume.attenuation_distance;
            }
            if (m.has_clearcoat) {
                out.clearcoat = m.clearcoat.clearcoat_factor;
                out.clearcoatRoughness = m.clearcoat.clearcoat_roughness_factor;
            }
            if (m.has_specular) {
                out.specularFactor = m.specular.specular_factor;
                out.specularColor = {m.specular.specular_color_factor[0], m.specular.specular_color_factor[1],
                                     m.specular.specular_color_factor[2]};
            }
            if (m.has_sheen) {
                out.sheenColor = {m.sheen.sheen_color_factor[0], m.sheen.sheen_color_factor[1],
                                  m.sheen.sheen_color_factor[2]};
                out.sheenRoughness = m.sheen.sheen_roughness_factor;
            }
        }
        scene.materials.push_back(std::move(out));
    }

    // Meshes: one ImpMesh per (node, primitive).
    int skippedPrims = 0;
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;
        const cgltf_mesh& mesh = *node.mesh;
        const int meshIndex = (int)(node.mesh - data->meshes);
        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles || prim.has_draco_mesh_compression) {
                ++skippedPrims;
                continue;
            }
            ImpMesh out;
            out.name = mesh.name ? mesh.name : "mesh" + std::to_string(meshIndex);
            out.node = (int)ni;
            out.skin = node.skin ? (int)(node.skin - data->skins) : -1;
            out.material = prim.material ? (int)(prim.material - data->materials) : -1;
            out.morphGroup = "m" + std::to_string(meshIndex);
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *jnt = nullptr, *wgt = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& at = prim.attributes[a];
                if (at.type == cgltf_attribute_type_position) pos = at.data;
                else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
                else if (at.type == cgltf_attribute_type_joints && at.index == 0) jnt = at.data;
                else if (at.type == cgltf_attribute_type_weights && at.index == 0) wgt = at.data;
            }
            if (!pos || pos->count == 0) {
                ++skippedPrims;
                continue;
            }
            const size_t vc = pos->count;
            std::vector<float> p = Unpack(pos);
            out.positions.resize(vc);
            for (size_t v = 0; v < vc; ++v) out.positions[v] = {p[v * 3], p[v * 3 + 1], p[v * 3 + 2]};
            if (nrm && nrm->count == vc) {
                std::vector<float> n = Unpack(nrm);
                out.normals.resize(vc);
                for (size_t v = 0; v < vc; ++v) out.normals[v] = {n[v * 3], n[v * 3 + 1], n[v * 3 + 2]};
            }
            if (uv && uv->count == vc) {
                std::vector<float> t = Unpack(uv);
                out.uvs.resize(vc);
                for (size_t v = 0; v < vc; ++v) out.uvs[v] = {t[v * 2], t[v * 2 + 1]};
            }
            if (out.skin >= 0 && jnt && wgt && jnt->count == vc && wgt->count == vc) {
                std::vector<float> w = Unpack(wgt);
                out.joints.resize(vc);
                out.weights.resize(vc);
                for (size_t v = 0; v < vc; ++v) {
                    cgltf_uint j4[4] = {0, 0, 0, 0};
                    cgltf_accessor_read_uint(jnt, v, j4, 4);
                    for (int k = 0; k < 4; ++k) out.joints[v][k] = (int)j4[k];
                    out.weights[v] = {w[v * 4], w[v * 4 + 1], w[v * 4 + 2], w[v * 4 + 3]};
                }
            } else {
                out.skin = -1;
            }
            if (prim.indices) {
                out.indices.resize(prim.indices->count);
                cgltf_accessor_unpack_indices(prim.indices, out.indices.data(), sizeof(uint32_t), out.indices.size());
            } else {
                out.indices.resize(vc);
                for (size_t v = 0; v < vc; ++v) out.indices[v] = (uint32_t)v;
            }
            for (cgltf_size t = 0; t < prim.targets_count; ++t) {
                ImpMorphTarget mt;
                mt.name = t < mesh.target_names_count && mesh.target_names[t] ? mesh.target_names[t]
                                                                              : out.morphGroup + "_" + std::to_string(t);
                for (cgltf_size a = 0; a < prim.targets[t].attributes_count; ++a) {
                    const cgltf_attribute& at = prim.targets[t].attributes[a];
                    if (at.type != cgltf_attribute_type_position || !at.data || at.data->count != vc) continue;
                    std::vector<float> d = Unpack(at.data);
                    for (size_t v = 0; v < vc; ++v) {
                        const XMFLOAT3 o{d[v * 3], d[v * 3 + 1], d[v * 3 + 2]};
                        if (o.x == 0 && o.y == 0 && o.z == 0) continue;
                        mt.vertices.push_back((uint32_t)v);
                        mt.offsets.push_back(o);
                    }
                }
                out.morphs.push_back(std::move(mt));  // kept even when empty: expression binds index by position
            }
            scene.meshes.push_back(std::move(out));
        }
    }
    if (skippedPrims > 0) scene.warnings.push_back(std::to_string(skippedPrims) + " non-triangle or compressed primitives skipped");
    if (scene.meshes.empty() && skippedPrims > 0 && scene.unsupported.empty()) scene.unsupported = "no readable triangle meshes";

    ReadVrm(data, scene);
    scene.UpdateWorld();
    return true;
}

} // namespace mmdx
