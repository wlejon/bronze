#include "codegen-llvm/llvm_strict_eq.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>

#include "abi/bronze_abi_tls.h"

namespace bronze::codegen_llvm {

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
llvm::Value* emitStrictEq(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* lhs,
                          llvm::Value* rhs) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* dblTy = builder.getDoubleTy();
    llvm::MDNode* likely = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);
    llvm::MDNode* unlikely = llvm::MDBuilder(ctx).createBranchWeights(1, 1048576);

    llvm::BasicBlock* seamBb = llvm::BasicBlock::Create(ctx, "seq.seam.ok", fn);
    llvm::BasicBlock* nonNumBb = llvm::BasicBlock::Create(ctx, "seq.nonnum", fn);
    llvm::BasicBlock* differBb = llvm::BasicBlock::Create(ctx, "seq.differ", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "seq.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "seq.done", fn);

    builder.CreateCondBr(emitStrictEqInlineEnabled(builder, abi), seamBb, slowBb, likely);
    builder.SetInsertPoint(seamBb);

    // Arm 1.
    llvm::Value* lhsNum = builder.CreateICmpULE(lhs, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS),
                                                "seq.lnum");
    llvm::Value* rhsNum = builder.CreateICmpULE(rhs, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS),
                                                "seq.rnum");
    llvm::BasicBlock* numBb = llvm::BasicBlock::Create(ctx, "seq.num", fn);
    builder.CreateCondBr(builder.CreateAnd(lhsNum, rhsNum, "seq.bothnum"), numBb, nonNumBb, likely);

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
    llvm::BasicBlock* strBb = llvm::BasicBlock::Create(ctx, "seq.str", fn);
    llvm::BasicBlock* strLenBb = llvm::BasicBlock::Create(ctx, "seq.strlen", fn);
    builder.CreateCondBr(isStr, strBb, strLenBb);

    // Arm 3, a String on the left: the helper's first two answers are made
    // here. A right operand that is not a string is false (7.2.15 step 1),
    // and two strings of different lengths are false (`StringHeader::equals`
    // opens on that compare; every string is flat, so the header's length is
    // the whole answer). Only two strings of one length reach the content
    // compare — which is what `unit.kind === 'titan'` on a miss usually is
    // not.
    builder.SetInsertPoint(strBb);
    llvm::Value* rtag = builder.CreateLShr(rhs, BRONZE_ABI_VALUE_TAG_SHIFT, "seq.rtag");
    llvm::Value* rIsStr =
        builder.CreateICmpEQ(rtag, builder.getInt64(BRONZE_ABI_TAG_STRING), "seq.risstr");
    llvm::BasicBlock* bothStrBb = llvm::BasicBlock::Create(ctx, "seq.bothstr", fn);
    builder.CreateCondBr(rIsStr, bothStrBb, doneBb);

    builder.SetInsertPoint(bothStrBb);
    llvm::Type* i8Ty = builder.getInt8Ty();
    llvm::Type* i16Ty = builder.getInt16Ty();
    llvm::Type* i32Ty = builder.getInt32Ty();
    llvm::Type* i64Ty = builder.getInt64Ty();
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::Value* lAddr =
        builder.CreateAnd(lhs, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK), "seq.laddr");
    llvm::Value* lHdr = builder.CreateIntToPtr(lAddr, ptrTy, "seq.lhdr");
    llvm::Value* lLenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, lHdr, BRONZE_ABI_STRING_LENGTH_OFFSET);
    llvm::Value* lLen = builder.CreateAlignedLoad(i32Ty, lLenPtr, llvm::Align(4), "seq.llen");

    llvm::Value* rAddr =
        builder.CreateAnd(rhs, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK), "seq.raddr");
    llvm::Value* rHdr = builder.CreateIntToPtr(rAddr, ptrTy, "seq.rhdr");
    llvm::Value* rLenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rHdr, BRONZE_ABI_STRING_LENGTH_OFFSET);
    llvm::Value* rLen = builder.CreateAlignedLoad(i32Ty, rLenPtr, llvm::Align(4), "seq.rlen");

    llvm::Value* sameLen = builder.CreateICmpEQ(lLen, rLen, "seq.samelen");
    llvm::BasicBlock* strCmpBb = llvm::BasicBlock::Create(ctx, "seq.strcmp", fn);
    builder.CreateCondBr(sameLen, strCmpBb, doneBb);

    builder.SetInsertPoint(strCmpBb);
    llvm::Value* lFlagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, lHdr, BRONZE_ABI_STRING_FLAGS_OFFSET);
    llvm::Value* lFlags =
        builder.CreateAlignedLoad(i32Ty, lFlagsPtr, llvm::Align(4), "seq.lflags");
    llvm::Value* rFlagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rHdr, BRONZE_ABI_STRING_FLAGS_OFFSET);
    llvm::Value* rFlags =
        builder.CreateAlignedLoad(i32Ty, rFlagsPtr, llvm::Align(4), "seq.rflags");

    llvm::Value* flagsOr = builder.CreateOr(lFlags, rFlags, "seq.flags.or");
    llvm::Value* utf16Bit = builder.CreateAnd(
        flagsOr, builder.getInt32(BRONZE_ABI_STRING_UTF16_BIT), "seq.utf16.bit");
    llvm::Value* hasUtf16 =
        builder.CreateICmpNE(utf16Bit, builder.getInt32(0), "seq.hasutf16");

    llvm::BasicBlock* latinBb = llvm::BasicBlock::Create(ctx, "seq.str.latin", fn);
    builder.CreateCondBr(hasUtf16, slowBb, latinBb, unlikely);

    builder.SetInsertPoint(latinBb);
    llvm::Value* lData =
        builder.CreateConstInBoundsGEP1_32(i8Ty, lHdr, BRONZE_ABI_STRING_DATA_OFFSET, "seq.ldata");
    llvm::Value* rData =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rHdr, BRONZE_ABI_STRING_DATA_OFFSET, "seq.rdata");

    llvm::BasicBlock* le8Bb = llvm::BasicBlock::Create(ctx, "seq.str.le8", fn);
    llvm::BasicBlock* gt8Bb = llvm::BasicBlock::Create(ctx, "seq.str.gt8", fn);
    llvm::Value* isLe8 = builder.CreateICmpULE(lLen, builder.getInt32(8), "seq.le8");
    builder.CreateCondBr(isLe8, le8Bb, gt8Bb);

    builder.SetInsertPoint(gt8Bb);
    llvm::BasicBlock* le16Bb = llvm::BasicBlock::Create(ctx, "seq.str.le16", fn);
    llvm::Value* isLe16 = builder.CreateICmpULE(lLen, builder.getInt32(16), "seq.le16");
    builder.CreateCondBr(isLe16, le16Bb, slowBb);

    builder.SetInsertPoint(le16Bb);
    llvm::Value* headL16 = builder.CreateAlignedLoad(i64Ty, lData, llvm::Align(1), "seq.h64.l");
    llvm::Value* headR16 = builder.CreateAlignedLoad(i64Ty, rData, llvm::Align(1), "seq.h64.r");
    llvm::Value* tailOff16 = builder.CreateSub(lLen, builder.getInt32(8), "seq.tail8.off");
    llvm::Value* lTail16Ptr = builder.CreateInBoundsGEP(i8Ty, lData, tailOff16, "seq.t64.lptr");
    llvm::Value* rTail16Ptr = builder.CreateInBoundsGEP(i8Ty, rData, tailOff16, "seq.t64.rptr");
    llvm::Value* tailL16 = builder.CreateAlignedLoad(i64Ty, lTail16Ptr, llvm::Align(1), "seq.t64.l");
    llvm::Value* tailR16 = builder.CreateAlignedLoad(i64Ty, rTail16Ptr, llvm::Align(1), "seq.t64.r");
    llvm::Value* headEq16 = builder.CreateICmpEQ(headL16, headR16, "seq.h64.eq");
    llvm::Value* tailEq16 = builder.CreateICmpEQ(tailL16, tailR16, "seq.t64.eq");
    llvm::Value* match16 = builder.CreateAnd(headEq16, tailEq16, "seq.match16");
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(le8Bb);
    llvm::BasicBlock* ge4Bb = llvm::BasicBlock::Create(ctx, "seq.str.ge4", fn);
    llvm::BasicBlock* lt4Bb = llvm::BasicBlock::Create(ctx, "seq.str.lt4", fn);
    llvm::Value* isGe4 = builder.CreateICmpUGE(lLen, builder.getInt32(4), "seq.ge4");
    builder.CreateCondBr(isGe4, ge4Bb, lt4Bb);

    builder.SetInsertPoint(ge4Bb);
    llvm::Value* headL8 = builder.CreateAlignedLoad(i32Ty, lData, llvm::Align(1), "seq.h32.l");
    llvm::Value* headR8 = builder.CreateAlignedLoad(i32Ty, rData, llvm::Align(1), "seq.h32.r");
    llvm::Value* tailOff8 = builder.CreateSub(lLen, builder.getInt32(4), "seq.tail4.off");
    llvm::Value* lTail8Ptr = builder.CreateInBoundsGEP(i8Ty, lData, tailOff8, "seq.t32.lptr");
    llvm::Value* rTail8Ptr = builder.CreateInBoundsGEP(i8Ty, rData, tailOff8, "seq.t32.rptr");
    llvm::Value* tailL8 = builder.CreateAlignedLoad(i32Ty, lTail8Ptr, llvm::Align(1), "seq.t32.l");
    llvm::Value* tailR8 = builder.CreateAlignedLoad(i32Ty, rTail8Ptr, llvm::Align(1), "seq.t32.r");
    llvm::Value* headEq8 = builder.CreateICmpEQ(headL8, headR8, "seq.h32.eq");
    llvm::Value* tailEq8 = builder.CreateICmpEQ(tailL8, tailR8, "seq.t32.eq");
    llvm::Value* match8 = builder.CreateAnd(headEq8, tailEq8, "seq.match8");
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(lt4Bb);
    llvm::BasicBlock* len0Bb = llvm::BasicBlock::Create(ctx, "seq.str.len0", fn);
    llvm::BasicBlock* len1Bb = llvm::BasicBlock::Create(ctx, "seq.str.len1", fn);
    llvm::BasicBlock* len23Bb = llvm::BasicBlock::Create(ctx, "seq.str.len23", fn);
    auto* sw = builder.CreateSwitch(lLen, len23Bb, 2);
    sw->addCase(builder.getInt32(0), len0Bb);
    sw->addCase(builder.getInt32(1), len1Bb);

    builder.SetInsertPoint(len0Bb);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(len1Bb);
    llvm::Value* bL = builder.CreateAlignedLoad(i8Ty, lData, llvm::Align(1), "seq.b.l");
    llvm::Value* bR = builder.CreateAlignedLoad(i8Ty, rData, llvm::Align(1), "seq.b.r");
    llvm::Value* match1 = builder.CreateICmpEQ(bL, bR, "seq.match1");
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(len23Bb);
    llvm::Value* headL2 = builder.CreateAlignedLoad(i16Ty, lData, llvm::Align(1), "seq.h16.l");
    llvm::Value* headR2 = builder.CreateAlignedLoad(i16Ty, rData, llvm::Align(1), "seq.h16.r");
    llvm::Value* tailOff2 = builder.CreateSub(lLen, builder.getInt32(2), "seq.tail2.off");
    llvm::Value* lTail2Ptr = builder.CreateInBoundsGEP(i8Ty, lData, tailOff2, "seq.t16.lptr");
    llvm::Value* rTail2Ptr = builder.CreateInBoundsGEP(i8Ty, rData, tailOff2, "seq.t16.rptr");
    llvm::Value* tailL2 = builder.CreateAlignedLoad(i16Ty, lTail2Ptr, llvm::Align(1), "seq.t16.l");
    llvm::Value* tailR2 = builder.CreateAlignedLoad(i16Ty, rTail2Ptr, llvm::Align(1), "seq.t16.r");
    llvm::Value* headEq2 = builder.CreateICmpEQ(headL2, headR2, "seq.h16.eq");
    llvm::Value* tailEq2 = builder.CreateICmpEQ(tailL2, tailR2, "seq.t16.eq");
    llvm::Value* match23 = builder.CreateAnd(headEq2, tailEq2, "seq.match23");
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(strLenBb);
    builder.CreateCondBr(isBig, slowBb, doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(abi.bronze_strict_eq, {lhs, rhs}, "seq.slowres");
    llvm::BasicBlock* slowEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(builder.getInt1Ty(), 11, "seq.result");
    result->addIncoming(numVal, numEndBb);
    result->addIncoming(builder.getTrue(), nonNumBb);
    result->addIncoming(builder.getFalse(), strBb);
    result->addIncoming(builder.getFalse(), bothStrBb);
    result->addIncoming(builder.getFalse(), strLenBb);
    result->addIncoming(slowVal, slowEndBb);
    result->addIncoming(builder.getTrue(), len0Bb);
    result->addIncoming(match1, len1Bb);
    result->addIncoming(match23, len23Bb);
    result->addIncoming(match8, ge4Bb);
    result->addIncoming(match16, le16Bb);
    return result;
}

}  // namespace bronze::codegen_llvm
