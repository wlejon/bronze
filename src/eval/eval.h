#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codegen-brass/brass_jit.h"
#include "embed/embed.h"
#include "runtime/host_globals.h"
#include "runtime/value.h"

namespace bronze::eval {

struct EvalOptions {
    std::string filename = "<eval>";
    std::vector<std::string> hostGlobals = {};
    bool retainSource = true;
};

// Retains a JIT compiled program in memory for the process lifetime so its machine
// code, data sections, and function pointers remain valid across executions.
void retainJitProgram(std::unique_ptr<BrassJitProgram> program);

// Evaluates a script in memory using the Brass JIT and returns the result as a CallResult.
// If code execution throws an exception, CallResult::thrown is true and value is the thrown error.
embed::CallResult evalScript(std::string_view source, const EvalOptions& options = {});

// Evaluates a script in memory and returns the Value directly.
// If an exception was thrown, leaves the exception pending in rtTls()->exception_cell
// and returns Value::fromUndefined().
Value evalScriptDirect(std::string_view source, const EvalOptions& options = {});

// Compiles and returns a dynamic function object of the requested kind (Ordinary, Generator,
// Async, AsyncGenerator).
// Takes arguments matching the Function constructor (parameters followed by body).
// If compilation fails, raises a SyntaxError into the runtime and returns Value::fromUndefined().
Value evalFunction(runtime::DynamicFunctionKind kind, std::span<const Value> args);

// Helper overload taking params and body as strings.
Value evalFunction(std::span<const std::string> params, std::string_view body,
                   runtime::DynamicFunctionKind kind = runtime::DynamicFunctionKind::Ordinary);

// Installs Bronze's native in-memory JIT evaluator as the dynamic eval and function hooks.
void installDefaultDynamicHooks();

}  // namespace bronze::eval
