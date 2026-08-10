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

#include "il/print.h"

namespace bronze {

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
        default:
            return nullptr;
    }
}

bool LLVMBackend::emitObject(const il::Module& module, const std::string& outputPath,
                            DiagnosticSink& diags) {
    llvm::LLVMContext ctx;
    auto llvmModule = std::make_unique<llvm::Module>(module.name, ctx);

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

    for (size_t i = 0; i < module.functions.size(); ++i) {
        const auto& func = module.functions[i];
        llvm::Function* llvmFunc = llvmFunctions[i];

        llvm::BasicBlock* bb = llvm::BasicBlock::Create(ctx, "entry", llvmFunc);
        llvm::IRBuilder<> builder(bb);

        std::vector<llvm::Value*> values(func.valueCount, nullptr);

        size_t argIdx = 0;
        for (auto& arg : llvmFunc->args()) {
            arg.setName(func.params[argIdx].name);
            values[argIdx] = &arg;
            argIdx++;
        }

        for (const auto& inst : func.body) {
            switch (inst.op) {
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
                    if (inst.type == il::Type::F64 || lhs->getType()->isDoubleTy()) {
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
                    if (inst.type == il::Type::F64 || lhs->getType()->isDoubleTy()) {
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
                    if (inst.type == il::Type::F64 || lhs->getType()->isDoubleTy()) {
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
                    if (inst.type == il::Type::F64 || lhs->getType()->isDoubleTy()) {
                        values[inst.result] = builder.CreateFDiv(lhs, rhs);
                    } else {
                        values[inst.result] = builder.CreateSDiv(lhs, rhs);
                    }
                    break;
                }
                case il::Op::CmpLt: {
                    if (inst.operands.size() < 2 || inst.result == il::kNoValue) {
                        diags.error(Span{}, "Invalid operands for CmpLt");
                        return false;
                    }
                    llvm::Value* lhs = values[inst.operands[0]];
                    llvm::Value* rhs = values[inst.operands[1]];
                    if (!lhs || !rhs) {
                        diags.error(Span{}, "Undefined value in CmpLt instruction");
                        return false;
                    }
                    if (lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy()) {
                        values[inst.result] = builder.CreateFCmpOLT(lhs, rhs);
                    } else {
                        values[inst.result] = builder.CreateICmpSLT(lhs, rhs);
                    }
                    break;
                }
                case il::Op::CmpGt: {
                    if (inst.operands.size() < 2 || inst.result == il::kNoValue) {
                        diags.error(Span{}, "Invalid operands for CmpGt");
                        return false;
                    }
                    llvm::Value* lhs = values[inst.operands[0]];
                    llvm::Value* rhs = values[inst.operands[1]];
                    if (!lhs || !rhs) {
                        diags.error(Span{}, "Undefined value in CmpGt instruction");
                        return false;
                    }
                    if (lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy()) {
                        values[inst.result] = builder.CreateFCmpOGT(lhs, rhs);
                    } else {
                        values[inst.result] = builder.CreateICmpSGT(lhs, rhs);
                    }
                    break;
                }
                case il::Op::CmpEq: {
                    if (inst.operands.size() < 2 || inst.result == il::kNoValue) {
                        diags.error(Span{}, "Invalid operands for CmpEq");
                        return false;
                    }
                    llvm::Value* lhs = values[inst.operands[0]];
                    llvm::Value* rhs = values[inst.operands[1]];
                    if (!lhs || !rhs) {
                        diags.error(Span{}, "Undefined value in CmpEq instruction");
                        return false;
                    }
                    if (lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy()) {
                        values[inst.result] = builder.CreateFCmpOEQ(lhs, rhs);
                    } else {
                        values[inst.result] = builder.CreateICmpEQ(lhs, rhs);
                    }
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
                    if (inst.operands.empty() || inst.type == il::Type::Void) {
                        builder.CreateRetVoid();
                    } else {
                        llvm::Value* retVal = values[inst.operands[0]];
                        if (!retVal) {
                            diags.error(Span{}, "Undefined return value in Ret instruction");
                            return false;
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
