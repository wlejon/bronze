// String and numeric concatenation chain code generation (+ operator spines).
//
// Chained additions ((a + b) + c) + d lower to ConcatBegin followed by
// ConcatAppend steps and sealed by ConcatEnd. When the accumulator is a
// string builder and operands are Latin-1 strings with sufficient capacity,
// ConcatAppend is inlined directly in machine code via LLVM IR without
// helper callouts. ConcatEnd inlines the builder sealing bit-clear.

#include "codegen-llvm/llvm_concat.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/MDBuilder.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_elem_typed.h"

namespace bronze::codegen_llvm {

namespace {

llvm::Value* canonicalizeNumeric(llvm::IRBuilder<>& builder, llvm::Value* sum) {
    llvm::Value* isNan = builder.CreateFCmpUNO(sum, sum);
    return builder.CreateSelect(isNan, builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS),
                                builder.CreateBitCast(sum, builder.getInt64Ty()));
}

}  // namespace

llvm::Value* emitConcatStep(llvm::IRBuilder<>& builder, llvm::Function* helper, llvm::Value* lhs,
                            llvm::Value* rhs, llvm::Value* remaining) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = builder.getInt8Ty();
    llvm::Type* i16Ty = builder.getInt16Ty();
    llvm::Type* i32Ty = builder.getInt32Ty();
    llvm::Type* i64Ty = builder.getInt64Ty();
    llvm::Type* ptrTy = builder.getPtrTy();

    llvm::BasicBlock* notNumBb = llvm::BasicBlock::Create(ctx, "cat.not_num", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "cat.done", fn);

    // 1. Number fast path: both operands are numbers
    const bool lhsProven = isProvenNumberValue(lhs);
    const bool rhsProven = isProvenNumberValue(rhs);
    llvm::BasicBlock* numBb = llvm::BasicBlock::Create(ctx, "cat.num", fn);
    if (lhsProven && rhsProven) {
        builder.CreateBr(numBb);
    } else {
        llvm::Value* lhsNum = lhsProven ? static_cast<llvm::Value*>(builder.getTrue())
                                        : builder.CreateICmpULE(lhs, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
        llvm::Value* rhsNum = rhsProven ? static_cast<llvm::Value*>(builder.getTrue())
                                        : builder.CreateICmpULE(rhs, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
        llvm::Value* cond = lhsProven ? rhsNum : (rhsProven ? lhsNum : builder.CreateAnd(lhsNum, rhsNum));
        auto* br = builder.CreateCondBr(cond, numBb, notNumBb);
        br->setMetadata(llvm::LLVMContext::MD_prof,
                        llvm::MDBuilder(ctx).createBranchWeights(1048576, 1));
    }

    builder.SetInsertPoint(numBb);
    llvm::Value* ld = unwrapBoxedDouble(lhs);
    if (!ld) ld = builder.CreateBitCast(lhs, builder.getDoubleTy());
    llvm::Value* rd = unwrapBoxedDouble(rhs);
    if (!rd) rd = builder.CreateBitCast(rhs, builder.getDoubleTy());
    llvm::Value* sum = builder.CreateFAdd(ld, rd);
    llvm::Value* numRes = canonicalizeNumeric(builder, sum);
    llvm::BasicBlock* numEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(notNumBb);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "cat.slow", fn);

    if (remaining == nullptr) {
        // ConcatAppend: fast path for inlined string-builder append
        llvm::BasicBlock* strAppendBb = llvm::BasicBlock::Create(ctx, "cat.str_append", fn);
        llvm::Value* lhsTag = builder.CreateLShr(lhs, builder.getInt64(BRONZE_ABI_VALUE_TAG_SHIFT));
        llvm::Value* rhsTag = builder.CreateLShr(rhs, builder.getInt64(BRONZE_ABI_VALUE_TAG_SHIFT));
        llvm::Value* lhsIsStr = builder.CreateICmpEQ(lhsTag, builder.getInt64(BRONZE_ABI_TAG_STRING));
        llvm::Value* rhsIsStr = builder.CreateICmpEQ(rhsTag, builder.getInt64(BRONZE_ABI_TAG_STRING));
        llvm::Value* bothStr = builder.CreateAnd(lhsIsStr, rhsIsStr);
        builder.CreateCondBr(bothStr, strAppendBb, slowBb);

        builder.SetInsertPoint(strAppendBb);
        llvm::Value* mask = builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK);
        llvm::Value* lhsAddr = builder.CreateAnd(lhs, mask);
        llvm::Value* rhsAddr = builder.CreateAnd(rhs, mask);
        llvm::Value* lhsHdr = builder.CreateIntToPtr(lhsAddr, ptrTy, "cat.lhdr");
        llvm::Value* rhsHdr = builder.CreateIntToPtr(rhsAddr, ptrTy, "cat.rhdr");

        // Check if lhs is a builder (HeapObjectHeader::flags has kBuilderFlag)
        llvm::Value* bFlagsPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, lhsHdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
        llvm::Value* bFlags =
            builder.CreateAlignedLoad(i16Ty, bFlagsPtr, llvm::Align(2), "cat.bflags");
        llvm::Value* isBuilder = builder.CreateICmpNE(
            builder.CreateAnd(bFlags, builder.getInt16(BRONZE_ABI_STRING_BUILDER_BIT)),
            builder.getInt16(0));

        // Check if both strings are Latin1 (UTF16 flag is clear on both)
        llvm::Value* lStrFlagsPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, lhsHdr, BRONZE_ABI_STRING_FLAGS_OFFSET);
        llvm::Value* lStrFlags =
            builder.CreateAlignedLoad(i32Ty, lStrFlagsPtr, llvm::Align(4), "cat.lstrflags");
        llvm::Value* rStrFlagsPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, rhsHdr, BRONZE_ABI_STRING_FLAGS_OFFSET);
        llvm::Value* rStrFlags =
            builder.CreateAlignedLoad(i32Ty, rStrFlagsPtr, llvm::Align(4), "cat.rstrflags");
        llvm::Value* bothLatin1 = builder.CreateICmpEQ(
            builder.CreateAnd(builder.CreateOr(lStrFlags, rStrFlags),
                              builder.getInt32(BRONZE_ABI_STRING_UTF16_BIT)),
            builder.getInt32(0));

        llvm::Value* canInline = builder.CreateAnd(isBuilder, bothLatin1);
        llvm::BasicBlock* capCheckBb = llvm::BasicBlock::Create(ctx, "cat.cap_check", fn);
        builder.CreateCondBr(canInline, capCheckBb, slowBb);

        builder.SetInsertPoint(capCheckBb);
        llvm::Value* lLenPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, lhsHdr, BRONZE_ABI_STRING_LENGTH_OFFSET);
        llvm::Value* oldLen =
            builder.CreateAlignedLoad(i32Ty, lLenPtr, llvm::Align(4), "cat.oldlen");
        llvm::Value* rLenPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, rhsHdr, BRONZE_ABI_STRING_LENGTH_OFFSET);
        llvm::Value* addLen =
            builder.CreateAlignedLoad(i32Ty, rLenPtr, llvm::Align(4), "cat.addlen");

        llvm::Value* newLen = builder.CreateAdd(oldLen, addLen, "cat.newlen");
        llvm::Value* hdrSizePtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, lhsHdr, BRONZE_ABI_HDR_SIZE_OFFSET);
        llvm::Value* hdrSize =
            builder.CreateAlignedLoad(i32Ty, hdrSizePtr, llvm::Align(4), "cat.hdrsize");

        // Required size = newLen + BRONZE_ABI_STRING_DATA_OFFSET + 1 (null terminator)
        llvm::Value* reqSize =
            builder.CreateAdd(newLen, builder.getInt32(BRONZE_ABI_STRING_DATA_OFFSET + 1));
        llvm::Value* fits = builder.CreateICmpULE(reqSize, hdrSize, "cat.fits");

        llvm::BasicBlock* copyBb = llvm::BasicBlock::Create(ctx, "cat.copy", fn);
        builder.CreateCondBr(fits, copyBb, slowBb);

        builder.SetInsertPoint(copyBb);
        llvm::Value* dstOffset =
            builder.CreateAdd(oldLen, builder.getInt32(BRONZE_ABI_STRING_DATA_OFFSET));
        llvm::Value* dstDataPtr = builder.CreateInBoundsGEP(i8Ty, lhsHdr, dstOffset);
        llvm::Value* srcDataPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, rhsHdr, BRONZE_ABI_STRING_DATA_OFFSET);
        llvm::Value* addLen64 = builder.CreateZExt(addLen, i64Ty);
        builder.CreateMemCpy(dstDataPtr, llvm::Align(1), srcDataPtr, llvm::Align(1), addLen64);

        // Store null terminator
        llvm::Value* termOffset =
            builder.CreateAdd(newLen, builder.getInt32(BRONZE_ABI_STRING_DATA_OFFSET));
        llvm::Value* termPtr = builder.CreateInBoundsGEP(i8Ty, lhsHdr, termOffset);
        builder.CreateAlignedStore(builder.getInt8(0), termPtr, llvm::Align(1));

        // Update length and clear cached hash
        builder.CreateAlignedStore(newLen, lLenPtr, llvm::Align(4));
        builder.CreateAlignedStore(builder.getInt32(0), lStrFlagsPtr, llvm::Align(4));

        llvm::BasicBlock* appendEndBb = builder.GetInsertBlock();
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(slowBb);
        llvm::Value* slowVal = builder.CreateCall(helper, {lhs, rhs});
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(doneBb);
        llvm::PHINode* result = builder.CreatePHI(i64Ty, 3, "cat.result");
        result->addIncoming(numRes, numEndBb);
        result->addIncoming(lhs, appendEndBb);
        result->addIncoming(slowVal, slowBb);
        return result;
    } else {
        // ConcatBegin
        builder.CreateBr(slowBb);

        builder.SetInsertPoint(slowBb);
        llvm::Value* slowVal = builder.CreateCall(helper, {lhs, rhs, remaining});
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(doneBb);
        llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "cat.result");
        result->addIncoming(numRes, numEndBb);
        result->addIncoming(slowVal, slowBb);
        return result;
    }
}

llvm::Value* emitConcatEnd(llvm::IRBuilder<>& builder, llvm::Function* helper, llvm::Value* val) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = builder.getInt8Ty();
    llvm::Type* i16Ty = builder.getInt16Ty();
    llvm::Type* i64Ty = builder.getInt64Ty();
    llvm::Type* ptrTy = builder.getPtrTy();

    llvm::BasicBlock* notNumBb = llvm::BasicBlock::Create(ctx, "cend.not_num", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "cend.done", fn);

    llvm::Value* isNum =
        builder.CreateICmpULE(val, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "cend.isnum");
    llvm::BasicBlock* entryBb = builder.GetInsertBlock();
    builder.CreateCondBr(isNum, doneBb, notNumBb);

    builder.SetInsertPoint(notNumBb);
    llvm::Value* tag = builder.CreateLShr(val, builder.getInt64(BRONZE_ABI_VALUE_TAG_SHIFT));
    llvm::Value* isStr =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_STRING), "cend.isstr");

    llvm::BasicBlock* strSealBb = llvm::BasicBlock::Create(ctx, "cend.str_seal", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "cend.slow", fn);
    builder.CreateCondBr(isStr, strSealBb, slowBb);

    builder.SetInsertPoint(strSealBb);
    llvm::Value* mask = builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK);
    llvm::Value* strAddr = builder.CreateAnd(val, mask);
    llvm::Value* strHdr = builder.CreateIntToPtr(strAddr, ptrTy, "cend.strhdr");
    llvm::Value* flagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, strHdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* curFlags =
        builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "cend.flags");
    llvm::Value* sealedFlags = builder.CreateAnd(
        curFlags, builder.getInt16(~static_cast<uint16_t>(BRONZE_ABI_STRING_BUILDER_BIT)));
    builder.CreateAlignedStore(sealedFlags, flagsPtr, llvm::Align(2));
    llvm::BasicBlock* sealEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(helper, {val});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 3, "cend.result");
    result->addIncoming(val, entryBb);
    result->addIncoming(val, sealEndBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

}  // namespace bronze::codegen_llvm
