#pragma once

#include <cstdint>
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
    // retained either way (retainJitProgram): closures hold raw code pointers
    // into it, and the unload contract keeps the image mapped.
    embed::ModuleHandle* moduleHandleOut = nullptr;
    // false compiles in the baseline tier (BrassBackend::setOptimize): the
    // whole optimizer skipped, so a program is running a few hundred
    // milliseconds after the source changed instead of seconds. Same
    // semantics, slower code — the tier for a host's edit-and-reload loop,
    // not for the build it ships.
    bool optimize = true;
    bool emitDebugInfo = false;
    bool moduleRegistry = false;
    // With moduleRegistry: publish the entry file itself too
    // (modules::ModuleOptions::publishEntry), for a host whose entry is a
    // module FILE another unit may import — a page's `<script type="module"
    // src>`. Never for a driver script or inline script text.
    bool publishEntry = false;
    std::vector<std::string> externalModules = {};
    std::optional<ExecutionTier> tier = std::nullopt;
};

struct CompiledScript {
    std::unique_ptr<BrassJitProgram> jitProgram;
    std::unique_ptr<BrassTieredProgram> tieredProgram;
    ExecutionTier tier = ExecutionTier::Tier2_Optimized;
    std::string resName;
    std::string errorMessage;
    bool success = false;
};

// Retains a JIT compiled program in memory for the process lifetime so its machine
// code, data sections, and function pointers remain valid across executions.
BRONZE_EMBED_API void retainJitProgram(std::unique_ptr<BrassJitProgram> program);

// Retains a tiered program (interpreter, baseline, or multi-tier) in memory.
BRONZE_EMBED_API void retainTieredProgram(std::unique_ptr<BrassTieredProgram> program);

// Clears all retained JIT compiled programs.
BRONZE_EMBED_API void clearRetainedJitPrograms();

// Compiles a script to JIT machine code. Safe to invoke on background worker threads.
BRONZE_EMBED_API std::unique_ptr<CompiledScript> compileScript(std::string_view source, const EvalOptions& options = {});

// Compiles a file to JIT machine code. Safe to invoke on background worker threads.
BRONZE_EMBED_API std::unique_ptr<CompiledScript> compileFile(const std::string& filePath, const EvalOptions& options = {});

// Executes a previously compiled script on the mutator thread and returns the result.
BRONZE_EMBED_API embed::CallResult runCompiledScript(std::unique_ptr<CompiledScript> script, const EvalOptions& options = {});

// Evaluates a script in memory using the Brass JIT and returns the result as a CallResult.
// If code execution throws an exception, CallResult::thrown is true and value is the thrown error.
BRONZE_EMBED_API embed::CallResult evalScript(std::string_view source, const EvalOptions& options = {});

// Evaluates a file and its transitive module graph directly in memory via Brass JIT.
BRONZE_EMBED_API embed::CallResult evalFile(const std::string& filePath, const EvalOptions& options = {});

// Evaluates a script in memory and returns the Value directly.
// If an exception was thrown, leaves the exception pending in rtTls()->exception_cell
// and returns Value::fromUndefined().
BRONZE_EMBED_API Value evalScriptDirect(std::string_view source, const EvalOptions& options = {});

// Compiles and returns a dynamic function object of the requested kind (Ordinary, Generator,
// Async, AsyncGenerator).
// Takes arguments matching the Function constructor (parameters followed by body).
// If compilation fails, raises a SyntaxError into the runtime and returns Value::fromUndefined().
BRONZE_EMBED_API Value evalFunction(runtime::DynamicFunctionKind kind, std::span<const Value> args);

// Helper overload taking params and body as strings.
BRONZE_EMBED_API Value evalFunction(std::span<const std::string> params, std::string_view body,
                                    runtime::DynamicFunctionKind kind = runtime::DynamicFunctionKind::Ordinary);

// Installs Bronze's native in-memory JIT evaluator as the dynamic eval and function hooks.
BRONZE_EMBED_API void installDefaultDynamicHooks();

}  // namespace bronze::eval
