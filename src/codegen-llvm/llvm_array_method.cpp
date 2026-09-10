#include "codegen-llvm/llvm_array_method.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/MDBuilder.h>

#include "abi/bronze_abi.h"

namespace bronze::codegen_llvm {

llvm::Value* emitMethodCallArrayPushDirect(
    llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
    const ModuleTables& tables, llvm::Value* thisVal,
    uint32_t keyIndex, uint32_t icIndex, llvm::Value* argVal,
    llvm::function_ref<llvm::Value*()> missEmit) {
    (void)abi;
    (void)globals;
    (void)tables;
    (void)keyIndex;
    (void)icIndex;
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::BasicBlock* thisBb = llvm::BasicBlock::Create(ctx, "arr.push.this", fn);
    llvm::BasicBlock* capBb = llvm::BasicBlock::Create(ctx, "arr.push.cap", fn);
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, "arr.push.fast", fn);
    llvm::BasicBlock* missBb = llvm::BasicBlock::Create(ctx, "arr.push.miss", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "arr.push.done", fn);

    // 1. Guard `this` is an Object
    llvm::Value* thisTag = builder.CreateLShr(thisVal, BRONZE_ABI_VALUE_TAG_SHIFT, "arr.push.thistag");
    llvm::Value* thisIsObj =
        builder.CreateICmpEQ(thisTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "arr.push.thisisobj");
    auto* brThis = builder.CreateCondBr(thisIsObj, thisBb, missBb);
    brThis->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 2. Guard `this` is Array with no side properties object
    builder.SetInsertPoint(thisBb);
    llvm::Value* thisAddr =
        builder.CreateAnd(thisVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* thisHdr = builder.CreateIntToPtr(thisAddr, ptrTy, "arr.push.arrhdr");
    llvm::Value* thisFlags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "arr.push.arrflags");
    llvm::Value* isArr =
        builder.CreateICmpEQ(thisFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY), "arr.push.isarr");

    llvm::Value* propsVal = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_PROPS_OFFSET),
        llvm::Align(8), "arr.push.props");
    llvm::Value* propsTag = builder.CreateLShr(propsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* hasNoProps =
        builder.CreateICmpNE(propsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "arr.push.noprops");
    auto* brArr = builder.CreateCondBr(builder.CreateAnd(isArr, hasNoProps), capBb, missBb);
    brArr->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 3. Capacity check
    builder.SetInsertPoint(capBb);
    llvm::Value* head = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_HEAD_OFFSET),
        llvm::Align(4), "arr.push.head");
    llvm::Value* len = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET),
        llvm::Align(4), "arr.push.len");
    llvm::Value* cap = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET),
        llvm::Align(4), "arr.push.cap");

    llvm::Value* actualSlot = builder.CreateAdd(head, len, "arr.push.actslot");
    llvm::Value* capOk = builder.CreateICmpULT(actualSlot, cap, "arr.push.capok");
    llvm::Value* lenSafe = builder.CreateICmpULT(len, builder.getInt32(0xFFFFFFFEu), "arr.push.lensafe");
    auto* brCap = builder.CreateCondBr(builder.CreateAnd(capOk, lenSafe), fastBb, missBb);
    brCap->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 4. Store element, increment length, return new length as boxed double
    builder.SetInsertPoint(fastBb);
    llvm::Value* elemsVal = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET),
        llvm::Align(8), "arr.push.elems");
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* slotIdx = builder.CreateAdd(builder.CreateZExt(actualSlot, i64Ty),
                                             builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    builder.CreateAlignedStore(argVal, slotPtr, llvm::Align(8));

    llvm::Value* newLen = builder.CreateAdd(len, builder.getInt32(1), "arr.push.newlen");
    builder.CreateAlignedStore(
        newLen, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET),
        llvm::Align(4));

    llvm::Value* newLenDbl = builder.CreateUIToFP(newLen, dblTy, "arr.push.newlendbl");
    llvm::Value* isNan = builder.CreateFCmpUNO(newLenDbl, newLenDbl);
    llvm::Value* rBits = builder.CreateBitCast(newLenDbl, i64Ty);
    llvm::Value* fastRes = builder.CreateSelect(
        isNan, builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS), rBits, "arr.push.fastval");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 5. Miss path
    builder.SetInsertPoint(missBb);
    llvm::Value* missRes = missEmit();
    llvm::BasicBlock* missEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 6. Merge
    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "arr.push.result");
    result->addIncoming(fastRes, fastEndBb);
    result->addIncoming(missRes, missEndBb);
    return result;
}

llvm::Value* emitMethodCallArrayPopDirect(
    llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
    const ModuleTables& tables, llvm::Value* thisVal,
    uint32_t keyIndex, uint32_t icIndex,
    llvm::function_ref<llvm::Value*()> missEmit) {
    (void)abi;
    (void)globals;
    (void)tables;
    (void)keyIndex;
    (void)icIndex;
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::BasicBlock* thisBb = llvm::BasicBlock::Create(ctx, "arr.pop.this", fn);
    llvm::BasicBlock* lenBb = llvm::BasicBlock::Create(ctx, "arr.pop.len", fn);
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, "arr.pop.fast", fn);
    llvm::BasicBlock* missBb = llvm::BasicBlock::Create(ctx, "arr.pop.miss", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "arr.pop.done", fn);

    // 1. Guard `this` is an Object
    llvm::Value* thisTag = builder.CreateLShr(thisVal, BRONZE_ABI_VALUE_TAG_SHIFT, "arr.pop.thistag");
    llvm::Value* thisIsObj =
        builder.CreateICmpEQ(thisTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "arr.pop.thisisobj");
    auto* brThis = builder.CreateCondBr(thisIsObj, thisBb, missBb);
    brThis->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 2. Guard `this` is Array with no side properties object
    builder.SetInsertPoint(thisBb);
    llvm::Value* thisAddr =
        builder.CreateAnd(thisVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* thisHdr = builder.CreateIntToPtr(thisAddr, ptrTy, "arr.pop.arrhdr");
    llvm::Value* thisFlags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "arr.pop.arrflags");
    llvm::Value* isArr =
        builder.CreateICmpEQ(thisFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY), "arr.pop.isarr");

    llvm::Value* propsVal = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_PROPS_OFFSET),
        llvm::Align(8), "arr.pop.props");
    llvm::Value* propsTag = builder.CreateLShr(propsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* hasNoProps =
        builder.CreateICmpNE(propsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "arr.pop.noprops");
    auto* brArr = builder.CreateCondBr(builder.CreateAnd(isArr, hasNoProps), lenBb, missBb);
    brArr->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 3. Length check: length > 0
    builder.SetInsertPoint(lenBb);
    llvm::Value* len = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET),
        llvm::Align(4), "arr.pop.len");
    llvm::Value* isZero = builder.CreateICmpEQ(len, builder.getInt32(0), "arr.pop.iszero");
    llvm::BasicBlock* zeroBb = llvm::BasicBlock::Create(ctx, "arr.pop.zero", fn);
    auto* brLen = builder.CreateCondBr(isZero, zeroBb, fastBb);
    brLen->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(zeroBb);
    llvm::BasicBlock* zeroEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 4. Fast path: read last elem, store hole, update length and head_offset
    builder.SetInsertPoint(fastBb);
    llvm::Value* head = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_HEAD_OFFSET),
        llvm::Align(4), "arr.pop.head");
    llvm::Value* lastIdx = builder.CreateSub(len, builder.getInt32(1), "arr.pop.lastidx");
    llvm::Value* actualSlot = builder.CreateAdd(head, lastIdx, "arr.pop.actslot");

    llvm::Value* elemsVal = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET),
        llvm::Align(8), "arr.pop.elems");
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* slotIdx = builder.CreateAdd(builder.CreateZExt(actualSlot, i64Ty),
                                             builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    llvm::Value* lastVal = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "arr.pop.lastval");

    // Overwrite slot with Hole
    builder.CreateAlignedStore(builder.getInt64(BRONZE_ABI_HOLE_BITS), slotPtr, llvm::Align(8));

    // Update length
    builder.CreateAlignedStore(
        lastIdx, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET),
        llvm::Align(4));

    // If new length == 0, reset head_offset to 0
    llvm::Value* isNewZero = builder.CreateICmpEQ(lastIdx, builder.getInt32(0), "arr.pop.isnewzero");
    llvm::Value* finalHead = builder.CreateSelect(isNewZero, builder.getInt32(0), head, "arr.pop.newhead");
    builder.CreateAlignedStore(
        finalHead, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_HEAD_OFFSET),
        llvm::Align(4));

    // If element was a Hole, return undefined, else lastVal
    llvm::Value* isHole = builder.CreateICmpEQ(lastVal, builder.getInt64(BRONZE_ABI_HOLE_BITS), "arr.pop.ishole");
    llvm::Value* fastRes = builder.CreateSelect(
        isHole, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), lastVal, "arr.pop.fastres");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 5. Miss path
    builder.SetInsertPoint(missBb);
    llvm::Value* missRes = missEmit();
    llvm::BasicBlock* missEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 6. Merge
    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 3, "arr.pop.result");
    result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), zeroEndBb);
    result->addIncoming(fastRes, fastEndBb);
    result->addIncoming(missRes, missEndBb);
    return result;
}

llvm::Value* emitMethodCallArrayShiftDirect(
    llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
    const ModuleTables& tables, llvm::Value* thisVal,
    uint32_t keyIndex, uint32_t icIndex,
    llvm::function_ref<llvm::Value*()> missEmit) {
    (void)abi;
    (void)globals;
    (void)tables;
    (void)keyIndex;
    (void)icIndex;
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::BasicBlock* thisBb = llvm::BasicBlock::Create(ctx, "arr.shift.this", fn);
    llvm::BasicBlock* lenBb = llvm::BasicBlock::Create(ctx, "arr.shift.len", fn);
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, "arr.shift.fast", fn);
    llvm::BasicBlock* missBb = llvm::BasicBlock::Create(ctx, "arr.shift.miss", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "arr.shift.done", fn);

    // 1. Guard `this` is an Object
    llvm::Value* thisTag = builder.CreateLShr(thisVal, BRONZE_ABI_VALUE_TAG_SHIFT, "arr.shift.thistag");
    llvm::Value* thisIsObj =
        builder.CreateICmpEQ(thisTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "arr.shift.thisisobj");
    auto* brThis = builder.CreateCondBr(thisIsObj, thisBb, missBb);
    brThis->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 2. Guard `this` is Array with no side properties object
    builder.SetInsertPoint(thisBb);
    llvm::Value* thisAddr =
        builder.CreateAnd(thisVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* thisHdr = builder.CreateIntToPtr(thisAddr, ptrTy, "arr.shift.arrhdr");
    llvm::Value* thisFlags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "arr.shift.arrflags");
    llvm::Value* isArr =
        builder.CreateICmpEQ(thisFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY), "arr.shift.isarr");

    llvm::Value* propsVal = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_PROPS_OFFSET),
        llvm::Align(8), "arr.shift.props");
    llvm::Value* propsTag = builder.CreateLShr(propsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* hasNoProps =
        builder.CreateICmpNE(propsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "arr.shift.noprops");
    auto* brArr = builder.CreateCondBr(builder.CreateAnd(isArr, hasNoProps), lenBb, missBb);
    brArr->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 3. Length check: length > 0
    builder.SetInsertPoint(lenBb);
    llvm::Value* len = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET),
        llvm::Align(4), "arr.shift.len");
    llvm::Value* isZero = builder.CreateICmpEQ(len, builder.getInt32(0), "arr.shift.iszero");
    llvm::BasicBlock* zeroBb = llvm::BasicBlock::Create(ctx, "arr.shift.zero", fn);
    auto* brLen = builder.CreateCondBr(isZero, zeroBb, fastBb);
    brLen->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(zeroBb);
    llvm::BasicBlock* zeroEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 4. Fast path: read first elem at head_offset, store hole, update head_offset and length
    builder.SetInsertPoint(fastBb);
    llvm::Value* head = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_HEAD_OFFSET),
        llvm::Align(4), "arr.shift.head");

    llvm::Value* elemsVal = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET),
        llvm::Align(8), "arr.shift.elems");
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* slotIdx = builder.CreateAdd(builder.CreateZExt(head, i64Ty),
                                             builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    llvm::Value* firstVal = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "arr.shift.firstval");

    // Overwrite slot with Hole
    builder.CreateAlignedStore(builder.getInt64(BRONZE_ABI_HOLE_BITS), slotPtr, llvm::Align(8));

    // Update length
    llvm::Value* newLen = builder.CreateSub(len, builder.getInt32(1), "arr.shift.newlen");
    builder.CreateAlignedStore(
        newLen, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET),
        llvm::Align(4));

    // Update head: head_offset + 1, unless newLen == 0 then 0
    llvm::Value* isNewZero = builder.CreateICmpEQ(newLen, builder.getInt32(0), "arr.shift.isnewzero");
    llvm::Value* incHead = builder.CreateAdd(head, builder.getInt32(1), "arr.shift.inchead");
    llvm::Value* finalHead = builder.CreateSelect(isNewZero, builder.getInt32(0), incHead, "arr.shift.newhead");
    builder.CreateAlignedStore(
        finalHead, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_HEAD_OFFSET),
        llvm::Align(4));

    // If element was a Hole, return undefined, else firstVal
    llvm::Value* isHole = builder.CreateICmpEQ(firstVal, builder.getInt64(BRONZE_ABI_HOLE_BITS), "arr.shift.ishole");
    llvm::Value* fastRes = builder.CreateSelect(
        isHole, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), firstVal, "arr.shift.fastres");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 5. Miss path
    builder.SetInsertPoint(missBb);
    llvm::Value* missRes = missEmit();
    llvm::BasicBlock* missEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 6. Merge
    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 3, "arr.shift.result");
    result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), zeroEndBb);
    result->addIncoming(fastRes, fastEndBb);
    result->addIncoming(missRes, missEndBb);
    return result;
}

}  // namespace bronze::codegen_llvm
