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

#include "codegen-llvm/llvm_convert.h"
#include "codegen-llvm/llvm_func.h"

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
    llvm::Value* lhsNum =
        builder.CreateICmpULE(lhs, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
    llvm::Value* rhsNum =
        builder.CreateICmpULE(rhs, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(builder.getContext(), name, fn);
    builder.CreateCondBr(builder.CreateAnd(lhsNum, rhsNum), fastBb, slowBb);
    builder.SetInsertPoint(fastBb);
    return nullptr;
}

// `a + b` over boxed operands: the number/number case — the loop-carried case
// in every allocation-free numeric loop — is an fadd and the canonicalizing
// re-box, mirroring the fast path at the top of bronze_dynamic_add; anything
// involving a string, an object or a symbol keeps the helper, which owns
// ToPrimitive and the concat/TypeError ladder.
llvm::Value* emitDynamicAdd(llvm::IRBuilder<>& builder, llvm::Function* helper, llvm::Value* lhs,
                            llvm::Value* rhs) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* dblTy = builder.getDoubleTy();

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "dadd.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "dadd.done", fn);

    branchIfBothNumbers(builder, lhs, rhs, slowBb, "dadd.fast");
    llvm::Value* sum =
        builder.CreateFAdd(builder.CreateBitCast(lhs, dblTy), builder.CreateBitCast(rhs, dblTy));
    // inf + -inf is NaN from two finite-looking inputs, so the sum needs the
    // same canonicalizing select the Box instruction emits.
    llvm::Value* isNan = builder.CreateFCmpUNO(sum, sum);
    llvm::Value* fastVal =
        builder.CreateSelect(isNan, builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS),
                             builder.CreateBitCast(sum, builder.getInt64Ty()));
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(helper, {lhs, rhs});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(builder.getInt64Ty(), 2, "dadd.result");
    result->addIncoming(fastVal, fastEndBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

// `a - b`, `a * b`, `a / b`, `a % b` over boxed operands. Same shape as
// `emitDynamicAdd` and for the same reason: the number/number case is the one
// a loop carries, and it is one machine instruction. What the helper owns is
// everything else — a string operand's ToNumber, an object's valueOf, and the
// BigInt algorithm with 13.15.3's mixing TypeError in front of it.
//
// The result needs the canonicalizing NaN select for the same reason `+` does:
// `inf - inf`, `0 * inf`, `0 / 0` and `x % 0` each produce a NaN out of two
// finite-looking inputs, and an uncanonicalized NaN is a bit pattern the value
// model does not admit.
llvm::Value* emitDynamicArith(llvm::IRBuilder<>& builder, llvm::Function* helper, il::Op op,
                              llvm::Value* lhs, llvm::Value* rhs) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* dblTy = builder.getDoubleTy();

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "darith.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "darith.done", fn);

    branchIfBothNumbers(builder, lhs, rhs, slowBb, "darith.fast");
    llvm::Value* l = builder.CreateBitCast(lhs, dblTy);
    llvm::Value* r = builder.CreateBitCast(rhs, dblTy);
    llvm::Value* num = op == il::Op::Sub   ? builder.CreateFSub(l, r)
                       : op == il::Op::Mul ? builder.CreateFMul(l, r)
                       : op == il::Op::Div ? builder.CreateFDiv(l, r)
                                           : builder.CreateFRem(l, r);
    llvm::Value* isNan = builder.CreateFCmpUNO(num, num);
    llvm::Value* fastVal =
        builder.CreateSelect(isNan, builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS),
                             builder.CreateBitCast(num, builder.getInt64Ty()));
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(helper, {lhs, rhs});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(builder.getInt64Ty(), 2, "darith.result");
    result->addIncoming(fastVal, fastEndBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

// The four relational operators over boxed operands: two numbers are one
// ORDERED fcmp — false for a NaN on either side, which is exactly 13.10's
// "undefined becomes false" for all four members of the family. Everything
// else (strings compare by code unit, objects unwrap) keeps the helper —
// except an `undefined` paired with a number (or another `undefined`), which
// a second, off-the-hot-path arm answers with constant false: 13.10.1 calls
// ToPrimitive on both operands first, but `undefined` and a number are
// already primitive, so no user code can run, ToNumeric(undefined) is NaN,
// and every ordered comparison against a NaN is false. three.js leans on
// this shape once per visible object per frame (`material.transmission >
// 0.0` in WebGLRenderList.push, where the property does not exist). The arm
// sits behind the BRONZE_NO_UNDEF_REL seam; the both-numbers arm is
// unchanged and never pays for it.
llvm::Value* emitDynamicRel(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Function* helper,
                            llvm::CmpInst::Predicate pred, llvm::Value* lhs, llvm::Value* rhs) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* dblTy = builder.getDoubleTy();

    llvm::BasicBlock* undefBb = llvm::BasicBlock::Create(ctx, "drel.undef", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "drel.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "drel.done", fn);

    branchIfBothNumbers(builder, lhs, rhs, undefBb, "drel.fast");
    llvm::Value* fastVal = builder.CreateFCmp(pred, builder.CreateBitCast(lhs, dblTy),
                                              builder.CreateBitCast(rhs, dblTy), "drel.cmp");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // Not both numbers: if the seam is on and each operand is a number or
    // `undefined`, at least one is `undefined` (both-numbers already left),
    // so the answer is false for all four ordered predicates.
    builder.SetInsertPoint(undefBb);
    llvm::Value* base = builder.CreateCall(abi.bronze_tls_block_addr, {}, "tls");
    llvm::Value* cellPtr = builder.CreateConstInBoundsGEP1_64(
        builder.getInt8Ty(), base, BRONZE_TLS_UNDEF_REL_ENABLED_OFF, "tls.undefrel");
    llvm::Value* cell =
        builder.CreateAlignedLoad(builder.getInt64Ty(), cellPtr, llvm::Align(8), "undefrel.seam");
    llvm::Value* seamOn = builder.CreateICmpNE(cell, builder.getInt64(0), "undefrel.on");
    llvm::Value* undef = builder.getInt64(BRONZE_ABI_UNDEFINED_BITS);
    llvm::Value* numMax = builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS);
    llvm::Value* lOk = builder.CreateOr(builder.CreateICmpULE(lhs, numMax),
                                        builder.CreateICmpEQ(lhs, undef), "drel.lok");
    llvm::Value* rOk = builder.CreateOr(builder.CreateICmpULE(rhs, numMax),
                                        builder.CreateICmpEQ(rhs, undef), "drel.rok");
    llvm::Value* take =
        builder.CreateAnd(seamOn, builder.CreateAnd(lOk, rOk), "drel.undef.take");
    llvm::BasicBlock* undefEndBb = builder.GetInsertBlock();
    builder.CreateCondBr(take, doneBb, slowBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(helper, {lhs, rhs});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(builder.getInt1Ty(), 3, "drel.result");
    result->addIncoming(fastVal, fastEndBb);
    result->addIncoming(builder.getFalse(), undefEndBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

// The seam word for the inline `===`. `bronze_tls_block_addr` is `readnone` +
// `willreturn`, so this call CSEs with the prologue's fetch and a loop hoists
// it — the same shape llvm_iter.cpp's `emitIterFastEnabled` uses, and for the
// same reason: this file has `AbiFns` but not `AbiGlobals`.
llvm::Value* emitStrictEqInlineEnabled(llvm::IRBuilder<>& builder, const AbiFns& abi) {
    llvm::Value* base = builder.CreateCall(abi.bronze_tls_block_addr, {}, "tls");
    llvm::Value* cellPtr = builder.CreateConstInBoundsGEP1_64(
        builder.getInt8Ty(), base, BRONZE_TLS_STRICT_EQ_INLINE_ENABLED_OFF, "tls.seqinline");
    llvm::Value* cell =
        builder.CreateAlignedLoad(builder.getInt64Ty(), cellPtr, llvm::Align(8), "seq.seam");
    return builder.CreateICmpNE(cell, builder.getInt64(0), "seq.seam.on");
}

// `a === b` over boxed operands, as three arms and a helper.
//
// The helper (rt_convert.cpp `bronze_strict_eq`) is four tests, and the
// chunk-4 sampler charged the CALL to it 2.71 % of the `many_meshes` frame —
// three.js asks this question about markers, `undefined`, `null` and object
// identity thousands of times a draw. Every one of those is answered here.
//
// The arms, and why each is exactly the helper's answer:
//
//  1. BOTH NUMBERS -> one ORDERED fcmp. This arm exists first and not as a
//     special case of bit equality, because bit equality gets both of the
//     IEEE-754 edges wrong in opposite directions: two values that are the
//     SAME NaN have identical bits and `===` says false, and `+0` and `-0`
//     have different bits and `===` says true. `fcmp oeq` is both of those,
//     which is why the helper's number row is `==` on doubles and not on bits.
//
//  2. NOT both numbers, and the BITS ARE EQUAL -> true. Sound because equal
//     bits means both operands are numbers or neither is, and arm 1 already
//     took the case where both are: so here neither is a number, and identical
//     bits are the same object, the same string, the same BigInt, the same
//     symbol or the same immediate. Every one of those is `===`.
//
//  3. Bits differ -> false, UNLESS the left operand is a String or a BigInt.
//     Those are the only two rows in the helper that can answer true for
//     different bits (content equality and mathematical-value equality); for
//     every other tag "different bits" is the helper's own final `aBits ==
//     bBits`. A number's top sixteen bits are below every tag, so the tag test
//     is safe on an operand arm 1 rejected.
//
// A String or BigInt on the left is the only thing that reaches the helper,
// and there it takes the path it always took.
llvm::Value* emitStrictEq(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* lhs,
                          llvm::Value* rhs) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* dblTy = builder.getDoubleTy();

    llvm::BasicBlock* seamBb = llvm::BasicBlock::Create(ctx, "seq.seam.ok", fn);
    llvm::BasicBlock* nonNumBb = llvm::BasicBlock::Create(ctx, "seq.nonnum", fn);
    llvm::BasicBlock* differBb = llvm::BasicBlock::Create(ctx, "seq.differ", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "seq.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "seq.done", fn);

    builder.CreateCondBr(emitStrictEqInlineEnabled(builder, abi), seamBb, slowBb);
    builder.SetInsertPoint(seamBb);

    // Arm 1.
    llvm::Value* lhsNum = builder.CreateICmpULE(lhs, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS),
                                                "seq.lnum");
    llvm::Value* rhsNum = builder.CreateICmpULE(rhs, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS),
                                                "seq.rnum");
    llvm::BasicBlock* numBb = llvm::BasicBlock::Create(ctx, "seq.num", fn);
    builder.CreateCondBr(builder.CreateAnd(lhsNum, rhsNum, "seq.bothnum"), numBb, nonNumBb);

    builder.SetInsertPoint(numBb);
    llvm::Value* numVal = builder.CreateFCmpOEQ(builder.CreateBitCast(lhs, dblTy),
                                                builder.CreateBitCast(rhs, dblTy), "seq.numcmp");
    llvm::BasicBlock* numEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // Arm 2.
    builder.SetInsertPoint(nonNumBb);
    builder.CreateCondBr(builder.CreateICmpEQ(lhs, rhs, "seq.samebits"), doneBb, differBb);

    // Arm 3.
    builder.SetInsertPoint(differBb);
    llvm::Value* tag = builder.CreateLShr(lhs, BRONZE_ABI_VALUE_TAG_SHIFT, "seq.tag");
    llvm::Value* isStr =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_STRING), "seq.isstr");
    llvm::Value* isBig =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_BIGINT), "seq.isbig");
    builder.CreateCondBr(builder.CreateOr(isStr, isBig, "seq.byvalue"), slowBb, doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(abi.bronze_strict_eq, {lhs, rhs}, "seq.slowres");
    llvm::BasicBlock* slowEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(builder.getInt1Ty(), 4, "seq.result");
    result->addIncoming(numVal, numEndBb);
    result->addIncoming(builder.getTrue(), nonNumBb);
    result->addIncoming(builder.getFalse(), differBb);
    result->addIncoming(slowVal, slowEndBb);
    return result;
}

}  // namespace

bool FunctionEmitter::emitArithmetic(const il::Instruction& inst) {
    const char* op = il::opName(inst.op);
    const bool unary = inst.op == il::Op::Neg || inst.op == il::Op::ToInt32 ||
                       inst.op == il::Op::NumTruthy || inst.op == il::Op::BitNot ||
                       inst.op == il::Op::ToNumeric || inst.op == il::Op::NumericStep;
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
            // The boxed form, which is a different operation: ToNumber runs
            // first, so a string is parsed and an object's valueOf is called.
            // That can execute program text, so it stays a helper call.
            values_[inst.result] = builder_.CreateCall(shared_.abi.bronze_to_int32, {lhs});
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
        llvm::Value* isNum =
            builder_.CreateICmpULE(lhs, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
        llvm::BasicBlock* fastBb =
            llvm::BasicBlock::Create(ctx, std::string(tag) + ".fast", fn);
        builder_.CreateCondBr(isNum, fastBb, slowBb);

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
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr: {
            if (inst.type == il::Type::Dynamic) {
                // Boxed operands, so the int32 conversion has not happened and
                // must not: on a BigInt pair the operator is defined over the
                // whole values, and ToInt32 would silently truncate them.
                llvm::Function* helper =
                    inst.op == il::Op::BitAnd   ? shared_.abi.bronze_dynamic_bitand
                    : inst.op == il::Op::BitOr  ? shared_.abi.bronze_dynamic_bitor
                    : inst.op == il::Op::BitXor ? shared_.abi.bronze_dynamic_bitxor
                    : inst.op == il::Op::Shl    ? shared_.abi.bronze_dynamic_shl
                    : inst.op == il::Op::Shr    ? shared_.abi.bronze_dynamic_shr
                                                : shared_.abi.bronze_dynamic_ushr;
                values_[inst.result] = builder_.CreateCall(helper, {lhs, rhs});
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
