#pragma once
// Online shader pack gallery (mmdx.codingbot.kr/shader-packs/index.json): fetches the index, downloads a pack's zip,
// checks its size and SHA-256 against the index and hands it to ShaderPackRegistry::InstallZip on the main thread.
// Network and disk work run on detached worker threads; every public call is main-thread only.
#include "render/ShaderPack.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mmdx {

// Gallery types this build can use. `look` / `world` are parsed and listed but stay incompatible until the build
// gains support for them (flip the constant when it does).
inline constexpr bool kPackTypeLookCompatible = true;
inline constexpr bool kPackTypeWorldCompatible = false;

struct RemotePack {
    std::string id, version, minAppVersion, license, homepage, repository;
    std::string type = "surface";
    std::string source;            // the index URL it was listed in
    std::string owner;             // publisher display name
    std::string updated;
    int apiVersion = 1;
    LocalizedText name, description, recommendedFor;
    std::vector<PackAuthor> authors;
    std::vector<std::string> tags;
    std::vector<std::string> requiresCaps;   // "requires": capability names (kPackCapabilities)
    bool builtIn = false;          // also shipped with the app
    bool verified = false;         // only honoured from the official index
    std::string previewUrl;        // absolute (resolved against the index URL); empty = none
    std::string previewBeforeUrl;  // absolute (resolved against the index URL); empty = none
    std::string downloadUrl;       // absolute
    std::string sha256;            // lower-case hex of the zip (also the zipSha alias)
    uint64_t size = 0;             // zip bytes
    int paramCount = 0;
    std::filesystem::path previewFile;         // local copy once downloaded (empty until then)
    std::filesystem::path previewBeforeFile;   // local copy once downloaded (empty until then)

    std::string& zipSha() { return sha256; }
    const std::string& zipSha() const { return sha256; }

    bool Compatible() const;       // apiVersion / minAppVersion / type support / known capabilities
};

// True for ShaderPackStore::kDefaultIndexUrl (case-insensitive, trailing query string ignored).
bool IsOfficialIndexUrl(const std::string& url);

// An installed Online pack updates only from the same source index, and only to a newer version.
bool UpdateAllowed(const ShaderPack& installed, const RemotePack& remote);

// "A, B, C +2" for cards and rows.
std::string PackAuthorLine(const std::vector<PackAuthor>& authors);
// Case-insensitive search over id, localized name, authors and tags (`needle` already lower-case).
bool PackMatches(const std::string& needle, const std::string& id, const LocalizedText& name,
                 const std::vector<PackAuthor>& authors, const std::vector<std::string>& tags);

// Parses an index document (exposed for tests). `indexUrl` resolves relative "preview" / "download" entries.
bool ParseShaderPackIndex(const std::string& json, const std::string& indexUrl, std::vector<RemotePack>& out,
                          std::string& error);
// Also parses the official index's top-level "revoked" list (ignored for other indexes).
bool ParseShaderPackIndex(const std::string& json, const std::string& indexUrl, std::vector<RemotePack>& out,
                          std::vector<std::string>* revoked, std::string& error);

class ShaderPackStore {
public:
    static constexpr const char* kDefaultIndexUrl = "https://mmdx.codingbot.kr/shader-packs/index.json";
    enum class IndexState { Idle, Loading, Ready, Failed };

    ~ShaderPackStore();

    // Starts fetching the index (no-op while loading). `url` may also be a local path / file: URL (testing).
    void Refresh(const std::string& url = kDefaultIndexUrl);
    // Fetches every index (official first), merging the packs; one source failing keeps the others.
    void Refresh(const std::vector<std::string>& urls);
    IndexState State() const { return state_; }
    const std::string& Error() const { return error_; }     // Korean, for the UI
    const std::string& IndexUrl() const { return url_; }
    const std::vector<RemotePack>& Packs() const { return packs_; }
    IndexState SourceState(const std::string& url) const;
    const std::string& SourceError(const std::string& url) const;
    // Union of the official index's revoked zip SHA-256 values.
    std::vector<std::string> RevokedHashes() const;

    // Downloads and installs a pack (no-op if it is already being installed).
    void Install(const RemotePack& pack);
    bool Installing(const std::string& id) const;
    float Progress(const std::string& id) const;            // 0..1 while downloading

    // Main thread, every frame: finishes the index fetch, the preview downloads and the installs (InstallZip).
    // Returns messages for toasts (Korean): {ok, text}.
    struct Event { bool ok; std::string text; };
    std::vector<Event> Poll(ShaderPackRegistry& registry);

private:
    struct IndexJob;
    struct InstallJob;
    struct PreviewJob;
    struct SourceStateEntry {
        std::string url;
        IndexState state = IndexState::Idle;
        std::string error;
    };
    std::vector<std::shared_ptr<IndexJob>> indexJobs_;
    std::vector<std::shared_ptr<InstallJob>> installs_;
    std::vector<std::shared_ptr<PreviewJob>> previews_;
    IndexState state_ = IndexState::Idle;
    std::string error_, url_;
    std::vector<RemotePack> packs_;
    std::vector<SourceStateEntry> sources_;
    std::vector<std::string> revoked_;
};

} // namespace mmdx
