// Scripts compiled ahead of time to loadable modules (eval.h, "Ahead-of-time
// modules"): built through the evaluator's own front end, written by brass's
// image writer, checked against their record before they run.

#include "eval/eval.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <brass/gc/native_frames.hpp>
#include <brass/interpreter/interpreter.hpp>
#include <brass/runtime/exception.hpp>

#include "abi/bronze_abi.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/module_image.h"
#include "embed/embed.h"
#include "embed/embed_native.h"
#include "eval/code_cache.h"
#include "eval/eval_internal.h"
#include "modules/modules.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/sampler.h"
#include "support/diagnostics.h"
#include "support/source.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace bronze::eval {

namespace {

namespace fs = std::filesystem;

// The entry every module is built under. Not the default `bronze_main`, whose
// stamp keeps a historical spelling (bronze_object_abi_fingerprint): a module
// here is one of several units a host loads, and each is looked up in its own
// image, so one name serves them all.
constexpr const char* kEntrySymbol = "bronze_unit";

// The text a failed compile renders, for a host's log.
std::string rendered(DiagnosticSink& diags, const SourceSet& sources, const char* fallback) {
    std::string msg = diags.render(sources);
    return msg.empty() ? std::string(fallback) : msg;
}

// The name a stored program keeps its trailing expression's value under: a
// function of the key, as a cached entry's is, so a rebuild with the same
// inputs writes the same module.
std::string resNameFor(const cache::Digest& key) { return "__bronze_eval_res_m" + key.hex().substr(0, 16); }

// The two platform primitives, and nothing else.
void* openImage(const fs::path& path, std::string& error) {
#ifdef _WIN32
    // The module's own directory first for anything it loads, which needs an
    // absolute path (a relative one silently drops the flag).
    std::error_code ec;
    const fs::path abs = fs::absolute(path, ec);
    HMODULE h = ::LoadLibraryExW((ec ? path : abs).wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        error = "Windows error " + std::to_string(static_cast<unsigned long>(::GetLastError()));
        return nullptr;
    }
    return reinterpret_cast<void*>(h);
#else
    // RTLD_LOCAL: the module's symbols must not join the global namespace and
    // capture calls the host makes into the runtime.
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* msg = ::dlerror();
        error = msg ? msg : "dlopen failed";
    }
    return h;
#endif
}

void* imageSymbol(void* image, const std::string& name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(image), name.c_str()));
#else
    return ::dlsym(image, name.c_str());
#endif
}

// Only for an image none of whose code has run: nothing can hold a pointer
// into it yet.
void closeImage(void* image) {
#ifdef _WIN32
    ::FreeLibrary(reinterpret_cast<HMODULE>(image));
#else
    ::dlclose(image);
#endif
}

// The entry called for its value, as the tiered engine's run() returns one: a
// module with top-level await answers its promise. A throw an interpreted
// frame made (code the module reached through eval) arrives as brass's
// interpreter exception and continues as the runtime's one exception kind.
uint64_t callEntry(void* entry) {
    uint64_t thrown = 0;
    try {
        brass::GeneratedCodeEntryScope scope;
        return reinterpret_cast<uint64_t (*)()>(entry)();
    } catch (const brass::InterpreterThrownException& e) {
        thrown = e.value().raw_bits();
    }
    throw brass::runtime::BrassException(brass::HostValue::from_raw(thrown));
}

}  // namespace

ModuleBuildResult buildModule(std::string_view source, const EvalOptions& options, const std::string& modulePath,
                              const std::string& recordPath) {
    ModuleBuildResult out;
    SourceSet sources;
    DiagnosticSink diags;
    detail::CompileInputs in;
    if (!detail::gatherInputs(options, in, diags)) {
        out.errorMessage = rendered(diags, sources, "the compile's inputs could not be read");
        return out;
    }
    modules::ModuleOptions modOpts = detail::moduleOptionsFor(options);
    modules::DependencyLog deps;
    modOpts.dependencyLog = &deps;
    const cache::Digest key = detail::programKey(cache::moduleKeyMaterial(), options, modOpts, in, source);
    const std::string resName = resNameFor(key);

    auto astModule = modules::loadProgramSource(std::string(source), options.filename, sources, diags, modOpts);
    if (diags.hasErrors() || !astModule) {
        out.errorMessage = rendered(diags, sources, "the module graph could not be loaded");
        return out;
    }
    std::unique_ptr<il::Module> ilModule = detail::lowerProgram(*astModule, options, in, resName, diags, sources);
    if (!ilModule) {
        out.errorMessage = rendered(diags, sources, "lowering failed");
        return out;
    }
    if (!options.retainSource) ilModule->sourceTexts.clear();

    BrassBackend backend;
    backend.setEntrySymbol(kEntrySymbol);
    backend.setHostGlobals(in.hostGlobals);
    backend.setEmitDebugInfo(options.emitDebugInfo);
    std::optional<brass::object::ObjectFile> obj = backend.buildObjectFile(*ilModule, diags);
    if (!obj) {
        out.errorMessage = rendered(diags, sources, "code generation failed");
        return out;
    }
    // No search path but the module's own directory: a module is only ever
    // mapped into a process already running the shared runtime, whose loaded
    // copy satisfies the import by name, and a build machine's path written
    // into the image would mean nothing on another.
    if (!codegen::writeModuleImage(*obj, modulePath, diags, kEntrySymbol, {}, {})) {
        out.errorMessage = rendered(diags, sources, "the module could not be written");
        return out;
    }

    cache::ModuleRecord record{resName, kEntrySymbol, fs::path(modulePath).filename().string()};
    std::string err;
    if (!cache::writeModuleRecord(fs::path(recordPath), key, record, sources, deps, err)) {
        out.errorMessage = err;
        return out;
    }
    out.filesRead = deps.filesRead;
    out.success = true;
    return out;
}

LoadedModule loadModule(std::string_view source, const EvalOptions& options, const std::string& recordPath) {
    LoadedModule out;
    DiagnosticSink diags;
    detail::CompileInputs in;
    if (!detail::gatherInputs(options, in, diags)) {
        out.why = rendered(diags, SourceSet{}, "the compile's inputs could not be read");
        return out;
    }
    const modules::ModuleOptions modOpts = detail::moduleOptionsFor(options);
    const cache::Digest key = detail::programKey(cache::moduleKeyMaterial(), options, modOpts, in, source);
    cache::ModuleRecord record;
    if (!cache::readModuleRecord(fs::path(recordPath), key, source, modOpts.moduleRoots, record, out.why)) {
        return out;
    }

    const fs::path modulePath = fs::path(recordPath).parent_path() / record.moduleFile;
    out.modulePath = modulePath.string();
    std::string error;
    void* image = openImage(modulePath, error);
    if (!image) {
        out.why = "the module could not be loaded (" + error + ")";
        return out;
    }
    // The stamp before anything else is looked up: nothing from an image of
    // unknown vintage runs, and the record's own ABI check is about the file
    // beside it, which may have been replaced.
    const auto* stamp = static_cast<const uint32_t*>(imageSymbol(image, record.entrySymbol + "_abi_fingerprint"));
    if (!stamp || *stamp != BRONZE_ABI_FINGERPRINT) {
        out.why = stamp ? "the module was compiled against another bronze ABI" : "the module carries no ABI stamp";
        closeImage(image);
        return out;
    }
    void* entry = imageSymbol(image, record.entrySymbol);
    if (!entry) {
        out.why = "the module exports no " + record.entrySymbol;
        closeImage(image);
        return out;
    }
    // The import table bound here, from the registry of the thread that will
    // run it, so a native missing from this host is a reason to compile
    // instead — not the FATAL the entry's own first instruction would raise.
    if (const void* table = imageSymbol(image, record.entrySymbol + "_native_imports")) {
        std::vector<std::string> missing;
        if (!embed::bindNativeImports(table, &missing)) {
            out.why = "the host registers no native for";
            for (const auto& m : missing) out.why += " " + m;
            closeImage(image);
            return out;
        }
    }
    out.resName = record.resName;
    out.entry = entry;
    out.ok = true;
    return out;
}

embed::CallResult runModule(const LoadedModule& module, const EvalOptions& options) {
    bronze::ShadowStackFrame rootFrame;
    if (!module.ok || !module.entry) {
        Value err;
        const std::string msg = "module not loaded: " + (module.why.empty() ? std::string("no entry") : module.why);
        runtime::rtTryCatch([&] { runtime::rtThrowSyntaxError(msg); }, err);
        return embed::CallResult{err, /*thrown=*/true};
    }
    // The thread that runs a module's entry runs its compiled code from then
    // on: what the sampling profiler needs to know (embed's runEntry says so too).
    runtime::samplerNoteJsThread();

    uint64_t epoch = 0;
    if (options.moduleHandleOut) {
        epoch = embed::beginModuleLoad();
        *options.moduleHandleOut = epoch;
    }
    // Held across the drain, which may collect: a module with top-level await
    // returns its promise, and the drain runs the rest of that module.
    Value entryOut = Value::fromUndefined();
    bool threw = false;
    {
        bronze::ShadowStackFrame stackFrame;
        threw = runtime::rtTryCatch([&] { entryOut = Value(callEntry(module.entry)); }, entryOut);
    }
    embed::Persistent entryVal{entryOut};
    embed::drainMicrotasks();
    if (options.moduleHandleOut) embed::endModuleLoad(epoch);

    if (threw) return embed::CallResult{entryVal.get(), /*thrown=*/true};
    if (entryVal.get().isObject() && embed::isPromise(entryVal.get())) {
        return embed::CallResult{entryVal.get(), /*thrown=*/false};
    }
    embed::GlobalValue g = embed::globalValue(module.resName);
    embed::Persistent result{g.found ? g.value : embed::undefined()};
    embed::GlobalValue glob = embed::globalValue("globalThis");
    if (glob.found) embed::deleteProperty(glob.value, module.resName);
    return embed::CallResult{result.get(), /*thrown=*/false};
}

}  // namespace bronze::eval
