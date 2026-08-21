#include "codegen-llvm/llvm_static_slot.h"

#include "abi/bronze_abi.h"
#include "il/il.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

#include <string>

namespace bronze::codegen_llvm {

namespace {

// The cell for one site, as an i64* into the module's `__bronze_static_shapes`.
llvm::Value* cellPtr(llvm::IRBuilder<>& builder, const ModuleTables& tables,
                     uint32_t cellIndex) {
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(builder.getContext());
    return builder.CreateConstInBoundsGEP1_32(i64Ty, tables.staticSlots, cellIndex,
                                              "static.cell");
}

}  // namespace

StaticSlotGuard emitStaticSlotGuard(llvm::IRBuilder<>& builder, const ModuleTables& tables,
                                    llvm::Value* objBits, uint32_t slot, uint32_t cellIndex,
                                    llvm::BasicBlock* doneBb, llvm::Value* store,
                                    const char* prefix) {
    StaticSlotGuard out;
    out.missBb = builder.GetInsertBlock();
    if (slot == il::Instruction::kNoStaticSlot || tables.staticSlots == nullptr) return out;

    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);
    const std::string p(prefix);

    llvm::BasicBlock* objBb = llvm::BasicBlock::Create(ctx, p + ".static.obj", fn);
    llvm::BasicBlock* hitBb = llvm::BasicBlock::Create(ctx, p + ".static.hit", fn);
    llvm::BasicBlock* missBb = llvm::BasicBlock::Create(ctx, p + ".static.miss", fn);

    // 1. An object at all. The same test the ordinary sequence opens with, and
    //    LLVM folds the two into one — which is why this costs a miss almost
    //    nothing.
    llvm::Value* tag = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    builder.CreateCondBr(
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), p + ".static.isobj"),
        objBb, missBb);

    // 2. Plain, and the exact shape this site published.
    //
    //    The flags test is not redundant with the shape compare: a non-plain
    //    heap object has no shape at this offset, so the word loaded there is
    //    some other field entirely and a coincidental match would index a slot
    //    array that does not exist. Cheap, and it is the difference between a
    //    guard and a hope.
    builder.SetInsertPoint(objBb);
    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, p + ".static.hdr");
    llvm::Value* flagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* flags =
        builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), p + ".static.flags");
    llvm::Value* shapePtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    llvm::Value* shape =
        builder.CreateAlignedLoad(i64Ty, shapePtr, llvm::Align(8), p + ".static.shape");
    llvm::Value* want = builder.CreateAlignedLoad(i64Ty, cellPtr(builder, tables, cellIndex),
                                                  llvm::Align(8), p + ".static.want");
    llvm::Value* ok = builder.CreateAnd(
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN)),
        builder.CreateICmpEQ(shape, want), p + ".static.ok");
    builder.CreateCondBr(ok, hitBb, missBb);

    // 3. The slot, at a compile-time constant offset. `slot` was checked
    //    against the published shape's own numbering before the cell was
    //    filled, so no bound and no capacity is re-tested here: a shape that
    //    carries slot N is a shape whose objects were grown to hold slot N
    //    before the shape word was stored (`setProp`'s ensureSlots ordering).
    builder.SetInsertPoint(hitBb);
    llvm::Value* slotPtr = nullptr;
    if (slot < BRONZE_ABI_OBJ_INLINE_SLOTS) {
        slotPtr = builder.CreateConstInBoundsGEP1_32(
            i8Ty, hdr, BRONZE_ABI_OBJ_SLOTS_OFFSET + slot * 8, p + ".static.slotp");
    } else {
        // The overflow block is an Object-tagged allocation whose payload is a
        // flat Value array; slot 4 is its payload word 0, which is word 1 of
        // the block counting its 8-byte header — the `- 3` the cache path uses.
        llvm::Value* ovPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_OVERFLOW_OFFSET);
        llvm::Value* ov =
            builder.CreateAlignedLoad(i64Ty, ovPtr, llvm::Align(8), p + ".static.ov");
        llvm::Value* ovAddr =
            builder.CreateAnd(ov, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
        llvm::Value* ovObj = builder.CreateIntToPtr(ovAddr, ptrTy);
        slotPtr = builder.CreateConstInBoundsGEP1_32(
            i64Ty, ovObj, slot - BRONZE_ABI_OBJ_INLINE_SLOTS + 1, p + ".static.slotp");
    }

    if (store != nullptr) {
        builder.CreateAlignedStore(store, slotPtr, llvm::Align(8));
    } else {
        out.value =
            builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), p + ".static.val");
    }
    out.hitBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(missBb);
    out.missBb = missBb;
    return out;
}

void emitStaticSlotPublish(llvm::IRBuilder<>& builder, const AbiFns& abi,
                           const ModuleTables& tables, llvm::Value* objBits, uint32_t keyIndex,
                           uint32_t slot, uint32_t cellIndex, bool forWrite,
                           llvm::BasicBlock* continueBb, const char* prefix) {
    if (slot == il::Instruction::kNoStaticSlot || tables.staticSlots == nullptr) {
        builder.CreateBr(continueBb);
        return;
    }
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    const std::string p(prefix);

    // Only while the cell is still zero. That is what makes this one-shot in
    // BOTH directions: a site that published stops calling, and a site whose
    // layout was wrong is left holding the refusal marker and also stops. The
    // reload is a global load on a path that has just made a call, and it is
    // what keeps a mispredicted site from paying two calls forever.
    llvm::BasicBlock* pubBb = llvm::BasicBlock::Create(ctx, p + ".static.publish", fn);
    llvm::Value* cur = builder.CreateAlignedLoad(i64Ty, cellPtr(builder, tables, cellIndex),
                                                 llvm::Align(8), p + ".static.cur");
    builder.CreateCondBr(builder.CreateICmpEQ(cur, builder.getInt64(0)), pubBb, continueBb);

    builder.SetInsertPoint(pubBb);
    builder.CreateCall(abi.bronze_static_shape_publish,
                       {objBits, emitKeyId(builder, tables, keyIndex),
                        cellPtr(builder, tables, cellIndex), builder.getInt32(slot),
                        builder.getInt1(forWrite)});
    builder.CreateBr(continueBb);
}

}  // namespace bronze::codegen_llvm
