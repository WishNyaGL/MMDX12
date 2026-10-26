#include "studio/RigPersistence.h"

#include "asset/PmxModel.h"

#include <json.hpp>

#include <fstream>

namespace mmdx::studio {

namespace {

const char* kRigFormatName = "mmdx12-studio-rig";

std::string ReadString(const nlohmann::json& j, const char* key, const std::string& def) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : def;
}

bool WriteRigAtomic(const std::filesystem::path& file, const std::string& bytes, std::string* error) {
    const std::filesystem::path tmp = std::filesystem::path(file) += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { if (error) *error = "cannot write rig file"; return false; }
        out.write(bytes.data(), (std::streamsize)bytes.size());
        out.flush();
        if (!out) { if (error) *error = "rig write failed"; return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) { if (error) *error = "rig rename failed"; return false; }
    return true;
}

} // namespace

bool SaveRig(const std::filesystem::path& file, const RigDefinition& rig, std::string* error) {
    nlohmann::json j;
    j["format"] = kRigFormatName;
    j["version"] = kRigFormatVersion;
    j["sourceModel"] = rig.sourceModelName;
    nlohmann::json bindings = nlohmann::json::array();
    for (const RigBoneBinding& b : rig.bindings) {
        nlohmann::json jb;
        jb["role"] = RigRoleName(b.role);
        jb["bone"] = b.boneIndex;
        jb["name"] = b.boneName;
        jb["confidence"] = b.confidence;
        bindings.push_back(std::move(jb));
    }
    j["bindings"] = std::move(bindings);
    return WriteRigAtomic(file, j.dump(2), error);
}

bool LoadRig(const std::filesystem::path& file, RigDefinition& out, std::string* error) {
    out.Clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open rig file";
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        if (error) *error = "rig file is not JSON";
        return false;
    }
    if (ReadString(j, "format", "") != kRigFormatName) {
        if (error) *error = "not a mmdx12 rig file";
        return false;
    }
    const int version = j.value("version", 0);
    if (version > kRigFormatVersion) {
        if (error) *error = "rig file version too new";
        return false;
    }
    out.sourceModelName = ReadString(j, "sourceModel", "");
    if (j.contains("bindings") && j["bindings"].is_array()) {
        for (const nlohmann::json& jb : j["bindings"]) {
            const std::string role = ReadString(jb, "role", "");
            int idx = -1;
            for (int r = 0; r < (int)RigRole::Count; ++r) {
                if (role == RigRoleName((RigRole)r)) { idx = r; break; }
            }
            if (idx < 0) continue;
            RigBoneBinding& b = out.bindings[(size_t)idx];
            b.role = (RigRole)idx;
            b.boneIndex = jb.value("bone", -1);
            b.boneName = ReadString(jb, "name", "");
            b.confidence = jb.value("confidence", 0.0f);
        }
    }
    return true;
}

void RebindRig(RigDefinition& rig, const PmxModel& model) {
    for (RigBoneBinding& b : rig.bindings) {
        if (b.boneName.empty()) {
            b.boneIndex = -1;
            b.confidence = 0.0f;
            continue;
        }
        // re-resolve by name (bone indices are not stable across model versions)
        const int idx = model.FindBone(b.boneName);
        if (idx < 0) {
            b.boneIndex = -1;
            b.confidence = 0.0f;
        } else {
            b.boneIndex = idx;
            b.confidence = std::max(b.confidence, 0.5f);  // still valid, just not freshly generated
        }
    }
}

} // namespace mmdx::studio
