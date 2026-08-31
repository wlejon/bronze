// Object emission: the O3 pipeline, the host target machine, and the parallel
// partition path. The IR this consumes is built by llvm_backend.cpp.

#include "codegen-llvm/llvm_emit.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
// The bitcode headers instantiate LLVM templates inside MSVC's own STL
// headers, which sit outside the -external:W0 shield the LLVM include dir
// gets — so the truncation warnings those instantiations raise are LLVM's,
// not bronze's, and are silenced for exactly these two includes.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244 4267)
#endif
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>

#include "codegen-llvm/llvm_env_promote.h"
#include "codegen-llvm/llvm_partition.h"
#include "support/source.h"
#include "support/timings.h"

namespace bronze::codegen_llvm {

namespace {

std::once_flag nativeTargetOnce;

// Everything a worker needs to build a TargetMachine of its own. A
// TargetMachine holds per-compilation state, so threads may not share one;
// the description is captured once and each worker constructs from it.
struct TargetDesc {
    std::string triple;
    std::string cpu;
    std::string features;
    bool pic = false;
};

std::unique_ptr<llvm::TargetMachine> makeTargetMachine(const TargetDesc& desc,
                                                       std::string& errOut) {
    std::string lookupError;
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(llvm::Triple(desc.triple), lookupError);
    if (!target) {
        errOut = "Failed to lookup host target: " + lookupError;
        return nullptr;
    }
    llvm::TargetOptions opt;
    // std::nullopt is LLVM's "pick the target's default", which is what every
    // static build has always emitted and what must not change here.
    const std::optional<llvm::Reloc::Model> reloc =
        desc.pic ? std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_) : std::nullopt;
    std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
        llvm::Triple(desc.triple), desc.cpu, desc.features, opt, reloc, std::nullopt,
        llvm::CodeGenOptLevel::Aggressive));
    if (!tm) errOut = "Failed to create LLVM target machine";
    return tm;
}

// The `BRONZE_XALIGN` value, parsed once. `error` non-empty means the variable
// was set to something this refuses; the emission path reports it and stops,
// because a run that quietly emitted the default alignment would answer a
// question nobody asked.
struct FunctionAlign {
    unsigned bytes = 0;
    std::string error;
};

const FunctionAlign& functionAlign() {
    static const FunctionAlign parsed = [] {
        FunctionAlign a;
        parseFunctionAlign(std::getenv("BRONZE_XALIGN"), a.bytes, a.error);
        return a;
    }();
    return parsed;
}

// BRONZE_DUMP_LLVM_IR=<prefix>: write `<prefix>.pre.ll` and `<prefix>.post.ll`
// around the O3 pipeline. This is the seam for reading what LLVM did with a
// guard — whether a shape load was hoisted, whether two compares merged — and
// the only honest way to answer that is the IR, not the disassembly, because
// the disassembly no longer knows which compare came from which site. A
// partitioned module suffixes the partition index, so the files stay distinct.
void dumpModuleIfAsked(llvm::Module& m, const char* phase, int part) {
    static const char* prefix = std::getenv("BRONZE_DUMP_LLVM_IR");
    if (prefix == nullptr || prefix[0] == '\0') return;
    std::string path = std::string(prefix);
    if (part >= 0) path += ".p" + std::to_string(part);
    path += std::string(".") + phase + ".ll";
    std::error_code ec;
    llvm::raw_fd_ostream os(path, ec, llvm::sys::fs::OF_Text);
    if (ec) {
        std::fprintf(stderr, "BRONZE_DUMP_LLVM_IR: cannot open %s: %s\n", path.c_str(),
                     ec.message().c_str());
        return;
    }
    m.print(os, nullptr);
}

// O3 middle-end + MC backend for one module: the whole pipeline for a small
// module, one partition's pipeline for a large one. `timing` may be true only
// on the single-module path — parallel workers would interleave the laps.
bool optimizeAndEmitOne(llvm::Module& m, llvm::TargetMachine& tm, const std::string& path,
                        bool timing, std::string& errOut, int dumpPart = -1) {
    auto t0 = std::chrono::steady_clock::now();
    auto lap = [&t0, timing](const char* what) {
        if (!timing) return;
        auto now = std::chrono::steady_clock::now();
        std::fprintf(stderr, "      %-12s %8.1f ms\n", what,
                     std::chrono::duration<double, std::milli>(now - t0).count());
        t0 = now;
    };

    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb(&tm);
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    // Stage R3, at the one extension point where the question it asks has an
    // answer: after the module SIMPLIFICATION pipeline — which is where the
    // inliner, EarlyCSE, GVN and LoopSimplify live — and before the function
    // optimization pipeline, so the phis it creates are there for everything
    // downstream to read. Asked any earlier, every sibling-closure call in a
    // loop is still a call and no region survives one iteration
    // (llvm_env_promote.h says why that is the whole stage).
    pb.registerOptimizerEarlyEPCallback([](llvm::ModulePassManager& early,
                                           llvm::OptimizationLevel,
                                           llvm::ThinOrFullLTOPhase) {
        early.addPass(codegen_llvm::EnvPromotionPass());
    });
    llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    dumpModuleIfAsked(m, "pre", dumpPart);
    mpm.run(m, mam);
    lap("opt-O3");
    dumpModuleIfAsked(m, "post", dumpPart);

    // After the optimizer, because what the seam wants aligned is what is
    // actually emitted: a body the inliner deleted has no placement to quantize
    // and a body it created would otherwise be missed.
    alignEmittedFunctions(m, functionAlign().bytes);

    std::error_code ec;
    llvm::raw_fd_ostream dest(path, ec, llvm::sys::fs::OF_None);
    if (ec) {
        errOut = "Could not open output file " + path + ": " + ec.message();
        return false;
    }
    llvm::legacy::PassManager pass;
    if (tm.addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        errOut = "Target machine cannot emit object file for this target";
        return false;
    }
    pass.run(m);
    dest.flush();
    lap("mc-emit");
    return true;
}

// Promote every module-local symbol so a partition can reference a definition
// that landed in another partition. Three properties of the promotion carry
// the correctness:
//
// - The NEW NAME is prefixed. An internal symbol's name was never a contract —
//   a JS `function malloc(){}` lowers to an internal symbol spelled `malloc` —
//   and externalizing it under its own spelling would collide with (or worse,
//   silently satisfy) the C runtime's symbol at link. Nothing resolves these
//   symbols by name at runtime; only the partition objects' relocations use
//   them, and those go through the rename together.
// - HIDDEN visibility. On ELF and Mach-O every default-visibility external in
//   a shared object is exported, so two loaded modules could resolve each
//   other's promoted internals. Hidden keeps the promotion inside the image;
//   COFF ignores visibility and exports only dllexport names, so it changes
//   nothing there.
// - Published names are untouched: they were never local, so they never enter
//   this loop, and their dllexport marking stays verifier-legal.
void promoteLocalsForSplit(llvm::Module& m) {
    unsigned anon = 0;
    for (llvm::GlobalValue& gv : m.global_values()) {
        if (!gv.hasLocalLinkage()) continue;
        const std::string base = gv.hasName() ? gv.getName().str() : std::to_string(anon++);
        gv.setName("__bronze_part$" + base);
        gv.setLinkage(llvm::GlobalValue::ExternalLinkage);
        gv.setVisibility(llvm::GlobalValue::HiddenVisibility);
    }
}

}  // namespace

bool parseFunctionAlign(const char* value, unsigned& bytes, std::string& errOut) {
    bytes = 0;
    errOut.clear();
    if (value == nullptr || value[0] == '\0') return true;
    char* end = nullptr;
    const unsigned long n = std::strtoul(value, &end, 10);
    // A power of two is what an alignment IS on every target bronze emits for,
    // and 4096 is a page: past that the request is about the loader, not the
    // instruction cache the seam exists to ask about.
    const bool wellFormed = end != nullptr && *end == '\0' && end != value && n >= 1 &&
                            n <= 4096 && (n & (n - 1)) == 0;
    if (!wellFormed) {
        errOut = std::string("BRONZE_XALIGN: expected a power of two in [1, 4096], got '") +
                 value + "'";
        return false;
    }
    bytes = static_cast<unsigned>(n);
    return true;
}

void alignEmittedFunctions(llvm::Module& m, unsigned bytes) {
    if (bytes == 0) return;
    for (llvm::Function& f : m) {
        if (f.isDeclaration()) continue;
        f.setAlignment(llvm::Align(bytes));
    }
}

// Emission cost is LINEAR in instruction count: ~50 µs/inst through O3 + MC,
// measured on the three.js bundle at both 1.7M and 7.2M instructions, with no
// single pass or function dominating (an optnone entry function and O1/O2
// pipelines all landed within noise). So the one lever that matters for a
// large module is running partitions of it in PARALLEL, and the split is by
// symbol-name hash (llvm::SplitModule), so the same input always produces the
// same partitions with the same contents.
//
// Below the threshold — every ordinary program — the single-object path runs
// exactly as it always has and writes exactly `outputPath`. Above it, only
// when the caller passed `emittedPaths` (i.e. it can link a LIST), the module
// is split and each partition is optimized and emitted on its own thread, in
// its own LLVMContext, handed over as bitcode because a worker may not touch
// the shared context. `emittedPaths` records what was actually written.
bool writeObjectFile(llvm::Module& llvmModule, const std::string& outputPath, bool pic,
                     const std::string& entrySymbol,
                     std::vector<std::string>* emittedPaths, DiagnosticSink& diags) {
    if (const FunctionAlign& align = functionAlign(); !align.error.empty()) {
        diags.error(Span{}, align.error);
        return false;
    }
    std::call_once(nativeTargetOnce, [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
    });

    TargetDesc desc;
    desc.triple = llvm::sys::getDefaultTargetTriple();
    desc.cpu = std::string(llvm::sys::getHostCPUName());
    llvm::SubtargetFeatures features;
    for (const auto& [feature, enabled] : llvm::sys::getHostCPUFeatures()) {
        features.AddFeature(feature, enabled);
    }
    desc.features = features.getString();
    desc.pic = pic;

    llvmModule.setTargetTriple(llvm::Triple(desc.triple));

    std::string tmErr;
    std::unique_ptr<llvm::TargetMachine> targetMachine = makeTargetMachine(desc, tmErr);
    if (!targetMachine) {
        diags.error(Span{}, tmErr);
        return false;
    }
    llvmModule.setDataLayout(targetMachine->createDataLayout());

    size_t instCount = 0;
    {
        size_t fnCount = 0, maxInsts = 0;
        std::string maxName;
        for (const llvm::Function& f : llvmModule) {
            if (f.isDeclaration()) continue;
            ++fnCount;
            size_t insts = 0;
            for (const llvm::BasicBlock& bb : f) insts += bb.size();
            instCount += insts;
            if (insts > maxInsts) {
                maxInsts = insts;
                maxName = std::string(f.getName());
            }
        }
        if (support::timingsEnabled()) {
            std::fprintf(stderr, "      scale: %zu fns, %zu insts, largest %s (%zu insts)\n",
                         fnCount, instCount, maxName.c_str(), maxInsts);
        }
    }

    // ~20 s of serial pipeline is where splitting starts paying for its
    // constant costs (split, bitcode round-trip, link of N objects); half the
    // threshold per partition keeps every partition big enough to be worth a
    // thread. The cap bounds peak memory: each worker holds a parsed
    // partition plus its analyses.
    constexpr size_t kPartitionThresholdInsts = 400'000;
    constexpr unsigned kMaxPartitions = 16;
    unsigned parts = 1;
    if (emittedPaths && instCount >= kPartitionThresholdInsts) {
        const unsigned byInsts =
            static_cast<unsigned>(instCount / (kPartitionThresholdInsts / 2));
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        parts = std::min({kMaxPartitions, hw, std::max(2u, byInsts)});
    }

    if (parts <= 1) {
        // BRONZE_EMIT_FN_SYMBOLS=1: run the split path's local-symbol
        // promotion even for a single object. A promoted symbol is EXTERNAL,
        // and external symbols are what the linker copies into the PDB's
        // publics stream — bronze emits no CodeView records, so an internal
        // function is invisible to dbghelp and a sampling profile of a small
        // program attributes everything to `bronze_main`. The split path gets
        // this for free (every large module is promoted to link at all); this
        // makes it OPT-IN for the small ones, because the renamed
        // `__bronze_part$` spellings are diagnostics, not a contract.
        static const bool emitFnSymbols = [] {
            const char* env = std::getenv("BRONZE_EMIT_FN_SYMBOLS");
            return env != nullptr && env[0] == '1';
        }();
        if (emitFnSymbols) promoteLocalsForSplit(llvmModule);
        std::string err;
        if (!optimizeAndEmitOne(llvmModule, *targetMachine, outputPath,
                                support::timingsEnabled(), err)) {
            diags.error(Span{}, err);
            return false;
        }
        if (emittedPaths) *emittedPaths = {outputPath};
        return true;
    }

    // Partition files sit beside the requested output as `<stem>.p<N><ext>`.
    std::vector<std::string> paths;
    {
        const std::filesystem::path base(outputPath);
        const std::string ext = base.extension().string();
        std::filesystem::path stem = base;
        stem.replace_extension();
        for (unsigned i = 0; i < parts; ++i) {
            paths.push_back(stem.string() + ".p" + std::to_string(i) + ext);
        }
    }

    promoteLocalsForSplit(llvmModule);

    // The bin packing, and the out-of-bin bodies each bin keeps so a direct
    // call across the split is still inlinable (llvm_partition.h). Everything
    // that is not a function goes to partition 0: data is near-free to emit
    // and this keeps the module's tables in one object.
    //
    // The assignment travels by NAME: promotion just gave every definition a
    // unique external name, and each worker re-finds its bin in a parsed copy
    // of the module rather than in this one — a worker may not touch the
    // shared LLVMContext, so bitcode is the handoff.
    const auto plan = planPartitions(llvmModule, parts, entrySymbol);

    // ONE serialization of the whole module is the only serial work between
    // the plan and the workers. (Its predecessor cloned and serialized a
    // partition per worker on this thread — 16 walks of the module — and that
    // serial ramp was longer than half a worker's whole job, so the last
    // worker started near the halfway point of the first one's run.)
    llvm::SmallString<0> bitcode;
    {
        llvm::raw_svector_ostream os(bitcode);
        llvm::WriteBitcodeToFile(llvmModule, os);
    }

    std::vector<std::string> errors(parts);
    std::vector<PartitionStats> partStats(parts);
    std::vector<double> partMillis(parts, 0.0);
    std::vector<std::thread> workers;
    workers.reserve(parts);
    for (unsigned i = 0; i < parts; ++i) {
        workers.emplace_back([&, i] {
            const auto begin = std::chrono::steady_clock::now();
            llvm::LLVMContext ctx;
            llvm::MemoryBufferRef buf(llvm::StringRef(bitcode.data(), bitcode.size()),
                                      "module");
            // Lazy: function bodies stay in the buffer until materialized, so
            // a worker only ever holds its own bin's bodies — the difference
            // between N× the module in memory and ~1× shared across workers.
            llvm::Expected<std::unique_ptr<llvm::Module>> partOr =
                llvm::getLazyBitcodeModule(buf, ctx);
            if (!partOr) {
                errors[i] = "partition " + std::to_string(i) +
                            ": bitcode parse failed: " + llvm::toString(partOr.takeError());
                return;
            }
            llvm::Module& part = **partOr;
            if (std::string err = applyPartition(part, plan, i, partStats[i]); !err.empty()) {
                errors[i] = "partition " + std::to_string(i) + ": " + err;
                return;
            }
            if (i != 0) {
                // Partition 0 owns every non-function definition.
                for (llvm::GlobalVariable& g : part.globals()) {
                    if (!g.hasInitializer()) continue;
                    g.setInitializer(nullptr);
                    g.setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
                }
            }
            std::string err;
            std::unique_ptr<llvm::TargetMachine> tm = makeTargetMachine(desc, err);
            if (!tm) {
                errors[i] = "partition " + std::to_string(i) + ": " + err;
                return;
            }
            if (!optimizeAndEmitOne(part, *tm, paths[i], /*timing=*/false, err,
                                    static_cast<int>(i))) {
                errors[i] = "partition " + std::to_string(i) + ": " + err;
            }
            partMillis[i] = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - begin)
                                .count();
        });
    }
    for (std::thread& w : workers) w.join();

    if (support::timingsEnabled()) {
        std::fprintf(stderr, "      partitions: %u (xpart cap %u, depth %u)\n", parts,
                     crossPartitionInlineCap(), crossPartitionInlineDepth());
        for (unsigned i = 0; i < parts; ++i) {
            std::fprintf(stderr, "        p%-2u %9zu insts %10.1f ms  +%zu borrowed (%zu insts)\n",
                         i, partStats[i].ownInsts, partMillis[i], partStats[i].keptFns,
                         partStats[i].keptInsts);
        }
    }
    bool ok = true;
    for (const std::string& err : errors) {
        if (err.empty()) continue;
        diags.error(Span{}, err);
        ok = false;
    }
    if (!ok) return false;
    *emittedPaths = orderPartitionPaths(plan, paths);
    return true;
}

}  // namespace bronze::codegen_llvm
