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
// (brass::runtime::HostSymbolProvider). It also creates the calling thread's
// bronze heap, which is the brass heap every brass interpreter on the thread
// then uses (runtime/heap.h, Heap::bind_thread). Idempotent.
void installBronzeHostSymbols();

// The same set, into an engine bronze builds itself. Installs the provider
// first.
void registerBronzeJitSymbols(brass::codegen::JitExecutionEngine& engine);

// Routes the calling thread's C++ -> JS calls (runtime/fn.h rtEnterJs) to a
// function the thread's running fast interpreter still interprets into that
// interpreter, rather than through the function's native stub. Per thread:
// called on each thread that runs a tiered program, as it runs it.
void installBronzeEnterJsHook();

// Readies an initialized pipeline for bronze: the provider (which reaches
// every engine the pipeline makes from here on) and its baseline compiler,
// which initialize() built before the provider may have been installed.
void registerBronzeMultiTierSymbols(brass::runtime::MultiTierPipeline& pipeline);

}  // namespace bronze
