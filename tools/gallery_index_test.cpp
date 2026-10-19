// Unit tests for the open gallery index data layer (app/ShaderPackStore.h): verified / revoked / type / requires
// parsing and source-pinned updates. Imitates look_preset_test.cpp: a tiny CHECK macro, prints PASS/FAIL per check and
// a summary, exit code 0 on success.
#include "app/ShaderPackStore.h"
#include "render/ShaderPack.h"
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

using namespace mmdx;

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
static const char* kOtherSha = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
static const char* kOfficial = ShaderPackStore::kDefaultIndexUrl;

static std::string Index(const std::string& type = "surface", const std::string& extra = "",
                         const std::string& revoked = "") {
    return std::string("{\"packs\":[{\"id\":\"test_pack\",\"version\":\"1.2.0\",\"type\":\"") + type +
           "\",\"download\":\"pack.zip\",\"sha256\":\"" + kSha + "\"" + extra + "}]" +
           (revoked.empty() ? "" : ",\"revoked\":" + revoked) + "}";
}

int main() {
    std::vector<RemotePack> packs;
    std::string error;

    // verified is honoured only for the official URL (case-insensitive, query string ignored)
    Check(ParseShaderPackIndex(Index("surface", ",\"verified\":true"), kOfficial, packs, error) && packs.size() == 1,
          "official parse");
    Check(packs.size() == 1 && packs[0].verified, "verified honoured from official index");
    Check(ParseShaderPackIndex(Index("surface", ",\"verified\":true"),
                               "https://MMDX.codingbot.kr/shader-packs/index.json?cache=1", packs, error) &&
              packs.size() == 1 && packs[0].verified,
          "verified honoured for case / query variants of the official URL");
    Check(ParseShaderPackIndex(Index("surface", ",\"verified\":true"),
                               "https://third.example.com/shader-packs/index.json", packs, error) &&
              packs.size() == 1 && !packs[0].verified,
          "verified ignored for third-party indexes");

    // revoked entries are dropped (official only)
    Check(ParseShaderPackIndex(Index("surface", "", "[\"" + std::string(kSha) + "\"]"), kOfficial, packs, error) &&
              packs.empty(),
          "revoked entry dropped");
    std::vector<std::string> revoked;
    Check(ParseShaderPackIndex(Index("surface", "", "[\"" + std::string(kSha) + "\"]"), kOfficial, packs, &revoked,
                               error) &&
              revoked.size() == 1 && revoked[0] == kSha,
          "revoked list parsed");
    Check(ParseShaderPackIndex(Index("surface", "", "[\"" + std::string(kSha) + "\"]"),
                               "https://third.example.com/index.json", packs, &revoked, error) &&
              packs.size() == 1 && revoked.empty(),
          "revoked ignored for third-party indexes");

    // types: look / world parsed but incompatible, surface / effect compatible
    Check(ParseShaderPackIndex(Index("look"), kOfficial, packs, error) && packs.size() == 1 && packs[0].type == "look",
          "look type parsed");
    Check(!packs.empty() && !packs[0].Compatible(), "look incompatible");
    Check(ParseShaderPackIndex(Index("world"), kOfficial, packs, error) && packs.size() == 1 &&
              packs[0].type == "world",
          "world type parsed");
    Check(!packs.empty() && !packs[0].Compatible(), "world incompatible");
    Check(ParseShaderPackIndex(Index("surface"), kOfficial, packs, error) && !packs.empty() && packs[0].Compatible(),
          "surface compatible");
    Check(ParseShaderPackIndex(Index("effect", ",\"apiVersion\":3"), kOfficial, packs, error) && !packs.empty() &&
              packs[0].Compatible(),
          "effect compatible");

    // requires: known is compatible, unknown is not
    Check(ParseShaderPackIndex(Index("surface", ",\"requires\":[\"state\"]"), kOfficial, packs, error) &&
              !packs.empty() && packs[0].Compatible(),
          "known requires compatible");
    Check(ParseShaderPackIndex(Index("surface", ",\"requires\":[\"no.such.cap\"]"), kOfficial, packs, error) &&
              !packs.empty() && !packs[0].Compatible(),
          "unknown requires incompatible");

    // owner / source
    Check(ParseShaderPackIndex(Index("surface", ",\"owner\":\"Publisher\""), kOfficial, packs, error) && !packs.empty() &&
              packs[0].owner == "Publisher" && packs[0].source == kOfficial,
          "owner and source parsed");

    // UpdateAllowed: same source newer = true, other source = false, not newer = false, built-in = false
    ShaderPack installed;
    installed.id = "test_pack";
    installed.version = "1.0.0";
    installed.source = PackSource::Online;
    installed.installedFrom = kOfficial;
    RemotePack remote;
    remote.id = "test_pack";
    remote.version = "1.1.0";
    remote.source = kOfficial;
    Check(UpdateAllowed(installed, remote), "same source newer updates");

    remote.source = "https://third.example.com/index.json";
    Check(!UpdateAllowed(installed, remote), "other source does not update");

    remote.source = kOfficial;
    remote.version = "1.0.0";
    Check(!UpdateAllowed(installed, remote), "same version does not update");
    remote.version = "0.9.0";
    Check(!UpdateAllowed(installed, remote), "older version does not update");

    ShaderPack builtIn = installed;
    builtIn.source = PackSource::BuiltIn;
    remote.version = "2.0.0";
    Check(!UpdateAllowed(builtIn, remote), "built-in never updates");

    // IsOfficialIndexUrl
    Check(IsOfficialIndexUrl(kOfficial), "official url");
    Check(!IsOfficialIndexUrl("https://example.com/index.json"), "other url is not official");

    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
