// Unit tests for the stage gallery data layer (app/StageStore.h / core/NetUtil.h): index parsing, SHA-256 / id checks,
// the install core (atomic rename, update keeps the old folder on failure) and the resumable-download decision seam.
// Imitates tools/gallery_index_test.cpp: a tiny CHECK macro, PASS/FAIL per check, exit code 0 on success.
//
// The stage zips are built here from a stage.json the test writes, with `tar.exe -a -cf` (bsdtar, shipped with
// Windows 10 1803+), the same extractor the app uses. No HTTP server: the resume logic is unit-tested through
// net::ResumeRangeHeader / net::ResumeDecision.
#include "app/StageStore.h"
#include "core/NetUtil.h"
#include "core/TextUtil.h"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace mmdx;
namespace fs = std::filesystem;

static int g_passed = 0, g_failed = 0;

static void Check(bool ok, const char* name, const char* fmt = "", ...) {
    if (ok) {
        ++g_passed;
        std::printf("PASS %s\n", name);
        return;
    }
    ++g_failed;
    std::printf("FAIL %s: ", name);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

static const char* kSha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static std::string Entry(const std::string& id, const std::string& extra = "") {
    return std::string("{\"id\":\"") + id + "\",\"version\":\"1.0.0\",\"name\":{\"ko\":\"테스트\",\"en\":\"Test\"}," +
           "\"download\":\"https://cdn.example.com/" + id + ".zip\",\"sha256\":\"" + kSha + "\"" + extra + "}";
}
static std::string IndexDoc(const std::string& entries) { return "{\"stages\":[" + entries + "]}"; }

// ---- filesystem helpers ----------------------------------------------------------------------------------------------

static std::string RunTar(const std::string& args) {
    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    const std::wstring cmd = std::wstring(sysDir) + L"\\tar.exe " + Utf8ToWide(args);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return "tar.exe could not start";
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0 ? std::string() : "tar.exe exit " + std::to_string(code);
}

static void WriteFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

static std::string ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A stage folder's stage.json (a valid in-memory manifest model list; the model file is never opened by the install).
static std::string StageJson(const std::string& id, const std::string& version, const std::string& extra = "") {
    return "{\"format\":\"mmdx-stage\",\"formatVersion\":1,\"id\":\"" + id + "\",\"version\":\"" + version +
           "\",\"name\":{\"ko\":\"스테이지\",\"en\":\"Stage\"},\"models\":[{\"file\":\"parts/room.glb\"}]" + extra + "}";
}

// Builds <tmp>/<name>/... and zips it to <tmp>/<name>.zip; `nested` puts the stage in one top folder (as zips made by
// "compress the folder" do). Returns the zip path (empty + `error` on failure).
static fs::path MakeStageZip(const fs::path& tmp, const std::string& name, const std::string& id,
                             const std::string& version, bool nested, std::string& error) {
    const fs::path folder = tmp / Utf8ToPath(name);
    std::error_code ec;
    fs::remove_all(folder, ec);
    const fs::path root = nested ? folder / Utf8ToPath(id) : folder;
    WriteFile(root / L"stage.json", StageJson(id, version));
    WriteFile(root / L"parts" / L"room.glb", "glTF");
    const fs::path zip = tmp / Utf8ToPath(name + ".zip");
    fs::remove(zip, ec);
    // Run tar from `folder` so the archive holds "stage.json" (or "<id>/stage.json"), never an absolute path.
    wchar_t oldDir[MAX_PATH] = {};
    GetCurrentDirectoryW(MAX_PATH, oldDir);
    SetCurrentDirectoryW(folder.wstring().c_str());
    const std::string tarArgs = "-a -cf \"" + PathToUtf8(zip) + "\" .";
    error = RunTar(tarArgs);
    SetCurrentDirectoryW(oldDir);
    if (!error.empty()) return {};
    if (!fs::is_regular_file(zip, ec)) {
        error = "zip not written";
        return {};
    }
    return zip;
}

int main() {
    std::vector<RemoteStage> stages;
    std::string error;

    // ---- index parsing -----------------------------------------------------------------------------------------------
    Check(ParseStageIndex(IndexDoc(Entry("cyclorama")), "https://mmdx.codingbot.kr/stages/index.json", stages, error) &&
              stages.size() == 1,
          "good index parses");
    Check(stages.size() == 1 && stages[0].id == "cyclorama" && stages[0].version == "1.0.0" &&
              stages[0].name.Get("en") == "Test" && stages[0].sha256 == kSha &&
              stages[0].downloadUrl == "https://cdn.example.com/cyclorama.zip",
          "entry fields parsed");
    Check(!ParseStageIndex("not json", "https://x/index.json", stages, error) && stages.empty() && !error.empty(),
          "bad json refused");
    Check(!ParseStageIndex("{\"packs\":[]}", "https://x/index.json", stages, error) && !error.empty(),
          "index without a stages array refused");

    // missing fields: no download / bad sha / bad id / no version are skipped, the rest survives
    Check(ParseStageIndex(IndexDoc(Entry("ok_stage") + "," + Entry("no_download", ",\"download\":\"\"")),
                          "https://x/index.json", stages, error) &&
              stages.size() == 1 && stages[0].id == "ok_stage",
          "entry without a download skipped");
    Check(ParseStageIndex(IndexDoc(Entry("bad_sha", ",\"sha256\":\"abc\"")), "https://x/index.json", stages, error) &&
              stages.empty(),
          "entry with a short sha256 skipped");
    Check(ParseStageIndex(IndexDoc(Entry("Bad/Id")), "https://x/index.json", stages, error) && stages.empty(),
          "entry with an invalid id skipped");
    Check(ParseStageIndex(IndexDoc(Entry("v1", ",\"version\":\"\"")), "https://x/index.json", stages, error) &&
              stages.empty(),
          "entry without a version skipped");
    Check(ParseStageIndex(IndexDoc(Entry("with_extras",
                                        ",\"author\":\"Someone\",\"license\":\"CC0-1.0\",\"size\":12345,"
                                        "\"tags\":[\"stage\",\"led\"],\"minEngine\":\"2.0.0\","
                                        "\"description\":{\"en\":\"A stage\"},\"preview\":\"prev/cyclorama.png\"")),
                          "https://x/stages/index.json", stages, error) &&
              stages.size() == 1 && stages[0].author == "Someone" && stages[0].license == "CC0-1.0" &&
              stages[0].size == 12345 && stages[0].tags.size() == 2 && stages[0].minEngine == "2.0.0" &&
              stages[0].previewUrl == "https://x/stages/prev/cyclorama.png",
          "optional fields and a relative preview parse");

    // revoked: stays listed with its reason and is never installed / updated
    {
        const bool okRevoked =
            ParseStageIndex(IndexDoc(Entry("blocked", ",\"revoked\":true,\"reason\":\"broken\"")),
                            "https://x/index.json", stages, error);
        Check(okRevoked && stages.size() == 1 && stages[0].revoked && stages[0].revokedReason == "broken",
              "revoked entry stays listed with its reason");
    }
    Check(ParseStageIndex(IndexDoc(Entry("blocked2", ",\"revoked\":{\"reason\":\"dmca\"}")), "https://x/index.json",
                          stages, error) &&
              stages.size() == 1 && stages[0].revoked && stages[0].revokedReason == "dmca",
          "revoked object form parsed");
    {
        InstalledStage inst;
        inst.id = "blocked";
        inst.version = "0.9.0";
        Check(!StageUpdateAvailable(inst, stages[0]), "revoked entry never updates");
    }

    // official URL / id shape / search
    Check(IsOfficialStageIndexUrl(StageStore::kDefaultIndexUrl) &&
              IsOfficialStageIndexUrl("https://MMDX.codingbot.kr/stages/index.json?x=1") &&
              !IsOfficialStageIndexUrl("https://x/stages/index.json"),
          "official index url detection");
    Check(IsValidStageId("cyclorama_2-x") && !IsValidStageId("") && !IsValidStageId("Bad/Id") &&
              !IsValidStageId("Upper"),
          "stage id shape");
    Check(ParseStageIndex(IndexDoc(Entry("cyclorama", ",\"tags\":[\"led\"]")), "https://x/index.json", stages, error) &&
              StageMatches("led", stages[0]) && StageMatches("cyclo", stages[0]) && !StageMatches("nomatch", stages[0]),
          "search matches id and tags");
    Check(stages.size() == 1 && stages[0].Compatible(), "no minEngine = compatible");
    Check(ParseStageIndex(IndexDoc(Entry("future", ",\"minEngine\":\"99.0.0\"")), "https://x/index.json", stages,
                          error) &&
              stages.size() == 1 && !stages[0].Compatible(),
          "a future minEngine is incompatible");
    Check(ParseStageIndex(IndexDoc(Entry("older", ",\"minEngine\":\"2.0.0\"")), "https://x/index.json", stages, error) &&
              stages.size() == 1 && stages[0].Compatible(),
          "an older minEngine is compatible");

    // UpdateAvailable only for a newer version of the same id
    {
        InstalledStage inst;
        inst.id = "cyclorama";
        inst.version = "1.0.0";
        RemoteStage remote;
        remote.id = "cyclorama";
        remote.version = "1.1.0";
        Check(StageUpdateAvailable(inst, remote), "newer version updates");
        remote.version = "1.0.0";
        Check(!StageUpdateAvailable(inst, remote), "same version does not update");
        remote.version = "0.9.0";
        Check(!StageUpdateAvailable(inst, remote), "older version does not update");
        remote.version = "2.0.0";
        remote.id = "other";
        Check(!StageUpdateAvailable(inst, remote), "other id does not update");
    }

    // ---- resumable download seam -------------------------------------------------------------------------------------
    Check(net::PartPathFor(fs::path(L"C:/x/a.zip")) == fs::path(L"C:/x/a.zip.part"), "part path is <dest>.part");
    Check(net::ResumeRangeHeader(0).empty(), "no range header without a .part");
    Check(net::ResumeRangeHeader(1234) == "Range: bytes=1234-", "range header from the .part size");
    Check(net::ResumeDecision(206, 1234) == net::ResumeAction::Append, "206 appends at the part size");
    Check(net::ResumeDecision(200, 1234) == net::ResumeAction::Start, "200 restarts from 0");
    Check(net::ResumeDecision(416, 1234) == net::ResumeAction::Start, "416 (bad range) restarts from 0");
    Check(net::ResumeDecision(404, 0) == net::ResumeAction::Fail, "404 fails");
    Check(net::ResumeDecision(206, 0) == net::ResumeAction::Start, "206 without a part starts at 0");

    // ---- install core ------------------------------------------------------------------------------------------------
    const fs::path tmp = fs::temp_directory_path() / (L"mmdx12_stage_store_test_" + std::to_wstring(GetTickCount64()));
    std::error_code ec;
    fs::create_directories(tmp, ec);
    const fs::path exeLike = tmp / L"bin";          // <exe>-like: stages/ lives under it
    const fs::path stagesRoot = exeLike / L"stages";

    fs::path zip = MakeStageZip(tmp, "cyclorama_src", "cyclorama", "1.0.0", false, error);
    Check(!zip.empty(), "test zip built (%s)", error.c_str());
    std::string zipErr;
    RemoteStage entry;
    entry.id = "cyclorama";
    entry.version = "1.0.0";
    entry.sha256 = net::Sha256OfFile(zip, nullptr);
    entry.size = (uint64_t)fs::file_size(zip, ec);
    Check(VerifyStageZip(zip, entry, zipErr), "zip verifies against its size / sha256 (%s)", zipErr.c_str());

    {
        RemoteStage wrong = entry;
        wrong.sha256 = kSha;
        Check(!VerifyStageZip(zip, wrong, zipErr) && zipErr == "SHA-256 mismatch", "sha mismatch refused");
        RemoteStage wrongSize = entry;
        wrongSize.size += 1;
        Check(!VerifyStageZip(zip, wrongSize, zipErr) && zipErr == "size mismatch", "size mismatch refused");
        Check(!fs::exists(stagesRoot / L"cyclorama", ec), "a refused zip writes nothing into stages/");
    }

    Check(InstallStageZip(zip, entry, stagesRoot, zipErr), "install from a local zip (%s)", zipErr.c_str());
    Check(fs::is_regular_file(stagesRoot / L"cyclorama" / L"stage.json", ec), "installed stage.json in stages/<id>");
    Check(fs::is_regular_file(stagesRoot / L"cyclorama" / L"parts" / L"room.glb", ec), "installed parts extracted");
    Check(ReadFile(stagesRoot / L"cyclorama" / L"stage.json").find("\"1.0.0\"") != std::string::npos,
          "installed version 1.0.0");

    {
        // the wrong id never lands (the folder keeps the old stage)
        RemoteStage wrongId = entry;
        wrongId.id = "not_cyclorama";
        Check(!InstallStageZip(zip, wrongId, stagesRoot, zipErr) && zipErr.find("the index says") != std::string::npos,
              "wrong id refused (%s)", zipErr.c_str());
        Check(!fs::exists(stagesRoot / L"not_cyclorama", ec), "a wrong-id install creates no folder");
        Check(ReadFile(stagesRoot / L"cyclorama" / L"stage.json").find("\"1.0.0\"") != std::string::npos,
              "the installed stage survives a refused install");
    }

    // update: a newer zip replaces the folder
    const fs::path zip2 = MakeStageZip(tmp, "cyclorama_v2", "cyclorama", "2.0.0", true, error);
    Check(!zip2.empty(), "nested test zip built (%s)", error.c_str());
    RemoteStage entry2 = entry;
    entry2.version = "2.0.0";
    entry2.sha256 = net::Sha256OfFile(zip2, nullptr);
    entry2.size = (uint64_t)fs::file_size(zip2, ec);
    Check(InstallStageZip(zip2, entry2, stagesRoot, zipErr), "update from a single-top-folder zip (%s)",
          zipErr.c_str());
    Check(ReadFile(stagesRoot / L"cyclorama" / L"stage.json").find("\"2.0.0\"") != std::string::npos,
          "update replaced the stage folder");
    {
        int leftovers = 0;
        for (fs::directory_iterator it(stagesRoot, ec), end; !ec && it != end; it.increment(ec)) ++leftovers;
        Check(leftovers == 1, "no scratch folder left behind", "%d entries", leftovers);
    }

    // update keeps the old version when the new one fails: a zip whose stage.json id differs
    const fs::path zip3 = MakeStageZip(tmp, "wrong_src", "someone_else", "3.0.0", false, error);
    RemoteStage entry3 = entry;
    entry3.version = "3.0.0";
    entry3.sha256 = net::Sha256OfFile(zip3, nullptr);
    entry3.size = (uint64_t)fs::file_size(zip3, ec);
    Check(!InstallStageZip(zip3, entry3, stagesRoot, zipErr), "a failing update is refused (%s)", zipErr.c_str());
    Check(ReadFile(stagesRoot / L"cyclorama" / L"stage.json").find("\"2.0.0\"") != std::string::npos,
          "a failing update keeps the old version");

    // a zip with no stage.json at all (two top folders: not one stage)
    {
        const fs::path src = tmp / L"twofolders";
        WriteFile(src / L"a" / L"stage.json", StageJson("two", "1.0.0"));
        WriteFile(src / L"b" / L"stage.json", StageJson("two_b", "1.0.0"));
        wchar_t oldDir[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, oldDir);
        SetCurrentDirectoryW(src.wstring().c_str());
        const fs::path zipTwo = tmp / L"twofolders.zip";
        fs::remove(zipTwo, ec);
        const std::string terr = RunTar("-a -cf \"" + PathToUtf8(zipTwo) + "\" .");
        SetCurrentDirectoryW(oldDir);
        RemoteStage two;
        two.id = "two";
        two.version = "1.0.0";
        two.sha256 = net::Sha256OfFile(zipTwo, nullptr);
        const bool installed = terr.empty() && InstallStageZip(zipTwo, two, stagesRoot, zipErr);
        Check(!installed && zipErr.find("single stage.json") != std::string::npos,
              "two top folders = no single stage.json (%s%s)", terr.c_str(), zipErr.c_str());
    }

    fs::remove_all(tmp, ec);

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
