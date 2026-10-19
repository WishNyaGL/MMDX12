#include "render/PackCrashGuard.h"

#include <json.hpp>

#include <algorithm>
#include <fstream>

namespace mmdx {
namespace {

namespace fs = std::filesystem;

bool Contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

void AddUnique(std::vector<std::string>& v, const std::string& s) {
    if (!s.empty() && !Contains(v, s)) v.push_back(s);
}

} // namespace

PackCrashGuard::PackCrashGuard(std::filesystem::path markerFile) : markerFile_(std::move(markerFile)) {}

void PackCrashGuard::Load() {
    if (loaded_) return;
    loaded_ = true;
    active_.clear();
    quarantined_.clear();
    try {
        std::ifstream in(markerFile_, std::ios::binary);
        if (!in) return;
        nlohmann::json j = nlohmann::json::parse(in);
        if (!j.is_object()) return;
        const auto readList = [&j](const char* key, std::vector<std::string>& out) {
            auto it = j.find(key);
            if (it == j.end() || !it->is_array()) return;
            for (const auto& x : *it)
                if (x.is_string()) AddUnique(out, x.get<std::string>());
        };
        readList("active", active_);
        readList("quarantined", quarantined_);
    } catch (const std::exception&) {
        active_.clear();
        quarantined_.clear();
    }
}

void PackCrashGuard::Save() const {
    try {
        std::error_code ec;
        const fs::path parent = markerFile_.parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
        const fs::path tmp = markerFile_.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            nlohmann::json j = {{"active", active_}, {"quarantined", quarantined_}};
            out << j.dump(2);
            out.flush();
            if (!out) return;
        }
        fs::rename(tmp, markerFile_, ec);
        if (ec) {
            std::error_code e2;
            fs::remove(markerFile_, e2);
            fs::rename(tmp, markerFile_, e2);
            if (e2) fs::remove(tmp, e2);
        }
    } catch (const std::exception&) {
        // never throws
    }
}

std::vector<std::string> PackCrashGuard::TakeSuspects() {
    Load();
    std::vector<std::string> suspects = active_;
    active_.clear();
    for (const std::string& id : suspects) AddUnique(quarantined_, id);
    Save();
    return suspects;
}

void PackCrashGuard::Begin(const std::string& packId) {
    Load();
    if (Contains(active_, packId)) return;
    AddUnique(active_, packId);
    Save();
}

void PackCrashGuard::Clear(const std::string& packId) {
    Load();
    const auto it = std::find(active_.begin(), active_.end(), packId);
    if (it == active_.end()) return;
    active_.erase(it);
    Save();
}

void PackCrashGuard::ClearAll() {
    Load();
    if (active_.empty()) return;
    active_.clear();
    Save();
}

bool PackCrashGuard::Quarantined(const std::string& packId) const {
    const_cast<PackCrashGuard*>(this)->Load();
    return Contains(quarantined_, packId);
}

void PackCrashGuard::Quarantine(const std::string& packId) {
    Load();
    if (Contains(quarantined_, packId)) return;
    AddUnique(quarantined_, packId);
    Save();
}

void PackCrashGuard::Release(const std::string& packId) {
    Load();
    const auto it = std::find(quarantined_.begin(), quarantined_.end(), packId);
    if (it == quarantined_.end()) return;
    quarantined_.erase(it);
    Save();
}

} // namespace mmdx
