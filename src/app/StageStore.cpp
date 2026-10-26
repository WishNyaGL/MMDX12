#include "app/StageStore.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <json.hpp>
#include <thread>

#include "core/I18n.h"
#include "core/Log.h"
#include "core/NetUtil.h"
#include "core/TextUtil.h"

namespace mmdx {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kMaxIndexBytes = 4ull << 20;
constexpr uint64_t kMaxZipBytes = 512ull << 20;   // docs/stage_format.md: a stage zip stays under 256 MB, hard cap 512

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

std::string Str(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

StageText Text(const nlohmann::json& j, const char* key) {
    StageText t;
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

std::string LowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return (char)tolower((unsigned char)c); });
    return s;
}

std::string Stamp() { return std::to_string(GetTickCount64()); }

// A unique scratch folder next to the installed stages (same volume, so the final rename is atomic). It lives under
// <stages>/.incoming so a leftover from a crash is never scanned as a stage (the scanner reads direct children only,
// and .incoming itself has no stage.json).
fs::path ScratchDir(const fs::path& stagesRoot, const std::string& prefix, const std::string& id) {
    return stagesRoot / L".incoming" / Utf8ToPath(prefix + id + "_" + Stamp());
}

// The stage.json of an extracted zip: its root, or the single top folder's. Empty = not a stage zip.
fs::path StageRootIn(const fs::path& dir) {
    std::error_code ec;
    const fs::path direct = dir / Utf8ToPath(kStageManifestName);
    if (fs::is_regular_file(direct, ec)) return dir;
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        dirs.push_back(it->path());
    }
    if (dirs.size() != 1) return {};
    const fs::path nested = dirs.front() / Utf8ToPath(kStageManifestName);
    return fs::is_regular_file(nested, ec) ? dirs.front() : fs::path();
}

// Drops a scratch folder and, while it is the last one, the .incoming folder that holds it (a clean install leaves
// nothing in stages/ but the stage folders).
void DropScratch(const fs::path& dir, const fs::path& scratchRoot) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove(scratchRoot, ec);   // only succeeds while it is empty
}

// Moves `src` onto `dest` (an existing `dest` is replaced only by the caller's old-folder dance).
bool MoveInto(const fs::path& src, const fs::path& dest, std::string& error) {
    std::error_code ec;
    fs::rename(src, dest, ec);
    if (ec) {
        error = "cannot move " + PathToUtf8(src) + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

} // namespace

// ---- data layer -----------------------------------------------------------------------------------------------------

bool IsOfficialStageIndexUrl(const std::string& url) {
    std::string a = LowerAscii(url);
    const size_t q = a.find('?');
    if (q != std::string::npos) a.resize(q);
    return a == LowerAscii(StageStore::kDefaultIndexUrl);
}

bool IsValidStageId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

bool RemoteStage::Compatible() const {
    return minEngine.empty() || net::CompareVersions(minEngine, MMDX12_VERSION) <= 0;
}

bool StageUpdateAvailable(const InstalledStage& installed, const RemoteStage& remote) {
    if (installed.id != remote.id || remote.revoked) return false;
    return net::CompareVersions(remote.version, installed.version) > 0;
}

bool StageMatches(const std::string& needle, const RemoteStage& stage) {
    if (needle.empty()) return true;
    if (ToLowerAscii(stage.id).find(needle) != std::string::npos) return true;
    for (const auto& [lang, text] : stage.name.byLang)
        if (ToLowerAscii(text).find(needle) != std::string::npos) return true;
    if (ToLowerAscii(stage.author).find(needle) != std::string::npos) return true;
    for (const std::string& t : stage.tags)
        if (ToLowerAscii(t).find(needle) != std::string::npos) return true;
    return false;
}

bool ParseStageIndex(const std::string& json, const std::string& indexUrl, std::vector<RemoteStage>& out,
                     std::string& error) {
    out.clear();
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        error = std::string("index: ") + e.what();
        return false;
    }
    if (!j.is_object() || !j.contains("stages") || !j["stages"].is_array()) {
        error = "index: no stages array";
        return false;
    }
    for (const auto& s : j["stages"]) {
        if (!s.is_object()) continue;
        RemoteStage r;
        r.id = Str(s, "id");
        r.version = Str(s, "version");
        if (!IsValidStageId(r.id) || r.version.empty()) continue;   // unusable entry
        r.source = indexUrl;
        r.author = Str(s, "author");
        r.license = Str(s, "license");
        r.minEngine = Str(s, "minEngine");
        r.name = Text(s, "name");
        if (r.name.Empty()) r.name.byLang[""] = r.id;
        r.description = Text(s, "description");
        if (auto t = s.find("tags"); t != s.end() && t->is_array())
            for (const auto& x : *t)
                if (x.is_string()) r.tags.push_back(x.get<std::string>());
        r.previewUrl = Resolve(indexUrl, Str(s, "preview"));
        r.downloadUrl = Resolve(indexUrl, Str(s, "download"));
        r.sha256 = LowerAscii(Str(s, "sha256"));
        r.size = s.value("size", (uint64_t)0);
        if (auto rv = s.find("revoked"); rv != s.end()) {
            if (rv->is_boolean()) r.revoked = rv->get<bool>();
            else if (rv->is_object()) {   // {"revoked": {"reason": "..."}} also reads as revoked
                r.revoked = true;
                r.revokedReason = Str(*rv, "reason");
            }
        }
        if (r.revokedReason.empty()) {
            // "reason" is free text or localized (a StageText's UI language, else its fallback)
            const StageText reason = Text(s, "reason");
            r.revokedReason = reason.Get(StageLanguage());
        }
        if (r.downloadUrl.empty() || r.sha256.size() != 64) continue;   // never install unverifiable files
        out.push_back(std::move(r));
    }
    return true;
}

// ---- install core (also used by tools/stage_store_test) -----------------------------------------------------------

bool VerifyStageZip(const fs::path& zip, const RemoteStage& stage, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(zip, ec)) {
        error = "no file";
        return false;
    }
    const uint64_t size = (uint64_t)fs::file_size(zip, ec);
    if (ec) {
        error = "cannot read " + PathToUtf8(zip);
        return false;
    }
    if (stage.size > 0 && size != stage.size) {
        error = "size mismatch";
        return false;
    }
    if (net::Sha256OfFile(zip, nullptr) != stage.sha256) {
        error = "SHA-256 mismatch";
        return false;
    }
    return true;
}

bool InstallStageZip(const fs::path& zip, const RemoteStage& stage, const fs::path& stagesRoot, std::string& error) {
    std::error_code ec;
    const fs::path scratchRoot = stagesRoot / L".incoming";
    fs::create_directories(stagesRoot, ec);

    // 1. extract into a scratch folder next to the stages and check the manifest
    const fs::path tmp = ScratchDir(stagesRoot, ".tmp_", stage.id);
    if (!net::ExtractZip(zip, tmp, error)) {
        DropScratch(tmp, scratchRoot);
        return false;
    }
    const fs::path root = StageRootIn(tmp);
    if (root.empty()) {
        DropScratch(tmp, scratchRoot);
        error = "no single stage.json at the zip root or in one top folder";
        return false;
    }
    {
        StageManifest manifest;
        std::string merr;
        if (!LoadStageManifest(root / Utf8ToPath(kStageManifestName), manifest, merr)) {
            DropScratch(tmp, scratchRoot);
            error = "stage.json: " + merr;
            return false;
        }
        if (manifest.id != stage.id) {
            DropScratch(tmp, scratchRoot);
            error = "stage.json id is \"" + manifest.id + "\", the index says \"" + stage.id + "\"";
            return false;
        }
        if (manifest.models.empty()) {
            DropScratch(tmp, scratchRoot);
            error = "stage.json lists no models";
            return false;
        }
    }

    // 2. one rename into stages/<id>: a fresh folder moves in as a whole; a top-folder layout is lifted out first
    const fs::path target = stagesRoot / Utf8ToPath(stage.id);
    const fs::path staged = root == tmp ? tmp : ScratchDir(stagesRoot, ".stage_", stage.id);
    if (root != tmp && !MoveInto(root, staged, error)) {
        DropScratch(tmp, scratchRoot);
        return false;
    }
    const fs::path old = ScratchDir(stagesRoot, ".old_", stage.id);
    const bool hadOld = fs::exists(target, ec);
    if (hadOld && !MoveInto(target, old, error)) {
        if (staged != tmp) DropScratch(staged, scratchRoot);
        DropScratch(tmp, scratchRoot);
        return false;
    }
    if (!MoveInto(staged, target, error)) {
        if (hadOld) {
            std::string restoreErr;
            MoveInto(old, target, restoreErr);   // put the previous version back
        }
        if (staged != tmp) DropScratch(staged, scratchRoot);
        DropScratch(tmp, scratchRoot);
        return false;
    }
    DropScratch(tmp, scratchRoot);
    if (hadOld) DropScratch(old, scratchRoot);
    return true;
}

// ---- jobs (shared with their worker thread; the worker only writes, then sets done) --------------------------------

struct StageStore::IndexJob {
    std::string url;
    std::atomic<bool> done{false};
    bool ok = false;
    std::string error;
    std::vector<RemoteStage> stages;
};

struct StageStore::InstallJob {
    RemoteStage stage;
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    bool ok = false;
    bool cancelled = false;
    std::string error;
};

struct StageStore::PreviewJob {
    std::string id, version;
    std::atomic<bool> done{false};
    fs::path file;   // empty on failure
};

// ---- paths ----------------------------------------------------------------------------------------------------------

fs::path StageStore::StagesRoot() { return ExecutableDir() / L"stages"; }
fs::path StageStore::DownloadDir() { return ExecutableDir() / L"cache" / L"stage_downloads"; }
fs::path StageStore::PreviewDir() { return ExecutableDir() / L"cache" / L"stage_previews"; }

// ---- store ----------------------------------------------------------------------------------------------------------

StageStore::StageStore() { RefreshInstalled(); }
StageStore::~StageStore() = default;   // detached workers hold their own job references

void StageStore::Refresh(const std::string& url) {
    if (state_ == IndexState::Loading) return;
    state_ = IndexState::Loading;
    error_.clear();
    url_ = url.empty() ? kDefaultIndexUrl : url;
    indexJobs_.clear();
    RefreshInstalled();
    auto job = std::make_shared<IndexJob>();
    job->url = url_;
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
        if (!body.empty()) job->ok = ParseStageIndex(body, job->url, job->stages, job->error);
        job->done = true;
    }).detach();
}

void StageStore::RefreshInstalled() {
    installed_.clear();
    std::error_code ec;
    const fs::path root = StagesRoot();
    if (!fs::is_directory(root, ec)) return;
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string name = PathToUtf8(it->path().filename());
        if (name.empty() || name[0] == '.') continue;   // scratch folders (.tmp_ / .old_ / .stage_)
        dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const fs::path& dir : dirs) {
        StageManifest manifest;
        std::string error;
        if (!LoadStageManifest(dir / Utf8ToPath(kStageManifestName), manifest, error)) continue;
        InstalledStage stage;
        stage.id = PathToUtf8(dir.filename());   // the folder name: the library id is "stages:<folder>"
        stage.version = manifest.version;
        stage.name = manifest.name;
        stage.dir = dir;
        installed_.push_back(std::move(stage));
    }
}

const InstalledStage* StageStore::FindInstalled(const std::string& id) const {
    for (const InstalledStage& s : installed_)
        if (s.id == id) return &s;
    return nullptr;
}

void StageStore::Install(const RemoteStage& stage) {
    if (Installing(stage.id) || stage.revoked) return;
    auto job = std::make_shared<InstallJob>();
    job->stage = stage;
    installs_.push_back(job);
    std::thread([job] {
        const RemoteStage& s = job->stage;
        const auto fail = [&](const std::string& why) {
            job->error = why;
            job->done = true;
        };
        if (!s.Compatible()) return fail("needs a newer app version");
        if (s.size > kMaxZipBytes) return fail("zip too large");

        // 1. resumable download into cache/stage_downloads/<id>-<version>.zip (a .part survives a cancel / crash)
        std::error_code ec;
        fs::create_directories(DownloadDir(), ec);
        const fs::path zip = DownloadDir() / Utf8ToPath(s.id + "-" + s.version + ".zip");
        if (!net::DownloadToFileResumable(s.downloadUrl, zip, &job->bytes, &job->total, &job->cancel, job->error)) {
            job->cancelled = job->cancel.load();
            if (job->cancelled) fs::remove(zip, ec);   // the .part stays: the next Install resumes
            return fail(job->error);
        }

        // 2. verify the index's size / SHA-256 before anything touches stages/
        if (!VerifyStageZip(zip, s, job->error)) {
            fs::remove(zip, ec);
            return fail(job->error);
        }

        // 3. extract into a scratch folder next to the stages, check the manifest and move it in (update keeps the
        //    previous version until the new one is in place)
        if (!InstallStageZip(zip, s, StagesRoot(), job->error)) return fail(job->error);
        fs::remove(zip, ec);
        job->ok = true;
        job->done = true;
    }).detach();
}

void StageStore::Cancel(const std::string& id) {
    for (const auto& j : installs_)
        if (j->stage.id == id) j->cancel.store(true);
}

bool StageStore::Installing(const std::string& id) const {
    for (const auto& j : installs_)
        if (j->stage.id == id) return true;
    return false;
}

float StageStore::Progress(const std::string& id) const {
    for (const auto& j : installs_) {
        if (j->stage.id != id) continue;
        const uint64_t total = j->total.load();
        if (total == 0) return 0.0f;
        return std::min(1.0f, (float)j->bytes.load() / (float)total);
    }
    return 0.0f;
}

bool StageStore::Uninstall(const std::string& idIn, std::string& error) {
    const std::string id = idIn;   // RefreshInstalled() below rebuilds installed_: never hold a reference into it
    if (!IsValidStageId(id)) {
        error = "invalid stage id";
        return false;
    }
    if (Installing(id)) {
        error = "still installing";
        return false;
    }
    const fs::path dir = StagesRoot() / Utf8ToPath(id);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        error = "not installed";
        return false;
    }
    // Only remove a folder that really is this stage: a hand-placed folder whose id differs is left alone.
    StageManifest manifest;
    std::string merr;
    if (LoadStageManifest(dir / Utf8ToPath(kStageManifestName), manifest, merr) && manifest.id != id) {
        error = "the folder's stage.json id differs (\"" + manifest.id + "\")";
        return false;
    }
    fs::remove_all(dir, ec);
    if (ec) {
        error = "cannot delete " + PathToUtf8(dir) + " (" + ec.message() + ")";
        return false;
    }
    RefreshInstalled();
    LOG_INFO("stage store: uninstalled %s", id.c_str());
    return true;
}

std::vector<StageStore::Event> StageStore::Poll() {
    std::vector<Event> events;
    if (!indexJobs_.empty()) {
        bool allDone = true;
        for (const auto& j : indexJobs_) allDone &= j->done.load();
        if (allDone) {
            std::vector<RemoteStage> merged;
            for (const auto& job : indexJobs_) {
                if (job->ok) {
                    LOG_INFO("stage store: %zu stages from %s", job->stages.size(), job->url.c_str());
                    for (RemoteStage& s : job->stages) merged.push_back(std::move(s));
                } else {
                    LOG_WARN("stage store: index %s: %s", job->url.c_str(), job->error.c_str());
                }
            }
            stages_ = std::move(merged);
            const bool anyOk = std::any_of(indexJobs_.begin(), indexJobs_.end(), [](const auto& j) { return j->ok; });
            state_ = anyOk ? IndexState::Ready : IndexState::Failed;
            if (!anyOk) {
                const std::string why = indexJobs_.empty() ? std::string() : indexJobs_.front()->error;
                error_ = std::string(Tr("스테이지 목록을 불러오지 못했습니다")) + (why.empty() ? "" : " (" + why + ")");
            }
            indexJobs_.clear();

            // previews: cached by id + version, downloaded once
            std::error_code ec;
            fs::create_directories(PreviewDir(), ec);
            for (const RemoteStage& s : stages_) {
                if (s.previewUrl.empty()) continue;
                const size_t dot = s.previewUrl.rfind('.');
                std::string ext = dot == std::string::npos ? ".png" : s.previewUrl.substr(dot);
                if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".webp") ext = ".png";
                const fs::path file = PreviewDir() / Utf8ToPath(s.id + "-" + s.version + ext);
                auto job = std::make_shared<PreviewJob>();
                job->id = s.id;
                job->version = s.version;
                previews_.push_back(job);
                if (fs::exists(file, ec)) {
                    job->file = file;
                    job->done = true;
                } else {
                    const std::string url = s.previewUrl;
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
    }
    for (auto it = previews_.begin(); it != previews_.end();) {
        if (!(*it)->done) {
            ++it;
            continue;
        }
        for (RemoteStage& s : stages_)
            if (s.id == (*it)->id && s.version == (*it)->version) s.previewFile = (*it)->file;
        it = previews_.erase(it);
    }
    for (auto it = installs_.begin(); it != installs_.end();) {
        InstallJob& j = **it;
        if (!j.done) {
            ++it;
            continue;
        }
        const std::string name = j.stage.name.Get(StageLanguage());
        if (j.ok) {
            RefreshInstalled();
            events.push_back({true, name + " " + j.stage.version + Tr(" 설치됨")});
            LOG_INFO("stage store: installed %s %s", j.stage.id.c_str(), j.stage.version.c_str());
        } else if (j.cancelled) {
            LOG_INFO("stage store: download of %s cancelled", j.stage.id.c_str());
        } else {
            events.push_back({false, name + ": " + j.error});
            LOG_WARN("stage store: install %s failed: %s", j.stage.id.c_str(), j.error.c_str());
        }
        it = installs_.erase(it);
    }
    return events;
}

} // namespace mmdx
