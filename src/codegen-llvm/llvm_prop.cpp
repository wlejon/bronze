#include "codegen-llvm/llvm_prop.h"

#include <optional>
#include <string_view>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

namespace {

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

}  // namespace

void emitPropSet(llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
                 llvm::GlobalVariable* icTable, llvm::Value* objBits, uint32_t keyIndex,
                 llvm::Value* valBits, uint32_t icIndex, bool strict, bool monomorphic,
                 std::string_view keyStr) {
    (void)monomorphic;
    llvm::Value* entry = icEntryPtr(builder, icTable, icIndex);

    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* checkBb = llvm::BasicBlock::Create(ctx, "ic.set.check", fn);
    llvm::BasicBlock* plainCheckBb = llvm::BasicBlock::Create(ctx, "ic.set.plain", fn);
    llvm::BasicBlock* hitBb = llvm::BasicBlock::Create(ctx, "ic.set.hit", fn);
    llvm::BasicBlock* inlineHitBb = llvm::BasicBlock::Create(ctx, "ic.set.inline", fn);
    llvm::BasicBlock* overflowHitBb = llvm::BasicBlock::Create(ctx, "ic.set.overflow", fn);
    llvm::BasicBlock* overflowAccessBb = llvm::BasicBlock::Create(ctx, "ic.set.overflow.access", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "ic.set.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "ic.set.done", fn);

    // 1. Is the receiver an object?
    llvm::Value* tag = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObject =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "ic.set.isobj");
    builder.CreateCondBr(isObject, checkBb, slowBb);

    // 2. Load flags from header
    builder.SetInsertPoint(checkBb);
    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "ic.set.hdr");

    llvm::Value* flagsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                              BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "ic.set.flags");

    auto optIdx = parseIndexKey(keyStr);
    if (optIdx.has_value()) {
        uint32_t idx = *optIdx;
        llvm::BasicBlock* arrElemBb = llvm::BasicBlock::Create(ctx, "ic.set.arr", fn);
        llvm::BasicBlock* arrWriteBb = llvm::BasicBlock::Create(ctx, "ic.set.arr.write", fn);
        llvm::BasicBlock* arrStoreBb = llvm::BasicBlock::Create(ctx, "ic.set.arr.store", fn);

        llvm::Value* isArr = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
        builder.CreateCondBr(isArr, arrElemBb, plainCheckBb);

        builder.SetInsertPoint(arrElemBb);
        llvm::Value* lenPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                BRONZE_ABI_ARRAY_LENGTH_OFFSET);
        llvm::Value* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "arr.len");
        llvm::Value* capPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
        llvm::Value* cap = builder.CreateAlignedLoad(i32Ty, capPtr, llvm::Align(4), "arr.cap");
        llvm::Value* propsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                  BRONZE_ABI_ARRAY_PROPS_OFFSET);
        llvm::Value* propsVal = builder.CreateAlignedLoad(i64Ty, propsPtr, llvm::Align(8), "arr.props");
        llvm::Value* propsTag = builder.CreateLShr(propsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
        llvm::Value* hasNoProps =
            builder.CreateICmpEQ(propsTag, builder.getInt64(BRONZE_ABI_TAG_UNDEFINED));
        llvm::Value* inBounds = builder.CreateICmpULT(builder.getInt32(idx), len);
        llvm::Value* inCap = builder.CreateICmpULT(builder.getInt32(idx), cap);
        llvm::Value* arrOk = builder.CreateAnd(builder.CreateAnd(inBounds, inCap), hasNoProps);
        builder.CreateCondBr(arrOk, arrWriteBb, slowBb);

        builder.SetInsertPoint(arrWriteBb);
        llvm::Value* elemsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                  BRONZE_ABI_ARRAY_ELEMS_OFFSET);
        llvm::Value* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "arr.elems");
        llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
        llvm::Value* elemsIsObj =
            builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
        builder.CreateCondBr(elemsIsObj, arrStoreBb, slowBb);

        builder.SetInsertPoint(arrStoreBb);
        llvm::Value* elemsAddr =
            builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
        llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
        llvm::Value* slotPtr = builder.CreateConstInBoundsGEP1_32(i64Ty, elemsObj, idx + 1);
        builder.CreateAlignedStore(valBits, slotPtr, llvm::Align(8));
        builder.CreateBr(doneBb);
    } else {
        builder.CreateBr(plainCheckBb);
    }

    // 3. Plain object guard: matching shape and depth 0
    builder.SetInsertPoint(plainCheckBb);
    llvm::Value* isPlain =
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN));
    llvm::Value* shapePtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                              BRONZE_ABI_OBJ_SHAPE_OFFSET);
    llvm::Value* shape = builder.CreateAlignedLoad(ptrTy, shapePtr, llvm::Align(8), "ic.set.shape");
    llvm::Value* cachedShape =
        builder.CreateAlignedLoad(ptrTy, entry, llvm::Align(8), "ic.set.cached");
    llvm::Value* shapeOk = builder.CreateICmpEQ(shape, cachedShape);

    llvm::Value* slotWordPtr = builder.CreateConstInBoundsGEP1_32(
        i64Ty, entry, static_cast<unsigned>(BRONZE_ABI_IC_SLOTWORD_OFFSET / sizeof(uint64_t)));
    llvm::Value* slotWord =
        builder.CreateAlignedLoad(i64Ty, slotWordPtr, llvm::Align(8), "ic.set.slotword");
    llvm::Value* depth = builder.CreateLShr(slotWord, 32);
    llvm::Value* depthOk = builder.CreateICmpEQ(depth, builder.getInt64(0));

    llvm::Value* hit = builder.CreateAnd(builder.CreateAnd(isPlain, shapeOk), depthOk, "ic.set.hit.cond");

    // 3b. The shape-transition arm: the entry's cached shape is one property
    // above the receiver's, and that property is this site's key — the state
    // a constructor body's `this.x = v` leaves behind on every `new` after
    // the first. The guards are the slow path's conclusions, checked in the
    // order that keeps every load safe (see ObjectHeader::setProp, whose
    // transition fast path this mirrors exactly):
    //   cached != null, cached->parent == receiver shape (same immutable
    //   chain, one add short), slotword < kInlineSlots (depth 0 AND an inline
    //   slot in one compare, so no overflow-growth allocation can be needed),
    //   cached->slot_index == cached_slot (the cached node OWNS the site's
    //   key — slot uniqueness along a chain makes that the key check),
    //   the four attribute bytes spell plain-data (an assignment creates
    //   nothing else), the receiver's shape is not somebody's prototype (the
    //   helper owns the epoch bump that add would owe), and the entry's fill
    //   epoch is current (the fill-time walk proved no inherited setter; every
    //   way one can appear bumps the epoch).
    //
    // Not emitted for `length` or index-spelled keys: those are the two names
    // a String exotic receiver refuses, and the refusal lives in the helper.
    const bool transitionArm = !keyStr.empty() && keyStr != "length" && !optIdx.has_value();
    if (transitionArm) {
        llvm::BasicBlock* transNullBb = llvm::BasicBlock::Create(ctx, "ic.set.trans.null", fn);
        llvm::BasicBlock* transParentBb = llvm::BasicBlock::Create(ctx, "ic.set.trans.parent", fn);
        llvm::BasicBlock* transNodeBb = llvm::BasicBlock::Create(ctx, "ic.set.trans.node", fn);
        llvm::BasicBlock* transHitBb = llvm::BasicBlock::Create(ctx, "ic.set.trans.hit", fn);

        builder.CreateCondBr(hit, hitBb, transNullBb);

        builder.SetInsertPoint(transNullBb);
        llvm::Value* cachedNonNull = builder.CreateICmpNE(
            cachedShape, llvm::Constant::getNullValue(ptrTy), "trans.cached");
        builder.CreateCondBr(builder.CreateAnd(isPlain, cachedNonNull), transParentBb, slowBb);

        builder.SetInsertPoint(transParentBb);
        llvm::Value* parentPtr = builder.CreateConstInBoundsGEP1_32(
            i8Ty, cachedShape, BRONZE_ABI_SHAPE_PARENT_OFFSET);
        llvm::Value* parent =
            builder.CreateAlignedLoad(ptrTy, parentPtr, llvm::Align(8), "trans.parent");
        llvm::Value* parentOk = builder.CreateICmpEQ(parent, shape);
        llvm::Value* slotSmall = builder.CreateICmpULT(
            slotWord, builder.getInt64(BRONZE_ABI_OBJ_INLINE_SLOTS), "trans.slotsmall");
        builder.CreateCondBr(builder.CreateAnd(parentOk, slotSmall), transNodeBb, slowBb);

        builder.SetInsertPoint(transNodeBb);
        llvm::Value* transSlot32 = builder.CreateTrunc(slotWord, i32Ty, "trans.slot32");
        llvm::Value* nodeSlotPtr = builder.CreateConstInBoundsGEP1_32(
            i8Ty, cachedShape, BRONZE_ABI_SHAPE_SLOTINDEX_OFFSET);
        llvm::Value* nodeSlot =
            builder.CreateAlignedLoad(i32Ty, nodeSlotPtr, llvm::Align(4), "trans.nodeslot");
        llvm::Value* slotIsNode = builder.CreateICmpEQ(nodeSlot, transSlot32);
        llvm::Value* attrsPtr = builder.CreateConstInBoundsGEP1_32(
            i8Ty, cachedShape, BRONZE_ABI_SHAPE_ATTRS_OFFSET);
        llvm::Value* attrs =
            builder.CreateAlignedLoad(i32Ty, attrsPtr, llvm::Align(4), "trans.attrs");
        llvm::Value* attrsOk = builder.CreateICmpEQ(
            attrs, builder.getInt32(BRONZE_ABI_SHAPE_ATTRS_PLAIN_DATA));
        llvm::Value* usedPtr = builder.CreateConstInBoundsGEP1_32(
            i8Ty, shape, BRONZE_ABI_SHAPE_USEDPROTO_OFFSET);
        llvm::Value* used =
            builder.CreateAlignedLoad(i8Ty, usedPtr, llvm::Align(1), "trans.usedproto");
        llvm::Value* notPrototype = builder.CreateICmpEQ(used, builder.getInt8(0));
        llvm::Value* epochPtr = builder.CreateConstInBoundsGEP1_32(
            i64Ty, entry, static_cast<unsigned>(BRONZE_ABI_IC_EPOCH_OFFSET / sizeof(uint64_t)));
        llvm::Value* fillEpoch =
            builder.CreateAlignedLoad(i64Ty, epochPtr, llvm::Align(8), "trans.fillepoch");
        llvm::Value* curEpoch = builder.CreateAlignedLoad(
            i64Ty, globals.bronze_proto_epoch, llvm::Align(8), "trans.epoch");
        llvm::Value* epochOk = builder.CreateICmpEQ(fillEpoch, curEpoch);
        llvm::Value* nodeOk = builder.CreateAnd(
            builder.CreateAnd(slotIsNode, attrsOk),
            builder.CreateAnd(notPrototype, epochOk), "trans.nodeok");
        builder.CreateCondBr(nodeOk, transHitBb, slowBb);

        builder.SetInsertPoint(transHitBb);
        llvm::Value* shapeSlotPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
        builder.CreateAlignedStore(cachedShape, shapeSlotPtr, llvm::Align(8));
        llvm::Value* transSlotsBase =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SLOTS_OFFSET);
        llvm::Value* transSlotPtr =
            builder.CreateInBoundsGEP(i64Ty, transSlotsBase, {transSlot32});
        builder.CreateAlignedStore(valBits, transSlotPtr, llvm::Align(8));
        builder.CreateBr(doneBb);
    } else {
        builder.CreateCondBr(hit, hitBb, slowBb);
    }

    // 4. Hit: inline slot or overflow slot
    builder.SetInsertPoint(hitBb);
    llvm::Value* slot32 = builder.CreateTrunc(slotWord, i32Ty, "ic.set.slot32");
    llvm::Value* isInline =
        builder.CreateICmpULT(slot32, builder.getInt32(BRONZE_ABI_OBJ_INLINE_SLOTS));
    builder.CreateCondBr(isInline, inlineHitBb, overflowHitBb);

    builder.SetInsertPoint(inlineHitBb);
    llvm::Value* slotsBase =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SLOTS_OFFSET);
    llvm::Value* inlineSlotPtr = builder.CreateInBoundsGEP(i64Ty, slotsBase, {slot32});
    builder.CreateAlignedStore(valBits, inlineSlotPtr, llvm::Align(8));
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(overflowHitBb);
    llvm::Value* overflowPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                 BRONZE_ABI_OBJ_OVERFLOW_OFFSET);
    llvm::Value* overflowVal =
        builder.CreateAlignedLoad(i64Ty, overflowPtr, llvm::Align(8), "ic.set.overflow");
    llvm::Value* overflowTag = builder.CreateLShr(overflowVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* overflowIsObj =
        builder.CreateICmpEQ(overflowTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    builder.CreateCondBr(overflowIsObj, overflowAccessBb, slowBb);

    builder.SetInsertPoint(overflowAccessBb);
    llvm::Value* overflowAddr =
        builder.CreateAnd(overflowVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* overflowObj = builder.CreateIntToPtr(overflowAddr, ptrTy);
    llvm::Value* slotIdx = builder.CreateSub(slot32, builder.getInt32(3));
    llvm::Value* overflowSlotPtr = builder.CreateInBoundsGEP(i64Ty, overflowObj, {slotIdx});
    builder.CreateAlignedStore(valBits, overflowSlotPtr, llvm::Align(8));
    builder.CreateBr(doneBb);

    // 5. The slow fallback call
    builder.SetInsertPoint(slowBb);
    builder.CreateCall(abi.bronze_prop_set,
                       {objBits, builder.getInt32(keyIndex), valBits, entry,
                        builder.getInt1(strict)});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
}

static llvm::Value* emitPropGetCall(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                    llvm::Value* entry, llvm::Value* objBits, uint32_t keyIndex) {
    return builder.CreateCall(abi.bronze_prop_get, {objBits, builder.getInt32(keyIndex), entry});
}

llvm::Value* emitPropGet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                         const AbiGlobals& globals, llvm::GlobalVariable* icTable,
                         llvm::Value* objBits, uint32_t keyIndex, uint32_t icIndex,
                         bool monomorphic, std::string_view keyStr) {
    (void)monomorphic;
    llvm::Value* entry = icEntryPtr(builder, icTable, icIndex);

    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* checkBb = llvm::BasicBlock::Create(ctx, "ic.check", fn);
    llvm::BasicBlock* plainCheckBb = llvm::BasicBlock::Create(ctx, "ic.plain", fn);
    llvm::BasicBlock* hitBb = llvm::BasicBlock::Create(ctx, "ic.hit", fn);
    llvm::BasicBlock* inlineHitBb = llvm::BasicBlock::Create(ctx, "ic.inline", fn);
    llvm::BasicBlock* overflowHitBb = llvm::BasicBlock::Create(ctx, "ic.overflow", fn);
    llvm::BasicBlock* overflowAccessBb = llvm::BasicBlock::Create(ctx, "ic.overflow.access", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "ic.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "ic.done", fn);

    // 1. Is the receiver an object at all?
    llvm::Value* tag = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObject =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "ic.isobj");
    builder.CreateCondBr(isObject, checkBb, slowBb);

    // 2. Load flags
    builder.SetInsertPoint(checkBb);
    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "ic.hdr");

    llvm::Value* flagsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                              BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "ic.flags");

    llvm::BasicBlock* arrLenBb = nullptr;
    llvm::Value* arrLenVal = nullptr;

    llvm::BasicBlock* arrUndefBb = nullptr;
    llvm::BasicBlock* arrPayloadBb = nullptr;
    llvm::Value* arrPayloadVal = nullptr;

    auto optIdx = parseIndexKey(keyStr);
    if (keyStr == "length") {
        arrLenBb = llvm::BasicBlock::Create(ctx, "ic.arr.len", fn);
        llvm::Value* isArr = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
        builder.CreateCondBr(isArr, arrLenBb, plainCheckBb);

        builder.SetInsertPoint(arrLenBb);
        llvm::Value* lenPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                BRONZE_ABI_ARRAY_LENGTH_OFFSET);
        llvm::Value* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "arr.len");
        llvm::Value* lenDbl = builder.CreateUIToFP(len, llvm::Type::getDoubleTy(ctx), "arr.len.dbl");
        arrLenVal = builder.CreateBitCast(lenDbl, i64Ty, "arr.len.bits");
        builder.CreateBr(doneBb);
    } else if (optIdx.has_value()) {
        uint32_t idx = *optIdx;
        llvm::BasicBlock* arrElemBb = llvm::BasicBlock::Create(ctx, "ic.arr.elem", fn);
        llvm::BasicBlock* arrReadBb = llvm::BasicBlock::Create(ctx, "ic.arr.read", fn);
        arrUndefBb = llvm::BasicBlock::Create(ctx, "ic.arr.undef", fn);
        arrPayloadBb = llvm::BasicBlock::Create(ctx, "ic.arr.payload", fn);

        llvm::Value* isArr = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
        builder.CreateCondBr(isArr, arrElemBb, plainCheckBb);

        builder.SetInsertPoint(arrElemBb);
        llvm::Value* lenPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                BRONZE_ABI_ARRAY_LENGTH_OFFSET);
        llvm::Value* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "arr.len");
        llvm::Value* inBounds = builder.CreateICmpULT(builder.getInt32(idx), len);
        builder.CreateCondBr(inBounds, arrReadBb, arrUndefBb);

        builder.SetInsertPoint(arrUndefBb);
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(arrReadBb);
        llvm::Value* elemsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                  BRONZE_ABI_ARRAY_ELEMS_OFFSET);
        llvm::Value* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "arr.elems");
        llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
        llvm::Value* elemsIsObj =
            builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
        builder.CreateCondBr(elemsIsObj, arrPayloadBb, slowBb);

        builder.SetInsertPoint(arrPayloadBb);
        llvm::Value* elemsAddr =
            builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
        llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
        llvm::Value* slotPtr = builder.CreateConstInBoundsGEP1_32(i64Ty, elemsObj, idx + 1);
        llvm::Value* elemVal = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "arr.elem.raw");
        llvm::Value* elemTag = builder.CreateLShr(elemVal, BRONZE_ABI_VALUE_TAG_SHIFT);
        llvm::Value* isHole = builder.CreateICmpEQ(elemTag, builder.getInt64(BRONZE_ABI_TAG_HOLE));
        arrPayloadVal = builder.CreateSelect(isHole, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), elemVal, "arr.elem");
        builder.CreateBr(doneBb);
    } else {
        builder.CreateBr(plainCheckBb);
    }

    // 3. Plain object guard: matching shape and depth 0
    builder.SetInsertPoint(plainCheckBb);
    llvm::Value* isPlain =
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN));
    llvm::Value* shapePtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                              BRONZE_ABI_OBJ_SHAPE_OFFSET);
    llvm::Value* shape = builder.CreateAlignedLoad(ptrTy, shapePtr, llvm::Align(8), "ic.shape");
    llvm::Value* cachedShape =
        builder.CreateAlignedLoad(ptrTy, entry, llvm::Align(8), "ic.cached");
    llvm::Value* shapeOk = builder.CreateICmpEQ(shape, cachedShape);

    llvm::Value* slotWordPtr = builder.CreateConstInBoundsGEP1_32(
        i64Ty, entry, static_cast<unsigned>(BRONZE_ABI_IC_SLOTWORD_OFFSET / sizeof(uint64_t)));
    llvm::Value* slotWord =
        builder.CreateAlignedLoad(i64Ty, slotWordPtr, llvm::Align(8), "ic.slotword");
    llvm::Value* depth = builder.CreateLShr(slotWord, 32);
    llvm::Value* depthOk = builder.CreateICmpEQ(depth, builder.getInt64(0));

    llvm::Value* shapeHit = builder.CreateAnd(isPlain, shapeOk, "ic.shape.cond");
    llvm::BasicBlock* depthSplitBb = llvm::BasicBlock::Create(ctx, "ic.depth.split", fn);
    builder.CreateCondBr(shapeHit, depthSplitBb, slowBb);

    // 3b. Depth 0 is the own-property hit below; depth > 0 is a PROTO hit,
    // which generated code now walks itself — the epoch check and the chain
    // walk mirror InlineCache::describes and ObjectHeader::cachedProtoHolder
    // exactly, and every guard miss (stale epoch, a non-object or non-plain
    // link, a dictionary on the path, an overflow slot on the holder) falls
    // back to the helper, which still owns the fatal tripwires.
    builder.SetInsertPoint(depthSplitBb);
    llvm::BasicBlock* protoCheckBb = llvm::BasicBlock::Create(ctx, "ic.proto.check", fn);
    llvm::BasicBlock* protoLoopBb = llvm::BasicBlock::Create(ctx, "ic.proto.loop", fn);
    llvm::BasicBlock* protoStepBb = llvm::BasicBlock::Create(ctx, "ic.proto.step", fn);
    llvm::BasicBlock* protoDictBb = llvm::BasicBlock::Create(ctx, "ic.proto.dict", fn);
    llvm::BasicBlock* protoResBb = llvm::BasicBlock::Create(ctx, "ic.proto.res", fn);
    builder.CreateCondBr(depthOk, hitBb, protoCheckBb);

    builder.SetInsertPoint(protoCheckBb);
    llvm::Value* protoSlot32 = builder.CreateTrunc(slotWord, i32Ty, "proto.slot32");
    llvm::Value* epochPtr = builder.CreateConstInBoundsGEP1_32(
        i64Ty, entry, static_cast<unsigned>(BRONZE_ABI_IC_EPOCH_OFFSET / sizeof(uint64_t)));
    llvm::Value* fillEpoch =
        builder.CreateAlignedLoad(i64Ty, epochPtr, llvm::Align(8), "proto.fillepoch");
    llvm::Value* curEpoch = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_proto_epoch, llvm::Align(8), "proto.epoch");
    llvm::Value* epochOk = builder.CreateICmpEQ(fillEpoch, curEpoch);
    builder.CreateCondBr(epochOk, protoLoopBb, slowBb);

    // The walk: `depth` steps of shape -> root -> prototype, each link
    // object-tagged, Plain, and not a dictionary.
    builder.SetInsertPoint(protoLoopBb);
    llvm::PHINode* curShape = builder.CreatePHI(ptrTy, 2, "proto.curshape");
    llvm::PHINode* stepIdx = builder.CreatePHI(i64Ty, 2, "proto.i");
    curShape->addIncoming(shape, protoCheckBb);
    stepIdx->addIncoming(builder.getInt64(0), protoCheckBb);
    llvm::Value* rootPtr = builder.CreateConstInBoundsGEP1_32(
        i8Ty, curShape, BRONZE_ABI_SHAPE_ROOT_OFFSET);
    llvm::Value* rootShape =
        builder.CreateAlignedLoad(ptrTy, rootPtr, llvm::Align(8), "proto.root");
    llvm::Value* rootNonNull =
        builder.CreateICmpNE(rootShape, llvm::Constant::getNullValue(ptrTy));
    llvm::BasicBlock* protoLoadBb = llvm::BasicBlock::Create(ctx, "ic.proto.load", fn);
    builder.CreateCondBr(rootNonNull, protoLoadBb, slowBb);

    builder.SetInsertPoint(protoLoadBb);
    llvm::Value* protoValPtr = builder.CreateConstInBoundsGEP1_32(
        i8Ty, rootShape, BRONZE_ABI_SHAPE_PROTO_OFFSET);
    llvm::Value* protoVal =
        builder.CreateAlignedLoad(i64Ty, protoValPtr, llvm::Align(8), "proto.val");
    llvm::Value* protoTag = builder.CreateLShr(protoVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* protoIsObj =
        builder.CreateICmpEQ(protoTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    builder.CreateCondBr(protoIsObj, protoStepBb, slowBb);

    builder.SetInsertPoint(protoStepBb);
    llvm::Value* protoAddr =
        builder.CreateAnd(protoVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* protoHdr = builder.CreateIntToPtr(protoAddr, ptrTy, "proto.hdr");
    llvm::Value* protoFlagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, protoHdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* protoFlags =
        builder.CreateAlignedLoad(i16Ty, protoFlagsPtr, llvm::Align(2), "proto.flags");
    llvm::Value* protoPlain =
        builder.CreateICmpEQ(protoFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN));
    builder.CreateCondBr(protoPlain, protoDictBb, slowBb);

    builder.SetInsertPoint(protoDictBb);
    llvm::Value* protoShapePtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, protoHdr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    llvm::Value* protoShape =
        builder.CreateAlignedLoad(ptrTy, protoShapePtr, llvm::Align(8), "proto.shape");
    llvm::Value* protoShapeNonNull =
        builder.CreateICmpNE(protoShape, llvm::Constant::getNullValue(ptrTy));
    llvm::BasicBlock* protoDictLoadBb = llvm::BasicBlock::Create(ctx, "ic.proto.dictload", fn);
    builder.CreateCondBr(protoShapeNonNull, protoDictLoadBb, slowBb);

    builder.SetInsertPoint(protoDictLoadBb);
    llvm::Value* dictPtr = builder.CreateConstInBoundsGEP1_32(
        i8Ty, protoShape, BRONZE_ABI_SHAPE_DICT_OFFSET);
    llvm::Value* dict = builder.CreateAlignedLoad(ptrTy, dictPtr, llvm::Align(8), "proto.dict");
    llvm::Value* notDict = builder.CreateICmpEQ(dict, llvm::Constant::getNullValue(ptrTy));
    llvm::Value* stepNext = builder.CreateAdd(stepIdx, builder.getInt64(1), "proto.inext");
    llvm::Value* walked = builder.CreateICmpEQ(stepNext, depth);
    llvm::BasicBlock* protoLatchBb = llvm::BasicBlock::Create(ctx, "ic.proto.latch", fn);
    builder.CreateCondBr(notDict, protoLatchBb, slowBb);

    builder.SetInsertPoint(protoLatchBb);
    curShape->addIncoming(protoShape, protoLatchBb);
    stepIdx->addIncoming(stepNext, protoLatchBb);
    builder.CreateCondBr(walked, protoResBb, protoLoopBb);

    builder.SetInsertPoint(protoResBb);
    llvm::BasicBlock* protoInlineBb = llvm::BasicBlock::Create(ctx, "ic.proto.inline", fn);
    llvm::BasicBlock* protoOverflowBb = llvm::BasicBlock::Create(ctx, "ic.proto.overflow", fn);
    llvm::BasicBlock* protoOverflowAccessBb =
        llvm::BasicBlock::Create(ctx, "ic.proto.overflow.access", fn);

    llvm::Value* protoIsInline =
        builder.CreateICmpULT(protoSlot32, builder.getInt32(BRONZE_ABI_OBJ_INLINE_SLOTS));
    builder.CreateCondBr(protoIsInline, protoInlineBb, protoOverflowBb);

    builder.SetInsertPoint(protoInlineBb);
    llvm::Value* holderSlots =
        builder.CreateConstInBoundsGEP1_32(i8Ty, protoHdr, BRONZE_ABI_OBJ_SLOTS_OFFSET);
    llvm::Value* holderSlotPtr = builder.CreateInBoundsGEP(i64Ty, holderSlots, {protoSlot32});
    llvm::Value* protoHitInlineVal =
        builder.CreateAlignedLoad(i64Ty, holderSlotPtr, llvm::Align(8), "proto.hit.inline.val");
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(protoOverflowBb);
    llvm::Value* protoOverflowPtr = builder.CreateConstInBoundsGEP1_32(
        i8Ty, protoHdr, BRONZE_ABI_OBJ_OVERFLOW_OFFSET);
    llvm::Value* protoOverflowVal =
        builder.CreateAlignedLoad(i64Ty, protoOverflowPtr, llvm::Align(8), "proto.overflow");
    llvm::Value* protoOverflowTag = builder.CreateLShr(protoOverflowVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* protoOverflowIsObj =
        builder.CreateICmpEQ(protoOverflowTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    builder.CreateCondBr(protoOverflowIsObj, protoOverflowAccessBb, slowBb);

    builder.SetInsertPoint(protoOverflowAccessBb);
    llvm::Value* protoOverflowAddr =
        builder.CreateAnd(protoOverflowVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* protoOverflowObj = builder.CreateIntToPtr(protoOverflowAddr, ptrTy);
    llvm::Value* protoSlotIdx = builder.CreateSub(protoSlot32, builder.getInt32(3));
    llvm::Value* protoOverflowSlotPtr =
        builder.CreateInBoundsGEP(i64Ty, protoOverflowObj, {protoSlotIdx});
    llvm::Value* protoHitOverflowVal =
        builder.CreateAlignedLoad(i64Ty, protoOverflowSlotPtr, llvm::Align(8), "proto.hit.overflow.val");
    builder.CreateBr(doneBb);

    // 4. Hit: inline slot or overflow slot
    builder.SetInsertPoint(hitBb);
    llvm::Value* slot32 = builder.CreateTrunc(slotWord, i32Ty, "ic.slot32");
    llvm::Value* isInline =
        builder.CreateICmpULT(slot32, builder.getInt32(BRONZE_ABI_OBJ_INLINE_SLOTS));
    builder.CreateCondBr(isInline, inlineHitBb, overflowHitBb);

    builder.SetInsertPoint(inlineHitBb);
    llvm::Value* slotsBase =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SLOTS_OFFSET);
    llvm::Value* inlineSlotPtr = builder.CreateInBoundsGEP(i64Ty, slotsBase, {slot32});
    llvm::Value* inlineVal = builder.CreateAlignedLoad(i64Ty, inlineSlotPtr, llvm::Align(8), "ic.inline.val");
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(overflowHitBb);
    llvm::Value* overflowPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                 BRONZE_ABI_OBJ_OVERFLOW_OFFSET);
    llvm::Value* overflowVal =
        builder.CreateAlignedLoad(i64Ty, overflowPtr, llvm::Align(8), "ic.overflow");
    llvm::Value* overflowTag = builder.CreateLShr(overflowVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* overflowIsObj =
        builder.CreateICmpEQ(overflowTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    builder.CreateCondBr(overflowIsObj, overflowAccessBb, slowBb);

    builder.SetInsertPoint(overflowAccessBb);
    llvm::Value* overflowAddr =
        builder.CreateAnd(overflowVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* overflowObj = builder.CreateIntToPtr(overflowAddr, ptrTy);
    llvm::Value* slotIdx = builder.CreateSub(slot32, builder.getInt32(3));
    llvm::Value* overflowSlotPtr = builder.CreateInBoundsGEP(i64Ty, overflowObj, {slotIdx});
    llvm::Value* overflowValLoaded =
        builder.CreateAlignedLoad(i64Ty, overflowSlotPtr, llvm::Align(8), "ic.overflow.val");
    builder.CreateBr(doneBb);

    // 5. Fallback call
    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = emitPropGetCall(builder, abi, entry, objBits, keyIndex);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    unsigned phiCount = 5;  // inlineHitBb, overflowAccessBb, slowBb, protoInlineBb, protoOverflowAccessBb
    if (arrLenBb) phiCount++;
    if (arrUndefBb) phiCount++;
    if (arrPayloadBb) phiCount++;

    llvm::PHINode* result = builder.CreatePHI(i64Ty, phiCount, "prop");
    result->addIncoming(inlineVal, inlineHitBb);
    result->addIncoming(overflowValLoaded, overflowAccessBb);
    result->addIncoming(slowVal, slowBb);
    result->addIncoming(protoHitInlineVal, protoInlineBb);
    result->addIncoming(protoHitOverflowVal, protoOverflowAccessBb);
    if (arrLenBb) result->addIncoming(arrLenVal, arrLenBb);
    if (arrUndefBb) result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), arrUndefBb);
    if (arrPayloadBb) result->addIncoming(arrPayloadVal, arrPayloadBb);
    return result;
}

}  // namespace bronze::codegen_llvm
