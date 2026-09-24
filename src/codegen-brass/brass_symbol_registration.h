#pragma once

#include <brass/vm/fast_interpreter.hpp>
#include <brass/codegen/baseline_jit.hpp>
#include <brass/codegen/jit_exec.hpp>
#include <brass/runtime/multi_tier_pipeline.hpp>

namespace bronze {

// Makes bronze's runtime the host brass binds generated code against: from
// now on every engine brass builds on its own — the tiered pipeline's
// baseline compiler, fast and reference interpreters, the tier-2 installer's
// JIT, a deoptimization's interpreter — is handed bronze's ABI helpers, the
// math and parallel runtimes and the fallback module tables
// (brass::runtime::HostSymbolProvider). It also makes bronze the owner of
// the heap (brass::HostHeap): no allocation brass's runtime makes on its own
// behalf can land outside bronze's collector. Idempotent.
void installBronzeHostSymbols();

// The same set, into an engine bronze builds itself. Each installs the
// provider first.
void registerBronzeFastInterpreterSymbols(brass::FastInterpreter& interp);
void registerBronzeBaselineSymbols(brass::codegen::BaselineJitCompiler& compiler);
void registerBronzeJitSymbols(brass::codegen::JitExecutionEngine& engine);

// Readies an initialized pipeline for bronze: the provider (which reaches
// every engine the pipeline makes from here on) and its baseline compiler,
// which initialize() built before the provider may have been installed.
void registerBronzeMultiTierSymbols(brass::runtime::MultiTierPipeline& pipeline);

}  // namespace bronze
