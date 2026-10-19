#pragma once
// Crash guard for shader packs (docs/effect_ecosystem.md §3, GPU safety). A marker file next to the exe records the
// packs that were being compiled / drawn when the process died; the next start takes them as suspects and can
// quarantine them until the user re-enables them. Writes are atomic (tmp + rename) and flushed before returning; no
// call throws.
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

class PackCrashGuard {
public:
    explicit PackCrashGuard(std::filesystem::path markerFile);   // e.g. <exe>/pack_guard.json
    // At startup: a marker left by the previous run names the packs that were being compiled / first drawn when
    // the process died (TDR / crash). Returns them (and clears the marker). Empty when the last run ended cleanly.
    std::vector<std::string> TakeSuspects();
    void Begin(const std::string& packId);   // before compiling / first frame of a pack (adds to the marker, flushed)
    void Clear(const std::string& packId);   // after the first successfully presented frame with it
    void ClearAll();
    // Persistent quarantine list (same file, "quarantined": [...]): suspects become quarantined until the user
    // re-enables them.
    bool Quarantined(const std::string& packId) const;
    void Quarantine(const std::string& packId);
    void Release(const std::string& packId);

private:
    void Load();
    void Save() const;

    std::filesystem::path markerFile_;
    std::vector<std::string> active_;       // packs in flight (the crash marker)
    std::vector<std::string> quarantined_;  // persistent
    bool loaded_ = false;
};

} // namespace mmdx
