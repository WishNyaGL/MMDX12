#include "app/ShaderPackStore.h"

#include <Windows.h>

#include <algorithm>
#include <json.hpp>
#include <fstream>
#include <mutex>
#include <thread>

#include "core/I18n.h"
#include "core/Log.h"
#include "core/NetUtil.h"
#include "core/TextUtil.h"

namespace mmdx {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kMaxIndexBytes = 4ull << 20;
constexpr uint64_t kMaxZipBytes = 40ull << 20;

bool IsAbsoluteUrl(const std::string& s) {
    return s.rfind("https://", 0) == 0 || s.rfind("http://", 0) == 0 || s.rfind("file:", 0) == 0;
}

// "preview" / "download" entries are relative to the index (a URL, or a local path while testing).
std::string Resolve(const std::string& base, const std::string& rel) {
    if (rel.empty() || IsAbsoluteUrl(rel)) return rel;
    if (net::IsLocalSource(base)) return PathToUtf8(net::LocalPathOf(base).parent_path() / Utf8ToPath(rel));
    const size_t slash = base.rfind('/');
    return (slash == std::string::npos ? base : base.substr(0, slash + 1)) + rel;
}

bool HttpUrl(const std::string& s) { return s.rfind("https://", 0) == 0 || s.rfind("http://", 0) == 0; }

std::string Str(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

LocalizedText Text(const nlohmann::json& j, const char* key) {
    LocalizedText t;
    auto it = j.find(key);
    if (it == j.end()) return t;
    if (it->is_string()) {
        t.byLang[""] = it->get<std::string>();
    } else if (it->is_object()) {
        for (const auto& [k, v] : it->items())
            if (v.is_string()) t.byLang[k] = v.get<std::string>();
    }
    return t;
}

fs::path PreviewDir() { return ExecutableDir() / L"shader_cache" / L"previews"; }

fs::path TempZip(const std::string& id, const std::string& version) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    return fs::path(tmp) / Utf8ToPath("mmdx12_pack_" + id + "-" + version + "-" + std::to_string(GetTickCount64()) + ".zip");
}

} // namespace

bool RemotePack::Compatible() const {
    if (type == "look" && !kPackTypeLookCompatible) return false;
    if (type == "world" && !kPackTypeWorldCompatible) return false;
    if (apiVersion > kPackApiVersion) return false;
    for (const std::string& cap : requiresCaps) {
        bool known = false;
        for (const PackCapability& c : kPackCapabilities) known |= c.name == cap;
        if (!known) return false;
    }
    return minAppVersion.empty() || net::CompareVersions(minAppVersion, MMDX12_VERSION) <= 0;
}

namespace {

std::string LowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return (char)tolower((unsigned char)c); });
    return s;
}

} // namespace

bool IsOfficialIndexUrl(const std::string& url) {
    std::string a = LowerAscii(url);
    const size_t q = a.find('?');
    if (q != std::string::npos) a.resize(q);
    return a == LowerAscii(ShaderPackStore::kDefaultIndexUrl);
}

bool ParseShaderPackIndex(const std::string& json, const std::string& indexUrl, std::vector<RemotePack>& out,
                          std::string& error) {
    return ParseShaderPackIndex(json, indexUrl, out, nullptr, error);
}

bool ParseShaderPackIndex(const std::string& json, const std::string& indexUrl, std::vector<RemotePack>& out,
                          std::vector<std::string>* revoked, std::string& error) {
    out.clear();
    if (revoked) revoked->clear();
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        error = std::string("index: ") + e.what();
        return false;
    }
    if (!j.is_object() || !j.contains("packs") || !j["packs"].is_array()) {
        error = "index: no packs array";
        return false;
    }
    const bool official = IsOfficialIndexUrl(indexUrl);
    std::vector<std::string> revokedHashes;
    if (official) {
        if (auto r = j.find("revoked"); r != j.end() && r->is_array())
            for (const auto& x : *r) {
                if (!x.is_string()) continue;
                const std::string h = LowerAscii(x.get<std::string>());
                if (h.size() == 64) revokedHashes.push_back(h);
            }
        if (revoked) *revoked = revokedHashes;
    }
    for (const auto& p : j["packs"]) {
        if (!p.is_object()) continue;
        RemotePack r;
        r.id = Str(p, "id");
        r.version = Str(p, "version");
        if (!ValidShaderPackId(r.id) || r.version.empty()) continue;   // unusable entry
        r.apiVersion = p.value("apiVersion", 1);
        r.type = Str(p, "type");
        if (r.type != "effect" && r.type != "look" && r.type != "world") r.type = "surface";
        r.source = indexUrl;
        r.owner = Str(p, "owner");
        r.verified = official && p.value("verified", false);
        r.updated = Str(p, "updated");
        r.minAppVersion = Str(p, "minAppVersion");
        r.name = Text(p, "name");
        if (r.name.Empty()) r.name.byLang[""] = r.id;
        r.description = Text(p, "description");
        r.recommendedFor = Text(p, "recommendedFor");
        if (auto a = p.find("authors"); a != p.end() && a->is_array()) {
            for (const auto& x : *a) {
                if (!x.is_object() || Str(x, "name").empty()) continue;
                PackAuthor au{Str(x, "name"), Str(x, "role"), Str(x, "url")};
                if (!HttpUrl(au.url)) au.url.clear();
                r.authors.push_back(std::move(au));
            }
        }
        r.license = Str(p, "license");
        r.homepage = HttpUrl(Str(p, "homepage")) ? Str(p, "homepage") : std::string();
        r.repository = HttpUrl(Str(p, "repository")) ? Str(p, "repository") : std::string();
        if (auto t = p.find("tags"); t != p.end() && t->is_array())
            for (const auto& x : *t)
                if (x.is_string()) r.tags.push_back(x.get<std::string>());
        if (auto t = p.find("requires"); t != p.end() && t->is_array())
            for (const auto& x : *t)
                if (x.is_string()) r.requiresCaps.push_back(x.get<std::string>());
        r.builtIn = p.value("builtIn", false);
        r.previewUrl = Resolve(indexUrl, Str(p, "preview"));
        r.previewBeforeUrl = Resolve(indexUrl, Str(p, "previewBefore"));
        r.downloadUrl = Resolve(indexUrl, Str(p, "download"));
        r.sha256 = Str(p, "sha256");
        std::transform(r.sha256.begin(), r.sha256.end(), r.sha256.begin(), [](char c) { return (char)tolower((unsigned char)c); });
        r.size = p.value("size", (uint64_t)0);
        r.paramCount = p.value("params", 0);
        if (r.downloadUrl.empty() || r.sha256.size() != 64) continue;   // never install unverifiable files
        bool isRevoked = false;
        for (const std::string& h : revokedHashes) isRevoked |= h == r.sha256;
        if (isRevoked) continue;
        out.push_back(std::move(r));
    }
    return true;
}

bool UpdateAllowed(const ShaderPack& installed, const RemotePack& remote) {
    if (installed.source != PackSource::Online) return false;
    if (installed.installedFrom.empty() || remote.source != installed.installedFrom) return false;
    return net::CompareVersions(remote.version, installed.version) > 0;
}

// ---- jobs (shared with their worker thread; the worker only writes, then sets done) ----------------------------

struct ShaderPackStore::IndexJob {
    std::string url;
    std::atomic<bool> done{false};
    bool ok = false;
    std::string error;
    std::vector<RemotePack> packs;
    std::vector<std::string> revoked;
};

struct ShaderPackStore::InstallJob {
    RemotePack pack;
    std::string indexUrl;
    std::atomic<uint64_t> bytes{0};
    std::atomic<bool> done{false};
    bool ok = false;
    std::string error;
    fs::path zip;
};

struct ShaderPackStore::PreviewJob {
    std::string id, version;
    bool before = false;
    std::atomic<bool> done{false};
    fs::path file;   // empty on failure
};

ShaderPackStore::~ShaderPackStore() = default;   // detached workers hold their own job references

void ShaderPackStore::Refresh(const std::string& url) { Refresh(std::vector<std::string>{url}); }

void ShaderPackStore::Refresh(const std::vector<std::string>& urls) {
    if (state_ == IndexState::Loading) return;
    state_ = IndexState::Loading;
    error_.clear();
    sources_.clear();
    indexJobs_.clear();
    // official first, then the user's, keeping the requested order within each group
    std::vector<std::string> ordered;
    for (const std::string& u : urls)
        if (IsOfficialIndexUrl(u)) ordered.push_back(u);
    for (const std::string& u : urls)
        if (!IsOfficialIndexUrl(u)) ordered.push_back(u);
    if (ordered.empty()) ordered.push_back(kDefaultIndexUrl);
    url_ = ordered.front();
    for (const std::string& u : ordered) {
        sources_.push_back({u, IndexState::Loading, std::string()});
        auto job = std::make_shared<IndexJob>();
        job->url = u;
        indexJobs_.push_back(job);
        std::thread([job] {
            std::string body;
            if (net::IsLocalSource(job->url)) {
                std::ifstream in(net::LocalPathOf(job->url), std::ios::binary);
                body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                if (!in && body.empty()) job->error = "cannot read " + job->url;
            } else {
                net::HttpGet(job->url, body, job->error, kMaxIndexBytes);
            }
            if (!body.empty()) job->ok = ParseShaderPackIndex(body, job->url, job->packs, &job->revoked, job->error);
            job->done = true;
        }).detach();
    }
}

ShaderPackStore::IndexState ShaderPackStore::SourceState(const std::string& url) const {
    for (const SourceStateEntry& s : sources_)
        if (s.url == url) return s.state;
    return IndexState::Idle;
}

const std::string& ShaderPackStore::SourceError(const std::string& url) const {
    static const std::string kEmpty;
    for (const SourceStateEntry& s : sources_)
        if (s.url == url) return s.error;
    return kEmpty;
}

std::vector<std::string> ShaderPackStore::RevokedHashes() const { return revoked_; }

void ShaderPackStore::Install(const RemotePack& pack) {
    if (Installing(pack.id)) return;
    auto job = std::make_shared<InstallJob>();
    job->pack = pack;
    job->indexUrl = pack.source.empty() ? url_ : pack.source;
    installs_.push_back(job);
    std::thread([job] {
        const RemotePack& p = job->pack;
        if (p.size > kMaxZipBytes) {
            job->error = "zip too large";
        } else {
            job->zip = TempZip(p.id, p.version);
            if (net::DownloadToFile(p.downloadUrl, job->zip, &job->bytes, nullptr, job->error)) {
                std::error_code ec;
                const uint64_t size = (uint64_t)fs::file_size(job->zip, ec);
                const std::string digest = net::Sha256OfFile(job->zip, nullptr);
                if (p.size > 0 && size != p.size) job->error = "size mismatch";
                else if (digest != p.sha256) job->error = "SHA-256 mismatch";
                else job->ok = true;
            }
            if (!job->ok) {
                std::error_code ec;
                fs::remove(job->zip, ec);
            }
        }
        job->done = true;
    }).detach();
}

bool ShaderPackStore::Installing(const std::string& id) const {
    for (const auto& j : installs_)
        if (j->pack.id == id) return true;
    return false;
}

float ShaderPackStore::Progress(const std::string& id) const {
    for (const auto& j : installs_)
        if (j->pack.id == id) return j->pack.size ? std::min(1.0f, (float)j->bytes.load() / (float)j->pack.size) : 0.0f;
    return 0.0f;
}

std::vector<ShaderPackStore::Event> ShaderPackStore::Poll(ShaderPackRegistry& registry) {
    std::vector<Event> events;
    if (!indexJobs_.empty()) {
        bool allDone = true;
        for (const auto& j : indexJobs_) allDone &= j->done.load();
        if (allDone) {
            std::vector<RemotePack> merged;
            std::vector<std::string> revoked;
            int failures = 0;
            for (const auto& job : indexJobs_) {
                for (SourceStateEntry& s : sources_) {
                    if (s.url != job->url) continue;
                    if (job->ok) {
                        s.state = IndexState::Ready;
                        s.error.clear();
                    } else {
                        s.state = IndexState::Failed;
                        s.error = job->error;
                        ++failures;
                        LOG_WARN("shader pack store: index %s: %s", job->url.c_str(), job->error.c_str());
                    }
                }
                if (job->ok) {
                    LOG_INFO("shader pack store: %zu packs from %s", job->packs.size(), job->url.c_str());
                    for (RemotePack& p : job->packs) merged.push_back(std::move(p));
                    if (IsOfficialIndexUrl(job->url)) revoked = job->revoked;
                }
            }
            packs_ = std::move(merged);
            revoked_ = std::move(revoked);
            registry.ApplyRevocations(revoked_);
            state_ = failures == (int)indexJobs_.size() ? IndexState::Failed : IndexState::Ready;
            if (state_ == IndexState::Failed) error_ = Tr("온라인 목록을 불러오지 못했습니다");
            // previews: cached by id + version, downloaded once
            std::error_code ec;
            fs::create_directories(PreviewDir(), ec);
            for (const RemotePack& p : packs_) {
                if (!p.previewUrl.empty()) {
                    const size_t dot = p.previewUrl.rfind('.');
                    std::string ext = dot == std::string::npos ? ".png" : p.previewUrl.substr(dot);
                    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".webp") ext = ".png";
                    const fs::path file = PreviewDir() / Utf8ToPath(p.id + "-" + p.version + ext);
                    auto job = std::make_shared<PreviewJob>();
                    job->id = p.id;
                    job->version = p.version;
                    job->before = false;
                    previews_.push_back(job);
                    if (fs::exists(file, ec)) {
                        job->file = file;
                        job->done = true;
                    } else {
                        const std::string url = p.previewUrl;
                        std::thread([job, url, file] {
                            std::string err;
                            const fs::path tmp = fs::path(file).concat(L".tmp");
                            if (net::DownloadToFile(url, tmp, nullptr, nullptr, err)) {
                                std::error_code e;
                                fs::rename(tmp, file, e);
                                if (!e) job->file = file;
                            }
                            job->done = true;
                        }).detach();
                    }
                }
                if (!p.previewBeforeUrl.empty()) {
                    const size_t dot = p.previewBeforeUrl.rfind('.');
                    std::string ext = dot == std::string::npos ? ".png" : p.previewBeforeUrl.substr(dot);
                    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".webp") ext = ".png";
                    const fs::path file = PreviewDir() / Utf8ToPath(p.id + "-" + p.version + "-before" + ext);
                    auto job = std::make_shared<PreviewJob>();
                    job->id = p.id;
                    job->version = p.version;
                    job->before = true;
                    previews_.push_back(job);
                    if (fs::exists(file, ec)) {
                        job->file = file;
                        job->done = true;
                    } else {
                        const std::string url = p.previewBeforeUrl;
                        std::thread([job, url, file] {
                            std::string err;
                            const fs::path tmp = fs::path(file).concat(L".tmp");
                            if (net::DownloadToFile(url, tmp, nullptr, nullptr, err)) {
                                std::error_code e;
                                fs::rename(tmp, file, e);
                                if (!e) job->file = file;
                            }
                            job->done = true;
                        }).detach();
                    }
                }
            }
            indexJobs_.clear();
        }
    }
    for (auto it = previews_.begin(); it != previews_.end();) {
        if (!(*it)->done) { ++it; continue; }
        for (RemotePack& p : packs_) {
            if (p.id == (*it)->id && p.version == (*it)->version) {
                if ((*it)->before) p.previewBeforeFile = (*it)->file;
                else p.previewFile = (*it)->file;
            }
        }
        it = previews_.erase(it);
    }
    for (auto it = installs_.begin(); it != installs_.end();) {
        InstallJob& j = **it;
        if (!j.done) { ++it; continue; }
        const std::string name = j.pack.name.Get(PackLanguage());
        if (j.ok) {
            std::string id, err;
            if (registry.InstallZip(j.zip, j.indexUrl, id, err)) {
                events.push_back({true, name + " " + j.pack.version + Tr(" 설치됨")});
                LOG_INFO("shader pack store: installed %s %s", j.pack.id.c_str(), j.pack.version.c_str());
            } else {
                events.push_back({false, name + ": " + err});
                LOG_WARN("shader pack store: install %s failed: %s", j.pack.id.c_str(), err.c_str());
            }
            std::error_code ec;
            fs::remove(j.zip, ec);
        } else {
            events.push_back({false, name + Tr(": 다운로드 실패")});
            LOG_WARN("shader pack store: download %s failed: %s", j.pack.id.c_str(), j.error.c_str());
        }
        it = installs_.erase(it);
    }
    return events;
}

} // namespace mmdx
