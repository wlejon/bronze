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
    uint32_t calleeIdx = (inst.operands[0] < funcRefIndex_.size())
                             ? funcRefIndex_[inst.operands[0]]
                             : UINT32_MAX;
    llvm::Function* knownWrapper = (calleeIdx < shared_.wrappers.size())
                                       ? shared_.wrappers[calleeIdx]
                                       : nullptr;
    llvm::Value* res = emitDynamicCallInline(
        builder_, abi, globals_, callee, thisVal, argc, argv, knownWrapper);
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

    if (emitMethodCallDirect(inst, thisVal, argc)) return true;

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

// The DIRECT method-call edge (il.h, `directTarget`): a guard on the site's own
// inline cache, and on its hit a call to the callee's TYPED ENTRY with the
// arguments in registers.
//
// What it deletes at a hit is the whole uniform boundary — the argument vector
// is not built, the wrapper's unpack does not run, and the callee is an
// ordinary internal function that LLVM may inline into this one. `emitArgv` is
// emitted into the MISS block instead of before the guard, which is the half of
// this that costs the fast path nothing.
//
// Answers false when the site has no target or the target's shape cannot be
// expressed as a fixed operand list, and then nothing has been emitted and the
// caller's ordinary path is untouched.
bool FunctionEmitter::emitMethodCallDirect(const il::Instruction& inst, llvm::Value* thisVal,
                                           uint32_t argc) {
    if (inst.directTarget >= shared_.entries.size()) return false;
    const il::Function& callee = shared_.module.functions[inst.directTarget];
    llvm::Function* entry = shared_.entries[inst.directTarget];
    llvm::Function* wrapper = shared_.wrappers[inst.directTarget];
    // A method is reached through a receiver, so `__this` must be a parameter
    // the convention carries; lowering refuses the rest and `arguments` forms
    // (lower_infer.cpp, `resolveDirectMethodTargets`) and this re-states the
    // one fact the emitted call would otherwise get silently wrong.
    if (!callee.needsThis || callee.needsArguments || callee.hasRestParam) return false;

    const size_t base = callee.firstSourceParam();
    const size_t fixed = callee.callerParamCount();
    if (argc > fixed) return false;

    const AbiFns& abi = shared_.abi;
    llvm::LLVMContext& ctx = shared_.ctx;

    // The arguments, read out of the value table BEFORE the guard splits the
    // block: they were reloaded from their root slots at the top of this
    // instruction, and the guard allocates nothing, so these bits are current
    // in the hit block too.
    std::vector<llvm::Value*> argBits;
    argBits.reserve(argc);
    for (uint32_t a = 0; a < argc; ++a) {
        llvm::Value* v = operand(inst, 1 + a, "Undefined argument in MethodCall instruction");
        if (!v) return false;
        argBits.push_back(v);
    }

    MethodDirectGuard guard = emitMethodDirectGuard(builder_, globals_, shared_.tables, thisVal,
                                                    inst.icIndex, wrapper, callee.needsEnv);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "mdc.done", llvmFunc_);

    // The hit: the typed entry, entered positionally.
    builder_.SetInsertPoint(guard.hit);
    std::vector<llvm::Value*> callArgs;
    if (callee.needsEnv) callArgs.push_back(guard.env);
    callArgs.push_back(thisVal);
    for (size_t p = 0; p < fixed; ++p) {
        const il::Type slot = callee.params[p + base].type;
        if (p >= argc) {
            // An omitted argument IS `undefined`, which is the value the
            // callee's default tests for. Lowering has already refused a site
            // whose padding would land in a typed slot.
            callArgs.push_back(builder_.getInt64(BRONZE_ABI_UNDEFINED_BITS));
            continue;
        }
        callArgs.push_back(slot == il::Type::F64 ? emitToNumberInline(builder_, abi, argBits[p])
                                                 : argBits[p]);
    }
    if (callArgs.size() != entry->getFunctionType()->getNumParams()) {
        // A drift between the IL signature and the LLVM one. Nothing after the
        // guard has been emitted into `hit` that a `br` cannot follow, so the
        // honest thing is a diagnosis rather than a mismatched call.
        require(false, "internal: direct method-call arity does not match the typed entry");
        return false;
    }
    llvm::CallInst* hitCall = builder_.CreateCall(entry, callArgs);
    // The ask for inlining, spent by `markDirectMethodInlining` once the
    // module is whole and the callee's size is known (llvm_call.h says why the
    // ask is here and the decision is not).
    hitCall->setMetadata(kDirectMethodMD, llvm::MDNode::get(ctx, {}));
    llvm::Value* hitRes = hitCall;
    switch (callee.returnType) {
        case il::Type::Void: hitRes = builder_.getInt64(BRONZE_ABI_UNDEFINED_BITS); break;
        case il::Type::F64: hitRes = emitBoxF64Inline(builder_, hitRes); break;
        case il::Type::Dynamic: break;
        default:
            require(false, "internal: unsupported return type on a direct method-call target");
            return false;
    }
    llvm::BasicBlock* hitEnd = builder_.GetInsertBlock();
    builder_.CreateBr(doneBb);

    // The miss: the site exactly as it was, argument vector and all.
    builder_.SetInsertPoint(guard.miss);
    bool ok = false;
    llvm::Value* argv = emitArgv(inst, 1, argc, ok);
    if (!ok) return false;
    llvm::Value* missRes =
        emitMethodCallInline(builder_, abi, globals_, shared_.tables, thisVal, inst.keyIndex,
                             inst.icIndex, argc, argv);
    llvm::BasicBlock* missEnd = builder_.GetInsertBlock();
    builder_.CreateBr(doneBb);

    builder_.SetInsertPoint(doneBb);
    if (inst.result != il::kNoValue) {
        llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt64Ty(), 2, "mdc.result");
        phi->addIncoming(hitRes, hitEnd);
        phi->addIncoming(missRes, missEnd);
        values_[inst.result] = phi;
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

    uint32_t calleeIdx = (inst.operands[0] < funcRefIndex_.size())
                             ? funcRefIndex_[inst.operands[0]]
                             : UINT32_MAX;
    llvm::Function* knownWrapper = (calleeIdx < shared_.wrappers.size())
                                       ? shared_.wrappers[calleeIdx]
                                       : nullptr;
    llvm::Function* knownEntry = (calleeIdx < shared_.entries.size())
                                     ? shared_.entries[calleeIdx]
                                     : nullptr;
    const il::Function* knownFunc = (calleeIdx < shared_.module.functions.size())
                                        ? &shared_.module.functions[calleeIdx]
                                        : nullptr;

    std::vector<llvm::Value*> directArgs;
    bool canDirect = false;
    if (knownEntry && knownFunc && !knownFunc->hasRestParam && !knownFunc->needsArguments) {
        size_t expectedParams = knownFunc->params.size();
        size_t envOffset = knownFunc->needsEnv ? 1 : 0;
        size_t thisOffset = envOffset + (knownFunc->needsThis ? 1 : 0);
        size_t sourceParamCount = expectedParams - thisOffset;
        if (argc == sourceParamCount) {
            bool typesMatch = true;
            for (size_t p = 0; p < argc; ++p) {
                il::ValueId opId = inst.operands[1 + p];
                llvm::Value* opVal = (opId < values_.size()) ? values_[opId] : nullptr;
                if (!opVal) { typesMatch = false; break; }
                llvm::Type* expectedTy = knownEntry->getFunctionType()->getParamType(static_cast<unsigned>(thisOffset + p));
                if (opVal->getType() != expectedTy) {
                    typesMatch = false;
                    break;
                }
                directArgs.push_back(opVal);
            }
            if (typesMatch && directArgs.size() == argc) {
                canDirect = true;
            } else {
                directArgs.clear();
            }
        }
    }

    if (constructSelfSlot_ != kNoSlot) {
        llvm::Value* res =
            emitConstructInline(builder_, abi, globals_, ctor, argc, argv,
                                slotAddr(constructSelfSlot_), knownWrapper, knownFunc,
                                canDirect ? knownEntry : nullptr, directArgs);
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
