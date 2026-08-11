// Arithmetic, negation, comparison and strict equality.
//
// Every binary form here asks the same question first: are the operands
// doubles? IL types say so directly for proven code; a Bool that reaches a
// numeric op is widened, which is the one coercion this file performs. A
// Dynamic operand never arrives at a compare — lowering routes those through
// unbox or strict.eq — so mismatched operand types are a lowering bug and are
// reported as one rather than coerced.

#include <string>

#include "codegen-llvm/llvm_func.h"

namespace bronze::codegen_llvm {

namespace {

bool isDoubleOperation(il::Type type, llvm::Value* lhs, llvm::Value* rhs) {
    return type == il::Type::F64 || lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy();
}

llvm::Value* widenBool(llvm::IRBuilder<>& builder, llvm::Value* v) {
    return v->getType()->isIntegerTy(1) ? builder.CreateUIToFP(v, builder.getDoubleTy()) : v;
}

}  // namespace

bool FunctionEmitter::emitArithmetic(const il::Instruction& inst) {
    const char* op = il::opName(inst.op);
    const bool unary = inst.op == il::Op::Neg;
    if (!require(inst.operands.size() >= (unary ? 1u : 2u) && inst.result != il::kNoValue,
                 (std::string("Invalid operands for ") + op).c_str())) {
        return false;
    }

    const std::string undefinedMsg = std::string("Undefined value in ") + op + " instruction";
    llvm::Value* lhs = operand(inst, 0, undefinedMsg.c_str());
    if (!lhs) return false;
    if (unary) {
        values_[inst.result] = builder_.CreateFNeg(widenBool(builder_, lhs));
        return true;
    }
    llvm::Value* rhs = operand(inst, 1, undefinedMsg.c_str());
    if (!rhs) return false;

    switch (inst.op) {
        case il::Op::StrictEq:
            values_[inst.result] = builder_.CreateCall(shared_.abi.bronze_strict_eq, {lhs, rhs});
            return true;

        case il::Op::Add:
            if (inst.type == il::Type::Dynamic) {
                values_[inst.result] =
                    builder_.CreateCall(shared_.abi.bronze_dynamic_add, {lhs, rhs});
                return true;
            }
            if (inst.type == il::Type::Str) {
                values_[inst.result] =
                    builder_.CreateCall(shared_.abi.bronze_string_concat, {lhs, rhs});
                return true;
            }
            [[fallthrough]];
        case il::Op::Sub:
        case il::Op::Mul:
        case il::Op::Div:
        case il::Op::Mod: {
            const bool asDouble = isDoubleOperation(inst.type, lhs, rhs);
            if (asDouble) {
                lhs = widenBool(builder_, lhs);
                rhs = widenBool(builder_, rhs);
            }
            switch (inst.op) {
                case il::Op::Add:
                    values_[inst.result] =
                        asDouble ? builder_.CreateFAdd(lhs, rhs) : builder_.CreateAdd(lhs, rhs);
                    break;
                case il::Op::Sub:
                    values_[inst.result] =
                        asDouble ? builder_.CreateFSub(lhs, rhs) : builder_.CreateSub(lhs, rhs);
                    break;
                case il::Op::Mul:
                    values_[inst.result] =
                        asDouble ? builder_.CreateFMul(lhs, rhs) : builder_.CreateMul(lhs, rhs);
                    break;
                case il::Op::Div:
                    values_[inst.result] =
                        asDouble ? builder_.CreateFDiv(lhs, rhs) : builder_.CreateSDiv(lhs, rhs);
                    break;
                default:
                    values_[inst.result] =
                        asDouble ? builder_.CreateFRem(lhs, rhs) : builder_.CreateSRem(lhs, rhs);
                    break;
            }
            return true;
        }

        default: break;  // the four comparisons
    }

    if (!require(lhs->getType() == rhs->getType(),
                 (std::string("Mismatched operand types in ") + op + " (lowering bug)").c_str())) {
        return false;
    }

    if (lhs->getType()->isDoubleTy()) {
        switch (inst.op) {
            case il::Op::CmpLt: values_[inst.result] = builder_.CreateFCmpOLT(lhs, rhs); break;
            case il::Op::CmpGt: values_[inst.result] = builder_.CreateFCmpOGT(lhs, rhs); break;
            case il::Op::CmpEq: values_[inst.result] = builder_.CreateFCmpOEQ(lhs, rhs); break;
            default: values_[inst.result] = builder_.CreateFCmpONE(lhs, rhs); break;
        }
        return true;
    }
    if (lhs->getType()->isIntegerTy(1) &&
        (inst.op == il::Op::CmpEq || inst.op == il::Op::CmpNe)) {
        values_[inst.result] = inst.op == il::Op::CmpEq ? builder_.CreateICmpEQ(lhs, rhs)
                                                        : builder_.CreateICmpNE(lhs, rhs);
        return true;
    }
    if (lhs->getType()->isIntegerTy(32)) {
        switch (inst.op) {
            case il::Op::CmpLt: values_[inst.result] = builder_.CreateICmpSLT(lhs, rhs); break;
            case il::Op::CmpGt: values_[inst.result] = builder_.CreateICmpSGT(lhs, rhs); break;
            case il::Op::CmpEq: values_[inst.result] = builder_.CreateICmpEQ(lhs, rhs); break;
            default: values_[inst.result] = builder_.CreateICmpNE(lhs, rhs); break;
        }
        return true;
    }
    return require(false, (std::string("Unsupported operand type in ") + op).c_str());
}

}  // namespace bronze::codegen_llvm
