// Arithmetic, negation, comparison and strict equality.
//
// Every binary form here asks the same question first: are the operands
// doubles? IL types say so directly for proven code; a Bool that reaches a
// numeric op is widened, which is the one coercion this file performs. A
// Dynamic operand never arrives at a compare — lowering routes those through
// unbox or strict.eq — so mismatched operand types are a lowering bug and are
// reported as one rather than coerced.

#include <string>

#include <llvm/IR/Constants.h>

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
    const bool unary = inst.op == il::Op::Neg || inst.op == il::Op::ToInt32 ||
                       inst.op == il::Op::NumTruthy;
    if (!require(inst.operands.size() >= (unary ? 1u : 2u) && inst.result != il::kNoValue,
                 (std::string("Invalid operands for ") + op).c_str())) {
        return false;
    }

    const std::string undefinedMsg = std::string("Undefined value in ") + op + " instruction";
    llvm::Value* lhs = operand(inst, 0, undefinedMsg.c_str());
    if (!lhs) return false;
    if (inst.op == il::Op::ToInt32) {
        // A call, not an `fptosi`: LLVM's is poison for a double outside the
        // int32 range, and ECMA-262 requires a wraparound modulo 2^32 there
        // (`2147483648 | 0` is -2147483648). An operand that is ALREADY an
        // int32 has nothing to convert, which is what makes a chain of
        // bitwise operators cost one conversion per source operand.
        if (lhs->getType()->isIntegerTy(32)) {
            values_[inst.result] = lhs;
        } else if (lhs->getType()->isIntegerTy(64)) {
            values_[inst.result] = builder_.CreateCall(shared_.abi.bronze_to_int32, {lhs});
        } else {
            values_[inst.result] =
                builder_.CreateCall(shared_.abi.bronze_to_int32_f64, {widenBool(builder_, lhs)});
        }
        return true;
    }
    if (inst.op == il::Op::NumTruthy) {
        // The ORDERED compare, which is false for NaN as well as for both
        // zeroes — exactly JS ToBoolean of a number. `cmp.ne` below is the
        // unordered one and would call NaN truthy.
        values_[inst.result] = builder_.CreateFCmpONE(
            widenBool(builder_, lhs), llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0));
        return true;
    }
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
        case il::Op::LooseEq:
            values_[inst.result] = builder_.CreateCall(shared_.abi.bronze_loose_eq, {lhs, rhs});
            return true;
        case il::Op::Pow:
            values_[inst.result] = builder_.CreateCall(
                shared_.abi.bronze_pow, {widenBool(builder_, lhs), widenBool(builder_, rhs)});
            return true;

        // The bitwise family. Both operands arrive as i32 (lowering puts a
        // to.int32 in front of each), and the result is the JS NUMBER that
        // int32 denotes — so the widening below is part of the operator, not
        // a coercion at its use site.
        case il::Op::BitAnd:
        case il::Op::BitOr:
        case il::Op::BitXor:
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr: {
            if (!require(lhs->getType()->isIntegerTy(32) && rhs->getType()->isIntegerTy(32),
                         (std::string("Non-i32 operand in ") + op + " (lowering bug)").c_str())) {
                return false;
            }
            llvm::Value* result = nullptr;
            switch (inst.op) {
                case il::Op::BitAnd: result = builder_.CreateAnd(lhs, rhs); break;
                case il::Op::BitOr: result = builder_.CreateOr(lhs, rhs); break;
                case il::Op::BitXor: result = builder_.CreateXor(lhs, rhs); break;
                default: {
                    // ToUint32(rhs) & 31, which the language specifies and
                    // LLVM requires: a shift by 32 or more is poison, while
                    // JS says `1 << 32` is 1. The mask is over the same 32
                    // bits ToInt32 produced, so ToUint32 and ToInt32 cannot
                    // disagree about the low five.
                    llvm::Value* count = builder_.CreateAnd(rhs, builder_.getInt32(31));
                    result = inst.op == il::Op::Shl   ? builder_.CreateShl(lhs, count)
                             : inst.op == il::Op::Shr ? builder_.CreateAShr(lhs, count)
                                                      : builder_.CreateLShr(lhs, count);
                    break;
                }
            }
            // `>>>` is the one member of the family whose result is ToUint32
            // rather than ToInt32, so it — and only it — widens as unsigned:
            // `-1 >>> 0` is 4294967295 and not -1.
            values_[inst.result] = inst.op == il::Op::UShr
                                       ? builder_.CreateUIToFP(result, builder_.getDoubleTy())
                                       : builder_.CreateSIToFP(result, builder_.getDoubleTy());
            return true;
        }

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
            // UNordered: `!==` is the negation of `===`, and `NaN !== NaN` is
            // true. The ordered form is `num.truthy` and means something else.
            default: values_[inst.result] = builder_.CreateFCmpUNE(lhs, rhs); break;
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
