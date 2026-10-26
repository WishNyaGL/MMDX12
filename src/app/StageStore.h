#pragma once
// Online stage gallery (mmdx.codingbot.kr/stages/index.json): fetches the index, downloads a stage's zip (resumable,
// with SHA-256 verification) and installs it into <exe>/stages/<id>/, the folder AssetLibrary scans as installed
// stages. Modelled on app/ShaderPackStore.* (sources / verified / revoked / SHA-256 / worker thread) with the stage
// rules of docs/stage_format.md: one stage.json ("format": "mmdx-stage") at the zip root or in a single top folder,
// whose id must be the index entry's id.
// Network and disk work run on detached worker threads; every public call is main-thread only.
#include "asset/StageManifest.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mmdx {

struct RemoteStage {
    std::string id, version, author, license;
    std::string source;        // the index URL it was listed in
    std::string minEngine;     // "minEngine": minimum app version (like a pack's minAppVersion)
    StageText name, description;
    std::vector<std::string> tags;
    std::string previewUrl;    // absolute (resolved against the index URL); empty = none
    std::string downloadUrl;   // absolute
    std::string sha256;        // lower-case hex of the zip
    uint64_t size = 0;         // zip bytes (0 = unknown)
    bool revoked = false;      // the gallery blocked it (never installed, an installed copy is flagged)
    std::string revokedReason;
    std::filesystem::path previewFile;   // local copy once downloaded (empty until then)

    // The app version this entry needs (empty or <= MMDX12_VERSION = usable).
    bool Compatible() const;
};

// An installed stage folder (<exe>/stages/<folder>/stage.json).
struct InstalledStage {
    std::string id, version;
    StageText name;
    std::filesystem::path dir;
};

// True for StageStore::kDefaultIndexUrl (case-insensitive, trailing query string ignored).
bool IsOfficialStageIndexUrl(const std::string& url);
// `[a-z0-9_-]{1,64}` (the stage.json "id" shape).
bool IsValidStageId(const std::string& id);
// "1.2.0" style dotted compare via net::CompareVersions; a newer remote version of an installed stage = true.
bool StageUpdateAvailable(const InstalledStage& installed, const RemoteStage& remote);
// Case-insensitive search over id, name, author and tags (`needle` already lower-case).
bool StageMatches(const std::string& needle, const RemoteStage& stage);

// Parses an index document (exposed for tests). `indexUrl` resolves relative "preview" entries. Entries without an
// id / version / download / sha256 are skipped; a revoked entry stays listed with its reason.
bool ParseStageIndex(const std::string& json, const std::string& indexUrl, std::vector<RemoteStage>& out,
                     std::string& error);

inline constexpr const char* kStageManifestFormat = "mmdx-stage";

// True when a downloaded zip matches the index entry: its byte size (when the entry states one) and its SHA-256.
// Exposed for tests; the store calls it before anything touches <exe>/stages. A mismatch deletes nothing (the caller
// decides) and sets `error` ("size mismatch" / "SHA-256 mismatch").
bool VerifyStageZip(const std::filesystem::path& zip, const RemoteStage& stage, std::string& error);

// The install core, exposed for tests (the store's worker thread calls it after the download): `zip` is the verified
// archive, `stagesRoot` the folder the stage folder is created in (<exe>/stages in the app). Extracts into a scratch
// folder there, requires exactly one stage.json ("format": "mmdx-stage") at the root or in one top folder whose id is
// `stage.id`, then renames it to <stagesRoot>/<stage.id>. An existing folder of that id is kept until the new one is in
// place and restored when the rename fails (an update). Never writes outside `stagesRoot`.
bool InstallStageZip(const std::filesystem::path& zip, const RemoteStage& stage, const std::filesystem::path& stagesRoot,
                     std::string& error);

class StageStore {
public:
    static constexpr const char* kDefaultIndexUrl = "https://mmdx.codingbot.kr/stages/index.json";
    enum class IndexState { Idle, Loading, Ready, Failed };

    StageStore();
    ~StageStore();

    // Starts fetching the index (no-op while loading). `url` may also be a local path / file: URL (testing).
    void Refresh(const std::string& url = kDefaultIndexUrl);
    IndexState State() const { return state_; }
    const std::string& Error() const { return error_; }     // Korean, for the UI
    const std::string& IndexUrl() const { return url_; }
    const std::vector<RemoteStage>& Stages() const { return stages_; }

    // Installed stages (<exe>/stages): rescanned by Refresh() and after every install / uninstall.
    void RefreshInstalled();
    const std::vector<InstalledStage>& Installed() const { return installed_; }
    const InstalledStage* FindInstalled(const std::string& id) const;

    // Downloads and installs a stage (no-op while it is already being installed or when it is revoked).
    void Install(const RemoteStage& stage);
    void Cancel(const std::string& id);                     // stops the transfer; the .part file stays for a resume
    bool Installing(const std::string& id) const;
    float Progress(const std::string& id) const;            // 0..1 while downloading (0 when the size is unknown)
    // Deletes <exe>/stages/<id>. The caller refuses while the stage is loaded in a scene / the studio.
    bool Uninstall(const std::string& id, std::string& error);

    // Main thread, every frame: finishes the index fetch, the preview downloads and the installs.
    // Returns messages for toasts (Korean): {ok, text}.
    struct Event { bool ok; std::string text; };
    std::vector<Event> Poll();

    // %LOCALAPPDATA%-free runtime folders next to the exe (like shader_packs / shader_cache).
    static std::filesystem::path StagesRoot();      // <exe>/stages
    static std::filesystem::path DownloadDir();     // <exe>/cache/stage_downloads (zip + its .part live here)
    static std::filesystem::path PreviewDir();      // <exe>/cache/stage_previews

private:
    struct IndexJob;
    struct InstallJob;
    struct PreviewJob;
    std::vector<std::shared_ptr<IndexJob>> indexJobs_;
    std::vector<std::shared_ptr<InstallJob>> installs_;
    std::vector<std::shared_ptr<PreviewJob>> previews_;
    IndexState state_ = IndexState::Idle;
    std::string error_, url_;
    std::vector<RemoteStage> stages_;
    std::vector<InstalledStage> installed_;
};

} // namespace mmdx
