// The run-time half of interpreted cold functions (cold_companion.h): an
// image's companion, built on the first call of any of its cold functions
// and kept for the life of the process, as the image is; each function's
// cell filled on its own first call.

#include "codegen-brass/cold_companion.h"

#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_symbol_registration.h"
#include "codegen-brass/brass_tiered_image.h"
#include "support/diagnostics.h"

#include <brass/mir/function.hpp>
#include <brass/mir/module.hpp>
#include <brass/runtime/code_installer.hpp>
#include <brass/runtime/multi_tier_pipeline.hpp>
#include <brass/runtime/osr_coordinator.hpp>
#include <brass/runtime/tiering.hpp>
#include <brass/vm/fast_interpreter.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace bronze::codegen {

namespace {

// As BrassTieredEngine's (brass_tiered_engine.cpp): a loop still running
// after this many interpreted backedges asks for its OSR entry.
constexpr uint64_t kOsrBackedgeThreshold = 1000;

[[noreturn]] void coldFatal(const std::string& what) {
    std::fprintf(stderr, "bronze: fatal: the image's interpreted cold functions: %s\n", what.c_str());
    std::fflush(stderr);
    std::abort();
}

// One image's companion. Never freed: the cells point into its pipeline, and
// the image they are in stays loaded for the life of the process.
struct Companion {
    il::Module il;
    ColdCompanionInputs inputs;
    std::unique_ptr<brass::Module> mir;
    std::unique_ptr<brass::runtime::FunctionDispatchTable> table;
    std::unique_ptr<TieredProgramImage> image;
    std::unordered_map<std::string, void*> links;
    // Per cell, its function's MIR name.
    std::vector<std::string> cellNames;
};

// Every image's companion built so far, by its link record.
std::unordered_map<const ColdLink*, Companion*>& companions() {
    static auto* map = new std::unordered_map<const ColdLink*, Companion*>();
    return *map;
}

std::mutex& companionsMutex() {
    static auto* mu = new std::mutex();
    return *mu;
}

Companion* build(const ColdLink& link) {
    auto* c = new Companion();
    std::string err;
    if (!decodeCompanion(std::string_view(link.blob, link.blobSize), c->il, c->inputs, err)) coldFatal(err);
    const char* name = link.names;
    for (uint64_t i = 0; i < link.linkCount; ++i) {
        const size_t len = std::strlen(name);
        c->links.emplace(std::string(name, len), link.addrs[i]);
        name += len + 1;
    }

    const std::string entry = link.entrySymbol;
    BrassBackend backend;
    backend.setEntrySymbol(entry);
    backend.setPerThreadModuleData(c->inputs.perThreadData);
    backend.setInProcess(true);
    backend.setOptimize(false);
    backend.setLazyBodies(true);
    // The build lowered this same blob and verified it.
    backend.setVerifyLowered(false);
    backend.setColdCompanion(&c->inputs);
    DiagnosticSink sink;
    c->mir = backend.buildMirModule(c->il, sink);
    if (!c->mir) coldFatal(sink.all().empty() ? "its MIR did not build" : sink.all().front().message);

    // A pipeline as an in-process program's at the Auto tier
    // (brass_tiered_engine.cpp), except that a call from native code runs
    // the function in Tier 0 until its own counts warm it: the image calls a
    // cold function natively, and compiling one on its first call would undo
    // the reason it was not compiled.
    brass::FastInterpreter::set_default_max_call_depth(SIZE_MAX);
    c->table = std::make_unique<brass::runtime::FunctionDispatchTable>();
    brass::runtime::MultiTierPipeline& pipeline = c->table->pipeline();
    brass::runtime::TieringConfig config;
    config.set_tier0_interpreter(brass::runtime::Tier0Interpreter::Fast);
    config.enable_background_compile = true;
    config.enable_background_tier1 = true;
    config.deopt_threshold = 2;
    config.native_call_requests_tier1 = false;
    pipeline.initialize(config);
    registerBronzeMultiTierSymbols(pipeline);
    pipeline.set_tier2_passes(backend.tierUpPasses());
    brass::runtime::OsrCoordinator& osr = c->table->osr();
    osr.set_threshold(kOsrBackedgeThreshold);
    osr.set_enabled(true);
    for (const auto& [sym, addr] : c->links) pipeline.register_external_symbol(sym, addr);
    pipeline.tiering().set_active_module(c->mir.get());

    // Stack traces and the profiler see its functions as the image's.
    Companion* raw = c;
    c->image = TieredProgramImage::attach(
        [raw](std::string_view sym) -> void* {
            auto it = raw->links.find(std::string(sym));
            return it != raw->links.end() ? it->second : nullptr;
        },
        *c->mir, c->il, entry, pipeline);

    const std::vector<std::string> names = computeUniqueFunctionNames(c->il);
    c->cellNames.resize(link.cellCount);
    for (size_t i = 0; i < c->inputs.interpreted.size(); ++i) {
        const uint32_t cell = c->inputs.interpreted[i];
        if (cell == UINT32_MAX) continue;
        if (cell >= link.cellCount) coldFatal("a cell out of range");
        c->cellNames[cell] = names[i];
    }
    return c;
}

}  // namespace

void resolveColdCell(const ColdLink* link, uint32_t cell) {
    if (link == nullptr || link->magic != kColdLinkMagic || cell >= link->cellCount) coldFatal("a bad link record");
    std::lock_guard<std::mutex> lock(companionsMutex());
    Companion*& c = companions()[link];
    if (c == nullptr) c = build(*link);
    // Another thread filled it while this one waited.
    std::atomic_ref<uint64_t> word(link->cells[cell]);
    if (word.load(std::memory_order_acquire) != 0) return;
    // The function's entry in the pipeline (its lazy stub, which runs it in
    // Tier 0 and follows it up the tiers), made on its first call: most of a
    // module's cold functions are never called.
    const std::string& name = c->cellNames[cell];
    const brass::Function* fn = c->mir->get_function(name);
    if (!fn) coldFatal("no function '" + name + "'");
    void* entry = c->table->pipeline().function_address(name, fn);
    if (!entry) coldFatal("no entry for '" + name + "'");
    word.store(reinterpret_cast<uint64_t>(entry), std::memory_order_release);
}

}  // namespace bronze::codegen
