// The backend's composition root: IL module in, object file out. The body of
// each function is llvm_func.cpp; every symbol generated code links against is
// llvm_abi.cpp.

#include "codegen-llvm/llvm_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <llvm/ADT/SmallString.h>
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
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/MemoryBufferRef.h>

static_assert(LLVM_VERSION_MAJOR >= 20, "Bronze LLVM backend requires LLVM 20 or higher");

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_abi.h"
#include "codegen-llvm/llvm_alias.h"
#include "codegen-llvm/llvm_call.h"
#include "codegen-llvm/llvm_frame.h"
#include "codegen-llvm/llvm_func.h"
#include "codegen-llvm/llvm_partition.h"
#include "codegen-llvm/llvm_pin.h"
#include "il/print.h"
#include "il/verifier.h"
#include "support/timings.h"

namespace bronze {

using codegen_llvm::AbiFns;
using codegen_llvm::AbiGlobals;
using codegen_llvm::emitPinnedParamUnbox;
using codegen_llvm::emitToNumberInline;
using codegen_llvm::FunctionEmitter;
using codegen_llvm::mapILType;

namespace {

// One LLVM function per IL function, in its typed form: inference's proven
// types are the signature, so a direct call passes an f64 in a register rather
// than boxing it.
bool declareEntries(const il::Module& module, llvm::Module& llvmModule, llvm::LLVMContext& ctx,
                    const std::string& entrySymbol, std::vector<llvm::Function*>& out,
                    DiagnosticSink& diags) {
    out.reserve(module.functions.size());
    for (const auto& func : module.functions) {
        llvm::Type* retTy = mapILType(func.returnType, ctx);
        if (!retTy) {
            diags.error(Span{}, "Unsupported return type in function " + func.name);
            return false;
        }
        std::vector<llvm::Type*> paramTys;
        paramTys.reserve(func.params.size());
        for (const auto& param : func.params) {
            llvm::Type* pTy = mapILType(param.type, ctx);
            if (!pTy) {
                diags.error(Span{}, "Unsupported parameter type in function " + func.name);
                return false;
            }
            paramTys.push_back(pTy);
        }
        // `main` is the program's entry point by name, so the IL function of
        // that name gets the caller's spelling — and it is the ONLY function
        // the object exports. A program is compiled whole into one object, so
        // every other function is internal; external linkage here handed a JS
        // function named `bind` to the system linker, where it collided with
        // ws2_32's export of the same name. That internal-by-default rule is
        // also what lets two compiled modules link into one image: the entry,
        // the ABI stamp and the host-globals manifest are the only names that
        // have to be distinct, and the latter two are named after the entry.
        const bool isEntry = (func.name == "main");
        const std::string symbol = isEntry ? entrySymbol : func.name;
        out.push_back(llvm::Function::Create(llvm::FunctionType::get(retTy, paramTys, false),
                                             isEntry ? llvm::Function::ExternalLinkage
                                                     : llvm::Function::InternalLinkage,
                                             symbol, &llvmModule));
    }
    return true;
}

// The frameless twin of every merge target: the typed entry's signature plus
// the two things a region needs — the base of the caller's frame region this
// call's slots live in, and the caller's view of the thread's ABI block. Both
// are pointers the caller already has, and handing them over is what leaves the
// callee's body without a prologue of its own to inline along with it.
void declareInlineVariants(const il::Module& module, llvm::Module& llvmModule,
                           const codegen_llvm::RegionPlan& regions,
                           const std::vector<llvm::Function*>& entries,
                           std::vector<llvm::Function*>& out) {
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(llvmModule.getContext());
    out.assign(module.functions.size(), nullptr);
    for (size_t i = 0; i < module.functions.size(); ++i) {
        if (!regions.isMergeTarget[i]) continue;
        llvm::FunctionType* entryTy = entries[i]->getFunctionType();
        std::vector<llvm::Type*> params(entryTy->param_begin(), entryTy->param_end());
        params.push_back(ptrTy);
        params.push_back(ptrTy);
        llvm::Function* variant = llvm::Function::Create(
            llvm::FunctionType::get(entryTy->getReturnType(), params, false),
            llvm::Function::InternalLinkage, entries[i]->getName() + ".inl", &llvmModule);
        // What the two of them ARE, said where the storage-alias families can
        // read it (llvm_alias.h): without this the body's every frame access
        // loses the StackFrame claim it carried when the frame was its own,
        // and every control word it caches has to be re-loaded around it.
        const unsigned regionArg = static_cast<unsigned>(params.size()) - 2;
        variant->addParamAttr(regionArg,
                              llvm::Attribute::get(llvmModule.getContext(),
                                                   codegen_llvm::kRegionParamAttr));
        variant->addParamAttr(regionArg + 1,
                              llvm::Attribute::get(llvmModule.getContext(),
                                                   codegen_llvm::kTlsParamAttr));
        out[i] = variant;
    }
}

// A function held as a VALUE is called through the uniform convention —
// `bronze_fn_code`: (env, this, argc, argv) — which knows nothing of the typed
// signature above. The wrapper is the adapter: it unpacks argv into the typed
// parameters, calls the entry, and boxes the result. A wrapper for a
// rest-parameter function ALLOCATES — it builds the rest array — and it does so
// before the entry's prologue has rooted anything. The rooting contract ("the
// callee roots its parameters before it can allocate") therefore does not cover
// it, and `env` and `this` arrive here as raw pointers in registers: a
// collection during that allocation moved the instance and left `this` pointing
// into from-space, which crashed `this.items = items` in a rest constructor
// under BRONZE_GC_STRESS.
//
// So the wrapper gets a root frame of its own for exactly those calls, and
// every named parameter is read out of argv BEFORE the first of them and
// carried through it.
//
// `argv` itself is not protected, and must not be assumed to be. It points
// into the CALLER's block, and the collector updates that block in place only
// when the caller is generated code, whose block lives in its own GC root
// frame. A BUILTIN calling back into JS — `Array.prototype.map`, `forEach`,
// `reduce`, `Array.from`'s mapper, the JSON replacer and reviver, the regexp
// replacer, `bronze_dynamic_call`'s arity-adaptation vector — builds its block
// in plain stack memory that nothing scans and nothing updates. Reading argv
// after an allocation is therefore a stale read, and it is a SILENT one: a
// forwarded header keeps its tag, so the value still looks like an array or a
// string and reports a garbage length.
//
// `live` is what must survive: `env` and `this` always, plus anything an
// EARLIER call here already built. A wrapper can make two of these — the rest
// array and the `arguments` object — and the second allocation moves the first.
// Rooting only env and this left the rest array pointing at the arguments array
// that had been allocated over it, so `rest.length` was the whole argument
// count. Only BRONZE_GC_STRESS=1 showed it, which is the argument for that mode
// existing.
llvm::Value* emitWrapperArrayCall(llvm::IRBuilder<>& builder, llvm::LLVMContext& ctx,
                                  const AbiGlobals& globals,
                                  const std::vector<llvm::Value**>& live,
                                  const std::function<llvm::Value*()>& build) {
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);
    const uint32_t slotCount = static_cast<uint32_t>(live.size());
    llvm::Type* slotsTy = llvm::ArrayType::get(i64Ty, slotCount);
    llvm::StructType* frameTy = llvm::StructType::get(ctx, {ptrTy, i64Ty, slotsTy});

    llvm::Value* frame = builder.CreateAlloca(frameTy, nullptr, "wrapframe");
    llvm::Value* slots = builder.CreateStructGEP(frameTy, frame, 2);
    std::vector<llvm::Value*> slotPtrs;
    for (uint32_t i = 0; i < slotCount; ++i) {
        slotPtrs.push_back(builder.CreateConstInBoundsGEP2_32(slotsTy, slots, 0, i));
        builder.CreateStore(*live[i], slotPtrs.back());
    }
    builder.CreateStore(builder.getInt64(slotCount), builder.CreateStructGEP(frameTy, frame, 1));
    builder.CreateStore(builder.CreateLoad(ptrTy, globals.bronze_gc_frame_top),
                        builder.CreateStructGEP(frameTy, frame, 0));
    builder.CreateStore(frame, globals.bronze_gc_frame_top);

    llvm::Value* built = build();

    for (uint32_t i = 0; i < slotCount; ++i) {
        *live[i] = builder.CreateLoad(i64Ty, slotPtrs[i]);
    }
    builder.CreateStore(builder.CreateLoad(ptrTy, builder.CreateStructGEP(frameTy, frame, 0)),
                        globals.bronze_gc_frame_top);
    return built;
}

void emitCallWrappers(const il::Module& module, llvm::Module& llvmModule, llvm::LLVMContext& ctx,
                      const AbiFns& abi, const codegen_llvm::ModuleTables& tables,
                      const std::string& entrySymbol,
                      const std::vector<llvm::Function*>& entries,
                      std::vector<llvm::Function*>& out) {
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::FunctionType* wrapperTy = llvm::FunctionType::get(
        i64Ty, {i64Ty, i64Ty, llvm::Type::getInt32Ty(ctx), llvm::PointerType::getUnqual(ctx)},
        false);

    out.resize(module.functions.size());
    for (size_t i = 0; i < module.functions.size(); ++i) {
        const auto& func = module.functions[i];
        std::string name = "__wrapper_" + (func.name == "main" ? entrySymbol : func.name);
        llvm::Function* wrapper = llvm::Function::Create(
            wrapperTy, llvm::Function::InternalLinkage, name, &llvmModule);
        out[i] = wrapper;

        llvm::IRBuilder<> builder(llvm::BasicBlock::Create(ctx, "entry", wrapper));
        // Only the two array builds below root anything, so only a wrapper
        // that makes one fetches its thread's ABI block.
        AbiGlobals globals;
        if (func.needsArguments || func.hasRestParam) {
            globals = bindTlsBlock(builder, abi);
        }
        auto argsIt = wrapper->arg_begin();
        llvm::Value* env = argsIt++;
        llvm::Value* thisArg = argsIt++;
        // argc: the entry's arity is fixed, so short calls are padded by the
        // caller — except for a REST parameter, which is the one thing that has
        // to know how many arguments there really were. This wrapper is the
        // only place that can see it.
        llvm::Value* argc = argsIt++;
        llvm::Value* argv = argsIt;

        // Synthetic leading parameters come from the calling convention, not
        // from argv: the environment from the closure, the receiver from the
        // caller.
        const size_t firstSourceParam = func.firstSourceParam();
        // Every named parameter is loaded out of argv HERE, before either
        // array is built, and then travels through the root frames below.
        //
        // The order used to be the other way round, on the reasoning that
        // building an array can collect and so anything loaded beforehand
        // would be stale. That is only true if `argv` itself survives the
        // collection — which holds when the caller is generated code, whose
        // block lives in its own GC root frame, and fails when the caller is a
        // BUILTIN: `builtin_array.cpp`'s `Value block[3]`, the JSON replacer's
        // and the regexp replacer's are plain stack memory that nothing scans
        // and nothing updates. So `["a"].map(function (s) { return
        // arguments.length + s.n; })` allocated the arguments object, moved
        // the heap, and then read `s` out of a block still holding pre-move
        // bits. Loading first and ROOTING is what makes the claim true rather
        // than assumed; `emitWrapperArrayCall` already updates every slot it
        // is given.
        std::vector<llvm::Value*> loaded;
        const size_t namedCount =
            func.params.size() - firstSourceParam - (func.hasRestParam ? 1 : 0);
        for (size_t n = 0; n < namedCount; ++n) {
            const uint32_t sourceIndex = static_cast<uint32_t>(n);
            // The unguarded load is correct because `FunctionHeader::call`
            // padded argv up to the declared arity before entering here. A
            // function that owns an `arguments` object declares arity 0 so that
            // padding does not happen — `f(1)` and `f(1, undefined)` must
            // disagree about `arguments.length` — so its own reads are the one
            // place that has to check.
            if (func.needsArguments) {
                loaded.push_back(builder.CreateCall(
                    abi.bronze_arg_at, {argc, argv, builder.getInt32(sourceIndex)}));
            } else {
                loaded.push_back(builder.CreateLoad(
                    i64Ty, builder.CreateGEP(i64Ty, argv, builder.getInt32(sourceIndex))));
            }
        }

        // Each array that is already built joins the next one's root frame,
        // alongside every named parameter loaded above.
        std::vector<llvm::Value**> live{&env, &thisArg};
        for (llvm::Value*& slot : loaded) live.push_back(&slot);

        llvm::Value* argumentsArg = nullptr;
        if (func.needsArguments) {
            argumentsArg = emitWrapperArrayCall(builder, ctx, globals, live, [&] {
                llvm::Value* calleeVal = nullptr;
                llvm::Value* isStrictVal = builder.getInt1(func.isStrict);
                if (func.isStrict) {
                    calleeVal = builder.getInt64(BRONZE_ABI_UNDEFINED_BITS);
                } else {
                    calleeVal = env;
                }
                return builder.CreateCall(abi.bronze_arguments_object,
                                          {argc, argv, calleeVal, isStrictVal});
            });
        }
        llvm::Value* restArg = nullptr;
        if (func.hasRestParam) {
            const uint32_t firstRest =
                static_cast<uint32_t>(func.params.size() - 1 - firstSourceParam);
            if (argumentsArg != nullptr) live.push_back(&argumentsArg);
            restArg = emitWrapperArrayCall(builder, ctx, globals, live, [&] {
                return builder.CreateCall(abi.bronze_rest_args,
                                          {argc, argv, builder.getInt32(firstRest)});
            });
        }

        std::vector<llvm::Value*> callArgs;
        if (func.needsEnv) callArgs.push_back(env);
        if (func.needsThis) callArgs.push_back(thisArg);
        if (func.needsArguments) callArgs.push_back(argumentsArg);
        for (size_t p = firstSourceParam; p < func.params.size(); ++p) {
            const uint32_t sourceIndex = static_cast<uint32_t>(p - firstSourceParam);
            if (func.hasRestParam && p + 1 == func.params.size()) {
                callArgs.push_back(restArg);
                break;
            }
            // Loaded and rooted above, and re-read out of the root frame by
            // every array build in between, so these bits are current.
            llvm::Value* bits = loaded[sourceIndex];
            // A PINNED parameter is checked, never coerced (llvm_pin.h). The
            // wrapper is the door every caller the static plan could not
            // enumerate comes through — an escaped closure, a host call, a
            // method reached off a value — so it is where a `param` pin stops
            // being a promise and starts being enforced. The check REPLACES
            // the ToNumber below rather than joining it, so the enforced form
            // is cheaper than the coercing one it succeeds.
            if (func.params[p].type == il::Type::F64 && func.params[p].pinned) {
                callArgs.push_back(emitPinnedParamUnbox(builder, abi, tables, bits,
                                                        func.params[p].pinKeyIndex));
                continue;
            }
            switch (func.params[p].type) {
                case il::Type::F64:
                    callArgs.push_back(emitToNumberInline(builder, abi, bits));
                    break;
                case il::Type::I32:
                    callArgs.push_back(builder.CreateCall(abi.bronze_unbox_i32, {bits}));
                    break;
                case il::Type::Bool:
                    callArgs.push_back(builder.CreateCall(abi.bronze_unbox_bool, {bits}));
                    break;
                default:
                    callArgs.push_back(bits);
                    break;
            }
        }

        llvm::Value* result = builder.CreateCall(entries[i], callArgs);
        switch (func.returnType) {
            case il::Type::Void:
                builder.CreateRet(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS));
                break;
            case il::Type::F64:
                builder.CreateRet(builder.CreateCall(abi.bronze_box_f64, {result}));
                break;
            case il::Type::I32:
                builder.CreateRet(builder.CreateCall(abi.bronze_box_i32, {result}));
                break;
            case il::Type::Bool:
                builder.CreateRet(builder.CreateCall(abi.bronze_box_bool, {result}));
                break;
            default:
                builder.CreateRet(result);
                break;
        }
    }
}

// Host target machine → object file(s). Runs LLVM's PassBuilder O3 pipeline
// and targets the host CPU and instruction set extensions.
//
// `pic` asks for position-independent code, and it is the one thing about this
// path that `--emit-shared` changes. bronze's ordinary output is NOT
// position-independent — which is why the driver links executables `-no-pie` on
// Linux and why tests/two_module links its host the same way — and on ELF
// x86-64 a non-PIC object simply cannot go into a shared object: GNU ld refuses
// the R_X86_64_32S relocations with "can not be used when making a shared
// object; recompile with -fPIC". A loadable module IS a shared object, so it is
// compiled as one. Mach-O is position-independent throughout and COFF has no
// such concept, so this is a no-op on both; asking for it uniformly under the
// shared flag is still right, because what it expresses is "this object is
// going into a library", which is true on all three.

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
    llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    dumpModuleIfAsked(m, "pre", dumpPart);
    mpm.run(m, mam);
    lap("opt-O3");
    dumpModuleIfAsked(m, "post", dumpPart);

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
                     std::vector<std::string>* emittedPaths, DiagnosticSink& diags) {
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
    const codegen_llvm::PartitionPlan plan =
        codegen_llvm::planPartitions(llvmModule, parts);

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
    std::vector<codegen_llvm::PartitionStats> partStats(parts);
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
            if (std::string err = codegen_llvm::applyPartition(part, plan, i, partStats[i]);
                !err.empty()) {
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
                     codegen_llvm::crossPartitionInlineCap(),
                     codegen_llvm::crossPartitionInlineDepth());
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
    *emittedPaths = std::move(paths);
    return true;
}

}  // namespace

bool LLVMBackend::emitObject(const il::Module& module, const std::string& outputPath,
                             DiagnosticSink& diags) {
    // Indented one level under the CLI's own phase lines, because these four
    // are the inside of its `codegen`.
    const bool timing = support::timingsEnabled();
    auto t0 = std::chrono::steady_clock::now();
    auto lap = [&t0, timing](const char* what) {
        if (!timing) return;
        auto now = std::chrono::steady_clock::now();
        std::fprintf(stderr, "    %-14s %8.1f ms\n", what,
                     std::chrono::duration<double, std::milli>(now - t0).count());
        t0 = now;
    };
    if (!il::verify(module, diags)) return false;
    lap("il-verify");

    llvm::LLVMContext ctx;
    auto llvmModule = std::make_unique<llvm::Module>(module.name, ctx);

    AbiFns abi;
    codegen_llvm::declareAbiSymbols(*llvmModule, ctx, abi, sharedRuntime_);

    // The three names a loadable module publishes and the marking that
    // publishes them. On COFF nothing leaves a DLL unnamed, so the export
    // storage class is set HERE rather than through a .def: the compiler is
    // the only side that knows what the entry is called. Everything else this
    // object defines already has internal linkage (declareEntries and
    // createTable say why), so `/DLL` over an object marked this way exports
    // exactly the entry, its stamp and its manifest — and on ELF and Mach-O,
    // where every global symbol is exported by default, the same three are the
    // only globals there are.
    auto publish = [&](llvm::GlobalValue* gv) {
#ifdef _WIN32
        if (sharedRuntime_) gv->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
#else
        // ELF and Mach-O have no such storage class, and asking LLVM to emit
        // one for them is a question about a concept the format does not have.
        // They need nothing: a global symbol in a shared object is exported.
        (void)gv;
#endif
    };

    // The object's ABI stamp: the hash of the bronze_abi.h THIS compiler was
    // built against, as a constant the runtime's program entry compares to
    // its own before running the program (bronze_abi.h, "Drift between two
    // BUILDS"). Data in the object rather than metadata, so an object from
    // before the stamp existed fails the LINK on this very name instead of
    // running unchecked.
    //
    // Named after the entry symbol, because the stamp is the object's SECOND
    // exported name and two compiled modules in one image must not collide on
    // it. The default entry keeps the historical spelling, which is the name
    // src/rt/rt.cpp and embed_run.cpp link against — and those two entries are
    // only ever handed the module they run as `bronze_main`.
    const std::string stampSymbol = entrySymbol_ == "bronze_main"
                                        ? std::string("bronze_object_abi_fingerprint")
                                        : entrySymbol_ + "_abi_fingerprint";
    publish(new llvm::GlobalVariable(
        *llvmModule, llvm::Type::getInt32Ty(ctx), /*isConstant=*/true,
        llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), BRONZE_ABI_FINGERPRINT),
        stampSymbol));

    // The host-globals manifest: `{ uint32_t count; char names[]; }` with the
    // names NUL-terminated and back to back, exactly as bronze_abi.h's
    // loadable-module section specifies. Emitted unconditionally — a module
    // built without `--host-globals` gets one with count 0 — because a loader
    // that cannot find the symbol has learned nothing: "compiled against no
    // globals" and "not a bronze module at all" would be the same observation.
    //
    // Bytes rather than a struct type with a string in it: this is data a
    // loader parses with a memcpy of four bytes and a walk, and the emitted
    // form has to be exactly what that reader expects on every target.
    {
        std::vector<uint8_t> manifest;
        const uint32_t count = static_cast<uint32_t>(hostGlobals_.size());
        for (unsigned i = 0; i < 4; ++i) {
            manifest.push_back(static_cast<uint8_t>((count >> (8 * i)) & 0xFFu));
        }
        for (const std::string& name : hostGlobals_) {
            manifest.insert(manifest.end(), name.begin(), name.end());
            manifest.push_back(0);
        }
        llvm::Constant* init = llvm::ConstantDataArray::get(ctx, manifest);
        auto* manifestVar =
            new llvm::GlobalVariable(*llvmModule, init->getType(), /*isConstant=*/true,
                                     llvm::GlobalValue::ExternalLinkage, init,
                                     entrySymbol_ + "_host_globals");
        // The count is read as a uint32_t through the symbol's address, so the
        // symbol has to be aligned for one.
        manifestVar->setAlignment(llvm::Align(4));
        publish(manifestVar);
    }

    // The tables this object file owns: the inline-cache sites, the key remap,
    // and the module-local global and function-singleton caches. All are data
    // in THIS object, which is what gives every site a stable address and lets
    // the checks be inlined; the IL verifier has already checked every icIndex
    // against the count, so the table cannot be indexed out of range.
    const codegen_llvm::ModuleTables tables =
        codegen_llvm::createModuleTables(*llvmModule, ctx, module);

    std::vector<llvm::Function*> entries;
    if (!declareEntries(module, *llvmModule, ctx, entrySymbol_, entries, diags)) return false;
    if (llvm::Function* entryFn = llvmModule->getFunction(entrySymbol_)) publish(entryFn);

    std::vector<llvm::Function*> wrappers;
    emitCallWrappers(module, *llvmModule, ctx, abi, tables, entrySymbol_, entries, wrappers);

    // One `new.target` anywhere disables the inline `new` fast path for the
    // whole module: the fast path skips the NewTargetScope push, and
    // bronze_get_new_target — this instruction's helper — is that scope's
    // only observer, so absence of the instruction is what makes the skip
    // unobservable.
    bool moduleHasNewTarget = false;
    for (const auto& ilFunc : module.functions) {
        for (const auto& block : ilFunc.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == il::Op::GetNewTarget) moduleHasNewTarget = true;
            }
        }
    }

    // Frames before bodies: a caller sizes its frame for the regions of the
    // callees it merges (llvm_frame.h), and a callee is commonly emitted after
    // its caller, so the layouts are all planned first.
    //
    // The REPRESENTATION plan comes first of all (llvm_repr.h, stage R2): the
    // frame layout reads it, because a `dynamic` value proven never to hold a
    // heap address needs no root slot, and the property emitters read it to
    // decide which of stage R1's store-side tests are statically satisfied.
    std::vector<codegen_llvm::ReprPlan> reprPlans;
    reprPlans.reserve(module.functions.size());
    for (const il::Function& ilFunc : module.functions) {
        reprPlans.push_back(codegen_llvm::planRepr(ilFunc));
    }
    std::vector<codegen_llvm::FramePlan> plans;
    plans.reserve(module.functions.size());
    for (size_t i = 0; i < module.functions.size(); ++i) {
        plans.push_back(codegen_llvm::planFrame(module.functions[i], moduleHasNewTarget,
                                                reprPlans[i]));
    }
    const codegen_llvm::RegionPlan regions = codegen_llvm::planRegions(module, plans);
    std::vector<llvm::Function*> inlineVariants;
    declareInlineVariants(module, *llvmModule, regions, entries, inlineVariants);

    const FunctionEmitter::Context shared{ctx,     module,   abi,   tables,
                                          entries, wrappers, diags, plans,
                                          regions, inlineVariants, moduleHasNewTarget,
                                          reprPlans};
    for (size_t i = 0; i < module.functions.size(); ++i) {
        const bool frameless = regions.isMergeTarget[i];
        FunctionEmitter emitter(shared, static_cast<uint32_t>(i),
                                frameless ? inlineVariants[i] : entries[i], frameless);
        if (!emitter.emit()) return false;
        if (frameless) {
            codegen_llvm::emitFrameForwarder(shared, static_cast<uint32_t>(i), entries[i],
                                             inlineVariants[i]);
        }
    }

    // Every used TLS-block fetch becomes a load of a module-local
    // thread_local cache (llvm_abi.cpp, cacheTlsFetches). CLI-time seam:
    // BRONZE_NO_TLS_CACHE=1 keeps the per-prologue accessor calls, so a
    // bisection recompiles the module rather than flipping a runtime word —
    // the shape being A/B'd is the emitted prologue itself.
    {
        const char* env = std::getenv("BRONZE_NO_TLS_CACHE");
        if (!(env && std::strcmp(env, "1") == 0)) {
            codegen_llvm::cacheTlsFetches(*llvmModule, abi.bronze_tls_block_addr);
        }
    }
    // The two storage-class alias families, assigned by pointer provenance
    // (llvm_alias.h, tagStackAndControlAccesses). After the TLS rewrite,
    // because the shape a TLS field is read through afterwards is the phi that
    // rewrite leaves. CLI-time seam: BRONZE_NO_STORAGE_ALIAS=1 emits the
    // module without them, which is the A/B for every guard this licenses LLVM
    // to fold.
    {
        const char* env = std::getenv("BRONZE_NO_STORAGE_ALIAS");
        if (!(env && std::strcmp(env, "1") == 0)) {
            codegen_llvm::tagStackAndControlAccesses(*llvmModule, abi.bronze_tls_block_addr);
        }
    }
    // Every direct method-call edge whose callee fits the budget asks to be
    // inlined at the site (llvm_call.h, markDirectMethodInlining). After the
    // TLS rewrite, so a callee's fetch is already the cached form when its body
    // is copied into a caller that has one of its own.
    codegen_llvm::markDirectMethodInlining(*llvmModule);
    lap("ir-build");

    std::string errStr;
    llvm::raw_string_ostream os(errStr);
    if (llvm::verifyModule(*llvmModule, &os)) {
        // The IL dump is the bisection seam: a module LLVM rejects almost
        // always names an IL construct that was lowered wrong.
        std::cerr << "IL Module Verification Failed:\n" << il::print(module) << "\n";
        std::cerr << "LLVM Module Verification Error:\n" << errStr << "\n";
        diags.error(Span{}, "LLVM module verification failed: " + errStr);
        return false;
    }
    lap("llvm-verify");

    bool ok = writeObjectFile(*llvmModule, outputPath, /*pic=*/sharedRuntime_,
                              emittedPathsOut_, diags);
    lap("obj-emit");
    return ok;
}

}  // namespace bronze
