// Constant-index property READS and WRITES in generated code: array element access
// and typed-array element access. Decomposed from llvm_prop_get.cpp and llvm_prop_set.cpp
// to keep them lean and prevent code bloat.

#include "codegen-llvm/llvm_prop_index.h"

#include "codegen-llvm/llvm_alias.h"
#include "codegen-llvm/llvm_prop_ic.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

llvm::Value* emitIndexPropGet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                             const Globals& globals, const ModuleTables& tables,
                             llvm::Value* objBits, llvm::Value* objSlot,
                             uint32_t keyIndex, uint32_t icIndex,
                             bool monomorphic, StaticSite site,
                             std::string_view keyStr, uint32_t idx,
                             ReceiverProof* proof, ProofJoin* join,
                             bool holeRawSlot) {
    (void)globals;
    (void)objSlot;
    (void)monomorphic;
    (void)site;
    (void)keyStr;

    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* checkBb = llvm::BasicBlock::Create(ctx, "ic.idx.check", fn);
    llvm::BasicBlock* arrElemBb = llvm::BasicBlock::Create(ctx, "ic.idx.arr.elem", fn);
    llvm::BasicBlock* arrReadBb = llvm::BasicBlock::Create(ctx, "ic.idx.arr.read", fn);
    llvm::BasicBlock* arrUndefBb = llvm::BasicBlock::Create(ctx, "ic.idx.arr.undef", fn);
    llvm::BasicBlock* arrPayloadBb = llvm::BasicBlock::Create(ctx, "ic.idx.arr.payload", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "ic.idx.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "ic.idx.done", fn);

    llvm::Value* arrPayloadVal = nullptr;

    // 1. Live receiver proof arm (llvm_recv_proof.h)
    ProvenRead proven;
    if (proof != nullptr && proof->live()) {
        proven = emitProvenElementRead(builder, *proof, idx, doneBb, holeRawSlot);
    }

    // 2. Receiver object tag check
    llvm::Value* tag = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObject =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "ic.idx.isobj");
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);
    builder.CreateCondBr(isObject, checkBb, slowBb, likelyBranch);

    // 3. Load flags from header
    builder.SetInsertPoint(checkBb);
    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "ic.idx.hdr");
    llvm::Value* flagsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "ic.idx.flags");
    llvm::Value* isArr = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
    builder.CreateCondBr(isArr, arrElemBb, slowBb, likelyBranch);

    // 4. Array element read
    builder.SetInsertPoint(arrElemBb);
    llvm::Value* lenPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "arr.len");
    tagArrayHeaderAccess(len, ctx);
    llvm::Value* inBounds = builder.CreateICmpULT(builder.getInt32(idx), len);
    builder.CreateCondBr(inBounds, arrReadBb, arrUndefBb, likelyBranch);

    builder.SetInsertPoint(arrUndefBb);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(arrReadBb);
    llvm::Value* elemsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "arr.elems");
    tagArrayHeaderAccess(elemsVal, ctx);
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* elemsIsObj = builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    builder.CreateCondBr(elemsIsObj, arrPayloadBb, slowBb, likelyBranch);

    builder.SetInsertPoint(arrPayloadBb);
    llvm::Value* elemsAddr = builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* headPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_HEAD_OFFSET);
    auto* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), "arr.head");
    tagArrayHeaderAccess(head, ctx);
    llvm::Value* actualIdx = builder.CreateAdd(head, builder.getInt32(idx), "arr.actidx");
    llvm::Value* slotIdx = builder.CreateAdd(builder.CreateZExt(actualIdx, i64Ty), builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    auto* elemVal = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "arr.elem.raw");
    tagArrayElementsAccess(elemVal, ctx);
    llvm::Value* elemTag = builder.CreateLShr(elemVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isHole = builder.CreateICmpEQ(elemTag, builder.getInt64(BRONZE_ABI_TAG_HOLE));
    arrPayloadVal =
        builder.CreateSelect(isHole, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), elemVal, "arr.elem");
    builder.CreateBr(doneBb);

    // 5. Slow path
    builder.SetInsertPoint(slowBb);
    llvm::Value* entry = icEntryPtr(builder, tables.icTable, icIndex);
    llvm::Value* slowVal = builder.CreateCall(
        abi.bronze_prop_get,
        {objBits, emitKeyId(builder, tables, keyIndex), entry},
        "prop.slow");
    llvm::BasicBlock* slowEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 6. Result join
    builder.SetInsertPoint(doneBb);
    unsigned phiCount = (proven.fastBb ? 1 : 0) + 3;
    llvm::PHINode* result = builder.CreatePHI(i64Ty, phiCount, "prop.idx");
    if (proven.fastBb) result->addIncoming(proven.value, proven.fastBb);
    result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), arrUndefBb);
    result->addIncoming(arrPayloadVal, arrPayloadBb);
    result->addIncoming(slowVal, slowEndBb);

    if (join != nullptr) {
        join->fastBb = proven.fastBb ? proven.fastBb : arrPayloadBb;
        join->doneBb = doneBb;
    }
    if (proof != nullptr) rejoinReceiverProof(builder, *proof, proven.fastBb ? proven.fastBb : arrPayloadBb, doneBb);
    return result;
}

void emitIndexPropSet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                     const Globals& globals, const ModuleTables& tables,
                     llvm::Value* objBits, llvm::Value* objSlot,
                     llvm::Value* valBits, uint32_t keyIndex,
                     uint32_t icIndex, bool monomorphic, StaticSite site,
                     std::string_view keyStr, uint32_t idx,
                     bool strict, ArrayStoreProof* proof, ProofJoin* join) {
    (void)globals;
    (void)objSlot;
    (void)monomorphic;
    (void)site;
    (void)keyStr;

    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* checkBb = llvm::BasicBlock::Create(ctx, "ic.set.idx.check", fn);
    llvm::BasicBlock* arrElemBb = llvm::BasicBlock::Create(ctx, "ic.set.idx.arr", fn);
    llvm::BasicBlock* arrWriteBb = llvm::BasicBlock::Create(ctx, "ic.set.idx.arr.write", fn);
    llvm::BasicBlock* arrStoreBb = llvm::BasicBlock::Create(ctx, "ic.set.idx.arr.store", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "ic.set.idx.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "ic.set.idx.done", fn);

    // 1. Live array store proof arm (llvm_array_store_proof.h)
    ProvenArrayStore proven;
    if (proof != nullptr && proof->live()) {
        proven = emitProvenArrayElementStore(builder, *proof, idx, valBits, doneBb);
    }

    // 2. Receiver object tag check
    llvm::Value* tag = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObject =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "ic.set.idx.isobj");
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);
    builder.CreateCondBr(isObject, checkBb, slowBb, likelyBranch);

    // 3. Flags check
    builder.SetInsertPoint(checkBb);
    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "ic.set.idx.hdr");
    llvm::Value* flagsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "ic.set.idx.flags");
    llvm::Value* isArr = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
    builder.CreateCondBr(isArr, arrElemBb, slowBb, likelyBranch);

    // 4. Array element write
    builder.SetInsertPoint(arrElemBb);
    llvm::Value* lenPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "arr.len");
    tagArrayHeaderAccess(len, ctx);
    llvm::Value* capPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
    auto* cap = builder.CreateAlignedLoad(i32Ty, capPtr, llvm::Align(4), "arr.cap");
    tagArrayHeaderAccess(cap, ctx);
    llvm::Value* headPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_HEAD_OFFSET);
    auto* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), "arr.head");
    tagArrayHeaderAccess(head, ctx);
    llvm::Value* actualIdx = builder.CreateAdd(head, builder.getInt32(idx), "arr.actidx");
    llvm::Value* propsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
    auto* propsVal = builder.CreateAlignedLoad(i64Ty, propsPtr, llvm::Align(8), "arr.props");
    tagArrayHeaderAccess(propsVal, ctx);
    llvm::Value* propsTag = builder.CreateLShr(propsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* hasNoProps =
        builder.CreateICmpEQ(propsTag, builder.getInt64(BRONZE_ABI_TAG_UNDEFINED));
    llvm::Value* inBounds = builder.CreateICmpULT(builder.getInt32(idx), len);
    llvm::Value* inCap = builder.CreateICmpULT(actualIdx, cap);
    llvm::Value* arrOk = builder.CreateAnd(builder.CreateAnd(inBounds, inCap), hasNoProps);
    builder.CreateCondBr(arrOk, arrWriteBb, slowBb, likelyBranch);

    builder.SetInsertPoint(arrWriteBb);
    llvm::Value* elemsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "arr.elems");
    tagArrayHeaderAccess(elemsVal, ctx);
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* elemsIsObj = builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    builder.CreateCondBr(elemsIsObj, arrStoreBb, slowBb, likelyBranch);

    builder.SetInsertPoint(arrStoreBb);
    llvm::Value* elemsAddr = builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* slotIdx = builder.CreateAdd(builder.CreateZExt(actualIdx, i64Ty), builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    auto* sArr = builder.CreateAlignedStore(valBits, slotPtr, llvm::Align(8));
    tagArrayElementsAccess(sArr, ctx);
    builder.CreateBr(doneBb);

    // 5. Slow path
    builder.SetInsertPoint(slowBb);
    llvm::Value* entry = icEntryPtr(builder, tables.icTable, icIndex);
    builder.CreateCall(
        abi.bronze_prop_set,
        {objBits, emitKeyId(builder, tables, keyIndex), valBits, entry, builder.getInt1(strict)});
    builder.CreateBr(doneBb);

    // 6. Join and proof rejoin
    builder.SetInsertPoint(doneBb);
    if (join != nullptr) {
        join->fastBb = proven.fastBb ? proven.fastBb : arrStoreBb;
        join->doneBb = doneBb;
    }
    if (proof != nullptr) rejoinArrayStoreProof(*proof, proven.fastBb ? proven.fastBb : arrStoreBb, doneBb);
}

} // namespace bronze::codegen_llvm
