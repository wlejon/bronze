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
#include "opt/spec/profile.h"
#include "support/diagnostics.h"
#include "support/source.h"

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
    // EvalOptions::feedbackInPath's bytes ("" for none or a missing file),
    // keyed, and the profile or harvest they parse to (none when they do
    // not: a damaged file costs the specialization, not the build).
    std::string feedbackText;
    std::optional<spec::TypeProfile> feedback;
};

// `keyOnly`: the inputs a key hashes and no more (a module's load checks its
// record against them): the profile's bytes are read and not parsed, which
// for a large harvest was most of what the check cost on the page thread.
bool gatherInputs(const EvalOptions& options, CompileInputs& in, DiagnosticSink& diags, bool keyOnly = false);

// The module options a compile loads its graph with, the realm's registry
// applied (EvalOptions::moduleRegistry).
modules::ModuleOptions moduleOptionsFor(const EvalOptions& options);

// Everything a compile reads, hashed under `processMaterial` (the code
// cache's processKeyMaterial, or a module record's moduleKeyMaterial): two
// compiles with the same key build the same program. `withFeedback` false
// leaves the profile out: the key a module's result global is named from, so
// a build on a harvest names it as the plain build it was harvested from did
// (the name is a property key of the program, and so part of the caches'
// layout the harvest is checked against, il/feedback_site.h icLayoutDigest).
cache::Digest programKey(std::string_view processMaterial, const EvalOptions& options,
                         const modules::ModuleOptions& modOpts, const CompileInputs& in,
                         std::string_view entryText, bool withFeedback = true,
                         std::vector<cache::KeyPart>* parts = nullptr);

// The name a module's trailing expression's value is kept under: a function
// of its key without the profile (programKey, `withFeedback` false, under the
// module material), so a build on a harvest, the build it was harvested from
// and a first run compiled in process to be harvested (EvalOptions::
// harvestOutPath) all name it alike.
std::string moduleResName(const EvalOptions& options, const modules::ModuleOptions& modOpts,
                          const CompileInputs& in, std::string_view entryText);

// The front end after the graph is loaded: the eval transform (a trailing
// expression's value kept under `resName`, top-level declarations made
// global), inference and lowering. Null with `diags` holding why.
// `aheadOfTime` is a module build (aot_module.cpp): the ahead-of-time front
// end, as `bronze build` lowers; buildModule then runs the optimiser pipeline
// (opt/pipeline/pipeline.h). A compile to be harvested (EvalOptions::
// harvestOutPath) lowers so too, since its harvest is read by a module build
// of the same program, by site numbers that lowering hands out. A tiered
// compile (compileScript) runs no
// optimiser over its IL: generic IL is mostly calls the redundancy pass cannot
// see through, and brass's own optimizer runs on each function it tiers up.
std::unique_ptr<il::Module> lowerProgram(ast::Module& astModule, const EvalOptions& options,
                                         const CompileInputs& in, const std::string& resName,
                                         DiagnosticSink& diags, SourceSet& sources,
                                         bool aheadOfTime = false);

// EvalOptions::classicScript, done to the parsed unit before anything reads it
// (classic_script.cpp): the top-level `var`s and function declarations become
// global object properties, instantiated at each script's start.
void instantiateClassicScripts(ast::Module& astModule, const std::string& marker);

}  // namespace bronze::eval::detail
