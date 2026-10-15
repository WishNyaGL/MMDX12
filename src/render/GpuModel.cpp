// GPU-side representation of one PMX model.
#include "render/GpuModel.h"
#include "asset/PmxModel.h"
#include "asset/ImageLoader.h"
#include "asset/ModelTraits.h"
#include "asset/PbrMaterial.h"
#include "render/ShaderInterop.h"
#include "core/Log.h"
#include <directx/d3dx12.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace mmdx {

namespace {

DirectX::XMFLOAT4X4 Identity4x4() {
    DirectX::XMFLOAT4X4 r;
    DirectX::XMStoreFloat4x4(&r, DirectX::XMMatrixIdentity());
    return r;
}

// Face-like material names of MME game rigs (a Face.fx with an SDF face map we
// don't have; flat shading is the closest match).
bool IsFlatFaceMaterial(const std::string& name) {
    std::string n = name;
    for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
    return n == "face" || n == "eyelash" || n == "eyebrow";
}

} // namespace

GpuModel::GpuModel() {
    // Process-wide model id. Never 0 (0 means "no model" in the MME cache keys).
    static std::atomic<uint64_t> sNextUid{1};
    uid_ = sNextUid.fetch_add(1, std::memory_order_relaxed);
}

// Persistently mapped, CPU-written per-frame buffer (UPLOAD heap, GENERIC_READ).
static ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, uint32_t bytes, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> res;
    if (!CheckHr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&res)),
                 "CreateCommittedResource(upload)"))
        return {};
    res->SetName(name);
    return res;
}

bool GpuModel::Create(Dx12Context& ctx, UploadBatch& batch, const PmxModel& pmx,
                      const std::vector<ImageRGBA8>& textures, const BuiltinTextures& builtin,
                      ModelRole role) {
    static const PbrModelData kNoPbr;
    return Create(ctx, batch, pmx, textures, builtin, role, kNoPbr);
}

bool GpuModel::Create(Dx12Context& ctx, UploadBatch& batch, const PmxModel& pmx,
                      const std::vector<ImageRGBA8>& textures, const BuiltinTextures& builtin,
                      ModelRole role, const PbrModelData& pbr) {
    ctx_ = &ctx;
    role_ = role;
    name_ = pmx.name;
    additionalUvCount_ = pmx.additionalUvCount;
    boneCount_ = (uint32_t)pmx.bones.size();
    vertexCount_ = (uint32_t)pmx.vertices.size();
    indexCount_ = (uint32_t)pmx.indices.size();

    // Vertices.
    std::vector<GpuVertex> verts(vertexCount_);
    for (uint32_t v = 0; v < vertexCount_; ++v) {
        const PmxVertex& src = pmx.vertices[v];
        GpuVertex& dst = verts[v];
        dst.position[0] = src.position.x;
        dst.position[1] = src.position.y;
        dst.position[2] = src.position.z;
        dst.normal[0] = src.normal.x;
        dst.normal[1] = src.normal.y;
        dst.normal[2] = src.normal.z;
        dst.uv[0] = src.uv.x;
        dst.uv[1] = src.uv.y;
        for (int i = 0; i < 4; ++i) {
            if (src.boneIndex[i] < 0) {
                dst.bones[i] = 0;
                dst.weights[i] = 0;
            } else {
                dst.bones[i] = (uint16_t)src.boneIndex[i];
                dst.weights[i] = src.boneWeight[i];
            }
        }
        dst.edgeScale = src.edgeScale;
    }
    if (vertexCount_ > 0) {
        boundsMin_ = boundsMax_ = pmx.vertices[0].position;
        for (const PmxVertex& v : pmx.vertices) {
            boundsMin_.x = std::min(boundsMin_.x, v.position.x);
            boundsMin_.y = std::min(boundsMin_.y, v.position.y);
            boundsMin_.z = std::min(boundsMin_.z, v.position.z);
            boundsMax_.x = std::max(boundsMax_.x, v.position.x);
            boundsMax_.y = std::max(boundsMax_.y, v.position.y);
            boundsMax_.z = std::max(boundsMax_.z, v.position.z);
        }
    }
    if (boneCount_ == 0) {
        // No bones: everything bound to bone 0 which holds an identity matrix.
        for (GpuVertex& d : verts) {
            d.bones[0] = 0;
            d.weights[0] = 1;
            for (int i = 1; i < 4; ++i) {
                d.bones[i] = 0;
                d.weights[i] = 0;
            }
        }
    }

    vb_ = batch.CreateBuffer(verts.data(), verts.size() * sizeof(GpuVertex),
                             D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                             L"model.vb");
    if (!vb_) {
        LOG_ERROR("model '%s': vertex buffer creation failed", name_.c_str());
        return false;
    }
    ib_ = batch.CreateBuffer(pmx.indices.data(), pmx.indices.size() * sizeof(uint32_t),
                             D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             L"model.ib");
    if (!ib_) {
        LOG_ERROR("model '%s': index buffer creation failed", name_.c_str());
        return false;
    }
    vbv_.StrideInBytes = sizeof(GpuVertex); // 60
    vbv_.SizeInBytes = (UINT)(verts.size() * sizeof(GpuVertex));
    vbv_.BufferLocation = vb_->GetGPUVirtualAddress();
    ibv_.Format = DXGI_FORMAT_R32_UINT;
    ibv_.SizeInBytes = (UINT)(pmx.indices.size() * sizeof(uint32_t));
    ibv_.BufferLocation = ib_->GetGPUVirtualAddress();

    // SDEF parameters (slot 3), precomputed like saba / MMD: with w1 = 1 - w0 and rw = R0 w0 + R1 w1,
    // R0' = C + R0 - rw, R1' = C + R1 - rw, stored as (C + R0') / 2 and (C + R1') / 2.
    std::vector<GpuSdef> sdef;
    for (uint32_t v = 0; v < vertexCount_ && boneCount_ > 0; ++v) {
        const PmxVertex& src = pmx.vertices[v];
        if (src.deform != PmxDeform::SDEF || src.boneIndex[0] < 0 || src.boneIndex[1] < 0) continue;
        if (sdef.empty()) sdef.assign(vertexCount_, GpuSdef{});
        const float w0 = src.boneWeight[0], w1 = 1.0f - w0;
        const DirectX::XMFLOAT3 &c = src.sdefC, &r0 = src.sdefR0, &r1 = src.sdefR1;
        const float rw[3] = {r0.x * w0 + r1.x * w1, r0.y * w0 + r1.y * w1, r0.z * w0 + r1.z * w1};
        const float cc[3] = {c.x, c.y, c.z}, a0[3] = {r0.x, r0.y, r0.z}, a1[3] = {r1.x, r1.y, r1.z};
        GpuSdef& d = sdef[v];
        for (int k = 0; k < 3; ++k) {
            d.c[k] = cc[k];
            d.cr0[k] = (cc[k] + (cc[k] + a0[k] - rw[k])) * 0.5f;
            d.cr1[k] = (cc[k] + (cc[k] + a1[k] - rw[k])) * 0.5f;
        }
        d.sdef = 1.0f;
    }
    hasSdef_ = !sdef.empty();
    if (!hasSdef_) sdef.assign(1, GpuSdef{});
    sdef_ = batch.CreateBuffer(sdef.data(), sdef.size() * sizeof(GpuSdef),
                               D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                               L"model.sdef");
    if (!sdef_) {
        LOG_ERROR("model '%s': SDEF buffer creation failed", name_.c_str());
        return false;
    }
    sdefVbv_.BufferLocation = sdef_->GetGPUVirtualAddress();
    sdefVbv_.SizeInBytes = (UINT)(sdef.size() * sizeof(GpuSdef));
    sdefVbv_.StrideInBytes = hasSdef_ ? sizeof(GpuSdef) : 0;

    // Additional UVs (slot 4): uploaded only when the model has any (PMX header count > 0).
    hasAdditionalUv_ = pmx.additionalUvCount > 0 && vertexCount_ > 0;
    std::vector<GpuAddUv> addUvs;
    if (hasAdditionalUv_) {
        addUvs.resize(vertexCount_);
        for (uint32_t v = 0; v < vertexCount_; ++v) {
            const PmxVertex& src = pmx.vertices[v];
            addUvs[v].uv1[0] = src.addUv[0].x; addUvs[v].uv1[1] = src.addUv[0].y;
            addUvs[v].uv1[2] = src.addUv[0].z; addUvs[v].uv1[3] = src.addUv[0].w;
            addUvs[v].uv2[0] = src.addUv[1].x; addUvs[v].uv2[1] = src.addUv[1].y;
            addUvs[v].uv2[2] = src.addUv[1].z; addUvs[v].uv2[3] = src.addUv[1].w;
        }
    } else {
        addUvs.assign(1, GpuAddUv{});
    }
    addUv_ = batch.CreateBuffer(addUvs.data(), addUvs.size() * sizeof(GpuAddUv),
                                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                L"model.adduv");
    if (!addUv_) {
        LOG_ERROR("model '%s': additional UV buffer creation failed", name_.c_str());
        return false;
    }
    addUvVbv_.BufferLocation = addUv_->GetGPUVirtualAddress();
    addUvVbv_.SizeInBytes = (UINT)(addUvs.size() * sizeof(GpuAddUv));
    addUvVbv_.StrideInBytes = hasAdditionalUv_ ? sizeof(GpuAddUv) : 0;

    // Bindless raw SRV over the add-UV buffer (ray tracing reads it as a ByteAddressBuffer).
    addUvSrv_ = DescriptorHeap::kInvalid;
    if (hasAdditionalUv_) {
        const uint32_t alloc = ctx.SrvHeap().Allocate(1);
        if (alloc == DescriptorHeap::kInvalid) {
            LOG_ERROR("model '%s': out of SRV descriptors (additional UV)", name_.c_str());
            return false;
        }
        addUvSrv_ = alloc;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.Buffer.NumElements = (UINT)(addUvs.size() * (sizeof(GpuAddUv) / 4));
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        ctx.Device()->CreateShaderResourceView(addUv_.Get(), &srv, ctx.SrvHeap().Cpu(alloc));
    }

    // Referenced texture indices.
    // MME data layout: a toon slot holding a UV-mapped shadow colour (_S) makes the sphere slot a
    // UV-space parameter mask (_H), so the sphere texture is bound whatever its PMX sphere mode.
    auto ToonMapMaterial = [&](const PmxMaterial& m) {
        if (m.sharedToon || m.toonIndex < 0 || (size_t)m.toonIndex >= textures.size()) return false;
        if (textures[m.toonIndex].Empty()) return false;
        return !IsToonRampImage(textures[m.toonIndex]);
    };
    auto SphereUsed = [&](const PmxMaterial& m) {
        const bool mode = m.sphereMode == PmxSphereMode::Multiply || m.sphereMode == PmxSphereMode::Add;
        return mode || (ToonMapMaterial(m) && m.sphereTextureIndex >= 0);
    };
    auto CollectTextureIndex = [&](std::vector<int32_t>& out, int32_t i) {
        if (i < 0 || (size_t)i >= textures.size()) return;
        if (textures[i].Empty()) return;
        if (std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    };
    std::vector<int32_t> referenced;
    for (const PmxMaterial& m : pmx.materials) {
        CollectTextureIndex(referenced, m.textureIndex);
        if (SphereUsed(m)) CollectTextureIndex(referenced, m.sphereTextureIndex);
        if (!m.sharedToon) CollectTextureIndex(referenced, m.toonIndex);
    }
    // PBR file maps (work unit P2): texture indices the Ray records reference through a decoded file.
    std::vector<int32_t> pbrFileUsed;
    for (size_t mi = 0; mi < pbr.materials.size() && mi < pmx.materials.size(); ++mi) {
        if (!pbr.materials[mi].valid) continue;
        for (int si = 0; si < kPbrSlotCount; ++si) {
            const PbrSlot& s = pbr.materials[mi].slots[si];
            if (s.tex.kind != PbrTexKind::File || s.tex.fileIndex < 0 ||
                (size_t)s.tex.fileIndex >= pbr.textures.size() || pbr.textures[(size_t)s.tex.fileIndex].Empty())
                continue;
            if (std::find(pbrFileUsed.begin(), pbrFileUsed.end(), s.tex.fileIndex) == pbrFileUsed.end())
                pbrFileUsed.push_back(s.tex.fileIndex);
        }
    }
    textures_.assign(textures.size(), {});
    for (int32_t i : referenced) {
        textures_[i] = batch.CreateTexture(textures[i], L"model.texture");
        if (!textures_[i]) {
            LOG_ERROR("model '%s': texture creation failed", name_.c_str());
            return false;
        }
    }
    // PBR file map textures (R8 for single-channel slots, RGBA8 otherwise).
    pbrIndex_.assign(pmx.materials.size(), DescriptorHeap::kInvalid);
    pbrTextures_.assign(pbr.textures.size(), {});
    for (int32_t i : pbrFileUsed) {
        const int channel = i < (int)pbr.textureChannels.size() ? pbr.textureChannels[(size_t)i] : -1;
        pbrTextures_[(size_t)i] = channel >= 0 ? batch.CreateTextureChannel8(pbr.textures[(size_t)i], channel,
                                                                             L"model.pbrTex")
                                               : batch.CreateTexture(pbr.textures[(size_t)i], L"model.pbrTex");
        if (!pbrTextures_[(size_t)i]) LOG_WARN("model '%s': a PBR map texture upload failed", name_.c_str());
    }

    // Material constants.
    std::vector<MaterialConstants> consts(pmx.materials.size(), MaterialConstants{});
    for (size_t mIdx = 0; mIdx < pmx.materials.size(); ++mIdx) {
        const PmxMaterial& m = pmx.materials[mIdx];
        MaterialConstants& c = consts[mIdx];
        c.diffuse = m.diffuse;
        c.specular = m.specular;
        c.specularPower = m.specularPower;
        c.ambient = m.ambient;
        c.edgeSize = m.edgeSize;
        c.edgeColor = m.edgeColor;
        bool hasTex = (m.textureIndex >= 0 && (size_t)m.textureIndex < textures_.size() && textures_[m.textureIndex]);
        bool sphereRef =
            (m.sphereMode == PmxSphereMode::Multiply || m.sphereMode == PmxSphereMode::Add) &&
            m.sphereTextureIndex >= 0 && (size_t)m.sphereTextureIndex < textures_.size() &&
            textures_[m.sphereTextureIndex];
        // A real sphere-slot texture is bound (Mode Multiply/Add, or the toon-map data layout).
        const bool sphereBound = sphereRef || (SphereUsed(m) && m.sphereTextureIndex >= 0 &&
                                               (size_t)m.sphereTextureIndex < textures_.size() &&
                                               textures_[m.sphereTextureIndex]);
        if (hasTex) c.flags |= MatFlag_HasTexture;
        if (sphereBound) c.flags |= MatFlag_HasSphereTex;
        if (sphereRef) {
            if (m.sphereMode == PmxSphereMode::Multiply) c.flags |= MatFlag_SphereMul;
            if (m.sphereMode == PmxSphereMode::Add) c.flags |= MatFlag_SphereAdd;
        }
        if (m.sharedToon) {
            c.flags |= MatFlag_HasToon;
        } else if (m.toonIndex >= 0 && (size_t)m.toonIndex < textures_.size() && textures_[m.toonIndex]) {
            c.flags |= MatFlag_HasToon;
            if (!IsToonRampImage(textures[m.toonIndex])) {
                // MME toon-map layout: toon = shadow colour (_S), sphere = UV-space parameter mask (_H),
                // which must not be added as an environment sphere.
                c.flags |= MatFlag_ToonMap;
                c.flags &= ~(MatFlag_SphereMul | MatFlag_SphereAdd);
                if (IsFlatFaceMaterial(m.name)) c.flags |= MatFlag_FlatShade;
            }
        }
        // No toon on a character material: the author wants it unmodelled (typically the face,
        // which often comes with painted hair shadows).
        if (!(c.flags & MatFlag_HasToon) && role != ModelRole::Stage) c.flags |= MatFlag_FlatShade;
        if (m.flags & PmxMat_ReceiveShadow) c.flags |= MatFlag_ReceiveShadow;
        if (role == ModelRole::Stage) c.flags |= MatFlag_Stage;
        // MMD has no roughness: treat a tight, bright specular as a hint of gloss.
        const float specLum = 0.299f * m.specular.x + 0.587f * m.specular.y + 0.114f * m.specular.z;
        const float tightness = std::clamp((m.specularPower - 5.0f) / 60.0f, 0.0f, 1.0f);
        c.reflectivity = std::clamp(specLum * tightness, 0.0f, 1.0f) * (role == ModelRole::Stage ? 0.5f : 0.12f);
        // そぼろ AutoLuminous convention: the author marks a glowing material with a black specular colour
        // (reflection colour 0,0,0) and a specular power (反射強度) of 100 or more (110 = the documented standard).
        // The shaders then add the material colour x texture as HDR radiance (AutoLumEmission). A PBR record that
        // already carries an emissive, or a Ray/glTF material, must not glow twice, so skip those.
        const bool hasPbrRecord = mIdx < pbr.materials.size() && pbr.materials[mIdx].valid;
        if (m.specularPower > 100.0f && specLum <= 1e-4f && !hasPbrRecord)
            c.flags |= MatFlag_AutoLuminous;
    }
    for (MaterialConstants& c : consts) {
        c.texMul = c.sphereMul = c.toonMul = {1, 1, 1, 1};
        c.texAdd = c.sphereAdd = c.toonAdd = {0, 0, 0, 0};
    }
    // Load-time AutoLuminous classification (kept so a shader pack turned off can restore it).
    autoLum_.assign(pmx.materials.size(), 0);
    for (size_t i = 0; i < consts.size(); ++i)
        if (consts[i].flags & MatFlag_AutoLuminous) autoLum_[i] = 1;
    // Per-frame ring (material morphs rewrite the constants at run time), every entry starts at the base.
    const size_t matCount = std::max<size_t>(consts.size(), 1);
    materialCb_ = CreateUploadBuffer(ctx.Device(), (uint32_t)(matCount * sizeof(MaterialConstants) * kRing),
                                     L"model.materialCb");
    if (!materialCb_ || FAILED(materialCb_->Map(0, nullptr, (void**)&materialMapped_))) {
        LOG_ERROR("model '%s': material constant buffer creation failed", name_.c_str());
        return false;
    }
    for (uint32_t r = 0; r < kRing; ++r) {
        if (!consts.empty())
            memcpy(materialMapped_ + r * matCount * sizeof(MaterialConstants), consts.data(),
                   consts.size() * sizeof(MaterialConstants));
        materialEntryVersion_[r] = 0;
    }
    materialVersion_ = 0;
    materialConsts_ = consts;
    baseConsts_ = consts;
    materialNames_.clear();
    materialTextures_.clear();
    // MME TOONCOLOR (spec §4.2): the bottom-left texel of the material's toon texture. A shared (built-in
    // toon01..10) toon has no decoded image here, so it stays white (logged by the object pass when used).
    mmeToonColor_.assign(pmx.materials.size(), DirectX::XMFLOAT3{1, 1, 1});
    for (size_t mi = 0; mi < pmx.materials.size(); ++mi) {
        const PmxMaterial& m = pmx.materials[mi];
        materialNames_.emplace_back(m.name, m.nameEn);
        const bool tex = m.textureIndex >= 0 && (size_t)m.textureIndex < pmx.textures.size();
        materialTextures_.push_back(tex ? pmx.textures[(size_t)m.textureIndex] : std::string());
        if (!m.sharedToon && m.toonIndex >= 0 && (size_t)m.toonIndex < textures.size() && !textures[m.toonIndex].Empty()) {
            const ImageRGBA8& img = textures[(size_t)m.toonIndex];
            if (!img.mips.empty() && !img.mips[0].pixels.empty() && img.mips[0].width > 0 && img.mips[0].height > 0) {
                const ImageRGBA8::Level& lv = img.mips[0];
                // Bottom-left corner = the last row's first texel.
                const size_t idx = ((size_t)lv.height - 1) * (size_t)lv.width * 4;
                if (idx + 3 < lv.pixels.size()) {
                    mmeToonColor_[mi] = {lv.pixels[idx + 0] / 255.0f, lv.pixels[idx + 1] / 255.0f,
                                         lv.pixels[idx + 2] / 255.0f};
                }
            }
        }
    }
    headBone_ = -1;
    for (const char* name : {"\xE9\xA0\xAD", "\xE9\xA6\x96"}) {  // 頭, else 首
        for (size_t b = 0; b < pmx.bones.size() && headBone_ < 0; ++b)
            if (pmx.bones[b].name == name) {
                headBone_ = (int32_t)b;
                headPos_ = pmx.bones[b].position;
            }
        if (headBone_ >= 0) break;
    }
    packId_.clear();
    packTextureFolder_.clear();    materialBase_.assign(pmx.materials.size(), {});
    std::vector<bool> morphable(pmx.materials.size(), false);
    for (const PmxMorph& mo : pmx.morphs) {
        if (mo.type != PmxMorphType::Material) continue;
        for (const auto& o : mo.materialOffsets) {
            if (o.material < 0) morphable.assign(morphable.size(), true);
            else if ((size_t)o.material < morphable.size()) morphable[(size_t)o.material] = true;
        }
    }

    // Per material: descriptor table and Material struct.
    materials_.clear();
    uint32_t indexStart = 0;
    for (size_t mIdx = 0; mIdx < pmx.materials.size(); ++mIdx) {
        const PmxMaterial& m = pmx.materials[mIdx];
        Material mat;
        mat.indexStart = indexStart;
        mat.indexCount = m.indexCount;
        indexStart += m.indexCount;
        mat.doubleSided = (m.flags & PmxMat_DoubleSided) != 0;
        materialBase_[mIdx].edgeFlag = (m.flags & PmxMat_Edge) != 0;
        materialBase_[mIdx].castFlag = (m.flags & (PmxMat_CastShadow | PmxMat_GroundShadow)) != 0;
        mat.visible = m.diffuse.w > 0.0f;
        mat.morphable = morphable[mIdx];
        mat.drawEdge = materialBase_[mIdx].edgeFlag && m.edgeSize > 0 && m.edgeColor.w > 0 && mat.visible;
        mat.castShadow = materialBase_[mIdx].castFlag && m.diffuse.w > 0.01f;
        mat.alphaTested = m.textureIndex >= 0 && (size_t)m.textureIndex < textures.size() &&
                          textures[m.textureIndex].hasAlpha;
        mat.sphereMode = (uint8_t)m.sphereMode;
        mat.edgeSize = m.edgeSize;
        mat.constants = materialCb_->GetGPUVirtualAddress() + mIdx * sizeof(MaterialConstants);

        uint32_t alloc = ctx.SrvHeap().Allocate(3);
        if (alloc == DescriptorHeap::kInvalid) {
            LOG_ERROR("model '%s': out of SRV descriptors", name_.c_str());
            return false;
        }
        srvAllocations_.push_back(alloc);
        mat.srvTable = alloc;

        ID3D12Device* device = ctx.Device();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = UINT(-1);

        // +0 texture
        if (m.textureIndex >= 0 && (size_t)m.textureIndex < textures_.size() && textures_[m.textureIndex])
            device->CreateShaderResourceView(textures_[m.textureIndex].Get(), &srv, ctx.SrvHeap().Cpu(alloc + 0));
        else
            device->CreateShaderResourceView(builtin.white.Get(), &srv, ctx.SrvHeap().Cpu(alloc + 0));
        // +1 sphere
        if ((m.sphereMode == PmxSphereMode::Multiply || m.sphereMode == PmxSphereMode::Add) &&
            m.sphereTextureIndex >= 0 && (size_t)m.sphereTextureIndex < textures_.size() &&
            textures_[m.sphereTextureIndex])
            device->CreateShaderResourceView(textures_[m.sphereTextureIndex].Get(), &srv,
                                             ctx.SrvHeap().Cpu(alloc + 1));
        else if ((consts[mIdx].flags & MatFlag_HasSphereTex) && m.sphereTextureIndex >= 0 &&
                 (size_t)m.sphereTextureIndex < textures_.size() && textures_[m.sphereTextureIndex])
            device->CreateShaderResourceView(textures_[m.sphereTextureIndex].Get(), &srv,
                                             ctx.SrvHeap().Cpu(alloc + 1));
        else
            device->CreateShaderResourceView(builtin.white.Get(), &srv, ctx.SrvHeap().Cpu(alloc + 1));
        // +2 toon
        if (m.sharedToon) {
            int32_t t = m.toonIndex < 0 ? 0 : (m.toonIndex > 9 ? 9 : m.toonIndex);
            device->CreateShaderResourceView(builtin.toon[t].Get(), &srv, ctx.SrvHeap().Cpu(alloc + 2));
        } else if (m.toonIndex >= 0 && (size_t)m.toonIndex < textures_.size() && textures_[m.toonIndex]) {
            device->CreateShaderResourceView(textures_[m.toonIndex].Get(), &srv, ctx.SrvHeap().Cpu(alloc + 2));
        } else {
            device->CreateShaderResourceView(builtin.white.Get(), &srv, ctx.SrvHeap().Cpu(alloc + 2));
        }
        materials_.push_back(mat);
    }

    // PBR records and their SRVs (work unit P2): one GpuPbrMaterial per material with a valid Ray record. The
    // file-map SRVs are allocated first, so the record can store their absolute heap index (one field read by the
    // raster bindless table and the RT table alike).
    const bool anyPbr = !pbr.materials.empty() &&
                        std::any_of(pbr.materials.begin(), pbr.materials.end(),
                                    [](const PbrMaterial& m) { return m.valid; });
    if (anyPbr) {
        pbrTextureSrvs_.assign(pbrTextures_.size(), DescriptorHeap::kInvalid);
        bool outOfDescriptors = false;
        for (size_t i = 0; i < pbrTextures_.size(); ++i) {
            if (!pbrTextures_[i]) continue;
            const uint32_t ta = ctx.SrvHeap().Allocate(1);
            if (ta == DescriptorHeap::kInvalid) {
                outOfDescriptors = true;
                pbrTextures_[i].Reset();
                continue;
            }
            pbrTextureSrvs_[i] = ta;
            D3D12_SHADER_RESOURCE_VIEW_DESC ts{};
            ts.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ts.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            ts.Format = i < pbr.textureChannels.size() && pbr.textureChannels[i] >= 0 ? DXGI_FORMAT_R8_UNORM
                                                                                       : DXGI_FORMAT_R8G8B8A8_UNORM;
            ts.Texture2D.MipLevels = UINT(-1);
            ctx.Device()->CreateShaderResourceView(pbrTextures_[i].Get(), &ts, ctx.SrvHeap().Cpu(ta));
        }
        if (outOfDescriptors) LOG_WARN("model '%s': ran out of SRV descriptors for PBR maps", name_.c_str());

        std::vector<GpuPbrMaterial> records;
        for (size_t mi = 0; mi < pmx.materials.size(); ++mi) {
            if (mi >= pbr.materials.size() || !pbr.materials[mi].valid) continue;
            const PbrMaterial& src = pbr.materials[mi];
            const PmxMaterial& pm = pmx.materials[mi];
            GpuPbrMaterial r{};
            r.flags = 1u | (src.alphaThreshold > 0.0f && src.alphaThreshold < 0.999f ? 2u : 0u) |
                      (src.emissiveEnable ? 4u : 0u);
            r.shadingModel = (uint32_t)std::max(0, src.shadingModel);
            r.alphaThreshold = src.alphaThreshold;
            r.diffuseAlpha = pm.diffuse.w;
            r.params.x = (float)src.albedoSubEnable;
            r.params.y = (float)src.albedoApplyDiffuse;
            for (int si = 0; si < kPbrSlotCount; ++si) {
                const PbrSlot& s = src.slots[si];
                GpuPbrSlot& d = r.slots[si];
                d.type = (uint32_t)std::max(0, s.type);
                d.swizzle = (uint32_t)std::clamp(s.swizzle, 0, 3);
                d.uvFlip = (uint32_t)std::max(0, s.uvFlip);
                d.applyScale = (uint32_t)std::max(0, s.applyScale);
                d.colorFlip = (uint32_t)std::max(0, s.colorFlip);
                d.loopU = s.loopU;
                d.loopV = s.loopV;
                d.value = {s.value[0], s.value[1], s.value[2], s.hasScale ? s.scale : 0.0f};
                d.uvScale = {s.uvScaleU, s.uvScaleV, 0.0f, 0.0f};
                if (s.tex.kind == PbrTexKind::File) {
                    d.textureSrv = (s.tex.fileIndex >= 0 && (size_t)s.tex.fileIndex < pbrTextureSrvs_.size() &&
                                    pbrTextureSrvs_[(size_t)s.tex.fileIndex] != DescriptorHeap::kInvalid)
                                       ? (int)pbrTextureSrvs_[(size_t)s.tex.fileIndex]
                                       : -1;
                    // A single-channel File map is uploaded R8 with the selected channel baked into R, so the
                    // shader reads channel 0; PMX-sourced maps stay RGBA and keep the declared swizzle.
                    if (s.tex.fileIndex >= 0 && (size_t)s.tex.fileIndex < pbr.textureChannels.size() &&
                        pbr.textureChannels[(size_t)s.tex.fileIndex] >= 0)
                        d.swizzle = 0;
                } else if (s.tex.kind == PbrTexKind::PmxDiffuse) {
                    d.textureSrv = (int)materials_[mi].srvTable + 0;
                } else if (s.tex.kind == PbrTexKind::PmxSphere) {
                    d.textureSrv = (int)materials_[mi].srvTable + 1;
                } else if (s.tex.kind == PbrTexKind::PmxToon) {
                    d.textureSrv = (int)materials_[mi].srvTable + 2;
                } else {
                    d.textureSrv = -1;
                }
            }
            pbrIndex_[mi] = (uint32_t)records.size();
            records.push_back(r);
        }
        pbrBuf_ = batch.CreateBuffer(records.data(), records.size() * sizeof(GpuPbrMaterial),
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     L"model.pbr");
        if (!pbrBuf_) {
            LOG_WARN("model '%s': PBR record buffer creation failed", name_.c_str());
            pbrIndex_.assign(pmx.materials.size(), DescriptorHeap::kInvalid);
        } else {
            const uint32_t alloc = ctx.SrvHeap().Allocate(1);
            if (alloc != DescriptorHeap::kInvalid) {
                pbrSrv_ = alloc;
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srv.Format = DXGI_FORMAT_R32_TYPELESS;
                srv.Buffer.NumElements = (UINT)(records.size() * sizeof(GpuPbrMaterial) / 4);
                srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                ctx.Device()->CreateShaderResourceView(pbrBuf_.Get(), &srv, ctx.SrvHeap().Cpu(alloc));
            } else {
                pbrBuf_.Reset();
                pbrIndex_.assign(pmx.materials.size(), DescriptorHeap::kInvalid);
            }
        }
    }

    // Per frame slot dynamic buffers.
    const uint32_t boneBytes = (boneCount_ == 0 ? 1u : boneCount_) * 64;
    const uint32_t morphBytes = (vertexCount_ == 0 ? 1u : vertexCount_) * 12;
    for (uint32_t s = 0; s < kRing; ++s) {
        boneBuf_[s] = CreateUploadBuffer(ctx.Device(), boneBytes, L"model.bones");
        if (!boneBuf_[s]) {
            LOG_ERROR("model '%s': bone buffer creation failed", name_.c_str());
            return false;
        }
        if (FAILED(boneBuf_[s]->Map(0, nullptr, &boneMapped_[s]))) {
            LOG_ERROR("model '%s': failed to map bone buffer", name_.c_str());
            return false;
        }
        DirectX::XMFLOAT4X4 identity = Identity4x4();
        for (uint32_t b = 0; b < (boneCount_ == 0 ? 1u : boneCount_); ++b)
            memcpy((uint8_t*)boneMapped_[s] + b * 64, &identity, 64);

        morphBuf_[s] = CreateUploadBuffer(ctx.Device(), morphBytes, L"model.morphs");
        if (!morphBuf_[s]) {
            LOG_ERROR("model '%s': morph buffer creation failed", name_.c_str());
            return false;
        }
        if (FAILED(morphBuf_[s]->Map(0, nullptr, &morphMapped_[s]))) {
            LOG_ERROR("model '%s': failed to map morph buffer", name_.c_str());
            return false;
        }
        memset(morphMapped_[s], 0, morphBytes);
        morphVbv_[s].StrideInBytes = 12;
        morphVbv_[s].SizeInBytes = morphBytes;
        morphVbv_[s].BufferLocation = morphBuf_[s]->GetGPUVirtualAddress();
        morphVersion_[s] = UINT64_MAX;
    }
    return true;
}

void GpuModel::SetMaterialBlendHints(const std::vector<uint8_t>& hints, uint8_t def) {
    if (materials_.empty()) return;
    materialBlend_.assign(materials_.size(), 0);
    for (size_t i = 0; i < materials_.size(); ++i) {
        const uint8_t h = i < hints.size() ? hints[i] : def;  // slots past the vector use the bare Obj default
        materialBlend_[i] = h;
        Material& mat = materials_[i];
        mat.additive = h == 2;
        mat.noDepthWrite = h >= 1;
        // A hinted material draws no edge (outline) and casts no shadow-map shadow. The base flags are cleared too,
        // so UpdateMaterials (which re-derives drawEdge / castShadow each frame) keeps them off. An ordinary (h == 0)
        // material is left untouched: models without .emd or with only ordinary effects render exactly as before.
        if (h >= 1) {
            mat.drawEdge = false;
            mat.castShadow = false;
            materialBase_[i].edgeFlag = false;
            materialBase_[i].castFlag = false;
        }
        for (MaterialConstants* c : {&baseConsts_[i], &materialConsts_[i]}) {
            c->flags &= ~(MatFlag_Additive | MatFlag_NoDepthWrite);
            if (h == 2) c->flags |= MatFlag_Additive | MatFlag_NoDepthWrite;   // additive implies no depth write
            else if (h == 1) c->flags |= MatFlag_NoDepthWrite;
        }
    }
    for (uint64_t& v : materialEntryVersion_) v = ~0ull;   // re-upload the constants on the next UpdateMaterials
    for (size_t i = 0; i < materials_.size(); ++i) materials_[i].constants = materialCb_->GetGPUVirtualAddress() + i * sizeof(MaterialConstants);
}

void GpuModel::UpdateSkinning(uint64_t frame, const std::vector<DirectX::XMFLOAT4X4>& skin) {
    const size_t count = std::min(skin.size(), (size_t)(boneCount_ == 0 ? 1u : boneCount_));
    const uint32_t r = (uint32_t)(frame % kRing);
    for (uint32_t i = 0; i < kRing; ++i) {
        if ((skinInitialized_ && i != r) || !boneMapped_[i]) continue;
        memcpy(boneMapped_[i], skin.data(), count * 64);
    }
    skinInitialized_ = true;
}

void GpuModel::UpdateMorphs(uint64_t frame, const std::vector<DirectX::XMFLOAT3>& deltas, uint64_t version) {
    const size_t count = std::min(deltas.size(), (size_t)vertexCount_);
    const uint32_t r = (uint32_t)(frame % kRing);
    for (uint32_t i = 0; i < kRing; ++i) {
        if ((morphInitialized_ && i != r) || !morphMapped_[i]) continue;
        if (version == morphVersion_[i]) continue;
        memcpy(morphMapped_[i], deltas.data(), count * 12);
        morphVersion_[i] = version;
    }
    morphInitialized_ = true;
}

void GpuModel::UpdateMaterials(uint64_t frame, const std::vector<PmxMorph::MaterialOffset>& mul,
                               const std::vector<PmxMorph::MaterialOffset>& add, uint64_t version, float opacity) {
    if (!materialMapped_ || materials_.empty()) return;
    const size_t n = materials_.size();
    const bool identity = mul.size() != n || add.size() != n;
    if (identity) version = 0;
    const bool alpha = opacity != 1.0f;
    if (alpha) version |= (1ull << 63);  // fold the opacity into the version so a change re-uploads (1 stays untouched)
    if (version != materialVersion_) {
        // final = base * mul + add (PMX material morph); the texture factors go to the shaders as is
        for (size_t i = 0; i < n; ++i) {
            const MaterialConstants& b = baseConsts_[i];
            MaterialConstants& c = materialConsts_[i];
            c = b;
            if (!identity) {
                const PmxMorph::MaterialOffset& m = mul[i];
                const PmxMorph::MaterialOffset& a = add[i];
                c.diffuse = {b.diffuse.x * m.diffuse.x + a.diffuse.x, b.diffuse.y * m.diffuse.y + a.diffuse.y,
                             b.diffuse.z * m.diffuse.z + a.diffuse.z,
                             std::clamp(b.diffuse.w * m.diffuse.w + a.diffuse.w, 0.0f, 1.0f)};
                c.specular = {b.specular.x * m.specular.x + a.specular.x, b.specular.y * m.specular.y + a.specular.y,
                              b.specular.z * m.specular.z + a.specular.z};
                c.specularPower = std::max(0.0f, b.specularPower * m.specularPower + a.specularPower);
                c.ambient = {b.ambient.x * m.ambient.x + a.ambient.x, b.ambient.y * m.ambient.y + a.ambient.y,
                             b.ambient.z * m.ambient.z + a.ambient.z};
                c.edgeColor = {b.edgeColor.x * m.edgeColor.x + a.edgeColor.x, b.edgeColor.y * m.edgeColor.y + a.edgeColor.y,
                               b.edgeColor.z * m.edgeColor.z + a.edgeColor.z,
                               std::clamp(b.edgeColor.w * m.edgeColor.w + a.edgeColor.w, 0.0f, 1.0f)};
                c.edgeSize = std::max(0.0f, b.edgeSize * m.edgeSize + a.edgeSize);
                c.texMul = m.textureFactor;
                c.texAdd = a.textureFactor;
                c.sphereMul = m.sphereFactor;
                c.sphereAdd = a.sphereFactor;
                c.toonMul = m.toonFactor;
                c.toonAdd = a.toonFactor;
            }
            if (alpha) c.diffuse.w = std::clamp(c.diffuse.w * opacity, 0.0f, 1.0f);  // studio accessory opacity
            Material& mat = materials_[i];
            mat.visible = c.diffuse.w > 0.0f;
            mat.drawEdge = materialBase_[i].edgeFlag && c.edgeSize > 0 && c.edgeColor.w > 0 && mat.visible;
            mat.castShadow = materialBase_[i].castFlag && c.diffuse.w > 0.01f;
            mat.edgeSize = c.edgeSize;
        }
        materialVersion_ = version;
    }
    const uint32_t r = (uint32_t)(frame % kRing);
    const size_t stride = n * sizeof(MaterialConstants);
    if (materialEntryVersion_[r] != materialVersion_) {
        memcpy(materialMapped_ + r * stride, materialConsts_.data(), stride);
        materialEntryVersion_[r] = materialVersion_;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS base = materialCb_->GetGPUVirtualAddress() + r * stride;
    for (size_t i = 0; i < n; ++i) materials_[i].constants = base + i * sizeof(MaterialConstants);
}

void GpuModel::SetShaderPack(const ShaderPack* pack, const PackParamValues& params,
                             const std::filesystem::path& textureFolder,
                             const MaterialClassOverrides& materials) {
    if (role_ == ModelRole::Stage) pack = nullptr;   // packs shade characters only
    const std::string id = pack ? pack->id : std::string();
    const uint32_t generation = ShaderPacks().Generation();
    if (id == packId_ && (!pack || (generation == packGeneration_ && params == packParams_ &&
                                    textureFolder == packTextureFolder_ &&
                                    packOverrides_ == materials)))
        return;
    packId_ = id;
    packGeneration_ = generation;
    packTextureFolder_ = pack ? textureFolder : std::filesystem::path();
    packParams_ = params;
    packOverrides_ = pack ? materials : MaterialClassOverrides{};
    if (!pack) {
        for (auto& m : materials_) m.packOff = false;
        // no pack = no class and no alpha key (clears the encoding of a pack switched off). A material the pack had
        // claimed gets its AutoLuminous flag back (the packs own / replace shading; below clears it again).
        for (size_t i = 0; i < baseConsts_.size(); ++i) {
            baseConsts_[i].packClass = 0;
            materialConsts_[i].packClass = 0;
            if (i < autoLum_.size() && autoLum_[i]) {
                baseConsts_[i].flags |= MatFlag_AutoLuminous;
                materialConsts_[i].flags |= MatFlag_AutoLuminous;
            }
        }
        for (uint64_t& v : materialEntryVersion_) v = ~0ull;
        return;
    }
    for (size_t i = 0; i < baseConsts_.size(); ++i) {
        const auto& names = materialNames_[i];
        uint32_t clsVal = 0;
        bool isOff = false;
        if (const auto it = packOverrides_.find(names.first); it != packOverrides_.end()) {
            if (it->second == kMaterialPackOff) {
                isOff = true;
                clsVal = kPackClassOff;
            } else if (it->second >= 0 && it->second <= 5) {
                clsVal = static_cast<uint32_t>(it->second);
            } else {
                clsVal = static_cast<uint32_t>(pack->Classify(names.first, names.second, materialTextures_[i]));
            }
        } else {
            clsVal = static_cast<uint32_t>(pack->Classify(names.first, names.second, materialTextures_[i]));
        }
        // The alpha key follows the material's FINAL class (per-material override included); kPackClassOff = no key.
        const float key = isOff ? 0.0f : PackAlphaKeyForClass(pack->alphaKey, clsVal);
        const uint32_t encodedClass = PackClassEncode(clsVal, key);
        if (i < materials_.size()) materials_[i].packOff = isOff;
        for (MaterialConstants* c : {&baseConsts_[i], &materialConsts_[i]}) {
            c->packClass = encodedClass;
            // A pack owns the shading of the materials it shades, so their AutoLuminous flag is cleared (no double
            // glow); a per-material "pack off" override (isOff) keeps the default shading and its glow.
            if (!isOff) c->flags &= ~MatFlag_AutoLuminous;
            else if (i < autoLum_.size() && autoLum_[i]) c->flags |= MatFlag_AutoLuminous;
            c->packHeadBone = headBone_ >= 0 ? (uint32_t)headBone_ : 0u;
            c->packHead = {headPos_.x, headPos_.y, headPos_.z, headBone_ >= 0 ? 1.0f : 0.0f};
            for (uint32_t k = 0; k < kPackMaxParams; ++k) (&c->packParams[0].x)[k] = params[k];
        }
    }
    // every ring entry is rewritten by the next UpdateMaterials calls
    for (uint64_t& v : materialEntryVersion_) v = ~0ull;
}

void GpuModel::SetMmeEffects(std::vector<std::filesystem::path> perMaterial) {
    mmeEffects_ = std::move(perMaterial);
}

bool GpuModel::HasMmeEffects() const {
    for (const auto& fx : mmeEffects_)
        if (!fx.empty()) return true;
    return false;
}

std::vector<std::filesystem::path> GpuModel::MmeEffectFiles() const {
    std::vector<std::filesystem::path> out;
    for (const auto& fx : mmeEffects_) {
        if (fx.empty()) continue;
        bool dup = false;
        for (const auto& e : out)
            if (e == fx) { dup = true; break; }
        if (!dup) out.push_back(fx);
    }
    return out;
}

void GpuModel::SetMmeEffect(const std::filesystem::path& fx, std::vector<int> materialMask) {
    mmeEffects_.assign(materials_.size(), std::filesystem::path());
    if (fx.empty()) return;
    if (materialMask.empty()) {
        for (auto& p : mmeEffects_) p = fx;
        return;
    }
    for (int m : materialMask)
        if (m >= 0 && (size_t)m < mmeEffects_.size()) mmeEffects_[(size_t)m] = fx;
}

ModelTraits GpuModel::Traits() const {
    ModelTraits t;
    t.additionalUvCount = hasAdditionalUv_ ? additionalUvCount_ : 0;
    t.materials = (int)materialConsts_.size();
    for (const MaterialConstants& c : materialConsts_)
        if (c.flags & MatFlag_ToonMap) ++t.toonMapMaterials;
    return t;
}

bool GpuModel::PackActive() const {
    if (packId_.empty()) return false;
    for (const Material& m : materials_)
        if (!m.packOff) return true;
    return false;
}

D3D12_GPU_VIRTUAL_ADDRESS GpuModel::BoneBuffer(uint64_t frame) const {
    const ComPtr<ID3D12Resource>& b = boneBuf_[frame % kRing];
    return b ? b->GetGPUVirtualAddress() : 0;
}

D3D12_GPU_VIRTUAL_ADDRESS GpuModel::PrevBoneBuffer(uint64_t frame) const {
    const ComPtr<ID3D12Resource>& b = boneBuf_[(frame + kRing - 1) % kRing];
    return b ? b->GetGPUVirtualAddress() : 0;
}

void GpuModel::Destroy() {
    if (!ctx_) return;
    if (rt_.srv != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(rt_.srv, 2);
    if (addUvSrv_ != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(addUvSrv_, 1);
    addUvSrv_ = DescriptorHeap::kInvalid;
    if (pbrSrv_ != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(pbrSrv_, 1);
    pbrSrv_ = DescriptorHeap::kInvalid;
    for (uint32_t s : pbrTextureSrvs_)
        if (s != DescriptorHeap::kInvalid) ctx_->SrvHeap().Free(s, 1);
    pbrTextureSrvs_.clear();
    pbrTextures_.clear();
    pbrIndex_.clear();
    pbrBuf_.Reset();
    rt_ = RtResources{};
    materialConsts_.clear();
    baseConsts_.clear();
    materialBase_.clear();
    if (materialCb_ && materialMapped_) materialCb_->Unmap(0, nullptr);
    materialMapped_ = nullptr;
    for (uint32_t a : srvAllocations_) ctx_->SrvHeap().Free(a, 3);
    srvAllocations_.clear();
    for (uint32_t s = 0; s < kRing; ++s) {
        if (boneBuf_[s] && boneMapped_[s]) {
            boneBuf_[s]->Unmap(0, nullptr);
            boneMapped_[s] = nullptr;
        }
        if (morphBuf_[s] && morphMapped_[s]) {
            morphBuf_[s]->Unmap(0, nullptr);
            morphMapped_[s] = nullptr;
        }
        boneBuf_[s].Reset();
        morphBuf_[s].Reset();
        morphVbv_[s] = {};
    }
    vb_.Reset();
    ib_.Reset();
    sdef_.Reset();
    sdefVbv_ = {};
    hasSdef_ = false;
    addUv_.Reset();
    addUvVbv_ = {};
    hasAdditionalUv_ = false;
    materialCb_.Reset();
    textures_.clear();
    materials_.clear();
    ctx_ = nullptr;
}

GpuModel::~GpuModel() {
    Destroy();
}

} // namespace mmdx
