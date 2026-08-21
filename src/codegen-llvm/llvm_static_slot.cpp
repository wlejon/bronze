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

// The shape's family word, at a fixed offset off the shape pointer.
llvm::Value* familyWord(llvm::IRBuilder<>& builder, llvm::Value* shape, const std::string& p) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::Value* shapePtr = builder.CreateIntToPtr(shape, ptrTy, p + ".static.shapep");
    llvm::Value* wordPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, shapePtr, BRONZE_ABI_SHAPE_FAMILY_OFFSET);
    return builder.CreateAlignedLoad(i64Ty, wordPtr, llvm::Align(8), p + ".static.fam");
}

// `stamp - (base + lo) <=u span`, with `lo` and `span` immediates.
//
// One subtract and one unsigned compare, and no separate "is it stamped" test:
// UNSTAMPED (0) and NONE (1) are below every id the runtime hands out, so both
// underflow the subtraction into a number no span can cover. The base is loaded
// from a module global rather than folded in because the runtime chooses it —
// which is what keeps two modules' class 3 apart.
llvm::Value* familyInRange(llvm::IRBuilder<>& builder, const ModuleTables& tables,
                           llvm::Value* stamp, const StaticSite& site, const std::string& p) {
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(builder.getContext());
    llvm::Value* base = builder.CreateAlignedLoad(i64Ty, tables.familyBase, llvm::Align(8),
                                                  p + ".static.fambase");
    llvm::Value* lo = builder.CreateAdd(base, builder.getInt64(site.familyLo));
    llvm::Value* rel = builder.CreateSub(stamp, lo, p + ".static.famrel");
    return builder.CreateICmpULE(rel, builder.getInt64(site.familySpan), p + ".static.famok");
}

}  // namespace

StaticSlotGuard emitStaticSlotGuard(llvm::IRBuilder<>& builder, const ModuleTables& tables,
                                    llvm::Value* objBits, const StaticSite& site,
                                    llvm::BasicBlock* doneBb, llvm::Value* store,
                                    const char* prefix) {
    StaticSlotGuard out;
    out.missBb = builder.GetInsertBlock();
    if (site.none()) return out;
    if (site.family() ? tables.familyBase == nullptr : tables.staticSlots == nullptr) return out;

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

    // 2. Plain, and then whichever question this site's guard asks of the shape.
    //
    //    The flags test is not redundant with either: a non-plain heap object
    //    has no shape at this offset, so the word loaded there is some other
    //    field entirely — for the identity form a coincidental match would index
    //    a slot array that does not exist, and for the family form the shape
    //    pointer itself would be a fabrication and the family load would go
    //    through it. Cheap, and it is the difference between a guard and a hope.
    builder.SetInsertPoint(objBb);
    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, p + ".static.hdr");
    llvm::Value* flagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* flags =
        builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), p + ".static.flags");
    llvm::Value* isPlain =
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN));

    llvm::Value* shapePtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    llvm::Value* shape =
        builder.CreateAlignedLoad(i64Ty, shapePtr, llvm::Align(8), p + ".static.shape");

    llvm::Value* shapeOk = nullptr;
    if (site.family()) {
        // A null shape would fault the family load, and only a plain object is
        // guaranteed to have one — hence the two-block split here where the
        // identity form can `and` its two compares together. `ObjectHeader::
        // create` refuses to build a plain object without a shape, so the null
        // case is unreachable in practice and the branch predicts perfectly;
        // what it buys is that the load is not speculated through a header this
        // guard has not yet agreed is an object header.
        llvm::BasicBlock* famBb = llvm::BasicBlock::Create(ctx, p + ".static.fam", fn);
        builder.CreateCondBr(isPlain, famBb, missBb);
        builder.SetInsertPoint(famBb);
        shapeOk = familyInRange(builder, tables, familyWord(builder, shape, p), site, p);
        builder.CreateCondBr(shapeOk, hitBb, missBb);
    } else {
        llvm::Value* want = builder.CreateAlignedLoad(i64Ty, cellPtr(builder, tables, site.cellIndex),
                                                      llvm::Align(8), p + ".static.want");
        llvm::Value* ok = builder.CreateAnd(isPlain, builder.CreateICmpEQ(shape, want),
                                            p + ".static.ok");
        builder.CreateCondBr(ok, hitBb, missBb);
    }

    // 3. The slot, at a compile-time constant offset. For the identity form the
    //    slot was checked against the published shape's own numbering before the
    //    cell was filled; for the family form the stamper checked the whole
    //    prefix, slot by slot, before it wrote the word. Either way no bound and
    //    no capacity is re-tested here: a shape that carries slot N is a shape
    //    whose objects were grown to hold slot N before the shape word was
    //    stored (`setProp`'s ensureSlots ordering).
    builder.SetInsertPoint(hitBb);
    llvm::Value* slotPtr = nullptr;
    if (site.slot < BRONZE_ABI_OBJ_INLINE_SLOTS) {
        slotPtr = builder.CreateConstInBoundsGEP1_32(
            i8Ty, hdr, BRONZE_ABI_OBJ_SLOTS_OFFSET + site.slot * 8, p + ".static.slotp");
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
            i64Ty, ovObj, site.slot - BRONZE_ABI_OBJ_INLINE_SLOTS + 1, p + ".static.slotp");
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
                           const ModuleTables& tables, llvm::Value* objBits,
                           llvm::Value* objSlot, uint32_t keyIndex, const StaticSite& site,
                           bool forWrite, llvm::BasicBlock* continueBb, const char* prefix) {
    if (site.none() || objSlot == nullptr ||
        (site.family() ? tables.familyBase == nullptr : tables.staticSlots == nullptr)) {
        builder.CreateBr(continueBb);
        return;
    }
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);
    const std::string p(prefix);

    // The receiver, AFTER the helper call this block sits behind. The register
    // the guard was built from named an address the collector may since have
    // vacated; the root slot is where the collector wrote the new one.
    objBits = builder.CreateAlignedLoad(i64Ty, objSlot, llvm::Align(8), p + ".static.obj.live");

    if (site.family()) {
        // The stamp is a fact about the SHAPE, so the one-shot test is on the
        // shape and not on anything this site owns: re-deriving the header here
        // costs three loads on a path that has just made a helper call, and it
        // is what keeps a site that meets a hundred shapes from calling the
        // stamper a hundred times each.
        llvm::BasicBlock* objBb = llvm::BasicBlock::Create(ctx, p + ".stamp.obj", fn);
        llvm::BasicBlock* shapeBb = llvm::BasicBlock::Create(ctx, p + ".stamp.shape", fn);
        llvm::BasicBlock* callBb = llvm::BasicBlock::Create(ctx, p + ".stamp.call", fn);
        llvm::Value* tag = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
        builder.CreateCondBr(
            builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT)), objBb, continueBb);

        builder.SetInsertPoint(objBb);
        llvm::Value* addr =
            builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
        llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, p + ".stamp.hdr");
        llvm::Value* flags = builder.CreateAlignedLoad(
            i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
            llvm::Align(2), p + ".stamp.flags");
        builder.CreateCondBr(
            builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN)), shapeBb,
            continueBb);

        builder.SetInsertPoint(shapeBb);
        llvm::Value* shape = builder.CreateAlignedLoad(
            i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
            llvm::Align(8), p + ".stamp.shape");
        llvm::Value* unstamped =
            builder.CreateICmpEQ(familyWord(builder, shape, p + ".stamp"),
                                 builder.getInt64(BRONZE_ABI_FAMILY_UNSTAMPED));
        builder.CreateCondBr(unstamped, callBb, continueBb);

        builder.SetInsertPoint(callBb);
        builder.CreateCall(abi.bronze_family_stamp, {objBits});
        builder.CreateBr(continueBb);
        return;
    }

    // Only while the cell is still zero. That is what makes this one-shot in
    // BOTH directions: a site that published stops calling, and a site whose
    // layout was wrong is left holding the refusal marker and also stops. The
    // reload is a global load on a path that has just made a call, and it is
    // what keeps a mispredicted site from paying two calls forever.
    llvm::BasicBlock* pubBb = llvm::BasicBlock::Create(ctx, p + ".static.publish", fn);
    llvm::Value* cur = builder.CreateAlignedLoad(i64Ty, cellPtr(builder, tables, site.cellIndex),
                                                 llvm::Align(8), p + ".static.cur");
    builder.CreateCondBr(builder.CreateICmpEQ(cur, builder.getInt64(0)), pubBb, continueBb);

    builder.SetInsertPoint(pubBb);
    builder.CreateCall(abi.bronze_static_shape_publish,
                       {objBits, emitKeyId(builder, tables, keyIndex),
                        cellPtr(builder, tables, site.cellIndex), builder.getInt32(site.slot),
                        builder.getInt1(forWrite)});
    builder.CreateBr(continueBb);
}

}  // namespace bronze::codegen_llvm
