#include <locale>
#include <string>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_call.h"
#include "codegen-llvm/llvm_method_call.h"
#include "codegen-llvm/llvm_construct.h"
#include "codegen-llvm/llvm_func.h"
#include "codegen-llvm/llvm_math.h"

namespace bronze::codegen_llvm {

bool FunctionEmitter::emitDynamicCall(const il::Instruction& inst) {
    const AbiFns& abi = shared_.abi;
    auto needs = [&](size_t operandCount, bool needsResult, const char* what) {
        return require(inst.operands.size() >= operandCount &&
                           (!needsResult || inst.result != il::kNoValue),
                       what);
    };
    auto callWith = [&](llvm::Function* fn, std::initializer_list<llvm::Value*> args) {
        llvm::Value* res = builder_.CreateCall(fn, args);
        if (inst.result != il::kNoValue) values_[inst.result] = res;
    };

    if (!needs(2, false, "Invalid operands for DynamicCall")) return false;
    llvm::Value* callee =
        operand(inst, 0, "Undefined callee or this in DynamicCall instruction");
    llvm::Value* thisVal =
        operand(inst, 1, "Undefined callee or this in DynamicCall instruction");
    if (!callee || !thisVal) return false;
    uint32_t argc = static_cast<uint32_t>(inst.operands.size() - 2);
    bool ok = false;
    llvm::Value* argv = emitArgv(inst, 2, argc, ok);
    if (!ok) return false;

    const uint32_t calleeKey = inst.operands[0] < propGetKey_.size()
                                    ? propGetKey_[inst.operands[0]]
                                    : UINT32_MAX;
    if (calleeKey < shared_.module.keyConstants.size()) {
        if (inst.result != il::kNoValue) {
            if (auto kind =
                    mathIntrinsicFor(shared_.module.keyConstants[calleeKey], argc)) {
                llvm::SmallVector<llvm::Value*, 2> args;
                for (uint32_t a = 0; a < argc; ++a) {
                    args.push_back(values_[inst.operands[2 + a]]);
                }
                values_[inst.result] = emitMathDirectCall(builder_, abi, *kind, callee,
                                                          thisVal, argc, argv, args);
                return true;
            }
        }
        if (shared_.module.keyConstants[calleeKey] == "push" && argc == 1) {
            llvm::Value* argVal = values_[inst.operands[2]];
            llvm::Value* res = emitArrayPushDirectCall(
                builder_, abi, callee, thisVal, argc, argv, argVal);
            if (inst.result != il::kNoValue) {
                values_[inst.result] = res;
            }
            return true;
        }
    }

    if (shared_.moduleHasNewTarget) {
        callWith(abi.bronze_dynamic_call,
                 {callee, thisVal, builder_.getInt32(argc), argv});
        return true;
    }
    llvm::Value* res = emitDynamicCallInline(
        builder_, abi, globals_, callee, thisVal, argc, argv);
    if (inst.result != il::kNoValue) {
        values_[inst.result] = res;
    }
    return true;
}

bool FunctionEmitter::emitMethodCall(const il::Instruction& inst) {
    const AbiFns& abi = shared_.abi;
    auto needs = [&](size_t operandCount, bool needsResult, const char* what) {
        return require(inst.operands.size() >= operandCount &&
                           (!needsResult || inst.result != il::kNoValue),
                       what);
    };

    if (!needs(1, false, "Invalid operands for MethodCall")) return false;
    llvm::Value* thisVal =
        operand(inst, 0, "Undefined this in MethodCall instruction");
    if (!thisVal) return false;
    uint32_t argc = static_cast<uint32_t>(inst.operands.size() - 1);
    bool ok = false;
    llvm::Value* argv = emitArgv(inst, 1, argc, ok);
    if (!ok) return false;

    llvm::Value* res = emitMethodCallInline(
        builder_, abi, globals_, shared_.tables, thisVal, inst.keyIndex, inst.icIndex, argc, argv);
    if (inst.result != il::kNoValue) {
        values_[inst.result] = res;
    }
    return true;
}

bool FunctionEmitter::emitMethodCallSpread(const il::Instruction& inst) {
    const AbiFns& abi = shared_.abi;
    auto needs = [&](size_t operandCount, bool needsResult, const char* what) {
        return require(inst.operands.size() >= operandCount &&
                           (!needsResult || inst.result != il::kNoValue),
                       what);
    };

    if (!needs(2, false, "Invalid operands for MethodCallSpread")) return false;
    llvm::Value* thisVal =
        operand(inst, 0, "Undefined this in MethodCallSpread instruction");
    llvm::Value* argsArr =
        operand(inst, 1, "Undefined args in MethodCallSpread instruction");
    if (!thisVal || !argsArr) return false;

    llvm::Value* res = emitMethodCallSpreadInline(
        builder_, abi, globals_, shared_.tables, thisVal, inst.keyIndex, inst.icIndex, argsArr);
    if (inst.result != il::kNoValue) {
        values_[inst.result] = res;
    }
    return true;
}

bool FunctionEmitter::emitConstruct(const il::Instruction& inst) {
    const AbiFns& abi = shared_.abi;
    auto needs = [&](size_t operandCount, bool needsResult, const char* what) {
        return require(inst.operands.size() >= operandCount &&
                           (!needsResult || inst.result != il::kNoValue),
                       what);
    };
    auto callWith = [&](llvm::Function* fn, std::initializer_list<llvm::Value*> args) {
        llvm::Value* res = builder_.CreateCall(fn, args);
        if (inst.result != il::kNoValue) values_[inst.result] = res;
    };

    if (!needs(1, false, "Invalid operands for Construct")) return false;
    llvm::Value* ctor = operand(inst, 0, "Undefined constructor in Construct instruction");
    if (!ctor) return false;
    uint32_t argc = static_cast<uint32_t>(inst.operands.size() - 1);
    bool ok = false;
    llvm::Value* argv = emitArgv(inst, 1, argc, ok);
    if (!ok) return false;

    if (constructSelfSlot_ != kNoSlot) {
        llvm::Value* res =
            emitConstructInline(builder_, abi, globals_, ctor, argc, argv,
                                slotAddr(constructSelfSlot_));
        if (inst.result != il::kNoValue) values_[inst.result] = res;
        return true;
    }
    callWith(abi.bronze_construct, {ctor, builder_.getInt32(argc), argv});
    return true;
}

bool FunctionEmitter::emitCall(const il::Instruction& inst) {
    if (!require(inst.calleeIndex < shared_.entries.size(),
                 "Invalid callee index in Call instruction")) {
        return false;
    }
    llvm::Function* callee = shared_.entries[inst.calleeIndex];
    std::vector<llvm::Value*> args;
    args.reserve(inst.operands.size());
    for (size_t i = 0; i < inst.operands.size(); ++i) {
        llvm::Value* arg = operand(inst, i, "Undefined argument in Call instruction");
        if (!arg) return false;
        args.push_back(arg);
    }
    llvm::Value* res = builder_.CreateCall(callee, args);
    if (inst.result != il::kNoValue && !callee->getReturnType()->isVoidTy()) {
        values_[inst.result] = res;
    }
    return true;
}

}  // namespace bronze::codegen_llvm
