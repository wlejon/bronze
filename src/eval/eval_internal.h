#pragma once

// The evaluator's front end, shared by its two back ends: the tiered engine
// (eval.cpp, compileScript) and the ahead-of-time module writer
// (aot_module.cpp, buildModule). One front end is the point: a module built
// ahead of time must be the program compileScript would have run, so both
// read their inputs, key them and lower them through exactly these functions.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ast/ast.h"
#include "eval/code_cache.h"
#include "eval/eval.h"
#include "il/il.h"
#include "lower/native_manifest.h"
#include "modules/modules.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/pins.h"

namespace bronze::eval::detail {

// BRONZE_TIMINGS=1: an embedded compile prints its phase times to stderr the
// way `bronze build --timings` does (support/timings.h).
bool evalTimingsEnabled();

void reportProgress(const EvalOptions& options, const char* phase, double fraction);

// What a compile reads besides its sources, gathered once: the key and the
// front end both read it.
struct CompileInputs {
    std::vector<std::string> hostGlobals;
    std::optional<lower::NativeManifest> nativeManifest;
    std::string manifestJson;  // "" when the program is compiled against no natives
    bool deferNativeBind = false;
    std::string pinsText;
    types::PinManifest pins;
    ExecutionTier tier = ExecutionTier::Auto;
};

bool gatherInputs(const EvalOptions& options, CompileInputs& in, DiagnosticSink& diags);

// The module options a compile loads its graph with, the realm's registry
// applied (EvalOptions::moduleRegistry).
modules::ModuleOptions moduleOptionsFor(const EvalOptions& options);

// Everything a compile reads, hashed under `processMaterial` (the code
// cache's processKeyMaterial, or a module record's moduleKeyMaterial): two
// compiles with the same key build the same program.
cache::Digest programKey(std::string_view processMaterial, const EvalOptions& options,
                         const modules::ModuleOptions& modOpts, const CompileInputs& in,
                         std::string_view entryText);

// The front end after the graph is loaded: the eval transform (a trailing
// expression's value kept under `resName`, top-level declarations made
// global), inference and lowering. Null with `diags` holding why.
std::unique_ptr<il::Module> lowerProgram(ast::Module& astModule, const EvalOptions& options,
                                         const CompileInputs& in, const std::string& resName,
                                         DiagnosticSink& diags, SourceSet& sources);

}  // namespace bronze::eval::detail
