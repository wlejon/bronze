#include "codegen-llvm/llvm_prop.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

void emitPropSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::GlobalVariable* icTable,
                 llvm::Value* objBits, uint32_t keyIndex, llvm::Value* valBits, uint32_t icIndex) {
    llvm::Value* entry = icEntryPtr(builder, icTable, icIndex);
    builder.CreateCall(abi.bronze_prop_set,
                       {objBits, builder.getInt32(keyIndex), valBits, entry});
}

// The slow arm, which is also the whole of an unproven site.
static llvm::Value* emitPropGetCall(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                    llvm::Value* entry, llvm::Value* objBits, uint32_t keyIndex) {
    return builder.CreateCall(abi.bronze_prop_get, {objBits, builder.getInt32(keyIndex), entry});
}

llvm::Value* emitPropGet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                         llvm::GlobalVariable* icTable, llvm::Value* objBits, uint32_t keyIndex,
                         uint32_t icIndex, bool monomorphic) {
    llvm::Value* entry = icEntryPtr(builder, icTable, icIndex);
    if (!monomorphic) {
        return emitPropGetCall(builder, abi, entry, objBits, keyIndex);
    }

    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* checkBb = llvm::BasicBlock::Create(ctx, "ic.check", fn);
    llvm::BasicBlock* hitBb = llvm::BasicBlock::Create(ctx, "ic.hit", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "ic.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "ic.done", fn);

    // 1. Is the receiver an object at all? Everything below dereferences the
    //    payload, so nothing may be loaded until this holds.
    llvm::Value* tag = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObject =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "ic.isobj");
    builder.CreateCondBr(isObject, checkBb, slowBb);

    // 2. The guard proper. All four loads are safe here: `flags` is in the
    //    heap header every object has, and the shape word is the first
    //    payload word, which BRONZE_ABI_OBJ_MIN_PAYLOAD pins as present on
    //    every Object-tagged allocation (asserted in rt_helpers.cpp). That
    //    is what lets the flag discrimination and the shape compare share
    //    one branch instead of nesting.
    builder.SetInsertPoint(checkBb);
    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "ic.hdr");

    //    Arrays, functions, Float32Arrays and ArrayBuffers all reach
    //    bronze_prop_get and none of them has a shape; `flags` is the only
    //    thing that tells them apart from a plain object, so mistaking one
    //    for a record would read a shape word that is really a length.
    llvm::Value* flagsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                              BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "ic.flags");
    llvm::Value* isPlain =
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN));

    //    Objects move, shapes do not (docs/0004 decisions 2 and 3), so the
    //    shape word is a stable identity to compare and the cached one needs
    //    no GC fixup. Nothing between this load and the slot load below can
    //    allocate, so there is no window for the object to move underneath.
    llvm::Value* shapePtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                              BRONZE_ABI_OBJ_SHAPE_OFFSET);
    llvm::Value* shape = builder.CreateAlignedLoad(ptrTy, shapePtr, llvm::Align(8), "ic.shape");
    llvm::Value* cachedShape =
        builder.CreateAlignedLoad(ptrTy, entry, llvm::Align(8), "ic.cached");
    llvm::Value* shapeOk = builder.CreateICmpEQ(shape, cachedShape);

    //    `cached_slot` and `cached_depth` are adjacent u32s, so this one
    //    word is (depth << 32) | slot and `< kInlineSlots` means BOTH
    //    "depth 0, an own property" and "slot is inline". Depth is not
    //    ignored here, it is half of the compare: a cache that remembered
    //    the slot and forgot the depth would read an ancestor's slot off the
    //    receiver and return a real string from the wrong object, which is
    //    the bug docs/0008 decision 2 is written against and which
    //    tests/oracle/cases/proto_chain.js and proto_chain_inline.js exist
    //    to catch. Depth > 0 and overflow slots both fall to the helper,
    //    which handles them exactly as it always has.
    llvm::Value* slotWordPtr = builder.CreateConstInBoundsGEP1_32(
        i64Ty, entry, static_cast<unsigned>(BRONZE_ABI_IC_SLOTWORD_OFFSET / sizeof(uint64_t)));
    llvm::Value* slotWord =
        builder.CreateAlignedLoad(i64Ty, slotWordPtr, llvm::Align(8), "ic.slotword");
    llvm::Value* slotOk =
        builder.CreateICmpULT(slotWord, builder.getInt64(BRONZE_ABI_OBJ_INLINE_SLOTS));

    llvm::Value* hit = builder.CreateAnd(builder.CreateAnd(isPlain, shapeOk), slotOk, "ic.hit.cond");
    builder.CreateCondBr(hit, hitBb, slowBb);

    // 3. The hit: one indexed load out of the inline slots.
    builder.SetInsertPoint(hitBb);
    llvm::Value* slotsBase =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SLOTS_OFFSET);
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, slotsBase, {slotWord});
    llvm::Value* fastVal = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "ic.val");
    builder.CreateBr(doneBb);

    // 4. The miss, and every case the guard does not cover: the helper,
    //    which fills the entry exactly as it did when it owned the table.
    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = emitPropGetCall(builder, abi, entry, objBits, keyIndex);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "prop");
    result->addIncoming(fastVal, hitBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

}  // namespace bronze::codegen_llvm
