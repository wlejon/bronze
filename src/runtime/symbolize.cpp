
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

// Whether `path` lies under the Windows directory (a system DLL).
bool isWindowsModule(const char* path) {
    static char winDir[MAX_PATH] = {0};
    static size_t winLen = 0;
    if (winLen == 0) {
        const UINT n = ::GetWindowsDirectoryA(winDir, MAX_PATH);
        winLen = (n > 0 && n < MAX_PATH) ? n : 0;
        if (winLen == 0) return true;  // unknown: keep export names
    }
    return path && ::_strnicmp(path, winDir, winLen) == 0;
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
    // A module whose PDB is missing or does not match the binary (a DLL
    // restaged while its build tree's PDB moved on) has only its exports:
    // SymFromAddr then names every PC after the nearest exported function
    // before it, which piles unrelated code onto a few export names. Such
    // a PC is named by its unwind-table function instead, marked so.
    // (Asked after SymFromAddr: deferred loading loads the module's symbols
    // on its first lookup.)
    // Windows' own DLLs keep their export names (they export what runs hot
    // in them: RtlAllocateHeap and the like).
    const bool found = ::SymFromAddr(::GetCurrentProcess(), static_cast<DWORD64>(pc), &disp, sym) != FALSE;
    IMAGEHLP_MODULE64 modInfo{};
    modInfo.SizeOfStruct = sizeof(modInfo);
    bool exportsOnly = false;
    if (found && ::SymGetModuleInfo64(::GetCurrentProcess(), static_cast<DWORD64>(pc), &modInfo) &&
        (modInfo.SymType == SymExport || modInfo.SymType == SymNone)) {
        exportsOnly = modInfo.PdbUnmatched || !isWindowsModule(modInfo.LoadedImageName[0] ? modInfo.LoadedImageName
                                                                                            : modInfo.ImageName);
    }
    if (found && !exportsOnly) {
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
            std::snprintf(out.name, sizeof(out.name), "%s+0x%llx%s", out.module,
                          static_cast<unsigned long long>(out.funcStart - imageBase),
                          exportsOnly ? " [exports only: no matching PDB]" : "");
        }
    }
#else
    (void)pc;
#endif
}

}  // namespace bronze::runtime
