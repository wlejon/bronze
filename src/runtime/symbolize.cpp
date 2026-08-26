
#include "runtime/symbolize.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
// dbghelp.h needs windows.h first; the pragma keeps the link line clean for
// every consumer of the runtime library, exactly as fatal.cpp already does.
#include <dbghelp.h>
#include <psapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "dbghelp.lib")
#endif
#endif

namespace bronze::runtime {

namespace {

#ifdef _WIN32
bool ensureSymInit() {
    static bool tried = false;
    static bool ok = false;
    if (!tried) {
        tried = true;
        ::SymSetOptions(::SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        // TRUE: enumerate and register every module already loaded. The dumps
        // run at exit, when app.dll and the runtime are still mapped (bro
        // never FreeLibrary's a compiled app), so their PDBs sitting beside
        // them resolve compiled-JS and runtime frames alike.
        ok = ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }
    return ok;
}

void moduleBasename(uint64_t pc, char* out, size_t outSize) {
    out[0] = '?';
    out[1] = 0;
    HMODULE mod = nullptr;
    if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(pc)), &mod) &&
        mod) {
        char path[MAX_PATH];
        if (::GetModuleFileNameA(mod, path, MAX_PATH)) {
            const char* base = std::strrchr(path, '\\');
            const char* base2 = std::strrchr(path, '/');
            if (base2 > base) base = base2;
            const char* name = base ? base + 1 : path;
            std::snprintf(out, outSize, "%s", name);
        }
    }
}
#endif

}  // namespace

void symbolizePc(uint64_t pc, SymbolizedPc& out) {
    out.name[0] = 0;
    out.module[0] = '?';
    out.module[1] = 0;
    out.funcStart = pc & ~0xFULL;
    out.resolved = false;
#ifdef _WIN32
    moduleBasename(pc, out.module, sizeof(out.module));
    if (!ensureSymInit()) return;
    char buf[sizeof(SYMBOL_INFO) + 512];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    if (::SymFromAddr(::GetCurrentProcess(), static_cast<DWORD64>(pc), &disp, sym)) {
        std::snprintf(out.name, sizeof(out.name), "%s", sym->Name);
        // sym->Address is the function's start; two samples anywhere inside
        // one function share this key.
        out.funcStart = sym->Address ? sym->Address : (pc & ~0xFULL);
        out.resolved = true;
    } else {
        // No symbol (a module without a PDB): fall back to the x64 unwind
        // table's function START so the PCs of one function still aggregate
        // into one row instead of one row per 16 bytes.
        DWORD64 imageBase = 0;
        if (PRUNTIME_FUNCTION rf =
                ::RtlLookupFunctionEntry(static_cast<DWORD64>(pc), &imageBase, nullptr)) {
            out.funcStart = imageBase + rf->BeginAddress;
            std::snprintf(out.name, sizeof(out.name), "%s+0x%llx", out.module,
                          static_cast<unsigned long long>(out.funcStart - imageBase));
        }
    }
#else
    (void)pc;
#endif
}

}  // namespace bronze::runtime
