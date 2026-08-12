// The backend's composition root: IL module in, object file out. The body of
// each function is llvm_func.cpp; every symbol generated code links against is
// llvm_abi.cpp.

#include "codegen-llvm/llvm_backend.h"

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

namespace bronze {

using codegen_llvm::AbiFns;
using codegen_llvm::AbiGlobals;
using codegen_llvm::FunctionEmitter;
using codegen_llvm::mapILType;

namespace {

// One LLVM function per IL function, in its typed form: inference's proven
// types are the signature, so a direct call passes an f64 in a register rather
// than boxing it (docs/0010).
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
        // that name gets the runtime's spelling.
        std::string symbol = (func.name == "main") ? "bronze_main" : func.name;
        out.push_back(llvm::Function::Create(llvm::FunctionType::get(retTy, paramTys, false),
                                             llvm::Function::ExternalLinkage, symbol,
                                             &llvmModule));
    }
    return true;
}

// A function held as a VALUE is called through the uniform convention —
// `bronze_fn_code`: (env, this, argc, argv) — which knows nothing of the typed
// signature above. The wrapper is the adapter: it unpacks argv into the typed
// parameters, calls the entry, and boxes the result.
// A wrapper for a rest-parameter function ALLOCATES — it builds the rest
// array — and it does so before the entry's prologue has rooted anything.
// docs/0006's contract ("the callee roots its parameters before it can
// allocate") therefore does not cover it, and `env` and `this` arrive here as
// raw pointers in registers: a collection during that allocation moved the
// instance and left `this` pointing into from-space, which crashed
// `this.items = items` in a rest constructor under BRONZE_GC_STRESS.
//
// So the wrapper gets a root frame of its own for exactly those calls.
// `argv` needs no protection: it points into the CALLER's frame, which the
// collector updates in place, so reading it after the allocation is correct.
//
// `live` is what must survive: `env` and `this` always, plus anything an
// EARLIER call here already built. A wrapper can make two of these — the rest
// array and the `arguments` object (docs/0027 decision 3) — and the second
// allocation moves the first. Rooting only env and this left the rest array
// pointing at the arguments array that had been allocated over it, so
// `rest.length` was the whole argument count. Only BRONZE_GC_STRESS=1 showed
// it, which is the argument for that mode existing.
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
        // caller — except for a REST parameter, which is the one thing that
        // has to know how many arguments there really were (docs/0017
        // decision 2). This wrapper is the only place that can see it.
        llvm::Value* argc = argsIt++;
        llvm::Value* argv = argsIt;

        // Synthetic leading parameters come from the calling convention, not
        // from argv: the environment from the closure (docs/0007), the
        // receiver from the caller (docs/0008).
        const size_t firstSourceParam = func.firstSourceParam();
        // Both arrays are built FIRST, before any typed parameter is unpacked:
        // building either can collect, and every value loaded out of argv
        // beforehand would be stale afterwards. Each one that is already built
        // joins the next one's root frame.
        llvm::Value* argumentsArg = nullptr;
        if (func.needsArguments) {
            argumentsArg = emitWrapperArrayCall(builder, ctx, globals, {&env, &thisArg}, [&] {
                return builder.CreateCall(abi.bronze_arguments_object, {argc, argv});
            });
        }
        llvm::Value* restArg = nullptr;
        if (func.hasRestParam) {
            const uint32_t firstRest =
                static_cast<uint32_t>(func.params.size() - 1 - firstSourceParam);
            std::vector<llvm::Value**> live{&env, &thisArg};
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
            // The unguarded load is correct because `FunctionHeader::call`
            // padded argv up to the declared arity before entering here. A
            // function that owns an `arguments` object declares arity 0 so
            // that padding does not happen — `f(1)` and `f(1, undefined)` must
            // disagree about `arguments.length` — so its own reads are the one
            // place that has to check (docs/0027 decision 3).
            llvm::Value* bits = nullptr;
            if (func.needsArguments) {
                bits = builder.CreateCall(abi.bronze_arg_at,
                                          {argc, argv, builder.getInt32(sourceIndex)});
            } else {
                bits = builder.CreateLoad(
                    i64Ty, builder.CreateGEP(i64Ty, argv, builder.getInt32(sourceIndex)));
            }
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
// LLVM's default pipeline is the optimizer (docs/0001 decision 6).
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
    if (!il::verify(module, diags)) return false;

    llvm::LLVMContext ctx;
    auto llvmModule = std::make_unique<llvm::Module>(module.name, ctx);

    AbiFns abi;
    AbiGlobals abiGlobals;
    codegen_llvm::declareAbiSymbols(*llvmModule, ctx, abi, abiGlobals);

    // The module's inline-cache table, one entry per property site lowering
    // numbered (docs/0010 decision 7). It is data in THIS object file, which is
    // what gives every site a stable address and lets the check be inlined; the
    // IL verifier has already checked every icIndex against the count, so the
    // table cannot be indexed out of range.
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

    std::string errStr;
    llvm::raw_string_ostream os(errStr);
    if (llvm::verifyModule(*llvmModule, &os)) {
        // The IL dump is the bisection seam: a module LLVM rejects almost
        // always names an IL construct that was lowered wrong.
        std::cerr << "IL Module Verification Failed:\n" << il::print(module) << "\n";
        diags.error(Span{}, "LLVM module verification failed: " + errStr);
        return false;
    }

    return writeObjectFile(*llvmModule, outputPath, diags);
}

}  // namespace bronze
