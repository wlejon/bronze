#include "codegen-llvm/llvm_string_method.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/MDBuilder.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_prop_ic.h"

namespace bronze::codegen_llvm {

llvm::Value* emitStringCharCodeAtCompute(llvm::IRBuilder<>& builder, llvm::Value* strVal,
                                        llvm::Value* idxVal, llvm::BasicBlock* failBb) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    // 1. Check if idxVal is a number: idxVal <= NUMBER_MAX_BITS
    llvm::Value* isNum = builder.CreateICmpULE(
        idxVal, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "cca.idx.isnum");
    llvm::BasicBlock* numBb = llvm::BasicBlock::Create(ctx, "cca.idx.num", fn);
    auto* brNum = builder.CreateCondBr(isNum, numBb, failBb);
    brNum->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 2. Check if idxVal is an exact integer that fits in i32
    builder.SetInsertPoint(numBb);
    llvm::Value* idxDbl = builder.CreateBitCast(idxVal, dblTy, "cca.idx.dbl");
    llvm::Value* idxI32 = builder.CreateFPToSI(idxDbl, i32Ty, "cca.idx.i32");
    llvm::Value* recheckDbl = builder.CreateSIToFP(idxI32, dblTy, "cca.idx.recheck");
    llvm::Value* isExactInt = builder.CreateFCmpOEQ(idxDbl, recheckDbl, "cca.idx.isint");
    llvm::BasicBlock* exactBb = llvm::BasicBlock::Create(ctx, "cca.idx.exact", fn);
    auto* brExact = builder.CreateCondBr(isExactInt, exactBb, failBb);
    brExact->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 3. String header load and bounds check
    builder.SetInsertPoint(exactBb);
    llvm::Value* strAddr =
        builder.CreateAnd(strVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* strHdr = builder.CreateIntToPtr(strAddr, ptrTy, "cca.str.hdr");
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, strHdr, BRONZE_ABI_STRING_LENGTH_OFFSET);
    auto* strLen = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "cca.str.len");
    markInvariant(strLen, ctx);

    // Bounds check: unsigned comparison checks both idxI32 >= 0 and idxI32 < strLen
    llvm::Value* inBounds = builder.CreateICmpULT(idxI32, strLen, "cca.inbounds");
    llvm::BasicBlock* inBoundsBb = llvm::BasicBlock::Create(ctx, "cca.inbounds.bb", fn);
    llvm::BasicBlock* oobBb = llvm::BasicBlock::Create(ctx, "cca.oob.bb", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "cca.done.bb", fn);
    auto* brBounds = builder.CreateCondBr(inBounds, inBoundsBb, oobBb);
    brBounds->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // Out of bounds: return canonical NaN
    builder.SetInsertPoint(oobBb);
    builder.CreateBr(doneBb);

    // In bounds: check Latin-1 vs UTF-16
    builder.SetInsertPoint(inBoundsBb);
    llvm::Value* dataBase =
        builder.CreateConstInBoundsGEP1_32(i8Ty, strHdr, BRONZE_ABI_STRING_DATA_OFFSET);
    llvm::Value* flagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, strHdr, BRONZE_ABI_STRING_FLAGS_OFFSET);
    llvm::Value* flags = builder.CreateAlignedLoad(i32Ty, flagsPtr, llvm::Align(4), "cca.flags");
    llvm::Value* isLatin1 = builder.CreateICmpEQ(
        builder.CreateAnd(flags, builder.getInt32(BRONZE_ABI_STRING_UTF16_BIT)),
        builder.getInt32(0), "cca.islatin1");

    llvm::BasicBlock* latin1Bb = llvm::BasicBlock::Create(ctx, "cca.latin1", fn);
    llvm::BasicBlock* utf16Bb = llvm::BasicBlock::Create(ctx, "cca.utf16", fn);
    llvm::BasicBlock* charJoinBb = llvm::BasicBlock::Create(ctx, "cca.charjoin", fn);
    auto* brLatin = builder.CreateCondBr(isLatin1, latin1Bb, utf16Bb);
    brLatin->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // Latin-1 branch:
    builder.SetInsertPoint(latin1Bb);
    llvm::Value* latin1Ptr = builder.CreateGEP(i8Ty, dataBase, idxI32, "cca.lat1ptr");
    llvm::Value* latin1Byte =
        builder.CreateAlignedLoad(i8Ty, latin1Ptr, llvm::Align(1), "cca.lat1byte");
    llvm::Value* latin1Char = builder.CreateZExt(latin1Byte, i32Ty, "cca.lat1char");
    builder.CreateBr(charJoinBb);

    // UTF-16 branch:
    builder.SetInsertPoint(utf16Bb);
    llvm::Value* utf16Ptr = builder.CreateGEP(i16Ty, dataBase, idxI32, "cca.utf16ptr");
    llvm::Value* utf16Word =
        builder.CreateAlignedLoad(i16Ty, utf16Ptr, llvm::Align(2), "cca.utf16word");
    llvm::Value* utf16Char = builder.CreateZExt(utf16Word, i32Ty, "cca.utf16char");
    builder.CreateBr(charJoinBb);

    // Join code unit and convert to double
    builder.SetInsertPoint(charJoinBb);
    llvm::PHINode* charVal = builder.CreatePHI(i32Ty, 2, "cca.codeunit");
    charVal->addIncoming(latin1Char, latin1Bb);
    charVal->addIncoming(utf16Char, utf16Bb);
    llvm::Value* charDbl = builder.CreateUIToFP(charVal, dblTy, "cca.chardbl");
    llvm::Value* charBits = builder.CreateBitCast(charDbl, i64Ty, "cca.charbits");
    builder.CreateBr(doneBb);

    // Final join
    builder.SetInsertPoint(doneBb);
    llvm::PHINode* res = builder.CreatePHI(i64Ty, 2, "cca.result");
    res->addIncoming(builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS), oobBb);
    res->addIncoming(charBits, charJoinBb);
    return res;
}

llvm::Value* emitMethodCallStringCharCodeAtDirect(
    llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
    const ModuleTables& tables, llvm::Value* thisVal,
    uint32_t keyIndex, uint32_t icIndex, uint32_t argc,
    llvm::ArrayRef<llvm::Value*> args,
    llvm::function_ref<llvm::Value*()> missEmit) {
    (void)keyIndex;
    if (argc != 1) {
        return missEmit();
    }

    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::Value* entry = icEntryPtr(builder, tables.icTable, icIndex);

    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::BasicBlock* primBb = llvm::BasicBlock::Create(ctx, "cca.m.prim", fn);
    llvm::BasicBlock* missBb = llvm::BasicBlock::Create(ctx, "cca.m.miss", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "cca.m.done", fn);

    // 1. Feature enable & string receiver tag check
    auto* enabled = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_method_call_ic_enabled, llvm::Align(8), "cca.m.enabled");
    markInvariant(enabled, ctx);
    llvm::Value* isEnabled = builder.CreateICmpNE(enabled, builder.getInt64(0), "cca.m.isenabled");
    llvm::Value* tag = builder.CreateLShr(thisVal, BRONZE_ABI_VALUE_TAG_SHIFT, "cca.m.tag");
    llvm::Value* isString =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_STRING), "cca.m.isstr");
    auto* brGuard = builder.CreateCondBr(builder.CreateAnd(isEnabled, isString), primBb, missBb);
    brGuard->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 2. Primitive IC form check (word 0 has receiver's tag in kind field under exotic bit)
    builder.SetInsertPoint(primBb);
    llvm::Value* primWord0 =
        builder.CreateAlignedLoad(i64Ty, entry, llvm::Align(8), "cca.m.prim.word0");
    llvm::Value* primLow = builder.CreateAnd(
        primWord0, builder.getInt64(0xFFFFFFFFull & ~BRONZE_ABI_METHOD_IC_CODE_GUARD_BIT),
        "cca.m.prim.low");
    llvm::Value* primExpect = builder.CreateOr(
        builder.CreateShl(tag, BRONZE_ABI_METHOD_IC_KIND_SHIFT),
        builder.getInt64(BRONZE_ABI_METHOD_IC_EXOTIC_BIT), "cca.m.prim.expect");
    llvm::Value* kindOk = builder.CreateICmpEQ(primLow, primExpect, "cca.m.prim.kindok");
    llvm::BasicBlock* holderBb = llvm::BasicBlock::Create(ctx, "cca.m.holder", fn);
    auto* brKind = builder.CreateCondBr(kindOk, holderBb, missBb);
    brKind->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 3. Holder (String.prototype) check
    builder.SetInsertPoint(holderBb);
    llvm::Value* holderBits = builder.CreateAlignedLoad(
        i64Ty,
        builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_ENV_WORD),
        llvm::Align(8), "cca.m.holderbits");
    llvm::Value* holderTag =
        builder.CreateLShr(holderBits, BRONZE_ABI_VALUE_TAG_SHIFT, "cca.m.holdertag");
    llvm::Value* holderIsObj =
        builder.CreateICmpEQ(holderTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "cca.m.holderisobj");
    llvm::BasicBlock* holderShapeBb = llvm::BasicBlock::Create(ctx, "cca.m.holdershape", fn);
    auto* brHolderObj = builder.CreateCondBr(holderIsObj, holderShapeBb, missBb);
    brHolderObj->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 4. Holder shape matches latched shape
    builder.SetInsertPoint(holderShapeBb);
    llvm::Value* holderAddr =
        builder.CreateAnd(holderBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* holderHdr = builder.CreateIntToPtr(holderAddr, ptrTy, "cca.m.holder");
    llvm::Value* holderShape = builder.CreateAlignedLoad(
        ptrTy,
        builder.CreateConstInBoundsGEP1_32(i8Ty, holderHdr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
        llvm::Align(8), "cca.m.holdershape");
    llvm::Value* latchedShapeInt = builder.CreateAlignedLoad(
        i64Ty,
        builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_AUX_WORD),
        llvm::Align(8), "cca.m.latchedshape");
    llvm::Value* latchedShape =
        builder.CreateIntToPtr(latchedShapeInt, ptrTy, "cca.m.latchedshapeptr");
    llvm::Value* shapeOk =
        builder.CreateICmpEQ(holderShape, latchedShape, "cca.m.shapeok");
    llvm::BasicBlock* slotFormBb = llvm::BasicBlock::Create(ctx, "cca.m.slotform", fn);
    auto* brShape = builder.CreateCondBr(shapeOk, slotFormBb, missBb);
    brShape->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 5. Slot form check
    builder.SetInsertPoint(slotFormBb);
    llvm::Value* arityWord = builder.CreateAlignedLoad(
        i64Ty,
        builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_ARITY_WORD),
        llvm::Align(8), "cca.m.arityword");
    llvm::Value* formBits =
        builder.CreateLShr(arityWord, BRONZE_ABI_METHOD_IC_SLOT_SHIFT, "cca.m.formbits");
    llvm::Value* isSlotForm =
        builder.CreateICmpNE(formBits, builder.getInt64(0), "cca.m.isslotform");
    llvm::BasicBlock* slotBb = llvm::BasicBlock::Create(ctx, "cca.m.slot", fn);
    auto* brSlot = builder.CreateCondBr(isSlotForm, slotBb, missBb);
    brSlot->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 6. Load slot from holder
    builder.SetInsertPoint(slotBb);
    llvm::Value* slotIdx = builder.CreateSub(formBits, builder.getInt64(1), "cca.m.slotidx");
    llvm::Value* isInline = builder.CreateICmpULT(
        slotIdx, builder.getInt64(BRONZE_ABI_OBJ_INLINE_SLOTS), "cca.m.isinline");
    llvm::BasicBlock* slotInlBb = llvm::BasicBlock::Create(ctx, "cca.m.slot.inl", fn);
    llvm::BasicBlock* slotOvBb = llvm::BasicBlock::Create(ctx, "cca.m.slot.ov", fn);
    llvm::BasicBlock* slotLoadBb = llvm::BasicBlock::Create(ctx, "cca.m.slot.load", fn);
    auto* brInl = builder.CreateCondBr(isInline, slotInlBb, slotOvBb);
    brInl->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(slotInlBb);
    llvm::Value* inlBase =
        builder.CreateConstInBoundsGEP1_32(i8Ty, holderHdr, BRONZE_ABI_OBJ_SLOTS_OFFSET);
    llvm::Value* inlPtr = builder.CreateGEP(i64Ty, inlBase, slotIdx, "cca.m.slot.inlptr");
    builder.CreateBr(slotLoadBb);

    builder.SetInsertPoint(slotOvBb);
    llvm::Value* ovBits = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, holderHdr, BRONZE_ABI_OBJ_OVERFLOW_OFFSET),
        llvm::Align(8), "cca.m.slot.ovbits");
    llvm::Value* ovAddr =
        builder.CreateAnd(ovBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* ovHdr = builder.CreateIntToPtr(ovAddr, ptrTy, "cca.m.slot.ovhdr");
    llvm::Value* ovBase = builder.CreateConstInBoundsGEP1_32(i8Ty, ovHdr, BRONZE_ABI_HDR_BYTES);
    llvm::Value* ovIdx = builder.CreateSub(
        slotIdx, builder.getInt64(BRONZE_ABI_OBJ_INLINE_SLOTS), "cca.m.ovidx");
    llvm::Value* ovPtr = builder.CreateGEP(i64Ty, ovBase, ovIdx, "cca.m.slot.ovptr");
    builder.CreateBr(slotLoadBb);

    builder.SetInsertPoint(slotLoadBb);
    llvm::PHINode* slotPtr = builder.CreatePHI(ptrTy, 2, "cca.m.slot.ptr");
    slotPtr->addIncoming(inlPtr, slotInlBb);
    slotPtr->addIncoming(ovPtr, slotOvBb);
    llvm::Value* slotVal =
        builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "cca.m.slot.val");

    // 7. Check slot value is Function
    llvm::Value* fnTag = builder.CreateLShr(slotVal, BRONZE_ABI_VALUE_TAG_SHIFT, "cca.m.fntag");
    llvm::Value* fnIsObj =
        builder.CreateICmpEQ(fnTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "cca.m.fnisobj");
    llvm::BasicBlock* fnHdrBb = llvm::BasicBlock::Create(ctx, "cca.m.fnhdr", fn);
    auto* brFnObj = builder.CreateCondBr(fnIsObj, fnHdrBb, missBb);
    brFnObj->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(fnHdrBb);
    llvm::Value* fnAddr =
        builder.CreateAnd(slotVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* fnPtr = builder.CreateIntToPtr(fnAddr, ptrTy, "cca.m.fnptr");
    auto* fnFlags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "cca.m.fnflags");
    markInvariant(fnFlags, ctx);
    llvm::Value* isFn = builder.CreateICmpEQ(
        fnFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_FUNCTION), "cca.m.isfn");
    llvm::BasicBlock* codeBb = llvm::BasicBlock::Create(ctx, "cca.m.code", fn);
    auto* brFn = builder.CreateCondBr(isFn, codeBb, missBb);
    brFn->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 8. Check function code pointer is bronze_string_char_code_at
    builder.SetInsertPoint(codeBb);
    auto* code = builder.CreateAlignedLoad(
        ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_CODE_OFFSET),
        llvm::Align(8), "cca.m.codeptr");
    markInvariant(code, ctx);
    llvm::Value* codeOk =
        builder.CreateICmpEQ(code, abi.bronze_string_char_code_at, "cca.m.codeok");
    llvm::BasicBlock* computeBb = llvm::BasicBlock::Create(ctx, "cca.m.compute", fn);
    auto* brCode = builder.CreateCondBr(codeOk, computeBb, missBb);
    brCode->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 9. Compute charCodeAt inline (fails to missBb if index is not an exact integer)
    builder.SetInsertPoint(computeBb);
    llvm::Value* fastVal = emitStringCharCodeAtCompute(builder, thisVal, args[0], missBb);
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 10. Miss path
    builder.SetInsertPoint(missBb);
    llvm::Value* missVal = missEmit();
    llvm::BasicBlock* missEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 11. Join
    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "cca.m.result");
    result->addIncoming(fastVal, fastEndBb);
    result->addIncoming(missVal, missEndBb);
    return result;
}

}  // namespace bronze::codegen_llvm
