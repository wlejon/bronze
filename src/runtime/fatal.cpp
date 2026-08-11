#include "runtime/fatal.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <crtdbg.h>
#include <windows.h>
#endif

namespace bronze {

static FatalHandler g_fatalHandler = nullptr;

void setFatalHandler(FatalHandler handler) {
    g_fatalHandler = handler;
}

void disableCrashDialogs() noexcept {
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
    disableCrashDialogs();
    std::abort();
}

}  // namespace bronze
