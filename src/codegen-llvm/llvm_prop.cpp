#include "codegen-llvm/llvm_prop.h"
#include "codegen-llvm/llvm_elem.h"
#include "codegen-llvm/llvm_alias.h"

#include <optional>
#include <string>
#include <string_view>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

namespace {

static void markInvariant(llvm::LoadInst* load, llvm::LLVMContext& ctx) {
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

struct ProtoWalkResult {
    llvm::Value* holderHdr{nullptr};
    llvm::BasicBlock* latchBb{nullptr};
};

// Walks `depth` steps along the prototype chain starting from `startShape`.
// If any check fails (null root, non-object proto, non-plain proto, dictionary shape), branches to slowBb.
// When `depth` steps are traversed, branches to `successBb`.
static ProtoWalkResult emitProtoChainWalk(
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

// Loads a slot value from holderHdr at slot32 (inline if < 4, overflow if >= 4).
// If overflow is required and not present/not an object, branches to slowBb.
// On success, branches to `successBb` and returns the loaded Value bits via PHI.
static llvm::Value* emitObjectSlotLoad(
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
        llvm::BasicBlock* taCheckBb = llvm::BasicBlock::Create(ctx, "ic.set.ta.check", fn);
        llvm::BasicBlock* taElemBb = llvm::BasicBlock::Create(ctx, "ic.set.ta", fn);
        llvm::BasicBlock* arrWriteBb = llvm::BasicBlock::Create(ctx, "ic.set.arr.write", fn);
        llvm::BasicBlock* arrStoreBb = llvm::BasicBlock::Create(ctx, "ic.set.arr.store", fn);

        llvm::Value* isArr = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
        builder.CreateCondBr(isArr, arrElemBb, taCheckBb);

        builder.SetInsertPoint(taCheckBb);
        llvm::Value* isTa = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY));
        builder.CreateCondBr(isTa, taElemBb, plainCheckBb);

        builder.SetInsertPoint(arrElemBb);
        llvm::Value* lenPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                BRONZE_ABI_ARRAY_LENGTH_OFFSET);
        llvm::Value* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "arr.len");
        llvm::Value* capPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
        llvm::Value* cap = builder.CreateAlignedLoad(i32Ty, capPtr, llvm::Align(4), "arr.cap");
        llvm::Value* headPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                 BRONZE_ABI_ARRAY_HEAD_OFFSET);
        llvm::Value* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), "arr.head");
        llvm::Value* actualIdx = builder.CreateAdd(head, builder.getInt32(idx), "arr.actidx");
        llvm::Value* propsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                  BRONZE_ABI_ARRAY_PROPS_OFFSET);
        llvm::Value* propsVal = builder.CreateAlignedLoad(i64Ty, propsPtr, llvm::Align(8), "arr.props");
        llvm::Value* propsTag = builder.CreateLShr(propsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
        llvm::Value* hasNoProps =
            builder.CreateICmpEQ(propsTag, builder.getInt64(BRONZE_ABI_TAG_UNDEFINED));
        llvm::Value* inBounds = builder.CreateICmpULT(builder.getInt32(idx), len);
        llvm::Value* inCap = builder.CreateICmpULT(actualIdx, cap);
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
        llvm::Value* slotIdx = builder.CreateAdd(builder.CreateZExt(actualIdx, i64Ty), builder.getInt64(1));
        llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
        builder.CreateAlignedStore(valBits, slotPtr, llvm::Align(8));
        builder.CreateBr(doneBb);

        // TypedArray store:
        builder.SetInsertPoint(taElemBb);
        llvm::Value* valIsNum =
            builder.CreateICmpULE(valBits, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
        llvm::BasicBlock* taLenBb = llvm::BasicBlock::Create(ctx, "ic.set.ta.len", fn);
        builder.CreateCondBr(valIsNum, taLenBb, slowBb);

        builder.SetInsertPoint(taLenBb);
        llvm::Value* taLenPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
        auto* taLen = builder.CreateAlignedLoad(i32Ty, taLenPtr, llvm::Align(4), "ic.set.ta.len");
        markInvariant(taLen, ctx);
        llvm::BasicBlock* taKindBb = llvm::BasicBlock::Create(ctx, "ic.set.ta.kind", fn);
        builder.CreateCondBr(builder.CreateICmpULT(builder.getInt32(idx), taLen), taKindBb, doneBb);

        builder.SetInsertPoint(taKindBb);
        llvm::Value* kindPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_KIND_OFFSET);
        auto* kind = builder.CreateAlignedLoad(i32Ty, kindPtr, llvm::Align(4), "ic.set.ta.kind");
        markInvariant(kind, ctx);

        llvm::BasicBlock* f64Bb = llvm::BasicBlock::Create(ctx, "ic.set.ta.f64", fn);
        llvm::BasicBlock* f32Bb = llvm::BasicBlock::Create(ctx, "ic.set.ta.f32", fn);
        llvm::SwitchInst* swKind = builder.CreateSwitch(kind, slowBb, 2);
        swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT64), f64Bb);
        swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT32), f32Bb);

        builder.SetInsertPoint(f64Bb);
        llvm::Value* p64 = emitTypedArrayElemPtr(builder, hdr, builder.getInt32(idx), 8);
        auto* s64 = builder.CreateAlignedStore(builder.CreateBitCast(valBits, llvm::Type::getDoubleTy(ctx)), p64, llvm::Align(8));
        tagTypedArrayAccess(s64, ctx);
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(f32Bb);
        llvm::Value* p32 = emitTypedArrayElemPtr(builder, hdr, builder.getInt32(idx), 4);
        llvm::Value* narrowed =
            builder.CreateFPTrunc(builder.CreateBitCast(valBits, llvm::Type::getDoubleTy(ctx)), llvm::Type::getFloatTy(ctx), "ic.set.f32.val");
        auto* s32 = builder.CreateAlignedStore(narrowed, p32, llvm::Align(4));
        tagTypedArrayAccess(s32, ctx);
        builder.CreateBr(doneBb);
    } else {
        builder.CreateBr(plainCheckBb);
    }

    // 3. Plain object guard
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
    llvm::Value* isAccessor = builder.CreateICmpNE(
        builder.CreateAnd(depth, builder.getInt64(static_cast<uint64_t>(BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG))),
        builder.getInt64(0), "ic.set.isaccessor");
    llvm::Value* realDepth = builder.CreateAnd(
        depth, builder.getInt64(~static_cast<uint64_t>(BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG)), "ic.set.realdepth");
    llvm::Value* depthOk = builder.CreateICmpEQ(depth, builder.getInt64(0));

    llvm::Value* hit = builder.CreateAnd(builder.CreateAnd(isPlain, shapeOk), depthOk, "ic.set.hit.cond");
    llvm::Value* shapeHit = builder.CreateAnd(isPlain, shapeOk);
    llvm::Value* accHit = builder.CreateAnd(shapeHit, isAccessor, "ic.set.acchit");

    llvm::BasicBlock* setAccCheckBb = llvm::BasicBlock::Create(ctx, "ic.set.acc.check", fn);
    llvm::BasicBlock* notHitBb = llvm::BasicBlock::Create(ctx, "ic.set.nothit", fn);

    builder.CreateCondBr(hit, hitBb, notHitBb);

    // Accessor setter fast path
    builder.SetInsertPoint(setAccCheckBb);
    llvm::Value* accEnabledVal = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_inline_accessor_enabled, llvm::Align(8), "set.acc.enabled");
    llvm::Value* accEnabled = builder.CreateICmpNE(accEnabledVal, builder.getInt64(0));
    llvm::Value* setRealDepthOk = builder.CreateICmpEQ(realDepth, builder.getInt64(0), "set.acc.realdepthok");

    llvm::BasicBlock* setAccProtoCheckBb = llvm::BasicBlock::Create(ctx, "ic.set.acc.proto.check", fn);
    llvm::BasicBlock* setAccDispatchBb = llvm::BasicBlock::Create(ctx, "ic.set.acc.dispatch", fn);
    llvm::BasicBlock* setAccDepthSplitBb = llvm::BasicBlock::Create(ctx, "ic.set.acc.depth.split", fn);
    builder.CreateCondBr(accEnabled, setAccDepthSplitBb, slowBb);

    builder.SetInsertPoint(setAccDepthSplitBb);
    builder.CreateCondBr(setRealDepthOk, setAccDispatchBb, setAccProtoCheckBb);

    // Proto check for setter:
    builder.SetInsertPoint(setAccProtoCheckBb);
    llvm::Value* setEpochPtr = builder.CreateConstInBoundsGEP1_32(
        i64Ty, entry, static_cast<unsigned>(BRONZE_ABI_IC_EPOCH_OFFSET / sizeof(uint64_t)));
    llvm::Value* setFillEpoch =
        builder.CreateAlignedLoad(i64Ty, setEpochPtr, llvm::Align(8), "set.acc.fillepoch");
    llvm::Value* setCurEpoch = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_proto_epoch, llvm::Align(8), "set.acc.epoch");
    llvm::Value* setEpochOk = builder.CreateICmpEQ(setFillEpoch, setCurEpoch);
    llvm::BasicBlock* setAccProtoEntryBb = llvm::BasicBlock::Create(ctx, "ic.set.acc.proto.entry", fn);
    builder.CreateCondBr(setEpochOk, setAccProtoEntryBb, slowBb);

    builder.SetInsertPoint(setAccProtoEntryBb);
    ProtoWalkResult setAccWalk = emitProtoChainWalk(
        builder, ctx, fn, shape, realDepth, setAccProtoEntryBb, slowBb, setAccDispatchBb, "ic.set.acc");

    // Dispatch setter
    builder.SetInsertPoint(setAccDispatchBb);
    llvm::PHINode* setHolderHdr = builder.CreatePHI(ptrTy, 2, "set.holder.hdr");
    setHolderHdr->addIncoming(hdr, setAccDepthSplitBb);
    setHolderHdr->addIncoming(setAccWalk.holderHdr, setAccWalk.latchBb);

    llvm::Value* setSlot32 = builder.CreateTrunc(slotWord, i32Ty, "set.slot32");
    llvm::Value* setterSlot32 = builder.CreateAdd(setSlot32, builder.getInt32(1), "set.setter.slot32");
    llvm::BasicBlock* setInvokeBb = llvm::BasicBlock::Create(ctx, "ic.set.acc.invoke", fn);
    llvm::Value* setterVal = emitObjectSlotLoad(
        builder, ctx, fn, setHolderHdr, setterSlot32, slowBb, setInvokeBb, "set.acc.slot");

    builder.SetInsertPoint(setInvokeBb);
    llvm::Value* setterTag = builder.CreateLShr(setterVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* setterIsObj =
        builder.CreateICmpEQ(setterTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::BasicBlock* setFnCheckBb = llvm::BasicBlock::Create(ctx, "ic.set.acc.fncheck", fn);
    builder.CreateCondBr(setterIsObj, setFnCheckBb, slowBb);

    builder.SetInsertPoint(setFnCheckBb);
    llvm::Value* setterAddr =
        builder.CreateAnd(setterVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* setterFnPtr = builder.CreateIntToPtr(setterAddr, ptrTy, "set.fn.ptr");
    llvm::Value* setterFlags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, setterFnPtr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "set.fn.flags");
    llvm::Value* setterIsFn = builder.CreateICmpEQ(
        setterFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_FUNCTION));

    llvm::Value* setterArity = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, setterFnPtr, BRONZE_ABI_FN_ARITY_OFFSET),
        llvm::Align(4), "set.fn.arity");
    llvm::Value* setterArityOk = builder.CreateICmpULE(setterArity, builder.getInt32(1));

    llvm::Value* setterCode = builder.CreateAlignedLoad(
        ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, setterFnPtr, BRONZE_ABI_FN_CODE_OFFSET),
        llvm::Align(8), "set.fn.code");
    llvm::Value* setterCodeNonNull =
        builder.CreateICmpNE(setterCode, llvm::Constant::getNullValue(ptrTy));

    llvm::Value* setterCallable = builder.CreateAnd(
        builder.CreateAnd(setterIsFn, setterArityOk), setterCodeNonNull);
    llvm::BasicBlock* setCallBb = llvm::BasicBlock::Create(ctx, "ic.set.acc.call", fn);
    builder.CreateCondBr(setterCallable, setCallBb, slowBb);

    builder.SetInsertPoint(setCallBb);
    llvm::Value* setterEnv = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, setterFnPtr, BRONZE_ABI_FN_ENV_OFFSET),
        llvm::Align(8), "set.fn.env");

    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    llvm::Value* argBuf = entryBuilder.CreateAlloca(i64Ty, nullptr, "set.argbuf");
    builder.CreateAlignedStore(valBits, argBuf, llvm::Align(8));

    llvm::FunctionType* setterFnTy =
        llvm::FunctionType::get(i64Ty, {i64Ty, i64Ty, i32Ty, ptrTy}, false);
    builder.CreateCall(setterFnTy, setterCode, {setterEnv, objBits, builder.getInt32(1), argBuf});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(notHitBb);
    const bool transitionArm = !keyStr.empty() && keyStr != "length" && !optIdx.has_value();
    if (transitionArm) {
        llvm::BasicBlock* transNullBb = llvm::BasicBlock::Create(ctx, "ic.set.trans.null", fn);
        llvm::BasicBlock* transParentBb = llvm::BasicBlock::Create(ctx, "ic.set.trans.parent", fn);
        llvm::BasicBlock* transNodeBb = llvm::BasicBlock::Create(ctx, "ic.set.trans.node", fn);
        llvm::BasicBlock* transHitBb = llvm::BasicBlock::Create(ctx, "ic.set.trans.hit", fn);

        builder.CreateCondBr(accHit, setAccCheckBb, transNullBb);

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
        llvm::Value* transDepth = builder.CreateLShr(slotWord, 32);
        llvm::Value* transDepthOk = builder.CreateICmpEQ(transDepth, builder.getInt64(0), "trans.depthok");
        builder.CreateCondBr(builder.CreateAnd(parentOk, transDepthOk), transNodeBb, slowBb);

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
        llvm::BasicBlock* transInlineBb = llvm::BasicBlock::Create(ctx, "ic.set.trans.inline", fn);
        llvm::BasicBlock* transOverflowBb = llvm::BasicBlock::Create(ctx, "ic.set.trans.overflow", fn);
        llvm::Value* isInline = builder.CreateICmpULT(
            transSlot32, builder.getInt32(BRONZE_ABI_OBJ_INLINE_SLOTS), "trans.isinline");
        builder.CreateCondBr(isInline, transInlineBb, transOverflowBb);

        builder.SetInsertPoint(transInlineBb);
        llvm::Value* shapeSlotPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
        builder.CreateAlignedStore(cachedShape, shapeSlotPtr, llvm::Align(8));
        llvm::Value* transSlotsBase =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SLOTS_OFFSET);
        llvm::Value* transSlotPtr =
            builder.CreateInBoundsGEP(i64Ty, transSlotsBase, transSlot32);
        builder.CreateAlignedStore(valBits, transSlotPtr, llvm::Align(8));
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(transOverflowBb);
        llvm::BasicBlock* transOverflowCheckCapBb =
            llvm::BasicBlock::Create(ctx, "ic.set.trans.overflow.checkcap", fn);
        llvm::BasicBlock* transOverflowAccessBb =
            llvm::BasicBlock::Create(ctx, "ic.set.trans.overflow.access", fn);

        llvm::Value* enabledVal = builder.CreateAlignedLoad(
            i64Ty, globals.bronze_inline_overflow_set_enabled, llvm::Align(8), "trans.overflow.enabled");
        llvm::Value* isEnabled = builder.CreateICmpNE(enabledVal, builder.getInt64(0));

        llvm::Value* overflowPtr = builder.CreateConstInBoundsGEP1_32(
            i8Ty, hdr, BRONZE_ABI_OBJ_OVERFLOW_OFFSET);
        llvm::Value* overflowVal =
            builder.CreateAlignedLoad(i64Ty, overflowPtr, llvm::Align(8), "trans.overflow");
        llvm::Value* overflowTag = builder.CreateLShr(overflowVal, BRONZE_ABI_VALUE_TAG_SHIFT);
        llvm::Value* overflowIsObj =
            builder.CreateICmpEQ(overflowTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
        builder.CreateCondBr(builder.CreateAnd(isEnabled, overflowIsObj), transOverflowCheckCapBb, slowBb);

        builder.SetInsertPoint(transOverflowCheckCapBb);
        llvm::Value* overflowAddr =
            builder.CreateAnd(overflowVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
        llvm::Value* overflowObj = builder.CreateIntToPtr(overflowAddr, ptrTy);
        llvm::Value* sizePtr = builder.CreateConstInBoundsGEP1_32(
            i8Ty, overflowObj, BRONZE_ABI_HDR_SIZE_OFFSET);
        llvm::Value* sizeVal = builder.CreateAlignedLoad(i32Ty, sizePtr, llvm::Align(4), "overflow.size");
        llvm::Value* capVal = builder.CreateSub(
            builder.CreateLShr(sizeVal, 3), builder.getInt32(1), "overflow.cap");
        llvm::Value* slotIdx = builder.CreateSub(transSlot32, builder.getInt32(3));
        llvm::Value* withinCap = builder.CreateICmpULT(slotIdx, capVal, "trans.withincap");
        builder.CreateCondBr(withinCap, transOverflowAccessBb, slowBb);

        builder.SetInsertPoint(transOverflowAccessBb);
        llvm::Value* overflowShapeSlotPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
        builder.CreateAlignedStore(cachedShape, overflowShapeSlotPtr, llvm::Align(8));
        llvm::Value* overflowSlotPtr =
            builder.CreateInBoundsGEP(i64Ty, overflowObj, slotIdx);
        builder.CreateAlignedStore(valBits, overflowSlotPtr, llvm::Align(8));
        builder.CreateBr(doneBb);
    } else {
        builder.CreateCondBr(accHit, setAccCheckBb, slowBb);
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
    llvm::Value* inlineSlotPtr = builder.CreateInBoundsGEP(i64Ty, slotsBase, slot32);
    builder.CreateAlignedStore(valBits, inlineSlotPtr, llvm::Align(8));
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(overflowHitBb);
    llvm::BasicBlock* overflowCheckCapBb =
        llvm::BasicBlock::Create(ctx, "ic.set.overflow.checkcap", fn);
    llvm::Value* enabledVal = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_inline_overflow_set_enabled, llvm::Align(8), "set.overflow.enabled");
    llvm::Value* isEnabled = builder.CreateICmpNE(enabledVal, builder.getInt64(0));

    llvm::Value* overflowPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                 BRONZE_ABI_OBJ_OVERFLOW_OFFSET);
    llvm::Value* overflowVal =
        builder.CreateAlignedLoad(i64Ty, overflowPtr, llvm::Align(8), "ic.set.overflow");
    llvm::Value* overflowTag = builder.CreateLShr(overflowVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* overflowIsObj =
        builder.CreateICmpEQ(overflowTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    builder.CreateCondBr(builder.CreateAnd(isEnabled, overflowIsObj), overflowCheckCapBb, slowBb);

    builder.SetInsertPoint(overflowCheckCapBb);
    llvm::Value* overflowAddr =
        builder.CreateAnd(overflowVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* overflowObj = builder.CreateIntToPtr(overflowAddr, ptrTy);
    llvm::Value* sizePtr = builder.CreateConstInBoundsGEP1_32(
        i8Ty, overflowObj, BRONZE_ABI_HDR_SIZE_OFFSET);
    llvm::Value* sizeVal = builder.CreateAlignedLoad(i32Ty, sizePtr, llvm::Align(4), "overflow.size");
    llvm::Value* capVal = builder.CreateSub(
        builder.CreateLShr(sizeVal, 3), builder.getInt32(1), "overflow.cap");
    llvm::Value* slotIdx = builder.CreateSub(slot32, builder.getInt32(3));
    llvm::Value* withinCap = builder.CreateICmpULT(slotIdx, capVal, "set.withincap");
    builder.CreateCondBr(withinCap, overflowAccessBb, slowBb);

    builder.SetInsertPoint(overflowAccessBb);
    llvm::Value* overflowSlotPtr = builder.CreateInBoundsGEP(i64Ty, overflowObj, slotIdx);
    builder.CreateAlignedStore(valBits, overflowSlotPtr, llvm::Align(8));
    builder.CreateBr(doneBb);

    // 5. Fallback call
    builder.SetInsertPoint(slowBb);
    builder.CreateCall(abi.bronze_prop_set,
                       {objBits, builder.getInt32(keyIndex), valBits, entry,
                        builder.getInt1(strict)});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
}

static llvm::Value* emitPropGetCall(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                    llvm::Value* entry, llvm::Value* objBits,
                                    uint32_t keyIndex) {
    return builder.CreateCall(abi.bronze_prop_get,
                              {objBits, builder.getInt32(keyIndex), entry}, "prop.slow");
}

llvm::Value* emitPropGet(llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
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
    llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* checkBb = llvm::BasicBlock::Create(ctx, "ic.check", fn);
    llvm::BasicBlock* plainCheckBb = llvm::BasicBlock::Create(ctx, "ic.plain", fn);
    llvm::BasicBlock* hitBb = llvm::BasicBlock::Create(ctx, "ic.hit", fn);
    llvm::BasicBlock* inlineHitBb = llvm::BasicBlock::Create(ctx, "ic.inline", fn);
    llvm::BasicBlock* overflowHitBb = llvm::BasicBlock::Create(ctx, "ic.overflow", fn);
    llvm::BasicBlock* overflowAccessBb = llvm::BasicBlock::Create(ctx, "ic.overflow.access", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "ic.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "ic.done", fn);

    // 1. Is the receiver an object?
    llvm::Value* tag = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObject =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "ic.isobj");
    builder.CreateCondBr(isObject, checkBb, slowBb);

    // 2. Load flags from header
    builder.SetInsertPoint(checkBb);
    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "ic.hdr");

    llvm::Value* flagsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                              BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "ic.flags");

    llvm::BasicBlock* arrLenBb = nullptr;
    llvm::Value* arrLenVal = nullptr;
    llvm::BasicBlock* taLenBb = nullptr;
    llvm::Value* taLenVal = nullptr;
    llvm::BasicBlock* arrUndefBb = nullptr;
    llvm::BasicBlock* arrPayloadBb = nullptr;
    llvm::Value* arrPayloadVal = nullptr;
    llvm::BasicBlock* taElemUndefBb = nullptr;
    llvm::BasicBlock* taF64Bb = nullptr;
    llvm::Value* taF64Val = nullptr;
    llvm::BasicBlock* taF32Bb = nullptr;
    llvm::Value* taF32Val = nullptr;
    llvm::BasicBlock* taI32Bb = nullptr;
    llvm::Value* taI32Val = nullptr;
    llvm::BasicBlock* taU32Bb = nullptr;
    llvm::Value* taU32Val = nullptr;
    llvm::BasicBlock* taI16Bb = nullptr;
    llvm::Value* taI16Val = nullptr;
    llvm::BasicBlock* taU16Bb = nullptr;
    llvm::Value* taU16Val = nullptr;
    llvm::BasicBlock* taI8Bb = nullptr;
    llvm::Value* taI8Val = nullptr;
    llvm::BasicBlock* taU8Bb = nullptr;
    llvm::Value* taU8Val = nullptr;
    llvm::BasicBlock* arrMethodHitBb = nullptr;
    llvm::Value* arrMethodVal = nullptr;
    llvm::BasicBlock* fnProtoHitBb = nullptr;
    llvm::Value* fnProtoVal = nullptr;

    auto optIdx = parseIndexKey(keyStr);
    if (keyStr == "length") {
        arrLenBb = llvm::BasicBlock::Create(ctx, "ic.arr.len", fn);
        taLenBb = llvm::BasicBlock::Create(ctx, "ic.ta.len", fn);
        llvm::BasicBlock* taCheckBb = llvm::BasicBlock::Create(ctx, "ic.ta.check", fn);
        llvm::Value* isArr = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
        builder.CreateCondBr(isArr, arrLenBb, taCheckBb);

        builder.SetInsertPoint(taCheckBb);
        llvm::Value* isTa = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY));
        builder.CreateCondBr(isTa, taLenBb, plainCheckBb);

        builder.SetInsertPoint(arrLenBb);
        llvm::Value* arrLenPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                   BRONZE_ABI_ARRAY_LENGTH_OFFSET);
        llvm::Value* arrLen = builder.CreateAlignedLoad(i32Ty, arrLenPtr, llvm::Align(4), "arr.len");
        llvm::Value* arrLenDbl = builder.CreateUIToFP(arrLen, llvm::Type::getDoubleTy(ctx), "arr.len.dbl");
        arrLenVal = builder.CreateBitCast(arrLenDbl, i64Ty, "arr.len.bits");
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(taLenBb);
        llvm::Value* taLenPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                  BRONZE_ABI_TA_LENGTH_OFFSET);
        llvm::Value* taLen = builder.CreateAlignedLoad(i32Ty, taLenPtr, llvm::Align(4), "ta.len");
        llvm::Value* taLenDbl = builder.CreateUIToFP(taLen, llvm::Type::getDoubleTy(ctx), "ta.len.dbl");
        taLenVal = builder.CreateBitCast(taLenDbl, i64Ty, "ta.len.bits");
        builder.CreateBr(doneBb);
    } else if (keyStr == "prototype") {
        llvm::BasicBlock* fnProtoBb = llvm::BasicBlock::Create(ctx, "ic.fn.proto", fn);
        fnProtoHitBb = llvm::BasicBlock::Create(ctx, "ic.fn.proto.hit", fn);
        llvm::Value* isFn = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_FUNCTION));
        builder.CreateCondBr(isFn, fnProtoBb, plainCheckBb);

        builder.SetInsertPoint(fnProtoBb);
        llvm::Value* protoPtr = builder.CreateConstInBoundsGEP1_32(
            i8Ty, hdr, BRONZE_ABI_FN_PROTOTYPE_OFFSET);
        fnProtoVal = builder.CreateAlignedLoad(i64Ty, protoPtr, llvm::Align(8), "fn.proto");
        llvm::Value* protoTag = builder.CreateLShr(fnProtoVal, BRONZE_ABI_VALUE_TAG_SHIFT);
        llvm::Value* protoIsObj = builder.CreateICmpEQ(protoTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
        builder.CreateCondBr(protoIsObj, fnProtoHitBb, slowBb);

        builder.SetInsertPoint(fnProtoHitBb);
        builder.CreateBr(doneBb);
    } else if (optIdx.has_value()) {
        uint32_t idx = *optIdx;
        llvm::BasicBlock* arrElemBb = llvm::BasicBlock::Create(ctx, "ic.arr.elem", fn);
        llvm::BasicBlock* taCheckBb = llvm::BasicBlock::Create(ctx, "ic.ta.check", fn);
        llvm::BasicBlock* taElemBb = llvm::BasicBlock::Create(ctx, "ic.ta.elem", fn);
        llvm::BasicBlock* arrReadBb = llvm::BasicBlock::Create(ctx, "ic.arr.read", fn);
        arrUndefBb = llvm::BasicBlock::Create(ctx, "ic.arr.undef", fn);
        arrPayloadBb = llvm::BasicBlock::Create(ctx, "ic.arr.payload", fn);

        llvm::Value* isArr = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
        builder.CreateCondBr(isArr, arrElemBb, taCheckBb);

        builder.SetInsertPoint(taCheckBb);
        llvm::Value* isTa = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY));
        builder.CreateCondBr(isTa, taElemBb, plainCheckBb);

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
        llvm::Value* headPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                 BRONZE_ABI_ARRAY_HEAD_OFFSET);
        llvm::Value* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), "arr.head");
        llvm::Value* actualIdx = builder.CreateAdd(head, builder.getInt32(idx), "arr.actidx");
        llvm::Value* slotIdx = builder.CreateAdd(builder.CreateZExt(actualIdx, i64Ty), builder.getInt64(1));
        llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
        llvm::Value* elemVal = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "arr.elem.raw");
        llvm::Value* elemTag = builder.CreateLShr(elemVal, BRONZE_ABI_VALUE_TAG_SHIFT);
        llvm::Value* isHole = builder.CreateICmpEQ(elemTag, builder.getInt64(BRONZE_ABI_TAG_HOLE));
        arrPayloadVal = builder.CreateSelect(isHole, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), elemVal, "arr.elem");
        builder.CreateBr(doneBb);

        // TypedArray element get:
        builder.SetInsertPoint(taElemBb);
        llvm::Value* taLenPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
        auto* taLen = builder.CreateAlignedLoad(i32Ty, taLenPtr, llvm::Align(4), "ic.ta.len");
        markInvariant(taLen, ctx);
        llvm::BasicBlock* taKindBb = llvm::BasicBlock::Create(ctx, "ic.ta.kind", fn);
        taElemUndefBb = llvm::BasicBlock::Create(ctx, "ic.ta.undef", fn);
        builder.CreateCondBr(builder.CreateICmpULT(builder.getInt32(idx), taLen), taKindBb, taElemUndefBb);

        builder.SetInsertPoint(taElemUndefBb);
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(taKindBb);
        llvm::Value* kindPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_KIND_OFFSET);
        auto* kind = builder.CreateAlignedLoad(i32Ty, kindPtr, llvm::Align(4), "ic.ta.kind");
        markInvariant(kind, ctx);

        taF64Bb = llvm::BasicBlock::Create(ctx, "ic.ta.f64", fn);
        taF32Bb = llvm::BasicBlock::Create(ctx, "ic.ta.f32", fn);
        llvm::BasicBlock* i32Bb = llvm::BasicBlock::Create(ctx, "ic.ta.i32", fn);
        llvm::BasicBlock* u32Bb = llvm::BasicBlock::Create(ctx, "ic.ta.u32", fn);
        llvm::BasicBlock* i16Bb = llvm::BasicBlock::Create(ctx, "ic.ta.i16", fn);
        llvm::BasicBlock* u16Bb = llvm::BasicBlock::Create(ctx, "ic.ta.u16", fn);
        llvm::BasicBlock* i8Bb = llvm::BasicBlock::Create(ctx, "ic.ta.i8", fn);
        llvm::BasicBlock* u8Bb = llvm::BasicBlock::Create(ctx, "ic.ta.u8", fn);

        llvm::SwitchInst* swKind = builder.CreateSwitch(kind, slowBb, 9);
        swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT64), taF64Bb);
        swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT32), taF32Bb);
        swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_INT32), i32Bb);
        swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_UINT32), u32Bb);
        swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_INT16), i16Bb);
        swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_UINT16), u16Bb);
        swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_INT8), i8Bb);
        swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_UINT8), u8Bb);
        swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_UINT8CLAMPED), u8Bb);

        builder.SetInsertPoint(taF64Bb);
        llvm::Value* p64 = emitTypedArrayElemPtr(builder, hdr, builder.getInt32(idx), 8);
        auto* d64 = builder.CreateAlignedLoad(dblTy, p64, llvm::Align(8), "ic.ta.d64");
        tagTypedArrayAccess(d64, ctx);
        taF64Val = emitBoxDouble(builder, d64);
        taF64Bb = builder.GetInsertBlock();
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(taF32Bb);
        llvm::Value* p32 = emitTypedArrayElemPtr(builder, hdr, builder.getInt32(idx), 4);
        auto* d32 = builder.CreateAlignedLoad(f32Ty, p32, llvm::Align(4), "ic.ta.d32");
        tagTypedArrayAccess(d32, ctx);
        taF32Val = emitBoxDouble(builder, builder.CreateFPExt(d32, dblTy));
        taF32Bb = builder.GetInsertBlock();
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(i32Bb);
        llvm::Value* pi32 = emitTypedArrayElemPtr(builder, hdr, builder.getInt32(idx), 4);
        auto* di32 = builder.CreateAlignedLoad(i32Ty, pi32, llvm::Align(4), "ic.ta.i32");
        tagTypedArrayAccess(di32, ctx);
        taI32Val = emitBoxDouble(builder, builder.CreateSIToFP(di32, dblTy));
        taI32Bb = builder.GetInsertBlock();
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(u32Bb);
        llvm::Value* pu32 = emitTypedArrayElemPtr(builder, hdr, builder.getInt32(idx), 4);
        auto* du32 = builder.CreateAlignedLoad(i32Ty, pu32, llvm::Align(4), "ic.ta.u32");
        tagTypedArrayAccess(du32, ctx);
        taU32Val = emitBoxDouble(builder, builder.CreateUIToFP(du32, dblTy));
        taU32Bb = builder.GetInsertBlock();
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(i16Bb);
        llvm::Value* pi16 = emitTypedArrayElemPtr(builder, hdr, builder.getInt32(idx), 2);
        auto* di16 = builder.CreateAlignedLoad(i16Ty, pi16, llvm::Align(2), "ic.ta.i16");
        tagTypedArrayAccess(di16, ctx);
        taI16Val = emitBoxDouble(builder, builder.CreateSIToFP(di16, dblTy));
        taI16Bb = builder.GetInsertBlock();
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(u16Bb);
        llvm::Value* pu16 = emitTypedArrayElemPtr(builder, hdr, builder.getInt32(idx), 2);
        auto* du16 = builder.CreateAlignedLoad(i16Ty, pu16, llvm::Align(2), "ic.ta.u16");
        tagTypedArrayAccess(du16, ctx);
        taU16Val = emitBoxDouble(builder, builder.CreateUIToFP(du16, dblTy));
        taU16Bb = builder.GetInsertBlock();
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(i8Bb);
        llvm::Value* pi8 = emitTypedArrayElemPtr(builder, hdr, builder.getInt32(idx), 1);
        auto* di8 = builder.CreateAlignedLoad(i8Ty, pi8, llvm::Align(1), "ic.ta.i8");
        tagTypedArrayAccess(di8, ctx);
        taI8Val = emitBoxDouble(builder, builder.CreateSIToFP(di8, dblTy));
        taI8Bb = builder.GetInsertBlock();
        builder.CreateBr(doneBb);

        builder.SetInsertPoint(u8Bb);
        llvm::Value* pu8 = emitTypedArrayElemPtr(builder, hdr, builder.getInt32(idx), 1);
        auto* du8 = builder.CreateAlignedLoad(i8Ty, pu8, llvm::Align(1), "ic.ta.u8");
        tagTypedArrayAccess(du8, ctx);
        taU8Val = emitBoxDouble(builder, builder.CreateUIToFP(du8, dblTy));
        taU8Bb = builder.GetInsertBlock();
        builder.CreateBr(doneBb);
    } else {
        llvm::BasicBlock* arrMethodBb = llvm::BasicBlock::Create(ctx, "ic.arr.method", fn);
        arrMethodHitBb = llvm::BasicBlock::Create(ctx, "ic.arr.method.hit", fn);
        llvm::Value* isArr = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
        builder.CreateCondBr(isArr, arrMethodBb, plainCheckBb);

        builder.SetInsertPoint(arrMethodBb);
        llvm::Value* enabledVal = builder.CreateAlignedLoad(
            i64Ty, globals.bronze_array_method_ic_enabled, llvm::Align(8), "arr.ic.enabled");
        llvm::Value* isEnabled = builder.CreateICmpNE(enabledVal, builder.getInt64(0));

        llvm::Value* propsPtr = builder.CreateConstInBoundsGEP1_32(
            i8Ty, hdr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
        llvm::Value* propsVal = builder.CreateAlignedLoad(i64Ty, propsPtr, llvm::Align(8), "arr.props");
        llvm::Value* propsTag = builder.CreateLShr(propsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
        llvm::Value* hasNoProps =
            builder.CreateICmpNE(propsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));

        llvm::Value* cachedShapeVal = builder.CreateAlignedLoad(ptrTy, entry, llvm::Align(8), "arr.cached");
        llvm::Value* cachedShapeInt = builder.CreatePtrToInt(cachedShapeVal, i64Ty);
        llvm::Value* isSentinel =
            builder.CreateICmpEQ(cachedShapeInt, builder.getInt64(BRONZE_ABI_IC_SHAPE_ARRAY_METHOD));

        llvm::Value* methodOk = builder.CreateAnd(
            isEnabled, builder.CreateAnd(hasNoProps, isSentinel), "arr.method.ok");
        builder.CreateCondBr(methodOk, arrMethodHitBb, slowBb);

        builder.SetInsertPoint(arrMethodHitBb);
        llvm::Value* slot32Ptr = builder.CreateConstInBoundsGEP1_32(
            i8Ty, entry, BRONZE_ABI_IC_SLOT_OFFSET);
        llvm::Value* methodId = builder.CreateAlignedLoad(
            i32Ty, slot32Ptr, llvm::Align(4), "arr.method.id");
        llvm::Value* methodTbl = builder.CreateAlignedLoad(
            ptrTy, globals.bronze_array_method_tbl, llvm::Align(8), "arr.method.tbl");
        llvm::Value* methodSlotPtr = builder.CreateInBoundsGEP(
            i64Ty, methodTbl, methodId);
        arrMethodVal = builder.CreateAlignedLoad(
            i64Ty, methodSlotPtr, llvm::Align(8), "arr.method.val");
        builder.CreateBr(doneBb);
    }

    // 3. Plain object guard
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
    llvm::Value* isAccessor = builder.CreateICmpNE(
        builder.CreateAnd(depth, builder.getInt64(static_cast<uint64_t>(BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG))),
        builder.getInt64(0), "ic.get.isaccessor");
    llvm::Value* realDepth = builder.CreateAnd(
        depth, builder.getInt64(~static_cast<uint64_t>(BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG)), "ic.get.realdepth");
    llvm::Value* depthOk = builder.CreateICmpEQ(depth, builder.getInt64(0));

    llvm::Value* shapeHit = builder.CreateAnd(isPlain, shapeOk, "ic.shape.cond");
    llvm::BasicBlock* depthSplitBb = llvm::BasicBlock::Create(ctx, "ic.depth.split", fn);
    builder.CreateCondBr(shapeHit, depthSplitBb, slowBb);

    builder.SetInsertPoint(depthSplitBb);
    llvm::BasicBlock* getAccCheckBb = llvm::BasicBlock::Create(ctx, "ic.get.acc.check", fn);
    llvm::BasicBlock* getDataDepthSplitBb = llvm::BasicBlock::Create(ctx, "ic.get.data.depth", fn);
    builder.CreateCondBr(isAccessor, getAccCheckBb, getDataDepthSplitBb);

    // Accessor getter fast path
    builder.SetInsertPoint(getAccCheckBb);
    llvm::Value* accEnabledVal = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_inline_accessor_enabled, llvm::Align(8), "get.acc.enabled");
    llvm::Value* accEnabled = builder.CreateICmpNE(accEnabledVal, builder.getInt64(0));
    llvm::Value* getRealDepthOk = builder.CreateICmpEQ(realDepth, builder.getInt64(0), "get.acc.realdepthok");

    llvm::BasicBlock* getAccProtoCheckBb = llvm::BasicBlock::Create(ctx, "ic.get.acc.proto.check", fn);
    llvm::BasicBlock* getAccDispatchBb = llvm::BasicBlock::Create(ctx, "ic.get.acc.dispatch", fn);
    llvm::BasicBlock* getAccDepthSplitBb = llvm::BasicBlock::Create(ctx, "ic.get.acc.depth.split", fn);
    builder.CreateCondBr(accEnabled, getAccDepthSplitBb, slowBb);

    builder.SetInsertPoint(getAccDepthSplitBb);
    builder.CreateCondBr(getRealDepthOk, getAccDispatchBb, getAccProtoCheckBb);

    // Proto check for getter:
    builder.SetInsertPoint(getAccProtoCheckBb);
    llvm::Value* getEpochPtr = builder.CreateConstInBoundsGEP1_32(
        i64Ty, entry, static_cast<unsigned>(BRONZE_ABI_IC_EPOCH_OFFSET / sizeof(uint64_t)));
    llvm::Value* getFillEpoch =
        builder.CreateAlignedLoad(i64Ty, getEpochPtr, llvm::Align(8), "get.acc.fillepoch");
    llvm::Value* getCurEpoch = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_proto_epoch, llvm::Align(8), "get.acc.epoch");
    llvm::Value* getEpochOk = builder.CreateICmpEQ(getFillEpoch, getCurEpoch);
    llvm::BasicBlock* getAccProtoEntryBb = llvm::BasicBlock::Create(ctx, "ic.get.acc.proto.entry", fn);
    builder.CreateCondBr(getEpochOk, getAccProtoEntryBb, slowBb);

    builder.SetInsertPoint(getAccProtoEntryBb);
    ProtoWalkResult getAccWalk = emitProtoChainWalk(
        builder, ctx, fn, shape, realDepth, getAccProtoEntryBb, slowBb, getAccDispatchBb, "ic.get.acc");

    // Dispatch getter
    builder.SetInsertPoint(getAccDispatchBb);
    llvm::PHINode* getHolderHdr = builder.CreatePHI(ptrTy, 2, "get.holder.hdr");
    getHolderHdr->addIncoming(hdr, getAccDepthSplitBb);
    getHolderHdr->addIncoming(getAccWalk.holderHdr, getAccWalk.latchBb);

    llvm::Value* getSlot32 = builder.CreateTrunc(slotWord, i32Ty, "get.slot32");
    llvm::BasicBlock* getInvokeBb = llvm::BasicBlock::Create(ctx, "ic.get.acc.invoke", fn);
    llvm::Value* getterVal = emitObjectSlotLoad(
        builder, ctx, fn, getHolderHdr, getSlot32, slowBb, getInvokeBb, "get.acc.slot");

    builder.SetInsertPoint(getInvokeBb);
    llvm::Value* getterIsUndef =
        builder.CreateICmpEQ(getterVal, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), "get.isundef");
    llvm::BasicBlock* getUndefBb = llvm::BasicBlock::Create(ctx, "ic.get.acc.undef", fn);
    llvm::BasicBlock* getFnObjCheckBb = llvm::BasicBlock::Create(ctx, "ic.get.acc.fnobjcheck", fn);
    builder.CreateCondBr(getterIsUndef, getUndefBb, getFnObjCheckBb);

    builder.SetInsertPoint(getUndefBb);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(getFnObjCheckBb);
    llvm::Value* getterTag = builder.CreateLShr(getterVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* getterIsObj =
        builder.CreateICmpEQ(getterTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::BasicBlock* getFnCheckBb = llvm::BasicBlock::Create(ctx, "ic.get.acc.fncheck", fn);
    builder.CreateCondBr(getterIsObj, getFnCheckBb, slowBb);

    builder.SetInsertPoint(getFnCheckBb);
    llvm::Value* getterAddr =
        builder.CreateAnd(getterVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* getterFnPtr = builder.CreateIntToPtr(getterAddr, ptrTy, "get.fn.ptr");
    llvm::Value* getterFlags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, getterFnPtr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "get.fn.flags");
    llvm::Value* getterIsFn = builder.CreateICmpEQ(
        getterFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_FUNCTION));

    llvm::Value* getterArity = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, getterFnPtr, BRONZE_ABI_FN_ARITY_OFFSET),
        llvm::Align(4), "get.fn.arity");
    llvm::Value* getterArityOk = builder.CreateICmpEQ(getterArity, builder.getInt32(0));

    llvm::Value* getterCode = builder.CreateAlignedLoad(
        ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, getterFnPtr, BRONZE_ABI_FN_CODE_OFFSET),
        llvm::Align(8), "get.fn.code");
    llvm::Value* getterCodeNonNull =
        builder.CreateICmpNE(getterCode, llvm::Constant::getNullValue(ptrTy));

    llvm::Value* getterCallable = builder.CreateAnd(
        builder.CreateAnd(getterIsFn, getterArityOk), getterCodeNonNull);
    llvm::BasicBlock* getCallBb = llvm::BasicBlock::Create(ctx, "ic.get.acc.call", fn);
    builder.CreateCondBr(getterCallable, getCallBb, slowBb);

    builder.SetInsertPoint(getCallBb);
    llvm::Value* getterEnv = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, getterFnPtr, BRONZE_ABI_FN_ENV_OFFSET),
        llvm::Align(8), "get.fn.env");

    llvm::FunctionType* getterFnTy =
        llvm::FunctionType::get(i64Ty, {i64Ty, i64Ty, i32Ty, ptrTy}, false);
    llvm::Value* getterRes = builder.CreateCall(
        getterFnTy, getterCode, {getterEnv, objBits, builder.getInt32(0), llvm::Constant::getNullValue(ptrTy)}, "acc.get.res");
    builder.CreateBr(doneBb);

    // 3b. Depth 0 is the own-property hit; depth > 0 is a PROTO hit
    builder.SetInsertPoint(getDataDepthSplitBb);
    llvm::BasicBlock* protoCheckBb = llvm::BasicBlock::Create(ctx, "ic.proto.check", fn);
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
    llvm::BasicBlock* protoEntryBb = llvm::BasicBlock::Create(ctx, "ic.proto.entry", fn);
    builder.CreateCondBr(epochOk, protoEntryBb, slowBb);

    builder.SetInsertPoint(protoEntryBb);
    llvm::BasicBlock* protoResBb = llvm::BasicBlock::Create(ctx, "ic.proto.res", fn);
    ProtoWalkResult protoWalk = emitProtoChainWalk(
        builder, ctx, fn, shape, depth, protoEntryBb, slowBb, protoResBb, "ic.proto");

    builder.SetInsertPoint(protoResBb);
    llvm::BasicBlock* protoLoadSuccessBb = llvm::BasicBlock::Create(ctx, "ic.proto.loadsucc", fn);
    llvm::Value* protoHitVal = emitObjectSlotLoad(
        builder, ctx, fn, protoWalk.holderHdr, protoSlot32, slowBb, protoLoadSuccessBb, "proto.slot");

    builder.SetInsertPoint(protoLoadSuccessBb);
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
    llvm::Value* inlineSlotPtr = builder.CreateInBoundsGEP(i64Ty, slotsBase, slot32);
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
    llvm::Value* overflowSlotPtr = builder.CreateInBoundsGEP(i64Ty, overflowObj, slotIdx);
    llvm::Value* overflowValLoaded =
        builder.CreateAlignedLoad(i64Ty, overflowSlotPtr, llvm::Align(8), "ic.overflow.val");
    builder.CreateBr(doneBb);

    // 5. Fallback call
    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = emitPropGetCall(builder, abi, entry, objBits, keyIndex);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    unsigned phiCount = 6;  // inlineHitBb, overflowAccessBb, slowBb, protoLoadSuccessBb, getCallBb, getUndefBb
    if (arrLenBb) phiCount++;
    if (taLenBb) phiCount++;
    if (arrUndefBb) phiCount++;
    if (arrPayloadBb) phiCount++;
    if (arrMethodHitBb) phiCount++;
    if (fnProtoHitBb) phiCount++;
    if (taElemUndefBb) phiCount++;
    if (taF64Bb) phiCount++;
    if (taF32Bb) phiCount++;
    if (taI32Bb) phiCount++;
    if (taU32Bb) phiCount++;
    if (taI16Bb) phiCount++;
    if (taU16Bb) phiCount++;
    if (taI8Bb) phiCount++;
    if (taU8Bb) phiCount++;

    llvm::PHINode* result = builder.CreatePHI(i64Ty, phiCount, "prop");
    result->addIncoming(inlineVal, inlineHitBb);
    result->addIncoming(overflowValLoaded, overflowAccessBb);
    result->addIncoming(slowVal, slowBb);
    result->addIncoming(protoHitVal, protoLoadSuccessBb);
    result->addIncoming(getterRes, getCallBb);
    result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), getUndefBb);
    if (arrLenBb) result->addIncoming(arrLenVal, arrLenBb);
    if (taLenBb) result->addIncoming(taLenVal, taLenBb);
    if (arrUndefBb) result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), arrUndefBb);
    if (arrPayloadBb) result->addIncoming(arrPayloadVal, arrPayloadBb);
    if (arrMethodHitBb) result->addIncoming(arrMethodVal, arrMethodHitBb);
    if (fnProtoHitBb) result->addIncoming(fnProtoVal, fnProtoHitBb);
    if (taElemUndefBb) result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), taElemUndefBb);
    if (taF64Bb) result->addIncoming(taF64Val, taF64Bb);
    if (taF32Bb) result->addIncoming(taF32Val, taF32Bb);
    if (taI32Bb) result->addIncoming(taI32Val, taI32Bb);
    if (taU32Bb) result->addIncoming(taU32Val, taU32Bb);
    if (taI16Bb) result->addIncoming(taI16Val, taI16Bb);
    if (taU16Bb) result->addIncoming(taU16Val, taU16Bb);
    if (taI8Bb) result->addIncoming(taI8Val, taI8Bb);
    if (taU8Bb) result->addIncoming(taU8Val, taU8Bb);
    return result;
}

}  // namespace bronze::codegen_llvm
