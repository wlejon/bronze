// The backend's composition root: IL module in, object file out. The body of
// each function is llvm_func.cpp; every symbol generated code links against is
// llvm_abi.cpp.

#include "codegen-llvm/llvm_backend.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
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

static_assert(LLVM_VERSION_MAJOR >= 20, "Bronze LLVM backend requires LLVM 20 or higher");

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_abi.h"
#include "codegen-llvm/llvm_func.h"
#include "il/print.h"
#include "il/verifier.h"
#include "support/timings.h"

namespace bronze {

using codegen_llvm::AbiFns;
using codegen_llvm::AbiGlobals;
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
                      const AbiFns& abi, const std::string& entrySymbol,
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
            switch (func.params[p].type) {
                case il::Type::F64:
                    callArgs.push_back(builder.CreateCall(abi.bronze_unbox_f64, {bits}));
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

// Host target machine → object file. Runs LLVM's PassBuilder O3 optimization pipeline
// and targets the host CPU and instruction set extensions.
//
// `pic` asks for position-independent code, and it is the one thing about this
// function that `--emit-shared` changes. bronze's ordinary output is NOT
// position-independent — which is why the driver links executables `-no-pie` on
// Linux and why tests/two_module links its host the same way — and on ELF
// x86-64 a non-PIC object simply cannot go into a shared object: GNU ld refuses
// the R_X86_64_32S relocations with "can not be used when making a shared
// object; recompile with -fPIC". A loadable module IS a shared object, so it is
// compiled as one. Mach-O is position-independent throughout and COFF has no
// such concept, so this is a no-op on both; asking for it uniformly under the
// shared flag is still right, because what it expresses is "this object is
// going into a library", which is true on all three.
bool writeObjectFile(llvm::Module& llvmModule, const std::string& outputPath, bool pic,
                     DiagnosticSink& diags) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    llvm::Triple targetTriple(llvm::sys::getDefaultTargetTriple());
    llvmModule.setTargetTriple(targetTriple);

    std::string lookupError;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(targetTriple, lookupError);
    if (!target) {
        diags.error(Span{}, "Failed to lookup host target: " + lookupError);
        return false;
    }

    std::string cpu = std::string(llvm::sys::getHostCPUName());
    llvm::SubtargetFeatures features;
    for (const auto& [feature, enabled] : llvm::sys::getHostCPUFeatures()) {
        features.AddFeature(feature, enabled);
    }

    llvm::TargetOptions opt;
    // std::nullopt is LLVM's "pick the target's default", which is what every
    // static build has always emitted and what must not change here.
    const std::optional<llvm::Reloc::Model> reloc =
        pic ? std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_) : std::nullopt;
    std::unique_ptr<llvm::TargetMachine> targetMachine(target->createTargetMachine(
        targetTriple, cpu, features.getString(), opt,
        reloc, std::nullopt, llvm::CodeGenOptLevel::Aggressive));
    if (!targetMachine) {
        diags.error(Span{}, "Failed to create LLVM target machine");
        return false;
    }
    llvmModule.setDataLayout(targetMachine->createDataLayout());

    // Run LLVM middle-end optimization pipeline (PassBuilder O3)
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;

    llvm::PassBuilder pb(targetMachine.get());

    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    mpm.run(llvmModule, mam);

    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        diags.error(Span{}, "Could not open output file " + outputPath + ": " + ec.message());
        return false;
    }

    constexpr auto kObjFileType = llvm::CodeGenFileType::ObjectFile;

    llvm::legacy::PassManager pass;
    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, kObjFileType)) {
        diags.error(Span{}, "Target machine cannot emit object file for this target");
        return false;
    }
    pass.run(llvmModule);
    dest.flush();
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
    codegen_llvm::declareAbiSymbols(*llvmModule, ctx, abi);

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
    emitCallWrappers(module, *llvmModule, ctx, abi, entrySymbol_, entries, wrappers);

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

    const FunctionEmitter::Context shared{ctx,     module,   abi,   tables,
                                          entries, wrappers, diags, moduleHasNewTarget};
    for (size_t i = 0; i < module.functions.size(); ++i) {
        FunctionEmitter emitter(shared, module.functions[i], entries[i]);
        if (!emitter.emit()) return false;
    }
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

    bool ok = writeObjectFile(*llvmModule, outputPath, /*pic=*/sharedRuntime_, diags);
    lap("obj-emit");
    return ok;
}

}  // namespace bronze
