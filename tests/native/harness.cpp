// The ahead-of-time half of the native suite: a host that LOADS a module
// compiled against a printed manifest, registers the natives the manifest
// described, binds the module's import table from that registry by name, and
// enters it. tests/shared_load/harness.cpp is the model (one runtime, one
// heap, the exported symbols read before anything runs); what this one adds
// is the fourth symbol, `<entry>_native_imports`, and the bind step.
//
// What each line of the pinned output proves:
//
//  * IMPORTS ARE DATA. The table names every native the module calls, with
//    the signature it was compiled against, and the harness prints them
//    before it has registered anything — a loader can diff a module against
//    its registry while refusing is still possible.
//  * BOUND AT LOAD, BY NAME. The module links no native library; its slots
//    start at bronze_native_unbound and are filled here from
//    embed::registerNative calls this process made. The module's lines are
//    the calls going through.
//  * ONE REGISTRY, ONE HEAP. A handle the module made owes a destructor the
//    harness registered, and the harness counts it running. A typed array the
//    HOST allocated crosses into a native the MODULE calls.
//  * A GAP IS A REFUSAL BY NAME. The second module was compiled against one
//    more native than the harness has; binding it fails naming that native
//    and its entry is never called.
//
// Links bronze::runtime_shared and NOTHING else from bronze — the same "only"
// tests/shared_load's harness explains.

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
#include "natives.h"

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
    std::fprintf(stderr, "native harness: %s\n", what.c_str());
    std::exit(1);
}

void* requireSymbol(LibHandle lib, const char* name) {
    void* sym = findSymbol(lib, name);
    if (sym == nullptr) die(std::string("module does not export ") + name);
    return sym;
}

// The four symbols of the loadable-module contract.
struct LoadedModule {
    embed::ModuleEntry entry{nullptr};
    uint32_t fingerprint{0};
    std::vector<std::string> hostGlobals;
    const void* imports{nullptr};
    std::vector<std::string> importNames;  // "<kind> <path>" per slot
    std::vector<std::string> importSignatures;
};

std::vector<std::string> readHostGlobals(const void* raw) {
    const auto* bytes = static_cast<const unsigned char*>(raw);
    uint32_t count = 0;
    std::memcpy(&count, bytes, sizeof(count));
    const char* cursor = reinterpret_cast<const char*>(bytes + sizeof(count));
    std::vector<std::string> names;
    for (uint32_t i = 0; i < count; ++i) {
        names.emplace_back(cursor);
        cursor += names.back().size() + 1;
    }
    return names;
}

// `{ u32 count; u32 namesOffset; u64 slots[count]; names }` with the names
// as count × ("name\0" "signature\0") — bronze_abi.h's loadable-module
// section. A memcpy and a walk, like the host-globals blob.
void readImports(LoadedModule& module) {
    const auto* bytes = static_cast<const unsigned char*>(module.imports);
    uint32_t count = 0;
    uint32_t namesOffset = 0;
    std::memcpy(&count, bytes, sizeof(count));
    std::memcpy(&namesOffset, bytes + 4, sizeof(namesOffset));
    const char* cursor = reinterpret_cast<const char*>(bytes + namesOffset);
    for (uint32_t i = 0; i < count; ++i) {
        module.importNames.emplace_back(cursor);
        cursor += module.importNames.back().size() + 1;
        module.importSignatures.emplace_back(cursor);
        cursor += module.importSignatures.back().size() + 1;
    }
}

LoadedModule loadModule(const char* path, const char* entryName) {
    LibHandle lib = openLibrary(path);
    if (lib == nullptr) die(std::string("cannot open ") + path);
    const std::string entry(entryName);
    LoadedModule module;
    module.entry = reinterpret_cast<embed::ModuleEntry>(requireSymbol(lib, entryName));
    module.fingerprint =
        *static_cast<const uint32_t*>(requireSymbol(lib, (entry + "_abi_fingerprint").c_str()));
    module.hostGlobals = readHostGlobals(requireSymbol(lib, (entry + "_host_globals").c_str()));
    module.imports = requireSymbol(lib, (entry + "_native_imports").c_str());
    readImports(module);
    return module;
}

Value globalObject() {
    const uint32_t key = bronze_register_key_string("globalThis");
    return Value(bronze_global_get(key, nullptr));
}

// `globalThis[name](args...)`, with the arguments re-rooted across the
// lookup (which allocates) — tests/shared_load/harness.cpp says why.
Value callGlobal(const char* name, std::span<const Value> args) {
    std::vector<embed::Persistent> rooted;
    for (const Value& arg : args) rooted.emplace_back(arg);
    Value fn = embed::getProperty(globalObject(), name);
    std::vector<Value> current;
    for (const embed::Persistent& arg : rooted) current.push_back(arg.get());
    embed::CallResult r = embed::call(fn, embed::undefined(), current);
    if (r.thrown) die(std::string("call to ") + name + " threw");
    return r.value;
}

void line(const std::string& text) {
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (const std::string& p : parts) {
        if (!out.empty()) out += sep;
        out += p;
    }
    return out;
}

std::string num(double d) { return std::to_string(static_cast<long long>(d)); }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) die("usage: harness <module> <module-with-missing-native>");
    embed::setupIo();

    const LoadedModule module = loadModule(argv[1], "bronze_native_demo");

    // Before anything runs or is registered: what the module needs.
    line("host globals: " + join(module.hostGlobals, " "));
    std::vector<std::string> importLines;
    for (size_t i = 0; i < module.importNames.size(); ++i) {
        importLines.push_back(module.importNames[i] +
                              (module.importSignatures[i].empty() ? "" : " " + module.importSignatures[i]));
    }
    line("imports: " + join(importLines, " | "));

    if (module.fingerprint != embed::abiFingerprint()) {
        die("module ABI fingerprint does not match the runtime's");
    }
    line("abi: module stamp matches the runtime");

    // The host's side: the natives, then the plain globals, then the bind.
    std::string err;
    if (!nt_natives::registerAll(err)) die(err);
    for (const std::string& name : module.hostGlobals) {
        if (name == "hostTick") {
            embed::registerGlobal(name, embed::fromDouble(7.0));
        } else if (name == "nt") {
            embed::registerGlobal(name, embed::createObject());
        } else {
            die("module needs a host global this host does not provide: " + name);
        }
    }
    std::vector<std::string> missing;
    if (!embed::bindNativeImports(module.imports, &missing)) {
        die("bind refused: " + join(missing, "; "));
    }
    line("bound: " + std::to_string(module.importNames.size()) + " imports");

    embed::runEntry(module.entry);

    // ---- the module's effects, seen from the C side ------------------------

    line("host: scale=" + num(nt_natives::g_timeScale));
    line("host: agents made=" + std::to_string(nt_natives::g_agentsMade));

    // ---- a host global's cache cell follows re-registration ----------------

    line("host: readTick=" + num(embed::toDouble(callGlobal("readTick", {}))));
    embed::registerGlobal("hostTick", embed::fromDouble(8.0));
    line("host: readTick after re-register=" + num(embed::toDouble(callGlobal("readTick", {}))));

    // ---- destructors the module's garbage owes, run by the host's collection

    {
        const int freedBefore = nt_natives::g_agentsFreed;
        const Value five = embed::fromDouble(5.0);
        callGlobal("makeAgents", {&five, 1});
        embed::collectGarbage();
        embed::collectGarbage();
        embed::drainFinalizers();
        line("host: agents freed=" + std::to_string(nt_natives::g_agentsFreed - freedBefore) +
             " kept hp=" + num(embed::toDouble(callGlobal("agentHp", {}))));
    }

    // ---- transferred blocks the module dropped, released by the host's collection

    {
        const int releasedBefore = nt_natives::g_bufReleased;
        const Value four = embed::fromDouble(4.0);
        callGlobal("makeBuffers", {&four, 1});
        embed::collectGarbage();
        embed::collectGarbage();
        embed::drainFinalizers();
        line("host: buffers released=" + std::to_string(nt_natives::g_bufReleased - releasedBefore) +
             " kept length=" + num(embed::toDouble(callGlobal("ownedLength", {}))));
    }

    // ---- a typed array the HOST built, into a native the MODULE calls ------

    {
        embed::Persistent floats{embed::createTypedArray(embed::elements::Float32, 3)};
        const float source[3] = {1.0f, 2.0f, 3.5f};
        unsigned char raw[sizeof(source)];
        std::memcpy(raw, source, sizeof(source));
        if (!embed::fillTypedArray(floats.get(), std::span<const uint8_t>(raw, sizeof(raw)))) {
            die("fillTypedArray refused a view it should have filled");
        }
        embed::collectGarbage();
        const Value arg = floats.get();
        const double sum = embed::toDouble(callGlobal("sumHost", {&arg, 1}));
        line("host: sumHost=" + std::to_string(sum).substr(0, 3));
    }

    // ---- the refusal -------------------------------------------------------

    const LoadedModule missingModule = loadModule(argv[2], "bronze_native_missing");
    line("imports of second module: " + join(missingModule.importNames, " | "));
    missing.clear();
    if (embed::bindNativeImports(missingModule.imports, &missing)) {
        die("the second module bound, but it names a native nothing registered");
    }
    for (const std::string& m : missing) line("refused: " + m);
    line("refused: second module not entered");
    return 0;
}
