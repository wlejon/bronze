// Arithmetic, negation, comparison and strict equality.
//
// Every binary form here asks the same question first: are the operands
// doubles? IL types say so directly for proven code; a Bool that reaches a
// numeric op is widened, which is the one coercion this file performs. A
// Dynamic operand never arrives at a machine compare — lowering routes those
// through unbox, strict.eq or the rel.* family, each of which leaves through a
// helper call above — so mismatched operand types at the compares below are a
// lowering bug and are reported as one rather than coerced.

#include <string>

#include <llvm/IR/Constants.h>
#include <llvm/IR/MDBuilder.h>

#include "codegen-llvm/llvm_convert.h"
#include "codegen-llvm/llvm_elem_typed.h"
#include "codegen-llvm/llvm_func.h"
#include "codegen-llvm/llvm_strict_eq.h"

namespace bronze::codegen_llvm {

namespace {

bool isDoubleOperation(il::Type type, llvm::Value* lhs, llvm::Value* rhs) {
    return type == il::Type::F64 || lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy();
}

llvm::Value* widenBool(llvm::IRBuilder<>& builder, llvm::Value* v) {
    return v->getType()->isIntegerTy(1) ? builder.CreateUIToFP(v, builder.getDoubleTy()) : v;
}

// The both-operands-are-numbers test a NaN-boxed pair answers with two
// unsigned compares: every number's bits are at or below NUMBER_MAX, and
// every non-number's are above it. Splits the current block; on the true
// edge the builder is in a fresh block.
llvm::Value* branchIfBothNumbers(llvm::IRBuilder<>& builder, llvm::Value* lhs, llvm::Value* rhs,
                                 llvm::BasicBlock* slowBb, const char* name) {
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    const bool lhsProven = isProvenNumberValue(lhs);
    const bool rhsProven = isProvenNumberValue(rhs);
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(builder.getContext(), name, fn);

    if (lhsProven && rhsProven) {
        builder.CreateBr(fastBb);
    } else {
        llvm::Value* lhsNum = lhsProven ? static_cast<llvm::Value*>(builder.getTrue())
                                        : builder.CreateICmpULE(lhs, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
        llvm::Value* rhsNum = rhsProven ? static_cast<llvm::Value*>(builder.getTrue())
                                        : builder.CreateICmpULE(rhs, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
        llvm::Value* cond = lhsProven ? rhsNum : (rhsProven ? lhsNum : builder.CreateAnd(lhsNum, rhsNum));
        auto* br = builder.CreateCondBr(cond, fastBb, slowBb);
        br->setMetadata(llvm::LLVMContext::MD_prof,
                        llvm::MDBuilder(builder.getContext()).createBranchWeights(1048576, 1));
    }
    builder.SetInsertPoint(fastBb);
    return nullptr;
}

// The canonicalizing re-box every inline numeric arm ends with: `inf + -inf`
// is NaN out of two finite-looking inputs, so the sum needs the same select
// the Box instruction emits.
llvm::Value* canonicalizeNumeric(llvm::IRBuilder<>& builder, llvm::Value* sum) {
    llvm::Value* isNan = builder.CreateFCmpUNO(sum, sum);
    return builder.CreateSelect(isNan, builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS),
                                builder.CreateBitCast(sum, builder.getInt64Ty()));
}

// `concat.begin` / `concat.append`: the SAME number/number fast path
// `emitDynamicAdd` has, because a `+` spine that turns out arithmetic must not
// pay for having been spelled as a chain — no accumulator is minted on that
// edge and every step is the fadd it always was. The slow edge is the
// accumulator helper, which owns ToPrimitive, the builder and the TypeError
// ladder. `remaining` is null for `append`, which takes no sizing hint.
llvm::Value* emitConcatStep(llvm::IRBuilder<>& builder, llvm::Function* helper, llvm::Value* lhs,
                            llvm::Value* rhs, llvm::Value* remaining) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "cat.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "cat.done", fn);

    branchIfBothNumbers(builder, lhs, rhs, slowBb, "cat.fast");
    llvm::Value* ld = unwrapBoxedDouble(lhs);
    if (!ld) ld = builder.CreateBitCast(lhs, builder.getDoubleTy());
    llvm::Value* rd = unwrapBoxedDouble(rhs);
    if (!rd) rd = builder.CreateBitCast(rhs, builder.getDoubleTy());
    llvm::Value* sum = builder.CreateFAdd(ld, rd);
    llvm::Value* fastVal = canonicalizeNumeric(builder, sum);
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = remaining != nullptr
                               ? builder.CreateCall(helper, {lhs, rhs, remaining})
                               : builder.CreateCall(helper, {lhs, rhs});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(builder.getInt64Ty(), 2, "cat.result");
    result->addIncoming(fastVal, fastEndBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

// `concat.end`: a Number accumulator is already the value the chain produced
// and there is nothing to seal, so one unsigned compare skips the call —
// which is what makes the numeric spine cost exactly what a chain of `add`
// cost. Anything else is the String accumulator this op exists to close.
llvm::Value* emitConcatEnd(llvm::IRBuilder<>& builder, llvm::Function* helper, llvm::Value* val) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* sealBb = llvm::BasicBlock::Create(ctx, "cend.seal", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "cend.done", fn);

    llvm::Value* isNum =
        builder.CreateICmpULE(val, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "cend.isnum");
    llvm::BasicBlock* entryBb = builder.GetInsertBlock();
    builder.CreateCondBr(isNum, doneBb, sealBb);

    builder.SetInsertPoint(sealBb);
    llvm::Value* sealed = builder.CreateCall(helper, {val});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(builder.getInt64Ty(), 2, "cend.result");
    result->addIncoming(val, entryBb);
    result->addIncoming(sealed, sealBb);
    return result;
}

// Returns an i1 indicating whether `v` is a primitive operand for arithmetic/relational operations
// (number, null, undefined, or boolean). Any other tag (Object, String, Symbol, BigInt) returns false.
llvm::Value* isArithmeticPrimitive(llvm::IRBuilder<>& builder, llvm::Value* v) {
    if (isProvenNumberValue(v)) {
        return builder.getTrue();
    }
    llvm::Value* isNum =
        builder.CreateICmpULE(v, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "is.num");
    llvm::Value* isNull =
        builder.CreateICmpEQ(v, builder.getInt64(BRONZE_ABI_NULL_BITS), "is.null");
    llvm::Value* isUndef =
        builder.CreateICmpEQ(v, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), "is.undef");
    llvm::Value* isTrue =
        builder.CreateICmpEQ(v, builder.getInt64(BRONZE_ABI_TRUE_BITS), "is.true");
    llvm::Value* isFalse =
        builder.CreateICmpEQ(v, builder.getInt64(BRONZE_ABI_FALSE_BITS), "is.false");
    return builder.CreateOr(
        isNum,
        builder.CreateOr(builder.CreateOr(isNull, isUndef), builder.CreateOr(isTrue, isFalse)),
        "is.prim");
}

// Converts a primitive boxed value (number, null, undefined, boolean) to double inline.
// Number -> bitcast double; null -> 0.0; undefined -> NaN; true -> 1.0; false -> 0.0.
llvm::Value* primitiveToDouble(llvm::IRBuilder<>& builder, llvm::Value* v) {
    llvm::Type* dblTy = builder.getDoubleTy();
    if (llvm::Value* unwrapped = unwrapBoxedDouble(v)) {
        return unwrapped;
    }
    if (isProvenNumberValue(v)) {
        return builder.CreateBitCast(v, dblTy, "cvt.num");
    }
    llvm::Value* isNum =
        builder.CreateICmpULE(v, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "cvt.isnum");
    llvm::Value* isUndef =
        builder.CreateICmpEQ(v, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), "cvt.isundef");
    llvm::Value* isTrue =
        builder.CreateICmpEQ(v, builder.getInt64(BRONZE_ABI_TRUE_BITS), "cvt.istrue");

    llvm::Value* numVal = builder.CreateBitCast(v, dblTy, "cvt.num");
    llvm::Value* nanVal =
        builder.CreateBitCast(builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS), dblTy, "cvt.nan");
    llvm::Value* zeroVal = llvm::ConstantFP::get(dblTy, 0.0);
    llvm::Value* oneVal = llvm::ConstantFP::get(dblTy, 1.0);

    llvm::Value* boolOrNull = builder.CreateSelect(isTrue, oneVal, zeroVal, "cvt.bool_or_null");
    llvm::Value* nonNum = builder.CreateSelect(isUndef, nanVal, boolOrNull, "cvt.nonnum");
    return builder.CreateSelect(isNum, numVal, nonNum, "cvt.dbl");
}

// `a + b` over boxed operands: the number/number case — the loop-carried case
// in every allocation-free numeric loop — is an fadd and the canonicalizing
// re-box, mirroring the fast path at the top of bronze_dynamic_add; if both operands
// are primitive non-strings (number, null, undefined, boolean), they are converted to
// double inline and added; anything involving a string, an object or a symbol keeps
// the helper, which owns ToPrimitive and the concat/TypeError ladder.
llvm::Value* emitDynamicAdd(llvm::IRBuilder<>& builder, llvm::Function* helper, llvm::Value* lhs,
                            llvm::Value* rhs) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* dblTy = builder.getDoubleTy();
    llvm::MDNode* likely = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::BasicBlock* checkBb = llvm::BasicBlock::Create(ctx, "dadd.check", fn);
    llvm::BasicBlock* primBb = llvm::BasicBlock::Create(ctx, "dadd.prim", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "dadd.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "dadd.done", fn);

    branchIfBothNumbers(builder, lhs, rhs, checkBb, "dadd.fast");
    llvm::Value* ld = unwrapBoxedDouble(lhs);
    if (!ld) ld = builder.CreateBitCast(lhs, dblTy);
    llvm::Value* rd = unwrapBoxedDouble(rhs);
    if (!rd) rd = builder.CreateBitCast(rhs, dblTy);
    llvm::Value* sum = builder.CreateFAdd(ld, rd);
    llvm::Value* fastVal = canonicalizeNumeric(builder, sum);
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(checkBb);
    llvm::Value* lPrim = isArithmeticPrimitive(builder, lhs);
    llvm::Value* rPrim = isArithmeticPrimitive(builder, rhs);
    llvm::Value* bothPrim = builder.CreateAnd(lPrim, rPrim, "dadd.bothprim");
    builder.CreateCondBr(bothPrim, primBb, slowBb, likely);

    builder.SetInsertPoint(primBb);
    llvm::Value* lDbl = primitiveToDouble(builder, lhs);
    llvm::Value* rDbl = primitiveToDouble(builder, rhs);
    llvm::Value* primSum = builder.CreateFAdd(lDbl, rDbl);
    llvm::Value* primVal = canonicalizeNumeric(builder, primSum);
    llvm::BasicBlock* primEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(helper, {lhs, rhs});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(builder.getInt64Ty(), 3, "dadd.result");
    result->addIncoming(fastVal, fastEndBb);
    result->addIncoming(primVal, primEndBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

// `a - b`, `a * b`, `a / b`, `a % b` over boxed operands. Same shape as
// `emitDynamicAdd` and for the same reason: the number/number case is the one
// a loop carries, and it is one machine instruction. If both operands are primitive
// (number, null, undefined, boolean), they are converted inline to double without
// helper calls. What the helper owns is everything else — a string operand's ToNumber,
// an object's valueOf, and the BigInt algorithm with 13.15.3's mixing TypeError in front of it.
llvm::Value* emitDynamicArith(llvm::IRBuilder<>& builder, llvm::Function* helper, il::Op op,
                              llvm::Value* lhs, llvm::Value* rhs) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* dblTy = builder.getDoubleTy();
    llvm::MDNode* likely = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::BasicBlock* checkBb = llvm::BasicBlock::Create(ctx, "darith.check", fn);
    llvm::BasicBlock* primBb = llvm::BasicBlock::Create(ctx, "darith.prim", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "darith.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "darith.done", fn);

    branchIfBothNumbers(builder, lhs, rhs, checkBb, "darith.fast");
    llvm::Value* l = unwrapBoxedDouble(lhs);
    if (!l) l = builder.CreateBitCast(lhs, dblTy);
    llvm::Value* r = unwrapBoxedDouble(rhs);
    if (!r) r = builder.CreateBitCast(rhs, dblTy);
    llvm::Value* num = op == il::Op::Sub   ? builder.CreateFSub(l, r)
                       : op == il::Op::Mul ? builder.CreateFMul(l, r)
                       : op == il::Op::Div ? builder.CreateFDiv(l, r)
                                           : builder.CreateFRem(l, r);
    llvm::Value* fastVal = canonicalizeNumeric(builder, num);
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(checkBb);
    llvm::Value* lPrim = isArithmeticPrimitive(builder, lhs);
    llvm::Value* rPrim = isArithmeticPrimitive(builder, rhs);
    llvm::Value* bothPrim = builder.CreateAnd(lPrim, rPrim, "darith.bothprim");
    builder.CreateCondBr(bothPrim, primBb, slowBb, likely);

    builder.SetInsertPoint(primBb);
    llvm::Value* lDbl = primitiveToDouble(builder, lhs);
    llvm::Value* rDbl = primitiveToDouble(builder, rhs);
    llvm::Value* primNum = op == il::Op::Sub   ? builder.CreateFSub(lDbl, rDbl)
                           : op == il::Op::Mul ? builder.CreateFMul(lDbl, rDbl)
                           : op == il::Op::Div ? builder.CreateFDiv(lDbl, rDbl)
                                               : builder.CreateFRem(lDbl, rDbl);
    llvm::Value* primVal = canonicalizeNumeric(builder, primNum);
    llvm::BasicBlock* primEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(helper, {lhs, rhs});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(builder.getInt64Ty(), 3, "darith.result");
    result->addIncoming(fastVal, fastEndBb);
    result->addIncoming(primVal, primEndBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

// The four relational operators over boxed operands: two numbers are one
// ORDERED fcmp — false for a NaN on either side, which is exactly 13.10's
// "undefined becomes false" for all four members of the family. Everything
// else (strings compare by code unit, objects unwrap) keeps the helper —
// except when both operands are primitive (number, null, undefined, boolean):
// if either operand is `undefined`, return false directly; if both are number,
// null, or boolean, convert each to double and compare inline.
llvm::Value* emitDynamicRel(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Function* helper,
                            llvm::CmpInst::Predicate pred, llvm::Value* lhs, llvm::Value* rhs) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* dblTy = builder.getDoubleTy();
    llvm::MDNode* likely = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::BasicBlock* checkBb = llvm::BasicBlock::Create(ctx, "drel.check", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "drel.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "drel.done", fn);

    branchIfBothNumbers(builder, lhs, rhs, checkBb, "drel.fast");
    llvm::Value* ld = unwrapBoxedDouble(lhs);
    if (!ld) ld = builder.CreateBitCast(lhs, dblTy);
    llvm::Value* rd = unwrapBoxedDouble(rhs);
    if (!rd) rd = builder.CreateBitCast(rhs, dblTy);
    llvm::Value* fastVal = builder.CreateFCmp(pred, ld, rd, "drel.cmp");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // Non-both-numbers path:
    builder.SetInsertPoint(checkBb);
    llvm::Value* base = builder.CreateCall(abi.bronze_tls_block_addr, {}, "tls");
    llvm::Value* cellPtr = builder.CreateConstInBoundsGEP1_64(
        builder.getInt8Ty(), base, BRONZE_TLS_UNDEF_REL_ENABLED_OFF, "tls.undefrel");
    llvm::Value* cell =
        builder.CreateAlignedLoad(builder.getInt64Ty(), cellPtr, llvm::Align(8), "undefrel.seam");
    llvm::Value* seamOn = builder.CreateICmpNE(cell, builder.getInt64(0), "undefrel.on");
    llvm::BasicBlock* primBb = llvm::BasicBlock::Create(ctx, "drel.prim", fn);
    builder.CreateCondBr(seamOn, primBb, slowBb, likely);

    builder.SetInsertPoint(primBb);
    llvm::Value* lPrim = isArithmeticPrimitive(builder, lhs);
    llvm::Value* rPrim = isArithmeticPrimitive(builder, rhs);
    llvm::Value* bothPrim = builder.CreateAnd(lPrim, rPrim, "drel.bothprim");
    llvm::BasicBlock* dispatchBb = llvm::BasicBlock::Create(ctx, "drel.dispatch", fn);
    builder.CreateCondBr(bothPrim, dispatchBb, slowBb, likely);

    builder.SetInsertPoint(dispatchBb);
    llvm::Value* undef = builder.getInt64(BRONZE_ABI_UNDEFINED_BITS);
    llvm::Value* lUndef = builder.CreateICmpEQ(lhs, undef, "drel.lundef");
    llvm::Value* rUndef = builder.CreateICmpEQ(rhs, undef, "drel.rundef");
    llvm::Value* hasUndef = builder.CreateOr(lUndef, rUndef, "drel.hasundef");
    llvm::BasicBlock* cmpBb = llvm::BasicBlock::Create(ctx, "drel.cmp.prim", fn);
    builder.CreateCondBr(hasUndef, doneBb, cmpBb);

    builder.SetInsertPoint(cmpBb);
    llvm::Value* lDbl = primitiveToDouble(builder, lhs);
    llvm::Value* rDbl = primitiveToDouble(builder, rhs);
    llvm::Value* cmpVal = builder.CreateFCmp(pred, lDbl, rDbl, "drel.primcmp");
    llvm::BasicBlock* cmpEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(helper, {lhs, rhs});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(builder.getInt1Ty(), 4, "drel.result");
    result->addIncoming(fastVal, fastEndBb);
    result->addIncoming(builder.getFalse(), dispatchBb);
    result->addIncoming(cmpVal, cmpEndBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

}  // namespace

bool FunctionEmitter::emitArithmetic(const il::Instruction& inst) {
    const char* op = il::opName(inst.op);
    const bool unary = inst.op == il::Op::Neg || inst.op == il::Op::ToInt32 ||
                       inst.op == il::Op::NumTruthy || inst.op == il::Op::BitNot ||
                       inst.op == il::Op::ToNumeric || inst.op == il::Op::NumericStep ||
                       inst.op == il::Op::ConcatEnd;
    if (!require(inst.operands.size() >= (unary ? 1u : 2u) && inst.result != il::kNoValue,
                 (std::string("Invalid operands for ") + op).c_str())) {
        return false;
    }

    const std::string undefinedMsg = std::string("Undefined value in ") + op + " instruction";
    llvm::Value* lhs = operand(inst, 0, undefinedMsg.c_str());
    if (!lhs) return false;
    if (inst.op == il::Op::ToInt32) {
        // Never a bare `fptosi`: LLVM's is poison for a double outside the
        // integer range, and ECMA-262 requires a wraparound modulo 2^32 there
        // (`2147483648 | 0` is -2147483648). `emitToInt32F64` is the guarded
        // form — the range test plus the two machine operations that ARE the
        // conversion inside it; llvm_convert.h has why they are exact. An
        // operand that is ALREADY an int32 has nothing to convert, which is
        // what makes a chain of bitwise operators cost one conversion per
        // source operand.
        if (lhs->getType()->isIntegerTy(32)) {
            values_[inst.result] = lhs;
        } else if (lhs->getType()->isIntegerTy(64)) {
            if (llvm::Value* unwrapped = unwrapBoxedDouble(lhs)) {
                values_[inst.result] = emitToInt32F64(builder_, shared_.abi, unwrapped);
                return true;
            }
            if (isProvenNumberValue(lhs)) {
                llvm::Value* fastDbl = builder_.CreateBitCast(lhs, builder_.getDoubleTy());
                values_[inst.result] = emitToInt32F64(builder_, shared_.abi, fastDbl);
                return true;
            }
            llvm::LLVMContext& ctx = builder_.getContext();
            llvm::Function* fn = builder_.GetInsertBlock()->getParent();
            llvm::BasicBlock* numBb = llvm::BasicBlock::Create(ctx, "toi32.box.num", fn);
            llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "toi32.box.slow", fn);
            llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "toi32.box.done", fn);
            llvm::Value* isNum = builder_.CreateICmpULE(
                lhs, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "toi32.box.isnum");
            auto* br = builder_.CreateCondBr(isNum, numBb, slowBb);
            br->setMetadata(llvm::LLVMContext::MD_prof,
                            llvm::MDBuilder(ctx).createBranchWeights(1048576, 1));

            builder_.SetInsertPoint(numBb);
            llvm::Value* fastDbl = builder_.CreateBitCast(lhs, builder_.getDoubleTy());
            llvm::Value* fastRes = emitToInt32F64(builder_, shared_.abi, fastDbl);
            llvm::BasicBlock* fastEndBb = builder_.GetInsertBlock();
            builder_.CreateBr(doneBb);

            builder_.SetInsertPoint(slowBb);
            llvm::Value* slowRes = builder_.CreateCall(shared_.abi.bronze_to_int32, {lhs});
            llvm::BasicBlock* slowEndBb = builder_.GetInsertBlock();
            builder_.CreateBr(doneBb);

            builder_.SetInsertPoint(doneBb);
            llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt32Ty(), 2, "toi32.box.result");
            phi->addIncoming(fastRes, fastEndBb);
            phi->addIncoming(slowRes, slowEndBb);
            values_[inst.result] = phi;
        } else {
            values_[inst.result] =
                emitToInt32F64(builder_, shared_.abi, widenBool(builder_, lhs));
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
    // 7.1.3 ToNumeric: a Number is already numeric and passes through
    // UNCHANGED, so the common case costs one compare and no call. The helper
    // owns the string parse, the object's valueOf and the BigInt row.
    if (inst.op == il::Op::ToNumeric || inst.op == il::Op::NumericStep) {
        llvm::LLVMContext& ctx = builder_.getContext();
        llvm::Function* fn = builder_.GetInsertBlock()->getParent();
        llvm::Type* dblTy = builder_.getDoubleTy();
        const bool step = inst.op == il::Op::NumericStep;
        const char* tag = step ? "step" : "tonum";

        llvm::BasicBlock* slowBb =
            llvm::BasicBlock::Create(ctx, std::string(tag) + ".slow", fn);
        llvm::BasicBlock* doneBb =
            llvm::BasicBlock::Create(ctx, std::string(tag) + ".done", fn);
        const bool numProven = isProvenNumberValue(lhs);
        llvm::BasicBlock* fastBb =
            llvm::BasicBlock::Create(ctx, std::string(tag) + ".fast", fn);
        if (numProven) {
            builder_.CreateBr(fastBb);
        } else {
            llvm::Value* isNum =
                builder_.CreateICmpULE(lhs, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
            builder_.CreateCondBr(isNum, fastBb, slowBb);
        }

        builder_.SetInsertPoint(fastBb);
        llvm::Value* fastVal = lhs;
        if (step) {
            llvm::Value* one = llvm::ConstantFP::get(dblTy, inst.immI32 > 0 ? 1.0 : -1.0);
            llvm::Value* sum = builder_.CreateFAdd(builder_.CreateBitCast(lhs, dblTy), one);
            // NaN + 1 is NaN, and the operand may be any NaN the heap holds,
            // so the sum needs the same canonicalizing select `+` emits.
            llvm::Value* isNan = builder_.CreateFCmpUNO(sum, sum);
            fastVal = builder_.CreateSelect(isNan,
                                            builder_.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS),
                                            builder_.CreateBitCast(sum, builder_.getInt64Ty()));
        }
        llvm::BasicBlock* fastEndBb = builder_.GetInsertBlock();
        builder_.CreateBr(doneBb);

        builder_.SetInsertPoint(slowBb);
        llvm::Value* slowVal =
            step ? builder_.CreateCall(shared_.abi.bronze_numeric_step,
                                       {lhs, builder_.getInt1(inst.immI32 > 0)})
                 : builder_.CreateCall(shared_.abi.bronze_to_numeric, {lhs});
        builder_.CreateBr(doneBb);

        builder_.SetInsertPoint(doneBb);
        llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt64Ty(), 2,
                                                std::string(tag) + ".result");
        phi->addIncoming(fastVal, fastEndBb);
        phi->addIncoming(slowVal, slowBb);
        values_[inst.result] = phi;
        return true;
    }
    if (inst.op == il::Op::BitNot) {
        // No inline number path: `~x` on a proven number is lowered as
        // `x ^ -1` and never reaches here, so every operand this op sees is
        // one lowering could not type.
        values_[inst.result] = builder_.CreateCall(shared_.abi.bronze_dynamic_bitnot, {lhs});
        return true;
    }
    if (inst.op == il::Op::ConcatEnd) {
        // Takes one operand like the negations below and is nothing like them,
        // so it leaves before the arm that assumes `unary` means `neg`.
        values_[inst.result] = emitConcatEnd(builder_, shared_.abi.bronze_concat_end, lhs);
        return true;
    }
    if (unary) {
        if (inst.type == il::Type::Dynamic) {
            values_[inst.result] = builder_.CreateCall(shared_.abi.bronze_dynamic_neg, {lhs});
            return true;
        }
        values_[inst.result] = builder_.CreateFNeg(widenBool(builder_, lhs));
        return true;
    }
    llvm::Value* rhs = operand(inst, 1, undefinedMsg.c_str());
    if (!rhs) return false;

    switch (inst.op) {
        case il::Op::StrictEq:
            values_[inst.result] = emitStrictEq(builder_, shared_.abi, lhs, rhs);
            return true;
        case il::Op::LooseEq:
            values_[inst.result] = builder_.CreateCall(shared_.abi.bronze_loose_eq, {lhs, rhs});
            return true;

        // The relational operators over boxed operands: the number/number
        // case is inlined; the runtime keeps ECMA-262 13.10.1's string branch
        // and the object unwrap.
        case il::Op::RelLt:
            values_[inst.result] = emitDynamicRel(builder_, shared_.abi, shared_.abi.bronze_rel_lt,
                                                  llvm::CmpInst::FCMP_OLT, lhs, rhs);
            return true;
        case il::Op::RelGt:
            values_[inst.result] = emitDynamicRel(builder_, shared_.abi, shared_.abi.bronze_rel_gt,
                                                  llvm::CmpInst::FCMP_OGT, lhs, rhs);
            return true;
        case il::Op::RelLe:
            values_[inst.result] = emitDynamicRel(builder_, shared_.abi, shared_.abi.bronze_rel_le,
                                                  llvm::CmpInst::FCMP_OLE, lhs, rhs);
            return true;
        case il::Op::RelGe:
            values_[inst.result] = emitDynamicRel(builder_, shared_.abi, shared_.abi.bronze_rel_ge,
                                                  llvm::CmpInst::FCMP_OGE, lhs, rhs);
            return true;
        case il::Op::Pow:
            if (inst.type == il::Type::Dynamic) {
                // No inline fast path, unlike the four below: even on two
                // numbers `**` is a call (Number::exponentiate is not an
                // instruction), so the branch would buy nothing.
                values_[inst.result] =
                    builder_.CreateCall(shared_.abi.bronze_dynamic_pow, {lhs, rhs});
                return true;
            }
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
        case il::Op::MathImul:
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr: {
            if (inst.type == il::Type::Dynamic) {
                // Boxed operands, so the int32 conversion has not happened and
                // must not blindly: on a BigInt pair the operator is defined
                // over the whole values, and ToInt32 would silently truncate
                // them. Two NUMBERS, though, are exactly the typed case below
                // — 6.1.6.1.17-22 are ToInt32 on each and the i32 operation —
                // and a dynamic slot holding a number is what a seeded
                // hash or a PRNG's `x ^ (x << 13)` is on every step, so that
                // pair is taken inline and everything else keeps the helper
                // (whose ToNumeric can run user code and raise).
                llvm::Function* helper =
                    inst.op == il::Op::BitAnd   ? shared_.abi.bronze_dynamic_bitand
                    : inst.op == il::Op::BitOr  ? shared_.abi.bronze_dynamic_bitor
                    : inst.op == il::Op::BitXor ? shared_.abi.bronze_dynamic_bitxor
                    : inst.op == il::Op::Shl    ? shared_.abi.bronze_dynamic_shl
                    : inst.op == il::Op::Shr    ? shared_.abi.bronze_dynamic_shr
                                                : shared_.abi.bronze_dynamic_ushr;
                llvm::LLVMContext& ctx = builder_.getContext();
                llvm::Function* fn = builder_.GetInsertBlock()->getParent();
                llvm::Type* dblTy = builder_.getDoubleTy();
                llvm::Type* i64Ty = builder_.getInt64Ty();
                const bool lhsProven = isProvenNumberValue(lhs);
                const bool rhsProven = isProvenNumberValue(rhs);
                llvm::BasicBlock* numBb = llvm::BasicBlock::Create(ctx, "dbit.num", fn);
                llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "dbit.slow", fn);
                llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "dbit.done", fn);

                if (lhsProven && rhsProven) {
                    builder_.CreateBr(numBb);
                } else {
                    llvm::Value* lhsNum = lhsProven ? static_cast<llvm::Value*>(builder_.getTrue())
                                                    : builder_.CreateICmpULE(lhs, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "dbit.lnum");
                    llvm::Value* rhsNum = rhsProven ? static_cast<llvm::Value*>(builder_.getTrue())
                                                    : builder_.CreateICmpULE(rhs, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "dbit.rnum");
                    llvm::Value* cond = lhsProven ? rhsNum : (rhsProven ? lhsNum : builder_.CreateAnd(lhsNum, rhsNum, "dbit.bothnum"));
                    auto* br = builder_.CreateCondBr(cond, numBb, slowBb);
                    br->setMetadata(llvm::LLVMContext::MD_prof,
                                    llvm::MDBuilder(ctx).createBranchWeights(1048576, 1));
                }

                builder_.SetInsertPoint(numBb);
                llvm::Value* ld = unwrapBoxedDouble(lhs);
                if (!ld) ld = builder_.CreateBitCast(lhs, dblTy);
                llvm::Value* rd = unwrapBoxedDouble(rhs);
                if (!rd) rd = builder_.CreateBitCast(rhs, dblTy);
                llvm::Value* li = emitToInt32F64(builder_, shared_.abi, ld);
                llvm::Value* ri = emitToInt32F64(builder_, shared_.abi, rd);
                llvm::Value* bits = nullptr;
                switch (inst.op) {
                    case il::Op::BitAnd: bits = builder_.CreateAnd(li, ri); break;
                    case il::Op::BitOr: bits = builder_.CreateOr(li, ri); break;
                    case il::Op::BitXor: bits = builder_.CreateXor(li, ri); break;
                    default: {
                        llvm::Value* count = builder_.CreateAnd(ri, builder_.getInt32(31));
                        bits = inst.op == il::Op::Shl   ? builder_.CreateShl(li, count)
                               : inst.op == il::Op::Shr ? builder_.CreateAShr(li, count)
                                                        : builder_.CreateLShr(li, count);
                        break;
                    }
                }
                llvm::Value* fastDbl = inst.op == il::Op::UShr
                                           ? builder_.CreateUIToFP(bits, dblTy)
                                           : builder_.CreateSIToFP(bits, dblTy);
                llvm::Value* fastVal = builder_.CreateBitCast(fastDbl, i64Ty, "dbit.fast");
                llvm::BasicBlock* numEndBb = builder_.GetInsertBlock();
                builder_.CreateBr(doneBb);

                builder_.SetInsertPoint(slowBb);
                llvm::Value* slowVal = builder_.CreateCall(helper, {lhs, rhs}, "dbit.slowres");
                llvm::BasicBlock* slowEndBb = builder_.GetInsertBlock();
                builder_.CreateBr(doneBb);

                builder_.SetInsertPoint(doneBb);
                llvm::PHINode* result = builder_.CreatePHI(i64Ty, 2, "dbit.result");
                result->addIncoming(fastVal, numEndBb);
                result->addIncoming(slowVal, slowEndBb);
                values_[inst.result] = result;
                return true;
            }
            if (!require(lhs->getType()->isIntegerTy(32) && rhs->getType()->isIntegerTy(32),
                         (std::string("Non-i32 operand in ") + op + " (lowering bug)").c_str())) {
                return false;
            }
            llvm::Value* result = nullptr;
            switch (inst.op) {
                case il::Op::BitAnd: result = builder_.CreateAnd(lhs, rhs); break;
                case il::Op::BitOr: result = builder_.CreateOr(lhs, rhs); break;
                case il::Op::BitXor: result = builder_.CreateXor(lhs, rhs); break;
                case il::Op::MathImul: result = builder_.CreateMul(lhs, rhs); break;
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

        case il::Op::ConcatBegin:
            values_[inst.result] =
                emitConcatStep(builder_, shared_.abi.bronze_concat_begin, lhs, rhs,
                               builder_.getInt32(static_cast<uint32_t>(inst.immI32)));
            return true;

        case il::Op::ConcatAppend:
            values_[inst.result] = emitConcatStep(
                builder_, shared_.abi.bronze_concat_append, lhs, rhs, /*remaining=*/nullptr);
            return true;

        case il::Op::Add:
            if (inst.type == il::Type::Dynamic) {
                values_[inst.result] =
                    emitDynamicAdd(builder_, shared_.abi.bronze_dynamic_add, lhs, rhs);
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
            if (inst.type == il::Type::Dynamic) {
                llvm::Function* helper = inst.op == il::Op::Sub   ? shared_.abi.bronze_dynamic_sub
                                         : inst.op == il::Op::Mul ? shared_.abi.bronze_dynamic_mul
                                         : inst.op == il::Op::Div ? shared_.abi.bronze_dynamic_div
                                                                  : shared_.abi.bronze_dynamic_mod;
                values_[inst.result] = emitDynamicArith(builder_, helper, inst.op, lhs, rhs);
                return true;
            }
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
            // ORDERED, like the two above: `NaN <= 1` is false, which the
            // unordered forms (ULE/UGE) would answer true. That difference is
            // the whole of ECMA-262 13.10's "undefined becomes false".
            case il::Op::CmpLe: values_[inst.result] = builder_.CreateFCmpOLE(lhs, rhs); break;
            case il::Op::CmpGe: values_[inst.result] = builder_.CreateFCmpOGE(lhs, rhs); break;
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
            case il::Op::CmpLe: values_[inst.result] = builder_.CreateICmpSLE(lhs, rhs); break;
            case il::Op::CmpGe: values_[inst.result] = builder_.CreateICmpSGE(lhs, rhs); break;
            case il::Op::CmpEq: values_[inst.result] = builder_.CreateICmpEQ(lhs, rhs); break;
            default: values_[inst.result] = builder_.CreateICmpNE(lhs, rhs); break;
        }
        return true;
    }
    return require(false, (std::string("Unsupported operand type in ") + op).c_str());
}

}  // namespace bronze::codegen_llvm
