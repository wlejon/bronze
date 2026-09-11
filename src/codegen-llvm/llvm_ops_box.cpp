// The IL boxing and unboxing operations: Box and Unbox.
// Factored out of llvm_ops.cpp to keep file sizes cleanly under the 1,000-line hard rule.

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_cache.h"
#include "codegen-llvm/llvm_func.h"

namespace bronze::codegen_llvm {

bool FunctionEmitter::emitBox(const il::Instruction& inst) {
    const AbiFns& abi = shared_.abi;
    if (inst.result == il::kNoValue) return true;

    // A Str box names a registered key when operands are empty (literal),
    // or calls bronze_box_str when boxing a dynamic const char* pointer.
    if (inst.boxType == il::Type::Str) {
        if (inst.operands.empty()) {
            values_[inst.result] =
                builder_.CreateCall(abi.bronze_box_str_key,
                                    {emitKeyId(builder_, shared_.tables, inst.keyIndex)});
            return true;
        }
        llvm::Value* src = operand(inst, 0, "Undefined value in Box Str instruction");
        if (!src) return false;
        values_[inst.result] = builder_.CreateCall(abi.bronze_box_str, {src});
        return true;
    }
    if (!require(inst.operands.size() >= 1 && inst.result != il::kNoValue,
                 "Invalid operands for Box")) {
        return false;
    }
    llvm::Value* src = operand(inst, 0, "Undefined value in Box instruction");
    if (!src) return false;

    if (inst.boxType == il::Type::F64 || src->getType()->isDoubleTy()) {
        // Boxing a double that was BITCAST OUT OF A VALUE is the identity, and
        // this is the one place that can see it. Two emitters produce such a
        // double and no third one does: the raw unbox (il.h `rawUnbox`) and
        // the pinned plain-array element read (`kElemKindPlainArrayF64`,
        // llvm_elem.cpp) — every other bitcast to double in this backend feeds
        // an arithmetic instruction or a phi, never the value table. Both carry
        // the claim that the source bits are a Number, and every Value that is
        // a Number carries a canonical NaN by construction, so the select below
        // can only choose the arm the bits came from.
        if (auto* cast = llvm::dyn_cast<llvm::BitCastInst>(src);
            cast != nullptr && cast->getSrcTy()->isIntegerTy(64)) {
            values_[inst.result] = cast->getOperand(0);
            return true;
        }
        llvm::Value* isNan = builder_.CreateFCmpUNO(src, src);
        llvm::Value* bitcast = builder_.CreateBitCast(src, builder_.getInt64Ty());
        values_[inst.result] = builder_.CreateSelect(
            isNan, builder_.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS), bitcast);
        return true;
    }
    if (inst.boxType == il::Type::Bool || src->getType()->isIntegerTy(1)) {
        llvm::Value* isTrue = builder_.CreateIsNotNull(src);
        llvm::Value* tagShifted =
            builder_.getInt64(static_cast<uint64_t>(BRONZE_ABI_TAG_BOOL) << BRONZE_ABI_VALUE_TAG_SHIFT);
        llvm::Value* zext = builder_.CreateZExt(isTrue, builder_.getInt64Ty());
        values_[inst.result] = builder_.CreateOr(tagShifted, zext);
        return true;
    }
    if (inst.boxType == il::Type::I32 || src->getType()->isIntegerTy(32)) {
        llvm::Value* tagShifted =
            builder_.getInt64(static_cast<uint64_t>(BRONZE_ABI_TAG_INT32) << BRONZE_ABI_VALUE_TAG_SHIFT);
        llvm::Value* zext = builder_.CreateZExt(src, builder_.getInt64Ty());
        values_[inst.result] = builder_.CreateOr(tagShifted, zext);
        return true;
    }
    values_[inst.result] = builder_.CreateCall(abi.bronze_box_f64, {src});
    return true;
}

bool FunctionEmitter::emitUnbox(const il::Instruction& inst) {
    const AbiFns& abi = shared_.abi;
    if (!require(inst.operands.size() >= 1 && inst.result != il::kNoValue,
                 "Invalid operands for Unbox")) {
        return false;
    }
    llvm::Value* src = operand(inst, 0, "Undefined value in Unbox instruction");
    if (!src) return false;

    if (inst.type == il::Type::I32) {
        llvm::Type* dblTy = builder_.getDoubleTy();
        llvm::Value* isNum = builder_.CreateICmpULE(
            src, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "unbox.i32.isnum");
        llvm::Value* fastDouble = builder_.CreateBitCast(src, dblTy);
        // "Is a number" is not enough to license the conversion, and this is the
        // same defect emitElemGuards' fptoui had: `fptosi` of a double outside the
        // destination range is POISON — not a wrong number, a value the optimizer
        // may assume never occurs — and it flows straight into the phi below.
        // ToInt32 also does not TRUNCATE out of range, it wraps modulo 2^32, so
        // an in-range test is what the language wants anyway; everything else
        // goes to the helper, which owns the wrap. NaN fails both ordered compares
        // and takes the same road.
        llvm::Value* ge = builder_.CreateFCmpOGE(
            fastDouble, llvm::ConstantFP::get(dblTy, -2147483648.0));
        llvm::Value* lt = builder_.CreateFCmpOLT(
            fastDouble, llvm::ConstantFP::get(dblTy, 2147483648.0));
        llvm::Value* inRange = builder_.CreateAnd(ge, lt);
        // The select is what makes the operand safe on EVERY path rather than
        // merely on the taken one: both live in this basic block, and ordering
        // the checks does not stop the optimizer from folding the conversion first.
        llvm::Value* safeDouble = builder_.CreateSelect(
            inRange, fastDouble, llvm::ConstantFP::get(dblTy, 0.0));
        llvm::Value* fastI32 =
            builder_.CreateFPToSI(safeDouble, builder_.getInt32Ty(), "unbox.i32.val");
        isNum = builder_.CreateAnd(isNum, inRange);

        llvm::LLVMContext& ctx = builder_.getContext();
        llvm::Function* fn = builder_.GetInsertBlock()->getParent();
        llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "unbox.i32.slow", fn);
        llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "unbox.i32.done", fn);
        llvm::BasicBlock* curBb = builder_.GetInsertBlock();

        builder_.CreateCondBr(isNum, doneBb, slowBb);

        builder_.SetInsertPoint(slowBb);
        llvm::Value* slowVal = builder_.CreateCall(abi.bronze_unbox_i32, {src});
        llvm::BasicBlock* slowEndBb = builder_.GetInsertBlock();
        builder_.CreateBr(doneBb);

        builder_.SetInsertPoint(doneBb);
        llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt32Ty(), 2, "unbox.i32.res");
        phi->addIncoming(fastI32, curBb);
        phi->addIncoming(slowVal, slowEndBb);
        values_[inst.result] = phi;
        return true;
    }

    if (inst.type == il::Type::Bool) {
        // Truthiness, three arms. The number arm is the shape it always was: an
        // ordered compare against 0.0, which answers false for NaN by itself. The
        // second arm (seam: BRONZE_NO_TRUTHY_INLINE=1, the tls word below) answers
        // every operand whose truthiness is its BIT PATTERN: `true`/`false`,
        // `undefined`/`null`/the hole are five exact constants, an Int32 is its low
        // payload against zero, and an object — three.js's `if (object.visible)`
        // receivers' other half being plain `material.transparent` booleans — is
        // always true (7.1.2 has no falsy object row; bronze hosts no IsHTMLDDA
        // exotic). A string's truthiness is its LENGTH and a BigInt's is its
        // limbs — both behind a pointer — so those two keep the bronze_unbox_bool
        // helper they always took.
        llvm::Value* isNum = builder_.CreateICmpULE(
            src, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "unbox.bool.isnum");
        llvm::Value* fastDouble = builder_.CreateBitCast(src, builder_.getDoubleTy());
        llvm::Value* fastTruthy = builder_.CreateFCmpONE(
            fastDouble, llvm::ConstantFP::get(builder_.getDoubleTy(), 0.0), "unbox.bool.val");

        llvm::LLVMContext& ctx = builder_.getContext();
        llvm::Function* fn = builder_.GetInsertBlock()->getParent();
        llvm::BasicBlock* tagBb = llvm::BasicBlock::Create(ctx, "unbox.bool.tag", fn);
        llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "unbox.bool.slow", fn);
        llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "unbox.bool.done", fn);
        llvm::BasicBlock* curBb = builder_.GetInsertBlock();

        builder_.CreateCondBr(isNum, doneBb, tagBb);

        builder_.SetInsertPoint(tagBb);
        llvm::Value* base = builder_.CreateCall(abi.bronze_tls_block_addr, {}, "tls");
        llvm::Value* seamPtr = builder_.CreateConstInBoundsGEP1_64(
            builder_.getInt8Ty(), base, BRONZE_TLS_TRUTHY_INLINE_ENABLED_OFF,
            "tls.truthy");
        llvm::Value* seamCell = builder_.CreateAlignedLoad(
            builder_.getInt64Ty(), seamPtr, llvm::Align(8), "truthy.seam");
        llvm::Value* seamOn =
            builder_.CreateICmpNE(seamCell, builder_.getInt64(0), "truthy.on");
        const uint64_t trueBits =
            (static_cast<uint64_t>(BRONZE_ABI_TAG_BOOL) << BRONZE_ABI_VALUE_TAG_SHIFT) | 1ull;
        const uint64_t falseBits =
            static_cast<uint64_t>(BRONZE_ABI_TAG_BOOL) << BRONZE_ABI_VALUE_TAG_SHIFT;
        llvm::Value* isTrue =
            builder_.CreateICmpEQ(src, builder_.getInt64(trueBits), "truthy.istrue");
        llvm::Value* isFalseLike = builder_.CreateOr(
            builder_.CreateOr(
                builder_.CreateICmpEQ(src, builder_.getInt64(falseBits)),
                builder_.CreateICmpEQ(src, builder_.getInt64(BRONZE_ABI_UNDEFINED_BITS))),
            builder_.CreateOr(
                builder_.CreateICmpEQ(src, builder_.getInt64(BRONZE_ABI_NULL_BITS)),
                builder_.CreateICmpEQ(src, builder_.getInt64(BRONZE_ABI_HOLE_BITS))),
            "truthy.isfalselike");
        llvm::Value* tag = builder_.CreateLShr(
            src, builder_.getInt64(BRONZE_ABI_VALUE_TAG_SHIFT), "truthy.tagbits");
        llvm::Value* isObj = builder_.CreateICmpEQ(
            tag, builder_.getInt64(BRONZE_ABI_TAG_OBJECT), "truthy.isobj");
        llvm::Value* isInt32 = builder_.CreateICmpEQ(
            tag, builder_.getInt64(BRONZE_ABI_TAG_INT32), "truthy.isint32");
        llvm::Value* int32Truthy = builder_.CreateICmpNE(
            builder_.CreateTrunc(src, builder_.getInt32Ty()),
            builder_.getInt32(0), "truthy.i32val");
        llvm::Value* known = builder_.CreateOr(
            builder_.CreateOr(isTrue, isFalseLike),
            builder_.CreateOr(isObj, isInt32), "truthy.known");
        llvm::Value* take = builder_.CreateAnd(seamOn, known, "truthy.take");
        llvm::Value* armVal = builder_.CreateOr(
            builder_.CreateOr(isTrue, isObj),
            builder_.CreateAnd(isInt32, int32Truthy), "truthy.armval");
        llvm::BasicBlock* tagEndBb = builder_.GetInsertBlock();
        builder_.CreateCondBr(take, doneBb, slowBb);

        builder_.SetInsertPoint(slowBb);
        llvm::Value* slowVal = builder_.CreateCall(abi.bronze_unbox_bool, {src});
        llvm::BasicBlock* slowEndBb = builder_.GetInsertBlock();
        builder_.CreateBr(doneBb);

        builder_.SetInsertPoint(doneBb);
        llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt1Ty(), 3, "unbox.bool.res");
        phi->addIncoming(fastTruthy, curBb);
        phi->addIncoming(armVal, tagEndBb);
        phi->addIncoming(slowVal, slowEndBb);
        values_[inst.result] = phi;
        return true;
    }

    if (inst.type == il::Type::F64 && inst.rawUnbox) {
        // The whole of an "unboxed f64 field", and it is one instruction.
        // bronze's Value is NaN-boxed with the number range at the BOTTOM of the
        // encoding (`kNumberMaxBits`), so a Number's 64 bits are exactly its
        // double's 64 bits — a slot holding one already IS raw f64 storage, and
        // the collector already walks past it as a non-pointer. Nothing about the
        // representation has to change; what changes is that a site carrying the
        // proof stops paying for the test.
        values_[inst.result] =
            builder_.CreateBitCast(src, builder_.getDoubleTy(), "unbox.raw");
        return true;
    }

    if (inst.type == il::Type::F64 && inst.nullishUnbox) {
        // ToNumber over a domain of three (il.h `nullishUnbox`), with no basic
        // block of its own. A branch here would split the surrounding arithmetic
        // into four blocks and put a phi in the middle of a chain of fmuls; two
        // selects cost less than that even when the number arm is the only one ever taken.
        llvm::Type* dblTy = builder_.getDoubleTy();
        llvm::Value* isNum = builder_.CreateICmpULE(
            src, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "unbox.nullish.isnum");
        llvm::Value* asDouble = builder_.CreateBitCast(src, dblTy);
        llvm::Value* isNull = builder_.CreateICmpEQ(
            src, builder_.getInt64(BRONZE_ABI_NULL_BITS), "unbox.nullish.isnull");
        // 7.1.4 table 14: null is +0, undefined is NaN. The pin says there is no
        // third nullish value, so the undefined arm is also the default arm.
        llvm::Value* nullishVal = builder_.CreateSelect(
            isNull, llvm::ConstantFP::get(dblTy, 0.0),
            llvm::ConstantFP::getNaN(dblTy), "unbox.nullish.other");
        values_[inst.result] =
            builder_.CreateSelect(isNum, asDouble, nullishVal, "unbox.nullish.val");
        return true;
    }

    if (inst.type == il::Type::F64) {
        llvm::Value* isNum = builder_.CreateICmpULE(
            src, builder_.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "unbox.isnum");
        llvm::Value* fastDouble = builder_.CreateBitCast(src, builder_.getDoubleTy());

        llvm::LLVMContext& ctx = builder_.getContext();
        llvm::Function* fn = builder_.GetInsertBlock()->getParent();
        llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "unbox.slow", fn);
        llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "unbox.done", fn);
        llvm::BasicBlock* curBb = builder_.GetInsertBlock();

        builder_.CreateCondBr(isNum, doneBb, slowBb);

        builder_.SetInsertPoint(slowBb);
        llvm::Value* slowVal = builder_.CreateCall(abi.bronze_unbox_f64, {src});
        llvm::BasicBlock* slowEndBb = builder_.GetInsertBlock();
        builder_.CreateBr(doneBb);

        builder_.SetInsertPoint(doneBb);
        llvm::PHINode* phi = builder_.CreatePHI(builder_.getDoubleTy(), 2, "unbox.val");
        phi->addIncoming(fastDouble, curBb);
        phi->addIncoming(slowVal, slowEndBb);
        values_[inst.result] = phi;
        return true;
    }

    if (inst.type == il::Type::Str) {
        values_[inst.result] = builder_.CreateCall(abi.bronze_unbox_str, {src});
        return true;
    }

    values_[inst.result] = builder_.CreateCall(abi.bronze_unbox_f64, {src});
    return true;
}

}  // namespace bronze::codegen_llvm
