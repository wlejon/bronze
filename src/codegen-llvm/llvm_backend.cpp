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
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

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
                    std::vector<llvm::Function*>& out, DiagnosticSink& diags) {
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
        // `main` is the runtime's entry point by name, so the IL function of
        // that name gets the runtime's spelling — and it is the ONLY function
        // the object exports. A program is compiled whole into one object, so
        // every other function is internal; external linkage here handed a JS
        // function named `bind` to the system linker, where it collided with
        // ws2_32's export of the same name.
        const bool isEntry = (func.name == "main");
        const std::string symbol = isEntry ? "bronze_main" : func.name;
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
                      const AbiFns& abi, const AbiGlobals& globals,
                      const std::vector<llvm::Function*>& entries,
                      std::vector<llvm::Function*>& out) {
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::FunctionType* wrapperTy = llvm::FunctionType::get(
        i64Ty, {i64Ty, i64Ty, llvm::Type::getInt32Ty(ctx), llvm::PointerType::getUnqual(ctx)},
        false);

    out.resize(module.functions.size());
    for (size_t i = 0; i < module.functions.size(); ++i) {
        const auto& func = module.functions[i];
        std::string name = "__wrapper_" + (func.name == "main" ? "bronze_main" : func.name);
        llvm::Function* wrapper = llvm::Function::Create(
            wrapperTy, llvm::Function::InternalLinkage, name, &llvmModule);
        out[i] = wrapper;

        llvm::IRBuilder<> builder(llvm::BasicBlock::Create(ctx, "entry", wrapper));
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

// Host target machine → object file. Nothing bronze-specific happens here;
// LLVM's default pipeline is the optimizer.
bool writeObjectFile(llvm::Module& llvmModule, const std::string& outputPath,
                     DiagnosticSink& diags) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    std::string targetTriple = llvm::sys::getDefaultTargetTriple();
    llvmModule.setTargetTriple(targetTriple);

    std::string lookupError;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(targetTriple, lookupError);
    if (!target) {
        diags.error(Span{}, "Failed to lookup host target: " + lookupError);
        return false;
    }

    llvm::TargetOptions opt;
    auto targetMachine = target->createTargetMachine(targetTriple, "generic", "", opt,
                                                     std::optional<llvm::Reloc::Model>());
    if (!targetMachine) {
        diags.error(Span{}, "Failed to create LLVM target machine");
        return false;
    }
    llvmModule.setDataLayout(targetMachine->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        diags.error(Span{}, "Could not open output file " + outputPath + ": " + ec.message());
        return false;
    }

    llvm::legacy::PassManager pass;
    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr,
                                           llvm::CodeGenFileType::ObjectFile)) {
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
    AbiGlobals abiGlobals;
    codegen_llvm::declareAbiSymbols(*llvmModule, ctx, abi, abiGlobals);

    // The module's inline-cache table, one entry per property site lowering
    // numbered. It is data in THIS object file, which is what gives every site
    // a stable address and lets the check be inlined; the IL verifier has
    // already checked every icIndex against the count, so the table cannot be
    // indexed out of range.
    llvm::GlobalVariable* icTable =
        codegen_llvm::createIcTable(*llvmModule, ctx, module.icSiteCount);

    std::vector<llvm::Function*> entries;
    if (!declareEntries(module, *llvmModule, ctx, entries, diags)) return false;

    std::vector<llvm::Function*> wrappers;
    emitCallWrappers(module, *llvmModule, ctx, abi, abiGlobals, entries, wrappers);

    const FunctionEmitter::Context shared{ctx,      module,   abi,      abiGlobals,
                                          icTable,  entries,  wrappers, diags};
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
        diags.error(Span{}, "LLVM module verification failed: " + errStr);
        return false;
    }
    lap("llvm-verify");

    bool ok = writeObjectFile(*llvmModule, outputPath, diags);
    lap("obj-emit");
    return ok;
}

}  // namespace bronze
