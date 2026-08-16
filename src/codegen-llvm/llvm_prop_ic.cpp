#include "codegen-llvm/llvm_prop_ic.h"

#include "codegen-llvm/llvm_abi.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

void markInvariant(llvm::LoadInst* load, llvm::LLVMContext& ctx) {
    load->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(ctx, {}));
}

std::optional<uint32_t> parseIndexKey(std::string_view key) {
    if (key.empty()) return std::nullopt;
    if (key == "0") return 0;
    if (key[0] == '0') return std::nullopt;
    uint64_t val = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return std::nullopt;
        val = val * 10 + static_cast<uint64_t>(c - '0');
        if (val > 4294967294ULL) return std::nullopt;
    }
    return static_cast<uint32_t>(val);
}

ProtoWalkResult emitProtoChainWalk(
    llvm::IRBuilder<>& builder, llvm::LLVMContext& ctx, llvm::Function* fn,
    llvm::Value* startShape, llvm::Value* depth, llvm::BasicBlock* entryBb,
    llvm::BasicBlock* slowBb, llvm::BasicBlock* successBb, const std::string& prefix) {
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* loopBb = llvm::BasicBlock::Create(ctx, prefix + ".proto.loop", fn);
    llvm::BasicBlock* loadBb = llvm::BasicBlock::Create(ctx, prefix + ".proto.load", fn);
    llvm::BasicBlock* stepBb = llvm::BasicBlock::Create(ctx, prefix + ".proto.step", fn);
    llvm::BasicBlock* dictBb = llvm::BasicBlock::Create(ctx, prefix + ".proto.dict", fn);
    llvm::BasicBlock* dictLoadBb = llvm::BasicBlock::Create(ctx, prefix + ".proto.dictload", fn);
    llvm::BasicBlock* latchBb = llvm::BasicBlock::Create(ctx, prefix + ".proto.latch", fn);

    builder.CreateBr(loopBb);

    builder.SetInsertPoint(loopBb);
    llvm::PHINode* curShape = builder.CreatePHI(ptrTy, 2, prefix + ".proto.curshape");
    llvm::PHINode* stepIdx = builder.CreatePHI(i64Ty, 2, prefix + ".proto.i");
    curShape->addIncoming(startShape, entryBb);
    stepIdx->addIncoming(builder.getInt64(0), entryBb);

    llvm::Value* rootPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, curShape, BRONZE_ABI_SHAPE_ROOT_OFFSET);
    llvm::Value* rootShape = builder.CreateAlignedLoad(ptrTy, rootPtr, llvm::Align(8), prefix + ".proto.root");
    llvm::Value* rootNonNull = builder.CreateICmpNE(rootShape, llvm::Constant::getNullValue(ptrTy));
    builder.CreateCondBr(rootNonNull, loadBb, slowBb);

    builder.SetInsertPoint(loadBb);
    llvm::Value* protoValPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, rootShape, BRONZE_ABI_SHAPE_PROTO_OFFSET);
    llvm::Value* protoVal = builder.CreateAlignedLoad(i64Ty, protoValPtr, llvm::Align(8), prefix + ".proto.val");
    llvm::Value* protoTag = builder.CreateLShr(protoVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* protoIsObj = builder.CreateICmpEQ(protoTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    builder.CreateCondBr(protoIsObj, stepBb, slowBb);

    builder.SetInsertPoint(stepBb);
    llvm::Value* protoAddr = builder.CreateAnd(protoVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* protoHdr = builder.CreateIntToPtr(protoAddr, ptrTy, prefix + ".proto.hdr");
    llvm::Value* protoFlagsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, protoHdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* protoFlags = builder.CreateAlignedLoad(i16Ty, protoFlagsPtr, llvm::Align(2), prefix + ".proto.flags");
    llvm::Value* protoPlain = builder.CreateICmpEQ(protoFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN));
    builder.CreateCondBr(protoPlain, dictBb, slowBb);

    builder.SetInsertPoint(dictBb);
    llvm::Value* protoShapePtr = builder.CreateConstInBoundsGEP1_32(i8Ty, protoHdr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    llvm::Value* protoShape = builder.CreateAlignedLoad(ptrTy, protoShapePtr, llvm::Align(8), prefix + ".proto.shape");
    llvm::Value* protoShapeNonNull = builder.CreateICmpNE(protoShape, llvm::Constant::getNullValue(ptrTy));
    builder.CreateCondBr(protoShapeNonNull, dictLoadBb, slowBb);

    builder.SetInsertPoint(dictLoadBb);
    llvm::Value* dictPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, protoShape, BRONZE_ABI_SHAPE_DICT_OFFSET);
    llvm::Value* dict = builder.CreateAlignedLoad(ptrTy, dictPtr, llvm::Align(8), prefix + ".proto.dict");
    llvm::Value* notDict = builder.CreateICmpEQ(dict, llvm::Constant::getNullValue(ptrTy));
    llvm::Value* stepNext = builder.CreateAdd(stepIdx, builder.getInt64(1), prefix + ".proto.inext");
    llvm::Value* walked = builder.CreateICmpEQ(stepNext, depth);
    builder.CreateCondBr(notDict, latchBb, slowBb);

    builder.SetInsertPoint(latchBb);
    curShape->addIncoming(protoShape, latchBb);
    stepIdx->addIncoming(stepNext, latchBb);
    builder.CreateCondBr(walked, successBb, loopBb);

    return {protoHdr, latchBb};
}

llvm::Value* emitObjectSlotLoad(
    llvm::IRBuilder<>& builder, llvm::LLVMContext& ctx, llvm::Function* fn,
    llvm::Value* holderHdr, llvm::Value* slot32, llvm::BasicBlock* slowBb,
    llvm::BasicBlock* successBb, const std::string& prefix) {
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* inlineBb = llvm::BasicBlock::Create(ctx, prefix + ".inline", fn);
    llvm::BasicBlock* overflowBb = llvm::BasicBlock::Create(ctx, prefix + ".overflow", fn);
    llvm::BasicBlock* overflowAccessBb = llvm::BasicBlock::Create(ctx, prefix + ".overflow.access", fn);

    llvm::Value* isInline = builder.CreateICmpULT(slot32, builder.getInt32(BRONZE_ABI_OBJ_INLINE_SLOTS));
    builder.CreateCondBr(isInline, inlineBb, overflowBb);

    builder.SetInsertPoint(inlineBb);
    llvm::Value* slotsBase = builder.CreateConstInBoundsGEP1_32(i8Ty, holderHdr, BRONZE_ABI_OBJ_SLOTS_OFFSET);
    llvm::Value* inlineSlotPtr = builder.CreateInBoundsGEP(i64Ty, slotsBase, slot32);
    llvm::Value* inlineVal = builder.CreateAlignedLoad(i64Ty, inlineSlotPtr, llvm::Align(8), prefix + ".inline.val");
    builder.CreateBr(successBb);

    builder.SetInsertPoint(overflowBb);
    llvm::Value* overflowPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, holderHdr, BRONZE_ABI_OBJ_OVERFLOW_OFFSET);
    llvm::Value* overflowVal = builder.CreateAlignedLoad(i64Ty, overflowPtr, llvm::Align(8), prefix + ".overflow");
    llvm::Value* overflowTag = builder.CreateLShr(overflowVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* overflowIsObj = builder.CreateICmpEQ(overflowTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    builder.CreateCondBr(overflowIsObj, overflowAccessBb, slowBb);

    builder.SetInsertPoint(overflowAccessBb);
    llvm::Value* overflowAddr = builder.CreateAnd(overflowVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* overflowObj = builder.CreateIntToPtr(overflowAddr, ptrTy);
    llvm::Value* slotIdx = builder.CreateSub(slot32, builder.getInt32(3));
    llvm::Value* overflowSlotPtr = builder.CreateInBoundsGEP(i64Ty, overflowObj, slotIdx);
    llvm::Value* overflowLoadedVal = builder.CreateAlignedLoad(i64Ty, overflowSlotPtr, llvm::Align(8), prefix + ".overflow.val");
    builder.CreateBr(successBb);

    builder.SetInsertPoint(successBb);
    llvm::PHINode* res = builder.CreatePHI(i64Ty, 2, prefix + ".val");
    res->addIncoming(inlineVal, inlineBb);
    res->addIncoming(overflowLoadedVal, overflowAccessBb);
    return res;
}

}  // namespace bronze::codegen_llvm
