// Unit test for the pack crash guard (render/PackCrashGuard.h): crash marker -> suspects, clean clear -> none,
// persistent quarantine and a corrupt file. Imitates look_preset_test.cpp.
#include "render/PackCrashGuard.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace mmdx;

static int g_passed = 0, g_failed = 0;

static void Check(bool ok, const char* name) {
    if (ok) {
        ++g_passed;
        std::printf("PASS %s\n", name);
        return;
    }
    ++g_failed;
    std::printf("FAIL %s\n", name);
}

int main() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "mmdx12_pack_guard_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path marker = dir / "pack_guard.json";

    {   // begin -> simulated crash (new object on the same file) -> TakeSuspects returns it
        PackCrashGuard guard(marker);
        guard.Begin("pack_a");
    }
    {
        PackCrashGuard guard(marker);
        const std::vector<std::string> suspects = guard.TakeSuspects();
        Check(suspects.size() == 1 && suspects[0] == "pack_a", "crash leaves a suspect");
        Check(guard.Quarantined("pack_a"), "suspect is quarantined");
    }
    {
        PackCrashGuard guard(marker);
        Check(guard.TakeSuspects().empty(), "marker cleared after TakeSuspects");
    }

    {   // clean Clear -> no suspects
        PackCrashGuard guard(marker);
        guard.Begin("pack_b");
        guard.Clear("pack_b");
    }
    {
        PackCrashGuard guard(marker);
        Check(guard.TakeSuspects().empty(), "clean clear leaves no suspect");
    }

    {   // quarantine persists, release removes it
        PackCrashGuard guard(marker);
        guard.Quarantine("pack_c");
    }
    {
        PackCrashGuard guard(marker);
        Check(guard.Quarantined("pack_c"), "quarantine persists");
        guard.Release("pack_c");
    }
    {
        PackCrashGuard guard(marker);
        Check(!guard.Quarantined("pack_c"), "release removes the quarantine");
    }

    {   // corrupt file -> empty, no throw
        {
            std::ofstream out(marker, std::ios::binary | std::ios::trunc);
            out << "{ this is not json";
        }
        PackCrashGuard guard(marker);
        Check(guard.TakeSuspects().empty(), "corrupt file gives no suspects");
        Check(!guard.Quarantined("pack_x"), "corrupt file gives no quarantine");
        guard.Begin("pack_d");
        Check(guard.TakeSuspects().size() == 1, "begin after a corrupt file still works");
        guard.ClearAll();
    }
    {
        PackCrashGuard guard(marker);
        Check(guard.TakeSuspects().empty(), "ClearAll leaves no suspects");
    }

    fs::remove_all(dir, ec);
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
