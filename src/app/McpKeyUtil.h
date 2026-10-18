#pragma once
// Shared helpers of the MCP key tools (AppMcpKeys.cpp / AppMcpMotion.cpp): VMD curve presets and their JSON form,
// per-parameter / per-channel curve objects, and the Euler <-> quaternion convention used by studio_set_bone.
#include <algorithm>
#include <cmath>
#include <string>

#include <DirectXMath.h>
#include <json.hpp>

#include "studio/StudioMotion.h"

namespace mmdx {

struct CurveValue {
    const char* name;
    uint8_t bytes[4];
};
inline constexpr CurveValue kCurvePresets[] = {
    {"linear", {20, 20, 107, 107}},       {"ease_in_out", {64, 0, 64, 127}},
    {"ease_in", {64, 0, 127, 127}},       {"ease_out", {0, 0, 64, 127}},
    {"smooth", {42, 0, 86, 127}},         {"fast_start", {0, 64, 64, 127}},
    {"slow_start", {64, 0, 107, 64}},
};
inline constexpr const char* kCameraCurveNames[6] = {"x", "y", "z", "rotation", "distance", "fov"};
inline constexpr const char* kBoneCurveNames[4] = {"x", "y", "z", "rotation"};

inline bool CurveFromString(const std::string& s, uint8_t out[4]) {
    for (const CurveValue& p : kCurvePresets) {
        if (s == p.name) {
            std::copy(p.bytes, p.bytes + 4, out);
            return true;
        }
    }
    return false;
}

inline bool CurveFromJson(const nlohmann::json& j, uint8_t out[4]) {
    if (j.is_string()) return CurveFromString(j.get<std::string>(), out);
    if (!j.is_array() || j.size() != 4) return false;
    for (int i = 0; i < 4; ++i) {
        if (!j[i].is_number_integer()) return false;
        const int v = j[i].get<int>();
        if (v < 0 || v > 127) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

inline const char* CurveName(const uint8_t c[4]) {
    for (const CurveValue& p : kCurvePresets)
        if (std::equal(p.bytes, p.bytes + 4, c)) return p.name;
    return nullptr;
}

inline nlohmann::json CurveToJson(const uint8_t c[4]) {
    if (const char* n = CurveName(c)) return n;
    return nlohmann::json::array({c[0], c[1], c[2], c[3]});
}

// Applies "curve" to a camera interpolation block (all params, or one preset / [4] per named param).
inline bool ApplyCameraCurve(const nlohmann::json& curve, uint8_t interp[24], std::string& error) {
    studio::FillLinearCameraInterp(interp);
    if (curve.is_null()) return true;
    uint8_t all[4];
    if (CurveFromJson(curve, all)) {
        for (int p = 0; p < 6; ++p) studio::SetCameraCurve(interp, p, all);
        return true;
    }
    if (!curve.is_object()) {
        error = "curve must be a preset name, [x1,y1,x2,y2] or an object per parameter";
        return false;
    }
    for (auto it = curve.begin(); it != curve.end(); ++it) {
        int param = -1;
        for (int i = 0; i < 6; ++i)
            if (it.key() == kCameraCurveNames[i]) param = i;
        if (param < 0) {
            error = "unknown camera curve parameter: " + it.key();
            return false;
        }
        uint8_t c[4];
        if (!CurveFromJson(it.value(), c)) {
            error = "invalid curve for param '" + it.key() + "'";
            return false;
        }
        studio::SetCameraCurve(interp, param, c);
    }
    return true;
}

inline bool ApplyBoneCurve(const nlohmann::json& curve, uint8_t interp[64], std::string& error) {
    studio::FillLinearInterp(interp);
    if (curve.is_null()) return true;
    uint8_t all[4];
    if (CurveFromJson(curve, all)) {
        for (int c = 0; c < 4; ++c) studio::SetBoneCurve(interp, c, all);
        return true;
    }
    if (!curve.is_object()) {
        error = "curve must be a preset name, [x1,y1,x2,y2] or an object per channel";
        return false;
    }
    for (auto it = curve.begin(); it != curve.end(); ++it) {
        int channel = -1;
        for (int i = 0; i < 4; ++i)
            if (it.key() == kBoneCurveNames[i]) channel = i;
        if (channel < 0) {
            error = "unknown bone curve channel: " + it.key();
            return false;
        }
        uint8_t c[4];
        if (!CurveFromJson(it.value(), c)) {
            error = "invalid curve for channel '" + it.key() + "'";
            return false;
        }
        studio::SetBoneCurve(interp, channel, c);
    }
    return true;
}

inline nlohmann::json CameraCurveToJson(const uint8_t interp[24]) {
    nlohmann::json o = nlohmann::json::object();
    for (int p = 0; p < 6; ++p) {
        uint8_t c[4];
        studio::GetCameraCurve(interp, p, c);
        o[kCameraCurveNames[p]] = CurveToJson(c);
    }
    return o;
}

inline nlohmann::json BoneCurveToJson(const uint8_t interp[64]) {
    nlohmann::json o = nlohmann::json::object();
    for (int c = 0; c < 4; ++c) {
        uint8_t b[4];
        studio::GetBoneCurve(interp, c, b);
        o[kBoneCurveNames[c]] = CurveToJson(b);
    }
    return o;
}

// The inverse of XMQuaternionRotationRollPitchYaw(x, y, z) (radians): its matrix is Ry * Rp * Rr.
inline void QuatToEuler(const DirectX::XMFLOAT4& in, float& x, float& y, float& z) {
    DirectX::XMFLOAT4 q = in;
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len > 1e-12f) {
        q.x /= len; q.y /= len; q.z /= len; q.w /= len;
    } else {
        q = {0, 0, 0, 1};
    }
    const float sp = std::clamp(-2.0f * (q.y * q.z - q.w * q.x), -1.0f, 1.0f);
    x = std::asin(sp);
    y = std::atan2(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    z = std::atan2(2.0f * (q.x * q.y + q.w * q.z), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));
}

inline DirectX::XMFLOAT4 EulerToQuat(float xDeg, float yDeg, float zDeg) {
    const DirectX::XMVECTOR q = DirectX::XMQuaternionRotationRollPitchYaw(
        DirectX::XMConvertToRadians(xDeg), DirectX::XMConvertToRadians(yDeg), DirectX::XMConvertToRadians(zDeg));
    DirectX::XMFLOAT4 out;
    DirectX::XMStoreFloat4(&out, q);
    return out;
}

}  // namespace mmdx
