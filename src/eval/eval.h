#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <filesystem>
#include "codegen-brass/brass_jit.h"
#include "codegen-brass/brass_tiered_engine.h"
#include "embed/embed.h"
#include "modules/modules.h"
#include "runtime/host_globals.h"
#include "runtime/value.h"

namespace bronze::eval {

// Where a compile is, for a host showing progress while it runs elsewhere.
// `phase` is one of "load" (the module graph read and parsed), "cache" (a
// stored program found valid), "infer", "lower", "codegen" and "done";
// `fraction` rises from 0 to 1 and is an estimate weighted by what each phase
// typically costs, not a measurement.
struct CompileProgress {
    const char* phase = "";
    double fraction = 0.0;
};

// Whether a compile used the on-disk code cache (src/eval/code_cache.h).
enum class CodeCacheStatus : uint8_t {
    Off,     // no cache directory, or nothing cacheable
    Hit,     // the stored IL was valid and the front end was skipped
    Miss,    // compiled from source (no entry, or a stale or damaged one)
};

struct EvalOptions {
    std::string filename = "<eval>";
    std::vector<std::string> hostGlobals = {};
    bool retainSource = true;
    std::vector<modules::ModuleRoot> moduleRoots = {};
    std::filesystem::path entryResolvesAs = {};
    std::string pinsPath = {};
    std::string censusOutPath = {};
    // No native-manifest option: the evaluator reads the natives this thread's
    // host registered (embed::registerNative) directly, and binds the program
    // to them before it runs.
    //
    // When set, the program's entry runs inside a beginModuleLoad/endModuleLoad
    // bracket (embed.h) and the handle is written here, so the host can later
    // unloadModule it — the root spans the program registered stop being roots
    // and its heap graph can die. This is what a host that swaps a whole
    // program for a newer version of itself passes; an eval() or new Function()
    // from inside a running program passes nothing and its registrations join
    // whatever bracket is current, exactly as before. The machine code is
    // retained either way (retainProgram): closures hold raw code pointers
    // into it, and the unload contract keeps the image mapped.
    embed::ModuleHandle* moduleHandleOut = nullptr;
    bool emitDebugInfo = false;
    bool moduleRegistry = false;
    // With moduleRegistry: publish the entry file itself too
    // (modules::ModuleOptions::publishEntry), for a host whose entry is a
    // module FILE another unit may import — a page's `<script type="module"
    // src>`. Never for a driver script or inline script text.
    bool publishEntry = false;
    std::vector<std::string> externalModules = {};
    // How the program runs (codegen-brass/brass_tiered_engine.h). Unset is
    // the process default (setDefaultTier), which is also what every eval()
    // and new Function() the program makes runs at.
    std::optional<ExecutionTier> tier = std::nullopt;
    // Compile once for every thread that runs the same program. A tiered
    // program's writable module data is per thread (bronze_module_instance),
    // so one compiled program can run on any number of threads, each run
    // starting from fresh globals and caches, and sharing the code each
    // tier-up and OSR compile installs. With this set, a program compiled
    // with the same sources and options as one compiled before (by any
    // thread, with this set) is that program, unless this thread already ran
    // it: then it is compiled again, as a second evaluation on one thread
    // must start from fresh module data. For a host that runs one script on
    // many threads, as workers do. Explicit Tier2_Optimized programs are
    // never shared.
    bool shareAcrossThreads = false;
    // With `moduleRegistry`: `externalModules` is the registry's list as
    // captured (captureThreadInputs), even when empty, and the compile does
    // not ask the realm again — the compiling thread may not be the realm's.
    bool externalModulesCaptured = false;
    // The natives manifest to compile against in place of this thread's
    // registry (embed::nativeManifestJson), captured on the thread that will
    // run the program; "" means none. The program's import table is then
    // bound by runCompiledScript, on the running thread, rather than by the
    // compile.
    std::optional<std::string> nativeManifestJson = std::nullopt;
    // The on-disk code cache (src/eval/code_cache.h): a directory where a
    // compile keeps its IL, keyed by everything the compile reads, so an
    // unchanged program skips parsing, inference and lowering next time.
    // Empty: no cache. Bounded to `codeCacheMaxBytes`, oldest entries first.
    std::string codeCacheDir = {};
    uint64_t codeCacheMaxBytes = uint64_t{256} << 20;
    // Called on the compiling thread at each phase boundary.
    std::function<void(const CompileProgress&)> onProgress = {};
};

struct CompiledScript {
    std::shared_ptr<BrassTieredProgram> program;
    ExecutionTier tier = ExecutionTier::Auto;
    std::string resName;
    std::string errorMessage;
    bool success = false;
    CodeCacheStatus cacheStatus = CodeCacheStatus::Off;
    // Why a cache lookup missed ("no entry", "source changed", ...), for a
    // host's log; empty on a hit or with no cache.
    std::string cacheNote;
    // The native import table runCompiledScript binds before the entry runs,
    // when the compile used a captured manifest; empty otherwise.
    std::string deferredNativeTable;
};

// Everything a compile reads from the CALLING thread's runtime state — its
// host globals, its registered natives, its realm's module registry — copied
// into `options`, so that compileScript/compileFile may then run on any
// thread and build the program the calling thread would have. The program is
// still run (runCompiledScript) on the calling thread.
BRONZE_EMBED_API void captureThreadInputs(EvalOptions& options);

// The tier a program compiles at when its options name none: Auto (the
// tiered pipeline: interpreted first, hot functions compiled to baseline and
// then optimized code in the background) unless the host says otherwise.
BRONZE_EMBED_API void setDefaultTier(ExecutionTier tier);
BRONZE_EMBED_API ExecutionTier defaultTier();

// Retains a compiled program for the process lifetime so its code, data
// image and function pointers remain valid across executions. At exit the
// retained programs' background compiles are stopped.
BRONZE_EMBED_API void retainProgram(std::shared_ptr<BrassTieredProgram> program);

// Drops every retained program's queued background compiles (tier-ups and
// OSR entries, which all run on brass's one process-wide compile pool) and
// waits out the ones running; later tier-ups queue again. For a host
// shutting its engine down while the process lives on. At exit the pool
// itself is shut down as well.
BRONZE_EMBED_API void stopBackgroundCompiles();

// Destroys every retained program.
BRONZE_EMBED_API void clearRetainedPrograms();

// Compiles a script. Safe to invoke on background worker threads; the
// program runs on whichever thread runs it (runCompiledScript).
BRONZE_EMBED_API std::unique_ptr<CompiledScript> compileScript(std::string_view source, const EvalOptions& options = {});

// Compiles a file and its module graph, as compileScript does a script.
BRONZE_EMBED_API std::unique_ptr<CompiledScript> compileFile(const std::string& filePath, const EvalOptions& options = {});

// Executes a previously compiled script on the mutator thread and returns the result.
BRONZE_EMBED_API embed::CallResult runCompiledScript(std::unique_ptr<CompiledScript> script, const EvalOptions& options = {});

// Evaluates a script in memory using the Brass JIT and returns the result as a CallResult.
// If code execution throws an exception, CallResult::thrown is true and value is the thrown error.
BRONZE_EMBED_API embed::CallResult evalScript(std::string_view source, const EvalOptions& options = {});

// Evaluates a file and its transitive module graph directly in memory via Brass JIT.
BRONZE_EMBED_API embed::CallResult evalFile(const std::string& filePath, const EvalOptions& options = {});

// Evaluates a script in memory and returns the Value directly.
// If the script throws, rethrows the thrown value into the caller (runtime/exception.h).
BRONZE_EMBED_API Value evalScriptDirect(std::string_view source, const EvalOptions& options = {});

// Compiles and returns a dynamic function object of the requested kind (Ordinary, Generator,
// Async, AsyncGenerator).
// Takes arguments matching the Function constructor (parameters followed by body).
// If compilation fails, throws a SyntaxError.
BRONZE_EMBED_API Value evalFunction(runtime::DynamicFunctionKind kind, std::span<const Value> args);

// Helper overload taking params and body as strings.
BRONZE_EMBED_API Value evalFunction(std::span<const std::string> params, std::string_view body,
                                    runtime::DynamicFunctionKind kind = runtime::DynamicFunctionKind::Ordinary);

// Installs Bronze's native in-memory JIT evaluator as the dynamic eval and function hooks.
BRONZE_EMBED_API void installDefaultDynamicHooks();

}  // namespace bronze::eval
