
#include "runtime/symbolize.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__) || defined(__linux__)
#include <cxxabi.h>
#include <dlfcn.h>
#endif

#ifdef __linux__
#include <algorithm>
#include <elf.h>
#include <string>
#include <unordered_map>
#include <vector>
#endif

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

#ifdef __linux__
struct ElfSymEntry {
    uint64_t value = 0;
    uint64_t size = 0;
    std::string name;
};

struct ElfModuleSymTable {
    uint64_t loadBias = 0;
    std::vector<ElfSymEntry> funcs;
};

const ElfModuleSymTable& getElfModuleSyms(const void* fbase, const char* fname) {
    static std::unordered_map<const void*, ElfModuleSymTable> cache;
    auto it = cache.find(fbase);
    if (it != cache.end()) return it->second;

    ElfModuleSymTable& table = cache[fbase];
    const char* path = (fname && fname[0]) ? fname : "/proc/self/exe";
    FILE* f = std::fopen(path, "rb");
    if (!f && std::strcmp(path, "/proc/self/exe") != 0) {
        f = std::fopen("/proc/self/exe", "rb");
    }
    if (!f) return table;

    Elf64_Ehdr ehdr{};
    if (std::fread(&ehdr, sizeof(ehdr), 1, f) != 1 ||
        std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        std::fclose(f);
        return table;
    }

    // For ET_DYN (PIE / shared object), dli_fbase is the load bias; for ET_EXEC,
    // symbol values are already virtual addresses so loadBias is 0.
    table.loadBias = (ehdr.e_type == ET_DYN) ? reinterpret_cast<uint64_t>(fbase) : 0;

    if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0 || ehdr.e_shentsize != sizeof(Elf64_Shdr)) {
        std::fclose(f);
        return table;
    }

    std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
    if (std::fseek(f, static_cast<long>(ehdr.e_shoff), SEEK_SET) != 0 ||
        std::fread(shdrs.data(), sizeof(Elf64_Shdr), ehdr.e_shnum, f) != ehdr.e_shnum) {
        std::fclose(f);
        return table;
    }

    for (const Elf64_Shdr& sh : shdrs) {
        if ((sh.sh_type != SHT_SYMTAB && sh.sh_type != SHT_DYNSYM) ||
            sh.sh_entsize != sizeof(Elf64_Sym) || sh.sh_link >= shdrs.size()) {
            continue;
        }
        const Elf64_Shdr& strSh = shdrs[sh.sh_link];
        if (strSh.sh_size == 0 || strSh.sh_size > (128ull << 20)) continue;
        std::vector<char> strtab(strSh.sh_size);
        if (std::fseek(f, static_cast<long>(strSh.sh_offset), SEEK_SET) != 0 ||
            std::fread(strtab.data(), 1, strSh.sh_size, f) != strSh.sh_size) {
            continue;
        }
        strtab.back() = '\0';

        const size_t nSyms = sh.sh_size / sizeof(Elf64_Sym);
        std::vector<Elf64_Sym> syms(nSyms);
        if (std::fseek(f, static_cast<long>(sh.sh_offset), SEEK_SET) != 0 ||
            std::fread(syms.data(), sizeof(Elf64_Sym), nSyms, f) != nSyms) {
            continue;
        }

        for (const Elf64_Sym& s : syms) {
            if (s.st_shndx == SHN_UNDEF || s.st_value == 0 || s.st_name >= strtab.size()) continue;
            const unsigned char type = ELF64_ST_TYPE(s.st_info);
            if (type != STT_FUNC && type != STT_NOTYPE) continue;
            const char* rawName = &strtab[s.st_name];
            if (!rawName[0]) continue;
            table.funcs.push_back(ElfSymEntry{s.st_value, s.st_size, rawName});
        }
    }
    std::fclose(f);

    std::sort(table.funcs.begin(), table.funcs.end(),
              [](const ElfSymEntry& a, const ElfSymEntry& b) {
                  if (a.value != b.value) return a.value < b.value;
                  return a.size > b.size;
              });
    return table;
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
        out.funcStart = sym->Address ? sym->Address : (pc & ~0xFULL);
        out.resolved = true;
    } else {
        DWORD64 imageBase = 0;
        if (PRUNTIME_FUNCTION rf =
                ::RtlLookupFunctionEntry(static_cast<DWORD64>(pc), &imageBase, nullptr)) {
            out.funcStart = imageBase + rf->BeginAddress;
            std::snprintf(out.name, sizeof(out.name), "%s+0x%llx%s", out.module,
                          static_cast<unsigned long long>(out.funcStart - imageBase),
                          exportsOnly ? " [exports only: no matching PDB]" : "");
        }
    }
#elif defined(__APPLE__) || defined(__linux__)
    Dl_info info{};
    if (::dladdr(reinterpret_cast<const void*>(static_cast<uintptr_t>(pc)), &info) == 0) return;
    if (info.dli_fname) {
        const char* base = std::strrchr(info.dli_fname, '/');
        std::snprintf(out.module, sizeof(out.module), "%s", base ? base + 1 : info.dli_fname);
    }
    const char* rawSymName = info.dli_sname;
    uint64_t symAddr = reinterpret_cast<uint64_t>(info.dli_saddr);
#ifdef __linux__
    if ((!rawSymName || !symAddr) && info.dli_fbase) {
        const ElfModuleSymTable& table = getElfModuleSyms(info.dli_fbase, info.dli_fname);
        if (!table.funcs.empty() && pc >= table.loadBias) {
            const uint64_t relPc = pc - table.loadBias;
            auto it = std::upper_bound(
                table.funcs.begin(), table.funcs.end(), relPc,
                [](uint64_t v, const ElfSymEntry& e) { return v < e.value; });
            if (it != table.funcs.begin()) {
                --it;
                if (it->size == 0 ? (relPc - it->value < 0x10000) : (relPc < it->value + it->size)) {
                    rawSymName = it->name.c_str();
                    symAddr = table.loadBias + it->value;
                }
            }
        }
    }
#endif
    if (rawSymName && symAddr) {
        int status = 0;
        char* demangled = abi::__cxa_demangle(rawSymName, nullptr, nullptr, &status);
        std::snprintf(out.name, sizeof(out.name), "%s", status == 0 && demangled ? demangled : rawSymName);
        std::free(demangled);
        out.funcStart = symAddr;
        out.resolved = true;
    }
#else
    (void)pc;
#endif
}

}  // namespace bronze::runtime
