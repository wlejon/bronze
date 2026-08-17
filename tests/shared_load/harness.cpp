// A host that LOADS a compiled bronze module instead of linking one.
//
// This is the shape bro needs and the shape nothing in the tree exercised
// before: the module is a DLL/.so/.dylib opened at run time, the runtime is a
// shared library the host and the module both bind to, and everything the host
// knows about the module it learned from three exported symbols.
//
// What each piece proves:
//
//  * ONE RUNTIME, ONE HEAP. The harness links the runtime ONLY through the
//    shared library — no static archive is anywhere on its link line — and the
//    module resolves its ABI calls to that same library. Every line below that
//    reads back something the module built, or hands the module something the
//    host built, is an assertion about that: two heaps would agree on nothing
//    once either had collected.
//
//  * THE THREE SYMBOLS. The entry, the ABI stamp and the host-globals manifest
//    (bronze_abi.h's loadable-module section). The manifest is read and printed
//    before the module runs, which is the point of it existing: a loader diffs
//    it against what it has registered while refusing is still possible.
//
//  * THE STAMP IS A GATE, NOT A LABEL. The second module the harness opens is
//    a fake whose stamp is wrong, and whose entry prints if it is ever called.
//    The pinned output is what says it was not.
//
//  * SURVIVAL ACROSS A HOST-TRIGGERED COLLECTION. The host collects between
//    every read-back, so a value the module allocated is answered from wherever
//    the collector moved it, through the host's own Persistent roots.
//
// Deliberately NOT embed::runMain(): that names `bronze_main` at link time and
// is not even in the shared runtime (cmake/bronze_shared_runtime.cmake says
// why). embed::runEntry takes the pointer the loader resolved.

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

// ---- the loader ------------------------------------------------------------

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

// Every failure here is fatal and named. A loader that cannot find a symbol has
// not been handed a bronze module, and carrying on to call whatever it did find
// is the class of thing this whole test exists to rule out.
[[noreturn]] void die(const std::string& what) {
    std::fflush(stdout);
    std::fprintf(stderr, "shared-load harness: %s\n", what.c_str());
    std::exit(1);
}

void* requireSymbol(LibHandle lib, const char* name) {
    void* sym = findSymbol(lib, name);
    if (sym == nullptr) die(std::string("module does not export ") + name);
    return sym;
}

// The three symbols of the loadable-module contract, resolved off one library.
struct LoadedModule {
    embed::ModuleEntry entry{nullptr};
    uint32_t fingerprint{0};
    std::vector<std::string> hostGlobals;
};

// `{ uint32_t count; char names[]; }` — count first, then `count`
// NUL-terminated UTF-8 names back to back. Parsed with a memcpy and a walk,
// which is the whole of what bronze_abi.h promises a loader has to do.
std::vector<std::string> readManifest(const void* raw) {
    const auto* bytes = static_cast<const unsigned char*>(raw);
    uint32_t count = 0;
    std::memcpy(&count, bytes, sizeof(count));
    const char* cursor = reinterpret_cast<const char*>(bytes + sizeof(count));
    std::vector<std::string> names;
    names.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        names.emplace_back(cursor);
        cursor += names.back().size() + 1;
    }
    return names;
}

LoadedModule loadModule(const char* path, const char* entryName) {
    LibHandle lib = openLibrary(path);
    if (lib == nullptr) die(std::string("cannot open ") + path);

    LoadedModule module;
    module.entry =
        reinterpret_cast<embed::ModuleEntry>(requireSymbol(lib, entryName));
    module.fingerprint = *static_cast<const uint32_t*>(
        requireSymbol(lib, (std::string(entryName) + "_abi_fingerprint").c_str()));
    module.hostGlobals =
        readManifest(requireSymbol(lib, (std::string(entryName) + "_host_globals").c_str()));
    return module;
}

// ---- reaching the module's globals -----------------------------------------

// The global object, through the two calls generated code makes: intern the
// name, then ask for the global. The harness is not a module and has no cache
// table of its own, so the cell is null.
Value globalObject() {
    const uint32_t key = bronze_register_key_string("globalThis");
    return Value(bronze_global_get(key, nullptr));
}

// Call `globalThis[name](args...)`.
//
// The arguments are re-rooted on the way in and re-read on the way out, and
// that is not ceremony: looking the function up ALLOCATES (the key string),
// and an allocation moves every heap value in sight. Passing a plain `Value`
// straight through to `call` is the mistake embed.h's GC contract is written
// against, and it does not fail loudly — under BRONZE_GC_STRESS this harness
// passed the string "record" in place of "alpha", because that is what the
// collector had put at the address the caller was still holding.
Value callGlobal(const char* name, std::span<const Value> args) {
    std::vector<embed::Persistent> rooted;
    rooted.reserve(args.size());
    for (const Value& arg : args) rooted.emplace_back(arg);

    Value fn = embed::getProperty(globalObject(), name);

    std::vector<Value> current;
    current.reserve(rooted.size());
    for (const embed::Persistent& arg : rooted) current.push_back(arg.get());

    embed::CallResult r = embed::call(fn, embed::undefined(), current);
    if (r.thrown) die(std::string("call to ") + name + " threw");
    return r.value;
}

void line(const std::string& text) {
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& p : parts) {
        if (!out.empty()) out += " ";
        out += p;
    }
    return out;
}

// The elements of an Int32Array, read through the raw view the embed API
// answers. Every use below re-reads it AFTER whatever collection it is about,
// because the pointer dies at the next allocation — embed.h's contract, and
// the reason this is a function rather than a cached pointer.
std::string int32Elements(Value view) {
    embed::TypedArrayInfo info = embed::typedArrayInfo(view);
    if (!info) die("expected a typed array");
    std::string out;
    for (uint32_t i = 0; i < info.elementCount; ++i) {
        int32_t element = 0;
        std::memcpy(&element, info.data + i * sizeof(int32_t), sizeof(int32_t));
        if (!out.empty()) out += " ";
        out += std::to_string(element);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) die("usage: harness <module> <fake-module>");
    embed::setupIo();

    const char* const kEntry = "bronze_shared_demo";
    const LoadedModule module = loadModule(argv[1], kEntry);

    // Before anything runs: what does this module need from its host? The
    // answer comes out of the module, not out of the build that produced it.
    line("manifest: " + join(module.hostGlobals));

    // And then the gate. The runtime is asked for its own fingerprint rather
    // than the harness using the constant it was compiled with: a host that
    // checked a module against its own build would be checking nothing.
    if (module.fingerprint != embed::abiFingerprint()) {
        die("module ABI fingerprint does not match the runtime's");
    }
    line("abi: module stamp matches the runtime");

    // The host's side of the manifest, registered before the module's top
    // level runs — which is when it reads them.
    //
    // DRIVEN BY the manifest rather than merely agreeing with it: the harness
    // walks what the module asked for and provides each name, and a name it
    // has nothing for is a refusal that names it. That is the whole use of the
    // symbol — a host that registered its globals from its own hardcoded list
    // would learn a name was missing when the program read it, which is
    // somewhere in the middle of the module's top level and far too late.
    for (const std::string& name : module.hostGlobals) {
        if (name == "hostLabel") {
            embed::registerGlobal(name, embed::fromUtf8("harness"));
        } else if (name == "hostTick") {
            embed::registerGlobal(name, embed::fromDouble(7.0));
        } else {
            die("module needs a host global this host does not provide: " + name);
        }
    }

    embed::runEntry(module.entry);

    // ---- the module's state, read back across collections ------------------

    const Value alpha = embed::fromUtf8("alpha");
    const double first = embed::toDouble(callGlobal("record", {&alpha, 1}));
    embed::collectGarbage();
    const Value beta = embed::fromUtf8("beta");
    const double second = embed::toDouble(callGlobal("record", {&beta, 1}));
    line("host: record=" + std::to_string(static_cast<int>(first)) + "," +
         std::to_string(static_cast<int>(second)));

    embed::collectGarbage();
    line("host: readItems=" + embed::toUtf8(callGlobal("readItems", {})));

    // ---- a typed array the HOST built, filled element by element -----------

    embed::Persistent bytes{embed::createTypedArray(embed::elements::Uint8, 3)};
    for (uint32_t i = 0; i < 3; ++i) {
        bytes.set(embed::setElement(bytes.get(), i, embed::fromDouble(4.0 + i)));
    }
    embed::collectGarbage();
    {
        const Value arg = bytes.get();
        line("host: sumBytes(uint8)=" +
             std::to_string(static_cast<int>(embed::toDouble(callGlobal("sumBytes", {&arg, 1})))));
    }

    // ---- and one filled from a span of host bytes --------------------------

    embed::Persistent floats{embed::createTypedArray(embed::elements::Float32, 4)};
    {
        const float source[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        unsigned char raw[sizeof(source)];
        std::memcpy(raw, source, sizeof(source));
        if (!embed::fillTypedArray(floats.get(), std::span<const uint8_t>(raw, sizeof(raw)))) {
            die("fillTypedArray refused a view it should have filled");
        }
    }
    embed::collectGarbage();
    {
        const Value arg = floats.get();
        line("host: sumBytes(float32)=" +
             std::to_string(static_cast<int>(embed::toDouble(callGlobal("sumBytes", {&arg, 1})))));
    }
    embed::collectGarbage();
    line("host: lastSum=" +
         std::to_string(static_cast<int>(embed::toDouble(callGlobal("lastSum", {})))));

    // ---- a typed array the MODULE built, read by the host ------------------

    const Value four = embed::fromDouble(4.0);
    embed::Persistent steps{callGlobal("makeSteps", {&four, 1})};
    line("host: steps=" + int32Elements(steps.get()));
    embed::collectGarbage();
    embed::collectGarbage();
    line("host: steps after gc=" + int32Elements(steps.get()));

    // ---- the refusal -------------------------------------------------------
    //
    // The same three symbols off a library that lies about one of them. The
    // fake's entry prints when it is called, so the absence of its line from
    // the pinned output is the assertion that it was not.
    const LoadedModule fake = loadModule(argv[2], kEntry);
    if (fake.fingerprint == embed::abiFingerprint()) {
        die("the fake module's fingerprint matches the runtime's; it cannot test a refusal");
    }
    line("refused: fake module stamp does not match the runtime, entry not called");

    return 0;
}
