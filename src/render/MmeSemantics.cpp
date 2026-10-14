// M3b: MME semantic binder (CPU only). See MmeSemantics.h. No D3D12 objects.
#include "render/MmeSemantics.h"
#include "render/GpuModel.h"
#include "render/RenderTypes.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <set>
#include <string_view>

using namespace DirectX;

namespace mmdx {
namespace {

// ---- providers -----------------------------------------------------------------------------------------------------
enum Provider {
    PNone = 0,
    PMatrix,
    PColor,          // DIFFUSE / AMBIENT / SPECULAR / EMISSIVE
    PSpecularPower,
    PPositionDirection,
    PViewportPixelSize,
    PTime,
    PElapsedTime,
    PMouse,
    PControlObject,
    PAddingTexture,
    PMultiplyingTexture,
    PAddingSphereTexture,
    PMultiplyingSphereTexture,
    // M4b: per-draw Material="Geometry" values and the by-name uniforms (spec draft §5).
    PObjectColor,          // DIFFUSE / AMBIENT / SPECULAR / EMISSIVE (Geometry) from MmeObjectValues
    PObjectSpecularPower,  // SPECULARPOWER (Geometry)
    PObjectToonColor,      // TOONCOLOR (Geometry)
    PObjectEdgeColor,      // EDGECOLOR (Geometry)
    PByName,               // use_texture / use_spheremap / ... (no semantic, spec §5)
    PTexValue,             // M7 fix 5: TEXTUREVALUE (a float4 array read from a named texture's texels)
};

// The by-name uniforms (spec draft §5). A parameter with one of these names takes a value with no semantic.
enum ByName {
    BNNone = 0,
    BNUseTexture,
    BNUseSpheremap,
    BNUseSubtexture,
    BNUseToon,
    BNSpadd,
    BNParthf,
    BNTransp,
    BNOpadd,
};

enum MatrixBase { MBWorld = 0, MBView, MBProj, MBWorldView, MBViewProj, MBWorldViewProj };
enum MatrixSuffix { MSPlain = 0, MSInverse, MSTranspose, MSInverseTranspose };
enum ColorKind { CKDiffuse = 0, CKAmbient, CKSpecular, CKEemissive };
enum PosDirKind { PDPosition = 0, PDDirection };

std::string ToLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// Parameter lookup by cbuffer variable name (case-insensitive; trailing array index stripped).
const MmeParam* FindParamCI(const MmeEffectDesc& fx, std::string_view name) {
    std::string s(name);
    const size_t br = s.find('[');
    if (br != std::string::npos) s = s.substr(0, br);
    for (const MmeParam& p : fx.params)
        if (p.name == s) return &p;
    const std::string lo = ToLower(s);
    for (const MmeParam& p : fx.params)
        if (ToLower(p.name) == lo) return &p;
    return nullptr;
}

// M4b: the by-name uniforms (spec draft §5). No semantic; the value is a material / object flag.
int ByNameId(const std::string& name) {
    const std::string lo = ToLower(name);
    if (lo == "use_texture") return BNUseTexture;
    if (lo == "use_spheremap") return BNUseSpheremap;
    if (lo == "use_subtexture") return BNUseSubtexture;
    if (lo == "use_toon") return BNUseToon;
    if (lo == "spadd") return BNSpadd;
    if (lo == "parthf") return BNParthf;
    if (lo == "transp") return BNTransp;
    if (lo == "opadd") return BNOpadd;
    return BNNone;
}

std::string AnnotationString(const std::vector<MmeAnnotation>& a, std::string_view key) {
    const MmeAnnotation* an = FindAnnotation(a, key);
    if (!an) return {};
    if (!an->value.str.empty()) return an->value.str;
    return {};
}

bool AnnotationBool(const std::vector<MmeAnnotation>& a, std::string_view key, bool def) {
    const MmeAnnotation* an = FindAnnotation(a, key);
    if (!an) return def;
    if (an->value.kind == MmeValue::String) {
        const std::string lo = ToLower(an->value.str);
        if (lo == "true") return true;
        if (lo == "false") return false;
    }
    if (!an->value.nums.empty()) return an->value.nums[0] != 0.0f;
    return def;
}

// Parses a geometry matrix semantic (Appendix B). Returns false when not a matrix semantic.
bool ParseMatrixSemantic(const std::string& sem, int& base, int& suffix) {
    std::string s = sem;
    auto strip = [&](std::string_view suf, int code) -> bool {
        if (s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0) {
            s.erase(s.size() - suf.size());
            suffix = code;
            return true;
        }
        return false;
    };
    if (!strip("INVERSETRANSPOSE", MSInverseTranspose))
        if (!strip("TRANSPOSE", MSTranspose))
            if (!strip("INVERSE", MSInverse)) suffix = MSPlain;
    if (s == "WORLD") base = MBWorld;
    else if (s == "VIEW") base = MBView;
    else if (s == "PROJECTION") base = MBProj;
    else if (s == "WORLDVIEW") base = MBWorldView;
    else if (s == "VIEWPROJECTION") base = MBViewProj;
    else if (s == "WORLDVIEWPROJECTION") base = MBWorldViewProj;
    else return false;
    return true;
}

// MMD accessory order, to verify.
XMMATRIX AccessoryWorld(const MmeControllerPanel& p) {
    const float s = p.si * 10.0f;
    return XMMatrixScaling(s, s, s) * XMMatrixRotationRollPitchYaw(p.rx, p.ry, p.rz) *
           XMMatrixTranslation(p.x, p.y, p.z);
}

} // namespace

// M5c: shared with the effect-object mesh draw (PassMmeOffscreen).
XMMATRIX MmeAccessoryWorld(const MmeControllerPanel& panel) { return AccessoryWorld(panel); }

// M5a fix 2: MMD self-shadow light camera. See MmeSemantics.h. Orthographic, centred on the union of the
// character (non-stage) models' bounds, xy half-extent = radius + 2, depth range 2000 (1 .. 2001) with the bounds
// centred around depth ~1000. The rationale: one 2048-texel step on a slope changes normalised z by ~1e-5
// (x1500 = 0.03 < the 0.3 bias -> no acne), a real occluder >= 0.5 units in front gives >= 2.5e-4 (x1500 >= 0.37
// -> shadow).
void BuildLightCamera(const DirectX::XMFLOAT3& lightDir, const std::vector<const GpuModel*>& models,
                      const std::vector<MmeSceneObject>& objects, DirectX::XMFLOAT4X4& view,
                      DirectX::XMFLOAT4X4& proj) {
    XMFLOAT3 lo{FLT_MAX, FLT_MAX, FLT_MAX};
    XMFLOAT3 hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    bool have = false;
    auto grow = [&](const XMFLOAT3& a, const XMFLOAT3& b) {
        lo.x = std::min(lo.x, a.x); lo.y = std::min(lo.y, a.y); lo.z = std::min(lo.z, a.z);
        hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y); hi.z = std::max(hi.z, b.z);
        have = true;
    };
    // Bones lie inside the skin: hair, skirts and sleeves reach a few units past them.
    constexpr float kSkinMargin = 3.0f;
    for (const GpuModel* m : models) {
        if (!m) continue;
        if (m->Role() == ModelRole::Stage) continue;   // characters / non-stage only
        const MmeSceneObject* posed = nullptr;
        for (const MmeSceneObject& o : objects)
            if (o.gpu == m && !o.boneWorld.empty()) { posed = &o; break; }
        if (posed) {
            // Models carry helper bones far from the body (IK targets, physics anchors, effect controllers):
            // drop bones farther than kBodyRadius from the median bone position.
            constexpr float kBodyRadius = 18.0f;
            std::vector<float> xs, ys, zs;
            for (const XMFLOAT4X4& b : posed->boneWorld) { xs.push_back(b._41); ys.push_back(b._42); zs.push_back(b._43); }
            auto median = [](std::vector<float>& v) {
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                return v[v.size() / 2];
            };
            const XMVECTOR mid = XMVectorSet(median(xs), median(ys), median(zs), 0);
            for (const XMFLOAT4X4& b : posed->boneWorld) {
                const XMFLOAT3 p{b._41, b._42, b._43};
                if (XMVectorGetX(XMVector3Length(XMLoadFloat3(&p) - mid)) > kBodyRadius) continue;
                grow({p.x - kSkinMargin, p.y - kSkinMargin, p.z - kSkinMargin},
                     {p.x + kSkinMargin, p.y + kSkinMargin, p.z + kSkinMargin});
            }
        } else {
            grow(m->BoundsMin(), m->BoundsMax());
        }
    }
    XMVECTOR center;
    float radius;
    if (have) {
        center = XMLoadFloat3(&lo) + XMLoadFloat3(&hi);
        center = XMVectorScale(center, 0.5f);
        const XMVECTOR ext = XMLoadFloat3(&hi) - XMLoadFloat3(&lo);
        radius = XMVectorGetX(XMVector3Length(ext)) * 0.5f;
    } else {
        center = XMVectorZero();
        radius = 25.0f;
    }
    const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&lightDir));
    const float half = radius + 2.0f;
    const XMVECTOR up = (std::fabs(lightDir.y) > 0.999f) ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
    // Bounds centred around depth ~1000, light eye pushed far back along the light direction.
    const XMVECTOR focus = XMVectorAdd(center, XMVectorScale(dir, 1000.0f));
    const XMVECTOR eye = XMVectorSubtract(focus, XMVectorScale(dir, 2000.0f));
    XMStoreFloat4x4(&view, XMMatrixLookAtLH(eye, focus, up));
    XMStoreFloat4x4(&proj, XMMatrixOrthographicLH(2.0f * half, 2.0f * half, 1.0f, 2001.0f));
}

namespace {

XMMATRIX CombineMatrix(int base, const XMMATRIX& world, const XMMATRIX& view, const XMMATRIX& proj) {
    switch (base) {
        case MBWorld: return world;
        case MBView: return view;
        case MBProj: return proj;
        case MBWorldView: return world * view;
        case MBViewProj: return view * proj;
        case MBWorldViewProj: return world * view * proj;
    }
    return world;
}

XMMATRIX ApplySuffix(int suffix, const XMMATRIX& m) {
    switch (suffix) {
        case MSInverse: return XMMatrixInverse(nullptr, m);
        case MSTranspose: return XMMatrixTranspose(m);
        case MSInverseTranspose: return XMMatrixTranspose(XMMatrixInverse(nullptr, m));
    }
    return m;
}

void PutFloats(std::vector<uint8_t>& b, uint32_t off, const float* v, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (off + (uint32_t)(i * 4) + 4 > b.size()) break;
        std::memcpy(b.data() + off + i * 4, &v[i], 4);
    }
}

bool GetFloats(const std::vector<uint8_t>& b, const MmeBoundVar& v, float out[4]) {
    const uint32_t n = std::min<uint32_t>(4, v.size / 4);
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        if (v.offset + i * 4 + 4 > b.size()) return false;
        // A non-float variable's bytes are int/uint/bool, not IEEE floats: convert per component.
        if (v.kind == MmeCbVar::Int) {
            int32_t x = 0;
            std::memcpy(&x, b.data() + v.offset + i * 4, 4);
            out[i] = (float)x;
        } else if (v.kind == MmeCbVar::UInt) {
            uint32_t x = 0;
            std::memcpy(&x, b.data() + v.offset + i * 4, 4);
            out[i] = (float)x;
        } else if (v.kind == MmeCbVar::Bool) {
            uint32_t x = 0;
            std::memcpy(&x, b.data() + v.offset + i * 4, 4);
            out[i] = x ? 1.0f : 0.0f;
        } else {
            std::memcpy(&out[i], b.data() + v.offset + i * 4, 4);
        }
    }
    return n > 0;
}

} // namespace

void MmeConstantWriter::Bind(const MmeEffectDesc& fx, const MmeStage& stage, std::vector<MmeDiag>& diags) {
    bytes_.assign(stage.cbSize, 0);
    vars_.clear();
    vars_.reserve(stage.cb.size());
    std::set<std::string> unknownLogged;
    for (const MmeCbVar& cv : stage.cb) {
        if (!cv.defaultValue.empty()) {
            const uint32_t n = std::min<uint32_t>((uint32_t)cv.defaultValue.size(), stage.cbSize > cv.offset ? stage.cbSize - cv.offset : 0);
            std::memcpy(bytes_.data() + cv.offset, cv.defaultValue.data(), n);
        }
        MmeBoundVar bv;
        bv.name = cv.name;
        bv.offset = cv.offset;
        bv.size = cv.size;
        bv.rows = cv.rows;
        bv.columns = cv.columns;
        bv.elements = cv.elements;
        bv.columnMajor = cv.columnMajor;
        bv.kind = cv.kind;
        bv.provider = PNone;

        const MmeParam* p = FindParamCI(fx, cv.name);
        if (p && !p->semantic.empty()) {
            bv.semantic = p->semantic;
            bv.hasSemantic = true;
            const std::string sem = p->semantic;   // already UPPER from M1
            const std::string obj = ToLower(AnnotationString(p->annotations, "Object"));
            int base = 0, suffix = 0;
            if (ParseMatrixSemantic(sem, base, suffix)) {
                bv.provider = PMatrix;
                bv.matrixBase = base;
                bv.matrixSuffix = suffix;
                bv.objectLight = (obj == "light");
            } else if (sem == "DIFFUSE" || sem == "AMBIENT" || sem == "SPECULAR" || sem == "EMISSIVE") {
                if (obj == "geometry") {
                    // M4b: per-material value (spec §4.2); the object pass fills MmeObjectValues. colorKind is
                    // set next (shared with the frame-level PColor path).
                    bv.provider = PObjectColor;
                    bv.objectGeometry = true;
                } else {
                    bv.provider = PColor;
                    bv.objectLight = (obj == "light");
                }
                bv.colorKind = (sem == "DIFFUSE")   ? CKDiffuse
                               : (sem == "AMBIENT") ? CKAmbient
                               : (sem == "SPECULAR") ? CKSpecular
                                                     : CKEemissive;
            } else if (sem == "SPECULARPOWER") {
                if (obj == "geometry") {
                    bv.provider = PObjectSpecularPower;
                    bv.objectGeometry = true;
                } else {
                    bv.provider = PSpecularPower;
                }
            } else if (sem == "TOONCOLOR") {
                bv.provider = PObjectToonColor;
                bv.objectGeometry = true;
            } else if (sem == "EDGECOLOR") {
                bv.provider = PObjectEdgeColor;
                bv.objectGeometry = true;
            } else if (sem == "POSITION" || sem == "DIRECTION") {
                bv.provider = PPositionDirection;
                bv.objectLight = (obj == "light");
                bv.posDirKind = (sem == "POSITION") ? PDPosition : PDDirection;
            } else if (sem == "VIEWPORTPIXELSIZE") {
                bv.provider = PViewportPixelSize;
            } else if (sem == "TIME") {
                bv.provider = PTime;
                bv.syncInEditMode = AnnotationBool(p->annotations, "SyncInEditMode", false);
            } else if (sem == "ELAPSEDTIME") {
                bv.provider = PElapsedTime;
                bv.syncInEditMode = AnnotationBool(p->annotations, "SyncInEditMode", false);
            } else if (sem == "MOUSEPOSITION" || sem == "LEFTMOUSEDOWN" || sem == "MIDDLEMOUSEDOWN" ||
                       sem == "RIGHTMOUSEDOWN") {
                bv.provider = PMouse;
            } else if (sem == "CONTROLOBJECT") {
                bv.provider = PControlObject;
                bv.coName = ToLower(AnnotationString(p->annotations, "name"));
                bv.coItem = AnnotationString(p->annotations, "item");
            } else if (sem == "ADDINGTEXTURE") {
                bv.provider = PAddingTexture;
            } else if (sem == "MULTIPLYINGTEXTURE") {
                bv.provider = PMultiplyingTexture;
            } else if (sem == "ADDINGSPHERETEXTURE") {
                bv.provider = PAddingSphereTexture;
            } else if (sem == "MULTIPLYINGSPHERETEXTURE") {
                bv.provider = PMultiplyingSphereTexture;
            } else if (sem == "TEXTUREVALUE") {
                bv.provider = PTexValue;
            } else {
                // Unknown (or metadata-only: STANDARDSGLOBAL / textures) -> keep the default, log once.
                if (sem != "STANDARDSGLOBAL" && sem.rfind("MATERIAL", 0) != 0 &&
                    sem != "RENDERCOLORTARGET" && sem != "RENDERDEPTHSTENCILTARGET" &&
                    sem != "OFFSCREENRENDERTARGET" && sem != "ANIMATEDTEXTURE" && sem != "TEXTUREVALUE" &&
                    sem != "TOONCOLOR" && sem != "EDGECOLOR" && sem != "GROUNDSHADOWCOLOR") {
                    if (unknownLogged.insert(sem).second) {
                        MmeDiag d;
                        d.sev = MmeDiag::Warning;
                        d.message = "MME binder: unknown semantic '" + sem + "' (default kept)";
                        diags.push_back(d);
                    }
                }
            }
        }
        // M4b: by-name uniforms (spec draft §5) take values with no semantic; only object techniques read them,
        // so the frame-level Write leaves them at their defaults and WriteObject fills them.
        if (bv.provider == PNone) {
            const int bn = ByNameId(cv.name);
            if (bn != BNNone) {
                bv.provider = PByName;
                bv.byName = bn;
            }
        }
        vars_.push_back(std::move(bv));
    }
}

void MmeConstantWriter::Write(const MmeFrameValues& v) {
    const XMMATRIX world = AccessoryWorld(v.self);
    XMMATRIX camView = XMLoadFloat4x4(&v.view);
    XMMATRIX camProj = XMLoadFloat4x4(&v.proj);
    // M5a fix 2: the MMD self-shadow light camera is computed once per frame by the caller and passed in, so the
    // zplot pass and every object_ss draw of one frame share the same matrices.
    const XMMATRIX lightView = XMLoadFloat4x4(&v.lightView);
    const XMMATRIX lightProj = XMLoadFloat4x4(&v.lightProj);

    for (const MmeBoundVar& bv : vars_) {
        if (bv.provider == PNone) continue;
        switch (bv.provider) {
            case PMatrix: {
                const XMMATRIX view = bv.objectLight ? lightView : camView;
                const XMMATRIX proj = bv.objectLight ? lightProj : camProj;
                XMMATRIX m = CombineMatrix(bv.matrixBase, world, view, proj);
                m = ApplySuffix(bv.matrixSuffix, m);
                if (bv.columnMajor) {
                    m = XMMatrixTranspose(m);
                }
                XMFLOAT4X4 f;
                XMStoreFloat4x4(&f, m);
                PutFloats(bytes_, bv.offset, &f.m[0][0], 16);
                break;
            }
            case PColor: {
                float c[4] = {0, 0, 0, 1};
                if (bv.objectLight) {
                    c[0] = v.lightColor.x; c[1] = v.lightColor.y; c[2] = v.lightColor.z; c[3] = 1.0f;
                } else {
                    // Controller accessory material (no .x mesh): diffuse (1,1,1,tr), ambient (1,1,1),
                    // specular (0,0,0), emissive (0,0,0).
                    switch (bv.colorKind) {
                        case CKDiffuse: c[0] = c[1] = c[2] = 1.0f; c[3] = v.self.tr; break;
                        case CKAmbient: c[0] = c[1] = c[2] = 1.0f; c[3] = 1.0f; break;
                        default: c[0] = c[1] = c[2] = 0.0f; c[3] = 1.0f; break;
                    }
                }
                PutFloats(bytes_, bv.offset, c, 4);
                break;
            }
            case PObjectColor: {
                // A Material="Geometry" colour read by a technique that is not an object draw (a scene effect's
                // auto-generated loop / material pass, or a post effect): the controller accessory's values, the same
                // as the frame-level PColor default (diffuse (1,1,1,tr), ambient (1,1,1), else (0,0,0,1)).
                float c[4] = {0, 0, 0, 1};
                switch (bv.colorKind) {
                    case CKDiffuse: c[0] = c[1] = c[2] = 1.0f; c[3] = v.self.tr; break;
                    case CKAmbient: c[0] = c[1] = c[2] = 1.0f; c[3] = 1.0f; break;
                    default: break;
                }
                PutFloats(bytes_, bv.offset, c, 4);
                break;
            }
            case PObjectSpecularPower: {
                const float c = 0.0f;
                PutFloats(bytes_, bv.offset, &c, 1);
                break;
            }
            case PObjectToonColor: {
                const float c[4] = {1, 1, 1, 1};
                PutFloats(bytes_, bv.offset, c, 4);
                break;
            }
            case PObjectEdgeColor: {
                const float c[4] = {0, 0, 0, 1};
                PutFloats(bytes_, bv.offset, c, 4);
                break;
            }
            case PPositionDirection: {
                float c[4] = {0, 0, 0, 0};
                if (bv.posDirKind == PDPosition) {
                    if (bv.objectLight) { c[0] = -v.lightDir.x; c[1] = -v.lightDir.y; c[2] = -v.lightDir.z; c[3] = 0.0f; }
                    else { c[0] = v.cameraPos.x; c[1] = v.cameraPos.y; c[2] = v.cameraPos.z; c[3] = 1.0f; }
                } else {
                    if (bv.objectLight) { c[0] = v.lightDir.x; c[1] = v.lightDir.y; c[2] = v.lightDir.z; }
                    else { c[0] = v.cameraDir.x; c[1] = v.cameraDir.y; c[2] = v.cameraDir.z; }
                    c[3] = 0.0f;
                }
                PutFloats(bytes_, bv.offset, c, 4);
                break;
            }
            case PViewportPixelSize: {
                const float c[4] = {v.viewportW, v.viewportH, 0.0f, 0.0f};
                PutFloats(bytes_, bv.offset, c, 4);
                break;
            }
            case PTime: {
                const float c = bv.syncInEditMode ? v.sceneTime : v.time;
                PutFloats(bytes_, bv.offset, &c, 1);
                break;
            }
            case PElapsedTime: {
                const float c = bv.syncInEditMode ? v.sceneElapsed : v.elapsed;
                PutFloats(bytes_, bv.offset, &c, 1);
                break;
            }
            case PMouse: {
                const float c[4] = {0, 0, 0, 0};
                PutFloats(bytes_, bv.offset, c, 4);
                break;
            }
            case PAddingTexture:
            case PAddingSphereTexture: {
                const float c[4] = {0, 0, 0, 0};
                PutFloats(bytes_, bv.offset, c, 4);
                break;
            }
            case PMultiplyingTexture:
            case PMultiplyingSphereTexture: {
                const float c[4] = {1, 1, 1, 1};
                PutFloats(bytes_, bv.offset, c, 4);
                break;
            }
            case PControlObject: {
                WriteControlObject(bv, v);
                break;
            }
            case PTexValue: {
                if (v.textureValue) {
                    uint32_t n = 0;
                    const float* texels = v.textureValue(bv.name, n);
                    if (texels && n) {
                        // Cap at the parameter's element count (a float array's rows * elements).
                        uint32_t written = n;
                        if (bv.elements) written = std::min(n, bv.elements);
                        const uint32_t cap = bv.size / 16;   // float4s the cb var can hold
                        if (cap) written = std::min(written, cap);
                        PutFloats(bytes_, bv.offset, texels, (size_t)written * 4);
                    }
                }
                break;
            }
            default: break;
        }
    }
}

void MmeConstantWriter::WriteControlObject(const MmeBoundVar& bv, const MmeFrameValues& v) {
    const bool isSelf = bv.coName.empty() || bv.coName == "(self)";
    const bool isOwner = bv.coName == "(offscreenowner)";
    const std::string item = bv.coItem;
    const std::string itemLo = ToLower(item);

    auto put = [&](const float* c, size_t n) { PutFloats(bytes_, bv.offset, c, n); };

    auto writePanel = [&](const MmeControllerPanel& p) {
        if (itemLo.empty()) {
            if (bv.kind == MmeCbVar::Bool) {
                const float c = p.visible ? 1.0f : 0.0f;
                put(&c, 1);
            } else if (bv.rows >= 4 && bv.columns >= 4) {
                XMFLOAT4X4 f;
                XMStoreFloat4x4(&f, AccessoryWorld(p));
                put(&f.m[0][0], 16);
            } else if (bv.size / 4 >= 3) {
                const float c[4] = {p.x, p.y, p.z, 1.0f};
                put(c, 4);
            } else {
                const float c = p.si * 10.0f;
                put(&c, 1);
            }
        } else if (itemLo == "x" || itemLo == "y" || itemLo == "z") {
            const float c = (itemLo == "x") ? p.x : (itemLo == "y") ? p.y : p.z;
            put(&c, 1);
        } else if (itemLo == "xyz") {
            const float c[4] = {p.x, p.y, p.z, 1.0f};
            put(c, 4);
        } else if (itemLo == "rx" || itemLo == "ry" || itemLo == "rz") {
            const float c = (itemLo == "rx") ? p.rx : (itemLo == "ry") ? p.ry : p.rz;
            put(&c, 1);
        } else if (itemLo == "rxyz") {
            const float c[4] = {p.rx, p.ry, p.rz, 0.0f};
            put(c, 4);
        } else if (itemLo == "si") {
            const float c = p.si * 10.0f;
            put(&c, 1);
        } else if (itemLo == "tr") {
            const float c = p.tr;
            put(&c, 1);
        }
    };

    if (isSelf) {
        writePanel(v.self);
        return;
    }
    if (isOwner) {
        // (OffscreenOwner): the owner effect's controller panel (only meaningful inside an offscreen draw).
        writePanel(v.offscreenOwner);
        return;
    }

    // Named object: ask the host. On failure use the spec's missing-object defaults.
    float out[16] = {0};
    int count = 0;
    bool ok = false;
    const int kind = (bv.kind == MmeCbVar::Bool)                     ? 0
                     : (bv.rows >= 4 && bv.columns >= 4)             ? 3
                     : (bv.size / 4 >= 3)                            ? 2
                                                                     : 1;
    if (v.lookup) ok = v.lookup(bv.coName, item, kind, out, count);
    if (ok && count > 0) {
        put(out, (size_t)count);
        return;
    }
    const bool looksPmx = (bv.coName.size() >= 4 &&
                           (bv.coName.compare(bv.coName.size() - 4, 4, ".pmx") == 0 ||
                            bv.coName.compare(bv.coName.size() - 4, 4, ".pmd") == 0));
    if (looksPmx) {
        if (bv.rows >= 4 && bv.columns >= 4) {
            const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            put(id, 16);
        } else if (bv.kind == MmeCbVar::Bool) {
            // bool = "the object is loaded": false for a missing model (mme_design.md CONTROLOBJECT defaults). True made controller-driven
            // effects (AirDust's WorldParticleController.pmd) switch to their absent controller's zero values.
            const float c = 0.0f;
            put(&c, 1);
        } else if (bv.size / 4 >= 3) {
            const float c[4] = {0, 0, 0, 1};
            put(c, 4);
        } else {
            // A named item (a morph such as "Transparent", a bone value) of a missing model reads 0 in MME; only the
            // item-less scalar (the model's scale) defaults to 1. Morph = 1 made WorldParticleController-driven
            // particles invisible (Alpha *= 1 - Transparent).
            const float c = itemLo.empty() ? 1.0f : 0.0f;
            put(&c, 1);
        }
    } else {
        // .x (or unknown) controller accessory that is not in the scene: MMD still reports the default accessory
        // panel (a scale-10 world, panel X/Y/Z = 0, Si = 10, Tr = 1, Rxyz = 0). M7 fix 3: an explicit panel item
        // (Tr, XYZ, Rxyz, Si…) uses those defaults; previously every scalar defaulted to 10, so e.g. a missing
        // `...Tr` returned 10 instead of 1 and drove glow/shadow mixes to black.
        if (bv.kind == MmeCbVar::Bool) {
            const float c = 0.0f;
            put(&c, 1);
        } else if (!itemLo.empty() && itemLo != "x" && itemLo != "y" && itemLo != "z") {
            writePanel(MmeControllerPanel{});
        } else if (bv.rows >= 4 && bv.columns >= 4) {
            XMFLOAT4X4 f;
            XMStoreFloat4x4(&f, XMMatrixScaling(10, 10, 10));
            put(&f.m[0][0], 16);
        } else if (bv.size / 4 >= 3) {
            const float c[4] = {0, 0, 0, 1};
            put(c, 4);
        } else {
            const float c = 10.0f;
            put(&c, 1);
        }
    }
}

bool MmeConstantWriter::SetScalar(const std::string& param, float value) {
    for (const MmeBoundVar& bv : vars_) {
        if (bv.name != param && ToLower(bv.name) != ToLower(param)) continue;
        if (bv.offset + 4 > bytes_.size()) return false;
        // Store the value in the variable's own representation (an int slot must not get float bits).
        if (bv.kind == MmeCbVar::Int) {
            const int32_t x = (int32_t)value;
            std::memcpy(bytes_.data() + bv.offset, &x, 4);
        } else if (bv.kind == MmeCbVar::UInt) {
            const uint32_t x = (uint32_t)value;
            std::memcpy(bytes_.data() + bv.offset, &x, 4);
        } else if (bv.kind == MmeCbVar::Bool) {
            const uint32_t x = value != 0.0f ? 1u : 0u;
            std::memcpy(bytes_.data() + bv.offset, &x, 4);
        } else {
            std::memcpy(bytes_.data() + bv.offset, &value, 4);
        }
        return true;
    }
    return false;
}

bool MmeConstantWriter::ReadParam(const MmeEffectDesc& fx, const std::vector<MmeConstantWriter*>& writers,
                                  const std::string& param, float out[4]) {
    for (const MmeConstantWriter* w : writers) {
        if (!w) continue;
        for (const MmeBoundVar& bv : w->vars_) {
            if (bv.name == param || ToLower(bv.name) == ToLower(param)) {
                return GetFloats(w->bytes_, bv, out);
            }
        }
    }
    // Fall back to the M1 initializer (the variable may be unused by the compiled shader).
    const MmeParam* p = FindParamCI(fx, param);
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (p && p->hasInit) {
        for (size_t i = 0; i < p->init.nums.size() && i < 4; ++i) out[i] = p->init.nums[i];
        return true;
    }
    return false;
}

bool MmeConstantWriter::ReadValue(const std::string& name, float out[4]) const {
    for (const MmeBoundVar& bv : vars_)
        if (bv.name == name || ToLower(bv.name) == ToLower(name)) return GetFloats(bytes_, bv, out);
    return false;
}

void MmeConstantWriter::WriteObject(const MmeObjectValues& o) {
    auto putb = [&](const MmeBoundVar& bv, bool value) {
        if (bv.offset + 4 > bytes_.size()) return;
        const uint32_t x = value ? 1u : 0u;
        std::memcpy(bytes_.data() + bv.offset, &x, 4);
    };
    for (const MmeBoundVar& bv : vars_) {
        switch (bv.provider) {
            case PObjectColor: {
                const XMFLOAT4* c = nullptr;
                switch (bv.colorKind) {
                    case CKDiffuse: c = &o.diffuse; break;
                    case CKAmbient: c = &o.ambient; break;
                    case CKSpecular: c = &o.specular; break;
                    default: c = &o.emissive; break;
                }
                PutFloats(bytes_, bv.offset, &c->x, 4);
                break;
            }
            case PObjectSpecularPower: {
                PutFloats(bytes_, bv.offset, &o.specularPower, 1);
                break;
            }
            case PObjectToonColor: {
                PutFloats(bytes_, bv.offset, &o.toonColor.x, 4);
                break;
            }
            case PObjectEdgeColor: {
                PutFloats(bytes_, bv.offset, &o.edgeColor.x, 4);
                break;
            }
            case PAddingTexture: {
                PutFloats(bytes_, bv.offset, &o.texAdd.x, 4);
                break;
            }
            case PMultiplyingTexture: {
                PutFloats(bytes_, bv.offset, &o.texMul.x, 4);
                break;
            }
            case PAddingSphereTexture: {
                PutFloats(bytes_, bv.offset, &o.sphereAdd.x, 4);
                break;
            }
            case PMultiplyingSphereTexture: {
                PutFloats(bytes_, bv.offset, &o.sphereMul.x, 4);
                break;
            }
            case PByName: {
                switch (bv.byName) {
                    case BNUseTexture: putb(bv, o.useTexture); break;
                    case BNUseSpheremap: putb(bv, o.useSpheremap); break;
                    case BNUseSubtexture: putb(bv, o.useSubtexture); break;
                    case BNUseToon: putb(bv, o.useToon); break;
                    case BNSpadd: putb(bv, o.spadd); break;
                    case BNParthf: putb(bv, o.parthf); break;
                    case BNTransp: putb(bv, o.transp); break;
                    case BNOpadd: putb(bv, o.opadd); break;
                    default: break;
                }
                break;
            }
            default: break;
        }
    }
}

} // namespace mmdx
