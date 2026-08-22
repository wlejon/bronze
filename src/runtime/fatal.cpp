#include "runtime/fatal.h"
#include "runtime/profile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <crtdbg.h>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace bronze {

namespace {
// A symbolized native backtrace on stderr — compiled-JS frames resolve to
// their function names when the module's .pdb sits beside it. Best-effort:
// every dbghelp failure just degrades a frame to a bare address.
void dumpBacktrace() {
#ifdef _WIN32
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    void* frames[62];
    USHORT n = CaptureStackBackTrace(0, 62, frames, nullptr);
    char symBuf[sizeof(SYMBOL_INFO) + 512];
    std::fprintf(stderr, "backtrace (%u frames):\n", (unsigned)n);
    for (USHORT i = 0; i < n; ++i) {
        DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        const char* name = "?";
        if (SymFromAddr(proc, addr, &disp, sym)) name = sym->Name;
        char modName[MAX_PATH] = "?";
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
            GetModuleFileNameA(mod, modName, MAX_PATH);
            const char* base = strrchr(modName, '\\');
            if (base) memmove(modName, base + 1, strlen(base));
        }
        std::fprintf(stderr, "  #%02u %p %s!%s+0x%llx\n", (unsigned)i, frames[i], modName,
                     name, (unsigned long long)disp);
    }
    std::fflush(stderr);
#endif
}
}  // namespace

static FatalHandler g_fatalHandler = nullptr;

void setFatalHandler(FatalHandler handler) {
    g_fatalHandler = handler;
}

void disableCrashDialogs() noexcept {
    runtime::initProfile();
#ifdef _WIN32
    // No "abort() has been called" box, no Watson report.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#ifdef _DEBUG
    // The debug CRT's own report dialog is separate from the above.
    static const int kReportModes[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
    for (int i = 0; i < 3; ++i) {
        _CrtSetReportMode(kReportModes[i], _CRTDBG_MODE_FILE);
        _CrtSetReportFile(kReportModes[i], _CRTDBG_FILE_STDERR);
    }
#endif
#endif
}

[[noreturn]] void fatal(const char* msg) {
    if (g_fatalHandler) {
        g_fatalHandler(msg);
    }
    std::fprintf(stderr, "Hard runtime error: %s\n", msg);
    std::fflush(stderr);
    dumpBacktrace();
    disableCrashDialogs();
    std::abort();
}

}  // namespace bronze
