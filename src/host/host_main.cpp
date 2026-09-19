// The program host: the executable half of every `bronze build` program.
//
// `bronze build app.js -o app` writes the compiled program as a loadable
// module beside its output (`app.dll` / `app.so` / `app.dylib`) and copies
// THIS binary to `app`. What this binary does is what src/rt/rt.cpp's `main`
// did for a statically linked program — stdio setup, the ABI stamp check,
// the code-range registration, the entry, the microtask checkpoint — with the
// program arriving through the loader instead of the linker: it opens the
// module named after itself, resolves the loadable-module contract
// src/abi/bronze_abi.h states, and runs it through embed::runEntry.
//
// It links the SHARED runtime and nothing else from bronze, exactly as a
// loading host must (cmake/bronze_shared_runtime.cmake says why a second
// copy of the runtime's state is the failure to avoid), so a program is three
// files in one directory: this host under the program's name, the module,
// and the runtime library both of them bind to.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <climits>
#include <cstdlib>
#else
#include <dlfcn.h>
#include <climits>
#include <unistd.h>
#endif

#include "abi/bronze_abi.h"
#include "embed/embed.h"

namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
constexpr const char* kModuleExtension = ".dll";
constexpr const char* kRuntimeFile = "bronze_runtime_shared.dll";
#elif defined(__APPLE__)
constexpr const char* kModuleExtension = ".dylib";
constexpr const char* kRuntimeFile = "libbronze_runtime_shared.dylib";
#else
constexpr const char* kModuleExtension = ".so";
constexpr const char* kRuntimeFile = "libbronze_runtime_shared.so";
#endif

[[noreturn]] void die(const std::string& what) {
    std::fflush(stdout);
    std::fprintf(stderr, "%s\n", what.c_str());
    std::exit(1);
}

// This executable's own path, from the OS rather than argv[0]: argv[0] is
// whatever the shell typed, and a program started through PATH has no
// directory in it at all.
fs::path ownPath() {
#if defined(_WIN32)
    wchar_t buffer[32768];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (len == 0 || len >= sizeof(buffer) / sizeof(buffer[0])) die("bronze host: cannot determine its own path");
    return fs::path(buffer);
#elif defined(__APPLE__)
    char buffer[PATH_MAX];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) die("bronze host: cannot determine its own path");
    char real[PATH_MAX];
    if (realpath(buffer, real) != nullptr) return fs::path(real);
    return fs::path(buffer);
#else
    char buffer[PATH_MAX];
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len <= 0) die("bronze host: cannot determine its own path");
    buffer[len] = '\0';
    return fs::path(buffer);
#endif
}

#if defined(_WIN32)
using LibHandle = HMODULE;
LibHandle openModule(const fs::path& path) {
    // The module's own directory is searched for its imports first, which
    // is where the runtime beside the program is.
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}
std::string lastError() {
    const DWORD code = GetLastError();
    char* text = nullptr;
    const DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message = (len && text) ? std::string(text, len) : ("error " + std::to_string(code));
    if (text) LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) {
        message.pop_back();
    }
    return message;
}
void* findSymbol(LibHandle lib, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(lib, name));
}
#else
using LibHandle = void*;
LibHandle openModule(const fs::path& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
std::string lastError() {
    const char* text = dlerror();
    return text ? text : "unknown error";
}
void* findSymbol(LibHandle lib, const char* name) { return dlsym(lib, name); }
#endif

void* requireSymbol(LibHandle lib, const fs::path& module, const char* name) {
    void* sym = findSymbol(lib, name);
    if (sym == nullptr) {
        die(module.string() + " does not export " + name + "; it is not a module bronze built");
    }
    return sym;
}

}  // namespace

int main() {
    bronze::embed::setupIo();

    const fs::path self = ownPath();
    fs::path module = self;
    module.replace_extension(kModuleExtension);

    LibHandle lib = openModule(module);
    if (lib == nullptr) {
        die("cannot load " + module.string() + ": " + lastError() +
            "\nA bronze-built program is the executable, the module of the same name beside it, "
            "and the runtime " + kRuntimeFile + " in the same directory.");
    }

    // The loadable-module contract, resolved by name (bronze_abi.h).
    auto entry = reinterpret_cast<bronze::embed::ModuleEntry>(requireSymbol(lib, module, "bronze_main"));
    const uint32_t stamp =
        *static_cast<const uint32_t*>(requireSymbol(lib, module, "bronze_object_abi_fingerprint"));

    // The module's ABI stamp against the runtime's, before any of its code
    // runs — the check rt.cpp's main made through runtime/abi_guard.h, made
    // here through the loader's fact instead of the linker's. The runtime is
    // asked for its own value at run time: a host that compared against the
    // constant it was compiled with would be checking the module against
    // itself.
    const uint32_t runtimeStamp = bronze::embed::abiFingerprint();
    if (stamp != runtimeStamp) {
        char text[256];
        std::snprintf(text, sizeof(text),
                      "bronze ABI mismatch: this program's module was compiled against ABI %08x, "
                      "but the runtime beside it speaks %08x. Recompile the app with the bronze "
                      "CLI built from the same tree as this runtime.",
                      stamp, runtimeStamp);
        die(text);
    }

    // Where the program's code is, for the stack walks Error.stack and the
    // sampler perform — the registration the static `main` performed off the
    // symbols the linker resolved.
    const auto* ranges =
        static_cast<const bronze_code_range*>(requireSymbol(lib, module, "bronze_object_code_ranges"));
    const uint32_t rangeCount =
        *static_cast<const uint32_t*>(requireSymbol(lib, module, "bronze_object_code_range_count"));
    bronze_register_code_ranges(ranges, rangeCount);

    // A GC root frame, the entry, then one drain of the microtask queue —
    // what runMain does for a linked program (embed_module.cpp).
    bronze::embed::runEntry(entry);
    return 0;
}
