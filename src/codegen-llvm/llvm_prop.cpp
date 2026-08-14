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

void emitPropSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::GlobalVariable* icTable,
                 llvm::Value* objBits, uint32_t keyIndex, llvm::Value* valBits, uint32_t icIndex,
                 bool strict, bool monomorphic, std::string_view keyStr) {
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
    builder.CreateCondBr(hit, hitBb, slowBb);

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
                         llvm::GlobalVariable* icTable, llvm::Value* objBits, uint32_t keyIndex,
                         uint32_t icIndex, bool monomorphic, std::string_view keyStr) {
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

    llvm::Value* hit = builder.CreateAnd(builder.CreateAnd(isPlain, shapeOk), depthOk, "ic.hit.cond");
    builder.CreateCondBr(hit, hitBb, slowBb);

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
    unsigned phiCount = 3;  // inlineHitBb, overflowAccessBb, slowBb
    if (arrLenBb) phiCount++;
    if (arrUndefBb) phiCount++;
    if (arrPayloadBb) phiCount++;

    llvm::PHINode* result = builder.CreatePHI(i64Ty, phiCount, "prop");
    result->addIncoming(inlineVal, inlineHitBb);
    result->addIncoming(overflowValLoaded, overflowAccessBb);
    result->addIncoming(slowVal, slowBb);
    if (arrLenBb) result->addIncoming(arrLenVal, arrLenBb);
    if (arrUndefBb) result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), arrUndefBb);
    if (arrPayloadBb) result->addIncoming(arrPayloadVal, arrPayloadBb);
    return result;
}

}  // namespace bronze::codegen_llvm
