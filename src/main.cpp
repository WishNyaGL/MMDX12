#include <Windows.h>
#include <shellapi.h>
#include <cstdio>

#include <dbghelp.h>

#include "app/App.h"
#include "core/Log.h"

#pragma comment(lib, "shell32.lib")

namespace {

// Symbolized crash report, written through the app logger (mmdx12.log is flushed by every line).
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    HANDLE proc = GetCurrentProcess();
    SymInitialize(proc, nullptr, TRUE);

    const DWORD64 addr = (DWORD64)ep->ExceptionRecord->ExceptionAddress;
    LOG_ERROR("CRASH 0x%08lX at 0x%llX", (unsigned long)ep->ExceptionRecord->ExceptionCode,
              (unsigned long long)addr);

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrStack.Mode = AddrModeFlat;

    for (int i = 0; i < 32 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, nullptr,
                                          SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
         ++i) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        auto* si = (SYMBOL_INFO*)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{sizeof(line)};
        DWORD ld = 0;
        const char* name = SymFromAddr(proc, sf.AddrPC.Offset, &disp, si) ? si->Name : "?";

        char module[MAX_PATH] = "?";
        IMAGEHLP_MODULE64 mi{sizeof(mi)};
        if (SymGetModuleInfo64(proc, sf.AddrPC.Offset, &mi) && mi.ModuleName[0]) {
            lstrcpynA(module, mi.ModuleName, sizeof(module));
        }

        if (SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &ld, &line))
            LOG_ERROR("  #%d %s!%s %s:%lu", i, module, name, line.FileName, line.LineNumber);
        else
            LOG_ERROR("  #%d %s!%s", i, module, name);
    }

    mmdx::LogCloseFile();
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    SetUnhandledExceptionFilter(CrashFilter);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    mmdx::AppOptions opt = mmdx::ParseCommandLine(argc, argv);
    LocalFree(argv);
    mmdx::App app;
    return app.Run(inst, opt);
}
