#include "codegen-llvm/llvm_backend.h"

#include <iostream>
#include <memory>
#include <system_error>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
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
#include "il/print.h"
#include "il/verifier.h"

namespace bronze {

// One llvm::Function* per entry in the ABI registry, named after the
// runtime symbol itself; populated in emitObject from the same X-macro
// list that declares the C prototypes, so the two sides cannot drift.
struct AbiFns {
#define BRONZE_ABI_FIELD(name, RET, PARAMS) llvm::Function* name;
    BRONZE_ABI_FUNCTIONS(BRONZE_ABI_FIELD)
#undef BRONZE_ABI_FIELD
};

static llvm::Type* mapILType(il::Type type, llvm::LLVMContext& ctx) {
    switch (type) {
        case il::Type::Void:
            return llvm::Type::getVoidTy(ctx);
        case il::Type::Bool:
            return llvm::Type::getInt1Ty(ctx);
        case il::Type::I32:
            return llvm::Type::getInt32Ty(ctx);
        case il::Type::F64:
            return llvm::Type::getDoubleTy(ctx);
        case il::Type::Str:
            return llvm::PointerType::getUnqual(ctx);
        case il::Type::Dynamic:
            return llvm::Type::getInt64Ty(ctx);
        default:
            return nullptr;
    }
}

bool LLVMBackend::emitObject(const il::Module& module, const std::string& outputPath,
                             DiagnosticSink& diags) {
    if (!il::verify(module, diags)) return false;
    llvm::LLVMContext ctx;
    auto llvmModule = std::make_unique<llvm::Module>(module.name, ctx);

    auto getOrDeclareFunc = [&](const std::string& name, llvm::FunctionType* fty) -> llvm::Function* {
        llvm::Function* fn = llvmModule->getFunction(name);
        if (!fn) {
            fn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, name, llvmModule.get());
        }
        return fn;
    };

    // Declare every ABI helper from the registry (src/abi/bronze_abi.h) by
    // rebinding its type tokens to llvm::Type*.
#define BRONZE_ABI_U64    llvm::Type::getInt64Ty(ctx)
#define BRONZE_ABI_U32    llvm::Type::getInt32Ty(ctx)
#define BRONZE_ABI_I32    llvm::Type::getInt32Ty(ctx)
#define BRONZE_ABI_F64    llvm::Type::getDoubleTy(ctx)
#define BRONZE_ABI_BOOL   llvm::Type::getInt1Ty(ctx)
#define BRONZE_ABI_CSTR   llvm::PointerType::getUnqual(ctx)
#define BRONZE_ABI_PU64   llvm::PointerType::getUnqual(ctx)
#define BRONZE_ABI_FNPTR  llvm::PointerType::getUnqual(ctx)
#define BRONZE_ABI_VOID   llvm::Type::getVoidTy(ctx)
#define BRONZE_ABI_NOARGS
#define BRONZE_ABI_UNPAREN(...) __VA_ARGS__
    AbiFns abi;
#define BRONZE_ABI_LLVM_DECLARE(name, RET, PARAMS) \
    abi.name = getOrDeclareFunc(#name, \
        llvm::FunctionType::get(RET, {BRONZE_ABI_UNPAREN PARAMS}, false));
    BRONZE_ABI_FUNCTIONS(BRONZE_ABI_LLVM_DECLARE)
#undef BRONZE_ABI_LLVM_DECLARE
#undef BRONZE_ABI_UNPAREN
#undef BRONZE_ABI_U64
#undef BRONZE_ABI_U32
#undef BRONZE_ABI_I32
#undef BRONZE_ABI_F64
#undef BRONZE_ABI_BOOL
#undef BRONZE_ABI_CSTR
#undef BRONZE_ABI_PU64
#undef BRONZE_ABI_FNPTR
#undef BRONZE_ABI_VOID
#undef BRONZE_ABI_NOARGS

    std::vector<llvm::Function*> llvmFunctions;
    llvmFunctions.reserve(module.functions.size());

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

        llvm::FunctionType* funcTy = llvm::FunctionType::get(retTy, paramTys, false);
        std::string symbol = (func.name == "main") ? "bronze_main" : func.name;
        llvm::Function* llvmFunc = llvm::Function::Create(
            funcTy, llvm::Function::ExternalLinkage, symbol, llvmModule.get());
        llvmFunctions.push_back(llvmFunc);
    }

    llvm::FunctionType* wrapperTy = llvm::FunctionType::get(
        llvm::Type::getInt64Ty(ctx),
        {llvm::Type::getInt64Ty(ctx), llvm::Type::getInt32Ty(ctx), llvm::PointerType::getUnqual(ctx)},
        false);
    std::vector<llvm::Function*> wrapperFunctions(module.functions.size());

    for (size_t i = 0; i < module.functions.size(); ++i) {
        const auto& func = module.functions[i];
        std::string wName = "__wrapper_" + (func.name == "main" ? "bronze_main" : func.name);
        llvm::Function* wFunc = llvm::Function::Create(wrapperTy, llvm::Function::InternalLinkage, wName, llvmModule.get());
        wrapperFunctions[i] = wFunc;

        llvm::BasicBlock* wBb = llvm::BasicBlock::Create(ctx, "entry", wFunc);
        llvm::IRBuilder<> wBuilder(wBb);

        auto argsIt = wFunc->arg_begin();
        llvm::Value* wThisArg = argsIt++;
        (void)wThisArg;
        llvm::Value* wArgc = argsIt++;
        (void)wArgc;
        llvm::Value* wArgv = argsIt;

        std::vector<llvm::Value*> callArgs;
        for (size_t p = 0; p < func.params.size(); ++p) {
            llvm::Value* slotPtr = wBuilder.CreateGEP(wBuilder.getInt64Ty(), wArgv, wBuilder.getInt32(static_cast<uint32_t>(p)));
            llvm::Value* rawBits = wBuilder.CreateLoad(wBuilder.getInt64Ty(), slotPtr);
            il::Type pType = func.params[p].type;
            if (pType == il::Type::F64) {
                callArgs.push_back(wBuilder.CreateCall(abi.bronze_unbox_f64, {rawBits}));
            } else if (pType == il::Type::I32) {
                callArgs.push_back(wBuilder.CreateCall(abi.bronze_unbox_i32, {rawBits}));
            } else if (pType == il::Type::Bool) {
                callArgs.push_back(wBuilder.CreateCall(abi.bronze_unbox_bool, {rawBits}));
            } else {
                callArgs.push_back(rawBits);
            }
        }

        llvm::Value* callRes = wBuilder.CreateCall(llvmFunctions[i], callArgs);
        if (func.returnType == il::Type::Void) {
            wBuilder.CreateRet(wBuilder.getInt64(0xFFF6000000000000ULL));
        } else if (func.returnType == il::Type::F64) {
            wBuilder.CreateRet(wBuilder.CreateCall(abi.bronze_box_f64, {callRes}));
        } else if (func.returnType == il::Type::I32) {
            wBuilder.CreateRet(wBuilder.CreateCall(abi.bronze_box_i32, {callRes}));
        } else if (func.returnType == il::Type::Bool) {
            wBuilder.CreateRet(wBuilder.CreateCall(abi.bronze_box_bool, {callRes}));
        } else if (func.returnType == il::Type::Dynamic) {
            wBuilder.CreateRet(callRes);
        } else {
            wBuilder.CreateRet(callRes);
        }
    }

    for (size_t i = 0; i < module.functions.size(); ++i) {
        const auto& func = module.functions[i];
        llvm::Function* llvmFunc = llvmFunctions[i];

        std::vector<llvm::BasicBlock*> llvmBlocks;
        llvmBlocks.reserve(func.blocks.size());
        for (size_t bIdx = 0; bIdx < func.blocks.size(); ++bIdx) {
            std::string bName = "b" + std::to_string(func.blocks[bIdx].id);
            llvmBlocks.push_back(llvm::BasicBlock::Create(ctx, bName, llvmFunc));
        }

        std::vector<llvm::Value*> values(func.valueCount, nullptr);

        size_t argIdx = 0;
        for (auto& arg : llvmFunc->args()) {
            arg.setName(func.params[argIdx].name);
            values[argIdx] = &arg;
            argIdx++;
        }

        std::vector<std::vector<llvm::PHINode*>> blockPhis(func.blocks.size());
        for (size_t bIdx = 0; bIdx < func.blocks.size(); ++bIdx) {
            const auto& block = func.blocks[bIdx];
            if (block.params.empty()) continue;
            llvm::IRBuilder<> phiBuilder(llvmBlocks[bIdx]);
            blockPhis[bIdx].reserve(block.params.size());
            for (const auto& param : block.params) {
                llvm::Type* pTy = mapILType(param.type, ctx);
                llvm::PHINode* phi = phiBuilder.CreatePHI(pTy, 0, "p" + std::to_string(param.id));
                values[param.id] = phi;
                blockPhis[bIdx].push_back(phi);
            }
        }

        if (func.name == "main" && !llvmBlocks.empty()) {
            llvm::IRBuilder<> entryBuilder(llvmBlocks[0]);
            for (size_t k = 0; k < module.keyConstants.size(); ++k) {
                llvm::Value* globalStr = entryBuilder.CreateGlobalStringPtr(module.keyConstants[k]);
                entryBuilder.CreateCall(abi.bronze_register_key_string, {entryBuilder.getInt32(static_cast<uint32_t>(k)), globalStr});
            }
        }

        for (size_t bIdx = 0; bIdx < func.blocks.size(); ++bIdx) {
            const auto& block = func.blocks[bIdx];
            llvm::BasicBlock* bb = llvmBlocks[bIdx];
            llvm::IRBuilder<> builder(bb);

            for (const auto& inst : block.instructions) {
                switch (inst.op) {
                    case il::Op::Jump: {
                        llvm::BasicBlock* tgtBb = llvmBlocks[inst.target.block];
                        for (size_t k = 0; k < inst.target.args.size(); ++k) {
                            llvm::Value* argVal = values[inst.target.args[k]];
                            blockPhis[inst.target.block][k]->addIncoming(argVal, bb);
                        }
                        builder.CreateBr(tgtBb);
                        break;
                    }
                    case il::Op::Branch: {
                        llvm::Value* condVal = values[inst.operands[0]];
                        llvm::BasicBlock* thenBb = llvmBlocks[inst.target.block];
                        for (size_t k = 0; k < inst.target.args.size(); ++k) {
                            llvm::Value* argVal = values[inst.target.args[k]];
                            blockPhis[inst.target.block][k]->addIncoming(argVal, bb);
                        }
                        llvm::BasicBlock* elseBb = llvmBlocks[inst.elseTarget.block];
                        for (size_t k = 0; k < inst.elseTarget.args.size(); ++k) {
                            llvm::Value* argVal = values[inst.elseTarget.args[k]];
                            blockPhis[inst.elseTarget.block][k]->addIncoming(argVal, bb);
                        }
                        builder.CreateCondBr(condVal, thenBb, elseBb);
                        break;
                    }
                case il::Op::ConstF64: {
                    if (inst.result == il::kNoValue) break;
                    values[inst.result] = llvm::ConstantFP::get(builder.getDoubleTy(), inst.immF64);
                    break;
                }
                case il::Op::ConstI32: {
                    if (inst.result == il::kNoValue) break;
                    values[inst.result] = builder.getInt32(inst.immI32);
                    break;
                }
                case il::Op::ConstBool: {
                    if (inst.result == il::kNoValue) break;
                    values[inst.result] = builder.getInt1(inst.immI32 != 0);
                    break;
                }
                case il::Op::ConstUndefined: {
                    if (inst.result == il::kNoValue) break;
                    values[inst.result] = builder.getInt64(BRONZE_ABI_UNDEFINED_BITS);
                    break;
                }
                case il::Op::ConstNull: {
                    if (inst.result == il::kNoValue) break;
                    values[inst.result] = builder.getInt64(BRONZE_ABI_NULL_BITS);
                    break;
                }
                case il::Op::Box: {
                    if (inst.result == il::kNoValue) break;
                    if (inst.boxType == il::Type::Str) {
                        llvm::Value* kIdx = builder.getInt32(inst.keyIndex);
                        values[inst.result] = builder.CreateCall(abi.bronze_box_str_key, {kIdx});
                    } else {
                        if (inst.operands.empty()) {
                            diags.error(Span{}, "Invalid operands for Box");
                            return false;
                        }
                        llvm::Value* srcVal = values[inst.operands[0]];
                        if (!srcVal) {
                            diags.error(Span{}, "Undefined value in Box instruction");
                            return false;
                        }
                        llvm::Function* targetBoxFn = abi.bronze_box_f64;
                        if (inst.boxType == il::Type::I32) targetBoxFn = abi.bronze_box_i32;
                        else if (inst.boxType == il::Type::Bool) targetBoxFn = abi.bronze_box_bool;
                        values[inst.result] = builder.CreateCall(targetBoxFn, {srcVal});
                    }
                    break;
                }
                case il::Op::Unbox: {
                    if (inst.operands.empty() || inst.result == il::kNoValue) {
                        diags.error(Span{}, "Invalid operands for Unbox");
                        return false;
                    }
                    llvm::Value* srcVal = values[inst.operands[0]];
                    if (!srcVal) {
                        diags.error(Span{}, "Undefined value in Unbox instruction");
                        return false;
                    }
                    llvm::Function* targetUnboxFn = abi.bronze_unbox_f64;
                    if (inst.type == il::Type::I32) targetUnboxFn = abi.bronze_unbox_i32;
                    else if (inst.type == il::Type::Bool) targetUnboxFn = abi.bronze_unbox_bool;
                    values[inst.result] = builder.CreateCall(targetUnboxFn, {srcVal});
                    break;
                }
                case il::Op::CreateObject: {
                    if (inst.result != il::kNoValue) {
                        values[inst.result] = builder.CreateCall(abi.bronze_create_object, {});
                    }
                    break;
                }
                case il::Op::CreateArray: {
                    if (inst.result != il::kNoValue) {
                        llvm::Value* lenVal = builder.getInt32(inst.immI32);
                        values[inst.result] = builder.CreateCall(abi.bronze_create_array, {lenVal});
                    }
                    break;
                }
                case il::Op::CreateFunction: {
                    if (inst.result != il::kNoValue) {
                        llvm::Value* targetWrapper = wrapperFunctions[inst.calleeIndex];
                        llvm::Value* arityVal = builder.getInt32(inst.immI32);
                        values[inst.result] = builder.CreateCall(abi.bronze_create_function, {targetWrapper, arityVal});
                    }
                    break;
                }
                case il::Op::Print: {
                    if (!inst.operands.empty()) {
                        llvm::Value* val = values[inst.operands[0]];
                        if (val) {
                            builder.CreateCall(abi.bronze_print_value, {val});
                        }
                    }
                    break;
                }
                case il::Op::PropGet: {
                    if (inst.operands.empty() || inst.result == il::kNoValue) {
                        diags.error(Span{}, "Invalid operands for PropGet");
                        return false;
                    }
                    llvm::Value* objVal = values[inst.operands[0]];
                    if (!objVal) {
                        diags.error(Span{}, "Undefined object in PropGet instruction");
                        return false;
                    }
                    llvm::Value* kIdx = builder.getInt32(inst.keyIndex);
                    llvm::Value* icIdx = builder.getInt32(inst.icIndex);
                    values[inst.result] = builder.CreateCall(abi.bronze_prop_get, {objVal, kIdx, icIdx});
                    break;
                }
                case il::Op::PropSet: {
                    if (inst.operands.size() < 2) {
                        diags.error(Span{}, "Invalid operands for PropSet");
                        return false;
                    }
                    llvm::Value* objVal = values[inst.operands[0]];
                    llvm::Value* valVal = values[inst.operands[1]];
                    if (!objVal || !valVal) {
                        diags.error(Span{}, "Undefined operand in PropSet instruction");
                        return false;
                    }
                    llvm::Value* kIdx = builder.getInt32(inst.keyIndex);
                    llvm::Value* icIdx = builder.getInt32(inst.icIndex);
                    builder.CreateCall(abi.bronze_prop_set, {objVal, kIdx, valVal, icIdx});
                    break;
                }
                case il::Op::DynamicCall: {
                    if (inst.operands.size() < 2) {
                        diags.error(Span{}, "Invalid operands for DynamicCall");
                        return false;
                    }
                    llvm::Value* calleeVal = values[inst.operands[0]];
                    llvm::Value* thisVal = values[inst.operands[1]];
                    if (!calleeVal || !thisVal) {
                        diags.error(Span{}, "Undefined callee or this in DynamicCall instruction");
                        return false;
                    }
                    uint32_t argc = static_cast<uint32_t>(inst.operands.size() - 2);
                    llvm::Value* argvPtr = nullptr;
                    if (argc > 0) {
                        llvm::Value* arrLen = builder.getInt32(argc);
                        argvPtr = builder.CreateAlloca(builder.getInt64Ty(), arrLen, "argv");
                        for (uint32_t a = 0; a < argc; ++a) {
                            llvm::Value* argV = values[inst.operands[2 + a]];
                            if (!argV) {
                                diags.error(Span{}, "Undefined argument in DynamicCall instruction");
                                return false;
                            }
                            llvm::Value* slotPtr = builder.CreateGEP(builder.getInt64Ty(), argvPtr, builder.getInt32(a));
                            builder.CreateStore(argV, slotPtr);
                        }
                    } else {
                        argvPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx));
                    }
                    llvm::Value* callRes = builder.CreateCall(abi.bronze_dynamic_call, {calleeVal, thisVal, builder.getInt32(argc), argvPtr});
                    if (inst.result != il::kNoValue) {
                        values[inst.result] = callRes;
                    }
                    break;
                }
                case il::Op::Add: {
                    if (inst.operands.size() < 2 || inst.result == il::kNoValue) {
                        diags.error(Span{}, "Invalid operands for Add");
                        return false;
                    }
                    llvm::Value* lhs = values[inst.operands[0]];
                    llvm::Value* rhs = values[inst.operands[1]];
                    if (!lhs || !rhs) {
                        diags.error(Span{}, "Undefined value in Add instruction");
                        return false;
                    }
                    if (inst.type == il::Type::Dynamic) {
                        values[inst.result] = builder.CreateCall(abi.bronze_dynamic_add, {lhs, rhs});
                    } else if (inst.type == il::Type::Str) {
                        values[inst.result] = builder.CreateCall(abi.bronze_string_concat, {lhs, rhs});
                    } else if (inst.type == il::Type::F64 || lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy()) {
                        if (lhs->getType()->isIntegerTy(1)) lhs = builder.CreateUIToFP(lhs, builder.getDoubleTy());
                        if (rhs->getType()->isIntegerTy(1)) rhs = builder.CreateUIToFP(rhs, builder.getDoubleTy());
                        values[inst.result] = builder.CreateFAdd(lhs, rhs);
                    } else {
                        values[inst.result] = builder.CreateAdd(lhs, rhs);
                    }
                    break;
                }
                case il::Op::Sub: {
                    if (inst.operands.size() < 2 || inst.result == il::kNoValue) {
                        diags.error(Span{}, "Invalid operands for Sub");
                        return false;
                    }
                    llvm::Value* lhs = values[inst.operands[0]];
                    llvm::Value* rhs = values[inst.operands[1]];
                    if (!lhs || !rhs) {
                        diags.error(Span{}, "Undefined value in Sub instruction");
                        return false;
                    }
                    if (inst.type == il::Type::F64 || lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy()) {
                        if (lhs->getType()->isIntegerTy(1)) lhs = builder.CreateUIToFP(lhs, builder.getDoubleTy());
                        if (rhs->getType()->isIntegerTy(1)) rhs = builder.CreateUIToFP(rhs, builder.getDoubleTy());
                        values[inst.result] = builder.CreateFSub(lhs, rhs);
                    } else {
                        values[inst.result] = builder.CreateSub(lhs, rhs);
                    }
                    break;
                }
                case il::Op::Mul: {
                    if (inst.operands.size() < 2 || inst.result == il::kNoValue) {
                        diags.error(Span{}, "Invalid operands for Mul");
                        return false;
                    }
                    llvm::Value* lhs = values[inst.operands[0]];
                    llvm::Value* rhs = values[inst.operands[1]];
                    if (!lhs || !rhs) {
                        diags.error(Span{}, "Undefined value in Mul instruction");
                        return false;
                    }
                    if (inst.type == il::Type::F64 || lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy()) {
                        if (lhs->getType()->isIntegerTy(1)) lhs = builder.CreateUIToFP(lhs, builder.getDoubleTy());
                        if (rhs->getType()->isIntegerTy(1)) rhs = builder.CreateUIToFP(rhs, builder.getDoubleTy());
                        values[inst.result] = builder.CreateFMul(lhs, rhs);
                    } else {
                        values[inst.result] = builder.CreateMul(lhs, rhs);
                    }
                    break;
                }
                case il::Op::Div: {
                    if (inst.operands.size() < 2 || inst.result == il::kNoValue) {
                        diags.error(Span{}, "Invalid operands for Div");
                        return false;
                    }
                    llvm::Value* lhs = values[inst.operands[0]];
                    llvm::Value* rhs = values[inst.operands[1]];
                    if (!lhs || !rhs) {
                        diags.error(Span{}, "Undefined value in Div instruction");
                        return false;
                    }
                    if (inst.type == il::Type::F64 || lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy()) {
                        if (lhs->getType()->isIntegerTy(1)) lhs = builder.CreateUIToFP(lhs, builder.getDoubleTy());
                        if (rhs->getType()->isIntegerTy(1)) rhs = builder.CreateUIToFP(rhs, builder.getDoubleTy());
                        values[inst.result] = builder.CreateFDiv(lhs, rhs);
                    } else {
                        values[inst.result] = builder.CreateSDiv(lhs, rhs);
                    }
                    break;
                }
                case il::Op::CmpLt:
                case il::Op::CmpGt:
                case il::Op::CmpEq:
                case il::Op::CmpNe: {
                    // Lowering guarantees same-typed operands here (dynamic
                    // comparisons go through unbox or strict.eq). Anything
                    // else is a lowering bug — never coerce raw bits.
                    if (inst.operands.size() < 2 || inst.result == il::kNoValue) {
                        diags.error(Span{}, std::string("Invalid operands for ") + il::opName(inst.op));
                        return false;
                    }
                    llvm::Value* lhs = values[inst.operands[0]];
                    llvm::Value* rhs = values[inst.operands[1]];
                    if (!lhs || !rhs) {
                        diags.error(Span{}, std::string("Undefined value in ") + il::opName(inst.op) + " instruction");
                        return false;
                    }
                    if (lhs->getType() != rhs->getType()) {
                        diags.error(Span{}, std::string("Mismatched operand types in ") + il::opName(inst.op) +
                                                " (lowering bug)");
                        return false;
                    }
                    if (lhs->getType()->isDoubleTy()) {
                        switch (inst.op) {
                            case il::Op::CmpLt: values[inst.result] = builder.CreateFCmpOLT(lhs, rhs); break;
                            case il::Op::CmpGt: values[inst.result] = builder.CreateFCmpOGT(lhs, rhs); break;
                            case il::Op::CmpEq: values[inst.result] = builder.CreateFCmpOEQ(lhs, rhs); break;
                            default: values[inst.result] = builder.CreateFCmpONE(lhs, rhs); break;
                        }
                    } else if (lhs->getType()->isIntegerTy(1) &&
                               (inst.op == il::Op::CmpEq || inst.op == il::Op::CmpNe)) {
                        values[inst.result] = inst.op == il::Op::CmpEq ? builder.CreateICmpEQ(lhs, rhs)
                                                                       : builder.CreateICmpNE(lhs, rhs);
                    } else if (lhs->getType()->isIntegerTy(32)) {
                        switch (inst.op) {
                            case il::Op::CmpLt: values[inst.result] = builder.CreateICmpSLT(lhs, rhs); break;
                            case il::Op::CmpGt: values[inst.result] = builder.CreateICmpSGT(lhs, rhs); break;
                            case il::Op::CmpEq: values[inst.result] = builder.CreateICmpEQ(lhs, rhs); break;
                            default: values[inst.result] = builder.CreateICmpNE(lhs, rhs); break;
                        }
                    } else {
                        diags.error(Span{}, std::string("Unsupported operand type in ") + il::opName(inst.op));
                        return false;
                    }
                    break;
                }
                case il::Op::StrictEq: {
                    if (inst.operands.size() < 2 || inst.result == il::kNoValue) {
                        diags.error(Span{}, "Invalid operands for StrictEq");
                        return false;
                    }
                    llvm::Value* lhs = values[inst.operands[0]];
                    llvm::Value* rhs = values[inst.operands[1]];
                    if (!lhs || !rhs) {
                        diags.error(Span{}, "Undefined value in StrictEq instruction");
                        return false;
                    }
                    values[inst.result] = builder.CreateCall(abi.bronze_strict_eq, {lhs, rhs});
                    break;
                }
                case il::Op::Mod: {
                    if (inst.operands.size() < 2 || inst.result == il::kNoValue) {
                        diags.error(Span{}, "Invalid operands for Mod");
                        return false;
                    }
                    llvm::Value* lhs = values[inst.operands[0]];
                    llvm::Value* rhs = values[inst.operands[1]];
                    if (!lhs || !rhs) {
                        diags.error(Span{}, "Undefined value in Mod instruction");
                        return false;
                    }
                    if (inst.type == il::Type::F64 || lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy()) {
                        if (lhs->getType()->isIntegerTy(1)) lhs = builder.CreateUIToFP(lhs, builder.getDoubleTy());
                        if (rhs->getType()->isIntegerTy(1)) rhs = builder.CreateUIToFP(rhs, builder.getDoubleTy());
                        values[inst.result] = builder.CreateFRem(lhs, rhs);
                    } else {
                        values[inst.result] = builder.CreateSRem(lhs, rhs);
                    }
                    break;
                }
                case il::Op::IsNullish: {
                    if (inst.operands.empty() || inst.result == il::kNoValue) {
                        diags.error(Span{}, "Invalid operands for IsNullish");
                        return false;
                    }
                    llvm::Value* srcVal = values[inst.operands[0]];
                    values[inst.result] = builder.CreateCall(abi.bronze_is_nullish, {srcVal});
                    break;
                }
                case il::Op::Call: {
                    if (inst.calleeIndex >= llvmFunctions.size()) {
                        diags.error(Span{}, "Invalid callee index in Call instruction");
                        return false;
                    }
                    llvm::Function* callee = llvmFunctions[inst.calleeIndex];
                    std::vector<llvm::Value*> args;
                    args.reserve(inst.operands.size());
                    for (auto opId : inst.operands) {
                        llvm::Value* argVal = values[opId];
                        if (!argVal) {
                            diags.error(Span{}, "Undefined argument in Call instruction");
                            return false;
                        }
                        args.push_back(argVal);
                    }
                    if (inst.result != il::kNoValue && !callee->getReturnType()->isVoidTy()) {
                        values[inst.result] = builder.CreateCall(callee, args);
                    } else {
                        builder.CreateCall(callee, args);
                    }
                    break;
                }
                case il::Op::Ret: {
                    if (inst.operands.empty()) {
                        builder.CreateRetVoid();
                    } else {
                        llvm::Value* retVal = values[inst.operands[0]];
                        if (!retVal) {
                            diags.error(Span{}, "Undefined return value in Ret instruction");
                            return false;
                        }
                        if (llvmFunc->getReturnType()->isDoubleTy() && retVal->getType()->isIntegerTy(1)) {
                            retVal = builder.CreateUIToFP(retVal, builder.getDoubleTy());
                        }
                        builder.CreateRet(retVal);
                    }
                    break;
                }
                default:
                    diags.error(Span{}, "Unsupported IL instruction opcode");
                    return false;
            }
            }
        }
    }

    std::string errStr;
    llvm::raw_string_ostream os(errStr);
    if (llvm::verifyModule(*llvmModule, &os)) {
        std::cerr << "IL Module Verification Failed:\n" << il::print(module) << "\n";
        diags.error(Span{}, "LLVM module verification failed: " + errStr);
        return false;
    }

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    std::string targetTriple = llvm::sys::getDefaultTargetTriple();
    llvmModule->setTargetTriple(targetTriple);

    std::string lookupError;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(targetTriple, lookupError);
    if (!target) {
        diags.error(Span{}, "Failed to lookup host target: " + lookupError);
        return false;
    }

    auto cpu = "generic";
    auto features = "";
    llvm::TargetOptions opt;
    auto rm = std::optional<llvm::Reloc::Model>();
    auto targetMachine = target->createTargetMachine(targetTriple, cpu, features, opt, rm);
    if (!targetMachine) {
        diags.error(Span{}, "Failed to create LLVM target machine");
        return false;
    }

    llvmModule->setDataLayout(targetMachine->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        diags.error(Span{}, "Could not open output file " + outputPath + ": " + ec.message());
        return false;
    }

    llvm::legacy::PassManager pass;
    auto fileType = llvm::CodeGenFileType::ObjectFile;

    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
        diags.error(Span{}, "Target machine cannot emit object file for this target");
        return false;
    }

    pass.run(*llvmModule);
    dest.flush();

    return true;
}

}  // namespace bronze
