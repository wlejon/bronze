// A host that SWAPS a loaded bronze module for a newer version of itself, in
// one process, against one shared runtime — the embed.h unload contract,
// exercised end to end.
//
// What each piece proves:
//
//  * UNLOAD DROPS THE ROOTS. Each version parks ~24MB in module scope and the
//    semispace is 32MB, so the process only survives the swap if
//    unloadModule really retired v1's spans and v1's payload died under v2's
//    allocations. There is no assertion to forget: retention IS the failure,
//    and it is fatal.
//
//  * UNLOAD KEEPS THE VALUES. A plain object v1 built is rooted in a host
//    Persistent across the swap and read back afterwards — unload retires
//    the module's tables as roots, not the module's surviving objects.
//
//  * THE IMAGE STAYS MAPPED. The harness never calls FreeLibrary/dlclose,
//    per the contract: v1's token is read back AFTER v2 ran, through
//    machinery that still compares v1's code pointers.
//
// The loader half (open, resolve the three symbols, check the stamp) is the
// same shape tests/shared_load/harness.cpp establishes; that test owns the
// refusal cases and the manifest, so this one does not repeat them.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "abi/bronze_abi.h"
#include "embed/embed.h"

namespace {

using bronze::Value;
namespace embed = bronze::embed;

#if defined(_WIN32)
using LibHandle = HMODULE;
LibHandle openLibrary(const char* path) { return LoadLibraryA(path); }
void* findSymbol(LibHandle lib, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(lib, name));
}
#else
using LibHandle = void*;
LibHandle openLibrary(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void* findSymbol(LibHandle lib, const char* name) { return dlsym(lib, name); }
#endif

[[noreturn]] void die(const std::string& what) {
    std::fflush(stdout);
    std::fprintf(stderr, "hot-swap harness: %s\n", what.c_str());
    std::exit(1);
}

void* requireSymbol(LibHandle lib, const char* name) {
    void* sym = findSymbol(lib, name);
    if (sym == nullptr) die(std::string("module does not export ") + name);
    return sym;
}

// Both versions are compiled with the SAME entry symbol — they are two
// versions of one module, and the name is the module's, not the build's.
constexpr const char* kEntry = "bronze_hot_module";

embed::ModuleEntry loadVersion(const char* path, const char* label) {
    LibHandle lib = openLibrary(path);
    if (lib == nullptr) die(std::string("cannot open ") + path);
    auto entry = reinterpret_cast<embed::ModuleEntry>(requireSymbol(lib, kEntry));
    const uint32_t stamp = *static_cast<const uint32_t*>(
        requireSymbol(lib, (std::string(kEntry) + "_abi_fingerprint").c_str()));
    if (stamp != embed::abiFingerprint()) {
        die(std::string(label) + " ABI fingerprint does not match the runtime's");
    }
    std::printf("abi: %s stamp matches the runtime\n", label);
    std::fflush(stdout);
    return entry;
}

Value globalObject() {
    const uint32_t key = bronze_register_key_string("globalThis");
    return Value(bronze_global_get(key, nullptr));
}

// Call `globalThis[name]()` — no arguments, so the rooting dance
// tests/shared_load/harness.cpp performs for its argument spans is not needed
// here; the lookup's own allocations concern only values this helper holds.
Value callGlobal(const char* name) {
    Value fn = embed::getProperty(globalObject(), name);
    embed::CallResult r = embed::call(fn, embed::undefined(), {});
    if (r.thrown) die(std::string("call to ") + name + " threw");
    return r.value;
}

void line(const std::string& text) {
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
}

std::string num(double d) { return std::to_string(static_cast<long long>(d)); }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) die("usage: harness <v1-module> <v2-module>");
    embed::setupIo();

    // ---- version 1 ---------------------------------------------------------

    embed::ModuleEntry v1 = loadVersion(argv[1], "v1");
    const embed::ModuleHandle h1 = embed::beginModuleLoad();
    embed::runEntry(v1);

    line("host: version=" + embed::toUtf8(callGlobal("hotVersion")));
    embed::collectGarbage();
    line("host: checksum=" + num(embed::toDouble(callGlobal("hotChecksum"))));

    // A v1-built object the host keeps across the swap.
    embed::Persistent keepsake{callGlobal("hotToken")};

    // ---- the swap ----------------------------------------------------------

    embed::unloadModule(h1);
    line("host: v1 unloaded");
    // v1's payload is NOT collectable yet — globalThis still holds v1's
    // functions, whose environment holds the payload. v2's entry overwrites
    // those names before allocating its own payload; the collection its
    // allocations force is where the 32MB semispace decides this test.

    embed::ModuleEntry v2 = loadVersion(argv[2], "v2");
    const embed::ModuleHandle h2 = embed::beginModuleLoad();
    embed::runEntry(v2);
    (void)h2;  // v2 stays loaded for the life of the process

    line("host: version=" + embed::toUtf8(callGlobal("hotVersion")));
    embed::collectGarbage();
    line("host: checksum=" + num(embed::toDouble(callGlobal("hotChecksum"))));

    // ---- v1's surviving value ----------------------------------------------

    embed::collectGarbage();
    const std::string tag = embed::toUtf8(embed::getProperty(keepsake.get(), "tag"));
    const double n = embed::toDouble(embed::getProperty(keepsake.get(), "n"));
    line("host: keepsake tag=" + tag + " n=" + num(n));

    return 0;
}
