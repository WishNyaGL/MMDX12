// Rig viewport controllers (stage 3): overlay rings for the body and the hands/feet. Hands/feet are chain
// controllers — dragging them solves the two-bone IK (RigSolver) and writes the result to the pose layer (one undo
// step per gesture), so keys are ordinary bone keys. Body rings just select the underlying bone (FK via the gizmo).
#include "app/App.h"

#include "studio/RigSolver.h"
#include "studio/StudioDoc.h"
#include "studio/Gizmo.h"  // BuildMouseRay / IntersectRayPlane / ViewProj

#include "app/Icons.h"
#include "app/UiKit.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "imgui.h"

#include <cmath>

namespace mmdx {

using namespace studio;
using namespace DirectX;

namespace {

// Chain controller ids.
enum : int { kChainLeftHand = 0, kChainRightHand, kChainLeftFoot, kChainRightFoot, kChainLeftToe, kChainRightToe };

struct ChainInfo {
    RigRole root, mid, end;
    const char* undo;
};
const ChainInfo kChains[6] = {
    {RigRole::LeftUpperArm,  RigRole::LeftForearm,  RigRole::LeftHand,  "본 이동 (리그)"},
    {RigRole::RightUpperArm, RigRole::RightForearm, RigRole::RightHand, "본 이동 (리그)"},
    {RigRole::LeftThigh,     RigRole::LeftShin,     RigRole::LeftFoot,  "본 이동 (리그)"},
    {RigRole::RightThigh,    RigRole::RightShin,    RigRole::RightFoot, "본 이동 (리그)"},
    {RigRole::LeftFoot,      RigRole::LeftToe,      RigRole::LeftToe,   "본 이동 (리그)"},
    {RigRole::RightFoot,     RigRole::RightToe,     RigRole::RightToe,  "본 이동 (리그)"},
};

// Find the model's own IK bone that drives a rig end role (arm/leg). Returns -1 when none.
int FindIkBoneForRole(const RigDefinition& rig, RigRole role) {
    return rig.IkBoneFor(role);
}
int FindLegIkBone(const PmxModel& pmx, int target) {
    for (int bi = 0; bi < (int)pmx.bones.size(); ++bi) {
        const PmxBone& b = pmx.bones[(size_t)bi];
        if ((b.flags & PmxBone_IK) && b.ikTargetIndex == target) return bi;
    }
    return -1;
}

struct BodyInfo {
    RigRole role;
    const char* label;
};
const BodyInfo kBody[6] = {
    {RigRole::Pelvis, "center"},
    {RigRole::Groove, "groove"},
    {RigRole::Spine,  "spine"},
    {RigRole::Chest,  "chest"},
    {RigRole::Neck,   "neck"},
    {RigRole::Head,   "head"},
};

} // namespace

void App::StudioRigViewport(float x0, float y0, float x1, float y1, bool hovered, bool& consumed) {
    StudioDoc& d = *studio_;
    StudioModel* m = StudioPoseModel();
    if (!m || !m->rig || !m->inst) return;
    // only characters get rig controllers
    if (m->kind != ModelKind::Character) return;
    if (!m->showRig) return;

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool inside = mouse.x >= x0 && mouse.x <= x1 && mouse.y >= y0 && mouse.y <= y1;
    const ui::Palette& p = ui::P();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    const float dpi = ui::Dpi();
    const float R = 9.0f * dpi;

    auto bodyWeight = [&](RigRole role) -> int {
        return m->rig->BoneFor(role);
    };

    // ---- draw + pick ----
    int hoverChain = -1;
    int hoverBody = -1;
    ImVec2 chainPos[6];
    bool chainProj[6] = {false, false, false, false, false, false};

    // hands/feet/toes rings
    for (int i = 0; i < 6; ++i) {
        const int end = m->rig->BoneFor(kChains[i].end);
        if (end < 0) continue;
        // draw on the model's ready IK bone (arm/leg) when present (the control target); else on the end bone
        int vis = end;
        {
            const int ik = m->rig->IkBoneFor(kChains[i].end);
            if (ik >= 0) vis = ik;
        }
        const XMFLOAT3 w = BoneJointWorld(*m->pmx, *m->inst, vis);
        ImVec2 s;
        if (!studioVp_.Project(w, s)) continue;
        chainPos[i] = s;
        chainProj[i] = true;
        const bool hover = inside && std::hypot(mouse.x - s.x, mouse.y - s.y) <= R + 3.0f * dpi;
        if (hover) hoverChain = i;
        const bool sel = d.rigController == i;
        const ImU32 col = hover ? IM_COL32(255, 220, 80, 255)
                          : sel ? IM_COL32(120, 200, 255, 255)
                                : IM_COL32(180, 230, 190, 230);
        dl->AddCircleFilled(s, R, ui::WithAlpha(p.sunken, 0.8f), 24);
        dl->AddCircle(s, R, col, 24, 2.0f * dpi);
        dl->AddCircleFilled(s, 3.0f * dpi, col, 12);
    }

    // body rings
    ImVec2 bodyCenterPx;
    bool bodyCenterProj = false;
    for (int i = 0; i < 6; ++i) {
        const int b = bodyWeight(kBody[i].role);
        if (b < 0) continue;
        const XMFLOAT3 w = BoneJointWorld(*m->pmx, *m->inst, b);
        ImVec2 s;
        if (!studioVp_.Project(w, s)) continue;
        if (i == 0) { bodyCenterPx = s; bodyCenterProj = true; }
        const bool hover = inside && std::hypot(mouse.x - s.x, mouse.y - s.y) <= R + 2.0f * dpi;
        if (hover) hoverBody = i;
        const bool sel = d.rigController == 6 + i;
        const ImU32 col = hover ? IM_COL32(255, 220, 80, 255)
                          : sel ? IM_COL32(120, 200, 255, 255)
                                : IM_COL32(120, 160, 240, 210);
        dl->AddCircle(s, R * 1.15f, col, 28, 2.0f * dpi);
        dl->AddCircleFilled(s, 2.5f * dpi, col, 12);
        if (i == 1 && bodyCenterProj) {
            // arrow from center -> groove (outline + fill for visibility)
            ImVec2 dir(s.x - bodyCenterPx.x, s.y - bodyCenterPx.y);
            const float len = std::hypot(dir.x, dir.y);
            if (len > 1e-3f) {
                dir.x /= len; dir.y /= len;
                const float hl = 7.0f * dpi;
                const ImVec2 tip(s.x - dir.x * R * 1.3f, s.y - dir.y * R * 1.3f);
                const ImVec2 base(tip.x - dir.x * hl, tip.y - dir.y * hl);
                const ImVec2 perp(-dir.y, dir.x);
                // outline (dark, thicker) then fill (orange)
                dl->AddLine(bodyCenterPx, s, IM_COL32(30, 30, 30, 220), 3.0f * dpi);
                dl->AddLine(bodyCenterPx, s, IM_COL32(255, 190, 100, 240), 1.5f * dpi);
                dl->AddTriangleFilled(tip, ImVec2(base.x + perp.x * 5.0f * dpi, base.y + perp.y * 5.0f * dpi),
                                      ImVec2(base.x - perp.x * 5.0f * dpi, base.y - perp.y * 5.0f * dpi),
                                      IM_COL32(30, 30, 30, 220));
                dl->AddTriangleFilled(tip, ImVec2(base.x + perp.x * 3.5f * dpi, base.y + perp.y * 3.5f * dpi),
                                      ImVec2(base.x - perp.x * 3.5f * dpi, base.y - perp.y * 3.5f * dpi),
                                      IM_COL32(255, 190, 100, 240));
            }
        }
    }

    // ---- interaction ----
    if (rigVpDrag_.active) {
        consumed = true;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // commit one undo step
            const char* undoName = rigVpDrag_.bodyBone >= 0 ? Tr("본 이동 (리그)") : Tr(kChains[rigVpDrag_.chainId].undo);
            StudioSetPose(undoName, studioPoseBefore_);
            rigVpDrag_.active = false;
            rigVpDrag_.bodyBone = -1;
        } else if (rigVpDrag_.bodyBone >= 0) {
            // pelvis drag: exact ray-plane solve (like hands/feet) so the pelvis follows the cursor
            StudioApplyPose(*m);
            const int bone = rigVpDrag_.bodyBone;
            MouseRay ray;
            if (BuildMouseRay(studioVp_, mouse, ray)) {
                XMFLOAT3 hit; float t = 0;
                if (IntersectRayPlane(ray, rigVpDrag_.planeOrigin, rigVpDrag_.planeNormal, hit, &t)) {
                    const XMFLOAT3 target{hit.x - rigVpDrag_.grabOffset.x,
                                          hit.y - rigVpDrag_.grabOffset.y,
                                          hit.z - rigVpDrag_.grabOffset.z};
                    const PmxBone& pb = m->pmx->bones[(size_t)bone];
                    const int parent = pb.parentIndex;
                    const XMMATRIX parentWorld = parent >= 0 ? XMLoadFloat4x4(&m->inst->BoneWorld(parent))
                                                             : XMMatrixIdentity();
                    const XMMATRIX inv = XMMatrixInverse(nullptr, parentWorld);
                    XMFLOAT3 v; XMStoreFloat3(&v, XMVector3TransformCoord(XMLoadFloat3(&target), inv));
                    XMFLOAT3 offset{0, 0, 0};
                    if (parent >= 0 && parent < (int)m->pmx->bones.size()) {
                        const XMFLOAT3& pos = pb.position;
                        const XMFLOAT3& pp = m->pmx->bones[(size_t)parent].position;
                        offset = {pos.x - pp.x, pos.y - pp.y, pos.z - pp.z};
                    }
                    PoseLayer& pose = m->pose;
                    const int frame = d.Frame();
                    if (pose.frame != frame) pose = PoseLayer{};
                    pose.frame = frame;
                    PoseBone vv = StudioCurrentPose(*m)[(size_t)bone];
                    vv.t = {v.x - offset.x, v.y - offset.y, v.z - offset.z};
                    pose.bones[bone] = vv;
                }
            }
        } else {
            // gizmo-style translation: the target follows the pointer 1:1 in the camera plane
            const float dx = mouse.x - rigVpDrag_.startMouse.x;
            const float dy = mouse.y - rigVpDrag_.startMouse.y;
            const XMFLOAT3& Rv = rigVpDrag_.viewRight;
            const XMFLOAT3& U = rigVpDrag_.viewUp;
            const float pw = rigVpDrag_.pixelWorld;
            const float tx = (dx * Rv.x - dy * U.x) * pw;
            const float ty = (dx * Rv.y - dy * U.y) * pw;
            const float tz = (dx * Rv.z - dy * U.z) * pw;

            PoseLayer& pose = m->pose;
            const int frame = d.Frame();
            if (pose.frame != frame) pose = PoseLayer{};
            pose.frame = frame;

            if (rigVpDrag_.footIkBone >= 0) {
                // drive a ready model IK bone (arm/leg) as an object: project the mouse ray onto the FIXED plane
                // captured at drag start (no chasing -> no drift). Re-apply the pose first so parents/world match.
                {
                    StudioApplyPose(*m);
                    const int bone = rigVpDrag_.footIkBone;
                    MouseRay ray;
                    if (BuildMouseRay(studioVp_, mouse, ray)) {
                        XMFLOAT3 hit;
                        float t = 0;
                        if (IntersectRayPlane(ray, rigVpDrag_.planeOrigin, rigVpDrag_.planeNormal, hit, &t)) {
                            const XMFLOAT3 target{hit.x - rigVpDrag_.grabOffset.x,
                                                  hit.y - rigVpDrag_.grabOffset.y,
                                                  hit.z - rigVpDrag_.grabOffset.z};
                            const PmxBone& pb = m->pmx->bones[(size_t)bone];
                            const int parent = pb.parentIndex;
                            const XMMATRIX parentWorld = parent >= 0 ? XMLoadFloat4x4(&m->inst->BoneWorld(parent))
                                                                     : XMMatrixIdentity();
                            const XMMATRIX inv = XMMatrixInverse(nullptr, parentWorld);
                            XMFLOAT3 v;
                            XMStoreFloat3(&v, XMVector3TransformCoord(XMLoadFloat3(&target), inv));
                            XMFLOAT3 offset{0, 0, 0};
                            if (parent >= 0 && parent < (int)m->pmx->bones.size()) {
                                const XMFLOAT3& pos = pb.position;
                                const XMFLOAT3& pp = m->pmx->bones[(size_t)parent].position;
                                offset = {pos.x - pp.x, pos.y - pp.y, pos.z - pp.z};
                            }
                            const XMFLOAT3 localT{v.x - offset.x, v.y - offset.y, v.z - offset.z};
                            PoseBone vv = StudioCurrentPose(*m)[(size_t)bone];
                            vv.t = localT;
                            pose.bones[bone] = vv;
                        }
                    }
                }
            } else {
                // hands: two-bone IK solve to the pointer target
                const XMFLOAT3& e = rigVpDrag_.drag.endPos;
                const XMFLOAT3 target{e.x + tx, e.y + ty, e.z + tz};
                RigChainResult r;
                if (SolveRigChainDrag(rigVpDrag_.drag, target, &r)) {
                    PoseBone v0 = StudioCurrentPose(*m)[(size_t)rigVpDrag_.chain.root];
                    v0.r = r.rootLocal;
                    PoseBone v1 = StudioCurrentPose(*m)[(size_t)rigVpDrag_.chain.mid];
                    v1.r = r.midLocal;
                    PoseBone v2 = StudioCurrentPose(*m)[(size_t)rigVpDrag_.chain.end];
                    v2.r = r.endLocal;
                    pose.bones[rigVpDrag_.chain.root] = v0;
                    pose.bones[rigVpDrag_.chain.mid] = v1;
                    pose.bones[rigVpDrag_.chain.end] = v2;
                }
            }
        }
        dl->PopClipRect();
        return;
    }

    // press: start a chain drag (move tool) or select a bone so the gizmo appears (rotate tool). The selection is
    // registered as a bone-pick (studioViewDrag_ = 3) so the viewport does not clear it as an "empty click".
    if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        if (hoverChain >= 0 && chainProj[hoverChain] && d.gizmoTool == 1) {
            const ChainInfo& ci = kChains[hoverChain];
            StudioSelectRigController(hoverChain, io.KeyCtrl);  // select the controller (viewport ring <-> timeline row)
            RigChain chain;
            if (ResolveRigChain(*m->rig, ci.root, ci.mid, ci.end, chain)) {
                rigVpDrag_.chain = chain;
                rigVpDrag_.drag = BeginRigChainDrag(*m->inst, chain);
                if (rigVpDrag_.drag.valid) {
                    rigVpDrag_.chainId = hoverChain;
                    rigVpDrag_.active = true;
                    rigVpDrag_.startMouse = mouse;
                    rigVpDrag_.bodyBone = -1;
                    studioPoseBefore_ = m->pose;
                    // choose the drive: if the model has a ready IK bone for this limb end, move that IK bone as an
                    // object; otherwise hands fall back to the two-bone IK.
                    const RigRole endRole = kChains[hoverChain].end;
                    rigVpDrag_.footIkBone = m->rig->IkBoneFor(endRole);
                    if (rigVpDrag_.footIkBone >= 0)
                        rigVpDrag_.planeOrigin = BoneJointWorld(*m->pmx, *m->inst, rigVpDrag_.footIkBone);
                    else
                        rigVpDrag_.planeOrigin = rigVpDrag_.drag.endPos;
                    {
                        // camera-right/up (for hands) and a plane normal facing the camera (from the point toward the
                        // eye), so projection is exact regardless of the matrix's sign conventions.
                        const XMFLOAT4X4& vm = studioVp_.view;
                        rigVpDrag_.viewRight = {vm._11, vm._21, vm._31};
                        rigVpDrag_.viewUp = {vm._12, vm._22, vm._32};
                        XMVECTOR n = XMVectorSubtract(XMLoadFloat3(&studioVp_.eye), XMLoadFloat3(&rigVpDrag_.planeOrigin));
                        const float nl = XMVectorGetX(XMVector3Length(n));
                        if (nl > 1e-6f) n = XMVectorScale(n, 1.0f / nl);
                        else n = XMVectorSet(0, 0, 1, 0);
                        XMStoreFloat3(&rigVpDrag_.planeNormal, n);
                        rigVpDrag_.pixelWorld = studioVp_.PixelWorldSize(rigVpDrag_.planeOrigin);
                        // grab offset: world point under the cursor (on the plane) minus the bone center, so a click
                        // anywhere on the ring does not jump the bone on the first frame.
                        MouseRay gr;
                        if (BuildMouseRay(studioVp_, mouse, gr)) {
                            XMFLOAT3 hit0;
                            float t0 = 0;
                            if (IntersectRayPlane(gr, rigVpDrag_.planeOrigin, rigVpDrag_.planeNormal, hit0, &t0)) {
                                rigVpDrag_.grabOffset = {hit0.x - rigVpDrag_.planeOrigin.x,
                                                         hit0.y - rigVpDrag_.planeOrigin.y,
                                                         hit0.z - rigVpDrag_.planeOrigin.z};
                            } else {
                                rigVpDrag_.grabOffset = {0, 0, 0};
                            }
                        } else {
                            rigVpDrag_.grabOffset = {0, 0, 0};
                        }
                    }
                    consumed = true;
                }
            }
        } else if (hoverBody >= 0) {
            const int b = bodyWeight(kBody[hoverBody].role);
            if (b < 0) { /* no bone */ }
            else if ((hoverBody == 0 || hoverBody == 1) && d.gizmoTool == 1) {
                // move tool: center (pelvis) and groove are draggable in the camera plane (exact ray-plane solve,
                // same style as hands/feet). Other body rings just select.
                StudioSelectRigController(6 + hoverBody, io.KeyCtrl);  // select the controller
                rigVpDrag_.bodyBone = b;
                rigVpDrag_.bodyStartT = StudioCurrentPose(*m)[(size_t)b].t;
                rigVpDrag_.startMouse = mouse;
                rigVpDrag_.active = true;
                rigVpDrag_.footIkBone = -1;
                rigVpDrag_.chainId = -1;
                rigVpDrag_.planeOrigin = BoneJointWorld(*m->pmx, *m->inst, b);
                {
                    const XMFLOAT4X4& vm = studioVp_.view;
                    rigVpDrag_.viewRight = {vm._11, vm._21, vm._31};
                    rigVpDrag_.viewUp = {vm._12, vm._22, vm._32};
                    XMVECTOR n = XMVectorSubtract(XMLoadFloat3(&studioVp_.eye), XMLoadFloat3(&rigVpDrag_.planeOrigin));
                    const float nl = XMVectorGetX(XMVector3Length(n));
                    if (nl > 1e-6f) n = XMVectorScale(n, 1.0f / nl);
                    else n = XMVectorSet(0, 0, 1, 0);
                    XMStoreFloat3(&rigVpDrag_.planeNormal, n);
                    rigVpDrag_.pixelWorld = studioVp_.PixelWorldSize(rigVpDrag_.planeOrigin);
                    MouseRay gr;
                    if (BuildMouseRay(studioVp_, mouse, gr)) {
                        XMFLOAT3 hit0; float t0 = 0;
                        if (IntersectRayPlane(gr, rigVpDrag_.planeOrigin, rigVpDrag_.planeNormal, hit0, &t0)) {
                            rigVpDrag_.grabOffset = {hit0.x - rigVpDrag_.planeOrigin.x,
                                                     hit0.y - rigVpDrag_.planeOrigin.y,
                                                     hit0.z - rigVpDrag_.planeOrigin.z};
                        } else rigVpDrag_.grabOffset = {0, 0, 0};
                    } else rigVpDrag_.grabOffset = {0, 0, 0};
                }
                studioPoseBefore_ = m->pose;
                consumed = true;
            } else {
                StudioSelectRigController(6 + hoverBody, io.KeyCtrl);  // select the controller first
                StudioSelectBone(b, false); studioViewDrag_ = 3; consumed = true;
            }
        } else if (hoverChain >= 0 && chainProj[hoverChain]) {
            // rotate tool: select the ready IK bone (arm/leg) when present, else the end bone (wrist/ankle)
            StudioSelectRigController(hoverChain, io.KeyCtrl);  // select the controller
            const int end = m->rig->BoneFor(kChains[hoverChain].end);
            int pick = end;
            {
                const int ik = m->rig->IkBoneFor(kChains[hoverChain].end);
                if (ik >= 0) pick = ik;
            }
            if (pick >= 0) { StudioSelectBone(pick, false); studioViewDrag_ = 3; consumed = true; }
        }
    }

    dl->PopClipRect();
}

} // namespace mmdx
