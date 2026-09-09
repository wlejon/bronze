#include "codegen-llvm/llvm_iter.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/MDBuilder.h>

#include <string>

namespace bronze::codegen_llvm {

namespace {

// The record's payload pointer, or a branch to `fail`. Every use of a record in
// generated code came from `iter.open`, so this is an invariant rather than a
// question the language asks — but it is asked anyway, cheaply, because the
// alternative is loading a `kind` word out of whatever the value happened to
// be.
llvm::Value* emitRecordPtr(llvm::IRBuilder<>& builder, llvm::Value* recBits,
                           llvm::BasicBlock* fail, const char* prefix) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::Value* tag = builder.CreateLShr(recBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObj =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::BasicBlock* kindBb =
        llvm::BasicBlock::Create(ctx, std::string(prefix) + "rec", fn);
    builder.CreateCondBr(isObj, kindBb, fail);

    builder.SetInsertPoint(kindBb);
    llvm::Value* addr =
        builder.CreateAnd(recBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "it.hdr");
    llvm::Value* flagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "it.flags");
    llvm::Value* isRec =
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ITERATOR));
    llvm::BasicBlock* okBb = llvm::BasicBlock::Create(ctx, std::string(prefix) + "ok", fn);
    builder.CreateCondBr(isRec, okBb, fail);

    builder.SetInsertPoint(okBb);
    return hdr;
}

// The seam word. `bronze_tls_block_addr` is `readnone` + `willreturn`, so the
// call CSEs with every other use of the block in the same function and a loop
// hoists it.
llvm::Value* emitIterFastEnabled(llvm::IRBuilder<>& builder, const AbiFns& abi) {
    llvm::Value* base = builder.CreateCall(abi.bronze_tls_block_addr, {}, "tls");
    llvm::Value* cellPtr = builder.CreateConstInBoundsGEP1_64(
        builder.getInt8Ty(), base, BRONZE_TLS_ITER_FAST_ENABLED_OFF, "tls.iterfast");
    llvm::Value* cell =
        builder.CreateAlignedLoad(builder.getInt64Ty(), cellPtr, llvm::Align(8), "it.seam");
    return builder.CreateICmpNE(cell, builder.getInt64(0), "it.seam.on");
}

}  // namespace

llvm::Value* emitIterOpen(llvm::IRBuilder<>& builder, const AbiFns& abi,
                          const AbiGlobals& globals, llvm::Value* srcBits) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "io.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "io.done", fn);

    llvm::BasicBlock* seamBb = llvm::BasicBlock::Create(ctx, "io.seam", fn);
    builder.CreateCondBr(emitIterFastEnabled(builder, abi), seamBb, slowBb);
    builder.SetInsertPoint(seamBb);

    // ONLY an Array source: `rtOpenIterator` classifies by the receiver's
    // header flags and nothing else — an array has no per-value say in how it
    // iterates (its @@iterator is the C table's, which rt_prop_write.cpp's
    // Array.prototype refusal keeps undecoratable) — so flags == Array is the
    // WHOLE of the open's classification for this kind, re-asked here. Every
    // other source (string, typed array, Map, Set, protocol) keeps the helper.
    llvm::Value* tag = builder.CreateLShr(srcBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObj = builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::BasicBlock* flagsBb = llvm::BasicBlock::Create(ctx, "io.flags", fn);
    builder.CreateCondBr(isObj, flagsBb, slowBb);

    builder.SetInsertPoint(flagsBb);
    llvm::Value* srcAddr =
        builder.CreateAnd(srcBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* srcHdr = builder.CreateIntToPtr(srcAddr, ptrTy, "io.srchdr");
    llvm::Value* srcFlagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, srcHdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* srcFlags =
        builder.CreateAlignedLoad(i16Ty, srcFlagsPtr, llvm::Align(2), "io.srcflags");
    llvm::Value* isArr =
        builder.CreateICmpEQ(srcFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
    llvm::Value* isMap =
        builder.CreateICmpEQ(srcFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_MAP));
    llvm::Value* canFastOpen = builder.CreateOr(isArr, isMap, "io.canfast");
    llvm::BasicBlock* allocBb = llvm::BasicBlock::Create(ctx, "io.alloc", fn);
    builder.CreateCondBr(canFastOpen, allocBb, slowBb);

    // The record, bump-allocated from the inline-allocation window exactly as
    // the inline `new` fast path allocates its instances (llvm_construct.cpp):
    // the window is from-space memory carved out for generated code, the
    // inline path only ever ADVANCES the cursor when the record fits, and a
    // window that is empty, invalidated, or disabled reads as headroom 0 and
    // falls to the helper, which can collect.
    builder.SetInsertPoint(allocBb);
    llvm::Value* cursor = builder.CreateAlignedLoad(i64Ty, globals.bronze_alloc_cursor,
                                                    llvm::Align(8), "io.cursor");
    llvm::Value* limit = builder.CreateAlignedLoad(i64Ty, globals.bronze_alloc_limit,
                                                   llvm::Align(8), "io.limit");
    llvm::Value* headroom = builder.CreateSub(limit, cursor, "io.headroom");
    llvm::Value* fits =
        builder.CreateICmpUGE(headroom, builder.getInt64(BRONZE_ABI_ITER_RECORD_BYTES), "io.fits");
    llvm::BasicBlock* buildBb = llvm::BasicBlock::Create(ctx, "io.build", fn);
    builder.CreateCondBr(fits, buildBb, slowBb);

    // The stores IterRecordHeader::create performs, spelled out: header word,
    // then the six Value fields. Pure pointer arithmetic — no call, no
    // collection — so the raw source bits stay valid across all of it.
    builder.SetInsertPoint(buildBb);
    builder.CreateAlignedStore(
        builder.CreateAdd(cursor, builder.getInt64(BRONZE_ABI_ITER_RECORD_BYTES)),
        globals.bronze_alloc_cursor, llvm::Align(8));
    llvm::Value* recPtr = builder.CreateIntToPtr(cursor, ptrTy, "io.rec");
    constexpr uint64_t kHeaderWord =
        static_cast<uint64_t>(BRONZE_ABI_TAG_OBJECT) |
        (static_cast<uint64_t>(BRONZE_ABI_OBJ_FLAGS_ITERATOR) << 16) |
        (static_cast<uint64_t>(BRONZE_ABI_ITER_RECORD_BYTES) << 32);
    builder.CreateAlignedStore(builder.getInt64(kHeaderWord), recPtr, llvm::Align(8));
    auto storeWord = [&](unsigned byteOffset, llvm::Value* word) {
        builder.CreateAlignedStore(
            word, builder.CreateConstInBoundsGEP1_32(i8Ty, recPtr, byteOffset), llvm::Align(8));
    };
    llvm::Value* undef = builder.getInt64(BRONZE_ABI_UNDEFINED_BITS);
    storeWord(BRONZE_ABI_ITER_TARGET_OFFSET, srcBits);
    storeWord(BRONZE_ABI_ITER_NEXTFN_OFFSET, undef);
    storeWord(BRONZE_ABI_ITER_CURRENT_OFFSET, undef);
    // cursor 0.0 and kind Array are BOTH all-zero bit patterns (a double's
    // Value is its IEEE bits; Kind::Array is 0).
    storeWord(BRONZE_ABI_ITER_CURSOR_OFFSET, builder.getInt64(0));
    llvm::Value* openKind = builder.CreateSelect(
        isArr, builder.getInt64(BRONZE_ABI_ITER_KIND_ARRAY_BITS),
        builder.getInt64(BRONZE_ABI_ITER_KIND_MAP_ENTRIES_BITS), "io.kind");
    storeWord(BRONZE_ABI_ITER_KIND_OFFSET, openKind);
    storeWord(BRONZE_ABI_ITER_DONE_OFFSET,
              builder.getInt64(static_cast<uint64_t>(BRONZE_ABI_TAG_BOOL)
                               << BRONZE_ABI_VALUE_TAG_SHIFT));
    llvm::Value* fastVal = builder.CreateOr(
        cursor,
        builder.getInt64(static_cast<uint64_t>(BRONZE_ABI_TAG_OBJECT)
                         << BRONZE_ABI_VALUE_TAG_SHIFT),
        "io.fastval");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(abi.bronze_iter_open, {srcBits}, "io.slowval");
    llvm::BasicBlock* slowEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "io.result");
    result->addIncoming(fastVal, fastEndBb);
    result->addIncoming(slowVal, slowEndBb);
    return result;
}

llvm::Value* emitIterStep(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* recBits) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);
    llvm::MDNode* unlikelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1, 1048576);

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "is.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "is.done", fn);

    llvm::BasicBlock* seamBb = llvm::BasicBlock::Create(ctx, "is.seam", fn);
    builder.CreateCondBr(emitIterFastEnabled(builder, abi), seamBb, slowBb);
    builder.SetInsertPoint(seamBb);

    llvm::Value* rec = emitRecordPtr(builder, recBits, slowBb, "is.");

    // The open's answer, and the whole basis of this path: kind == Array or MapIterator.
    llvm::Value* kindPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_KIND_OFFSET);
    llvm::Value* kind = builder.CreateAlignedLoad(i64Ty, kindPtr, llvm::Align(8), "is.kind");
    llvm::BasicBlock* liveBb = llvm::BasicBlock::Create(ctx, "is.live", fn);
    llvm::BasicBlock* chkMapBb = llvm::BasicBlock::Create(ctx, "is.chkmap", fn);
    llvm::BasicBlock* mapIterLiveBb = llvm::BasicBlock::Create(ctx, "is.mapiter", fn);

    builder.CreateCondBr(
        builder.CreateICmpEQ(kind, builder.getInt64(BRONZE_ABI_ITER_KIND_ARRAY_BITS)), liveBb,
        chkMapBb);

    builder.SetInsertPoint(chkMapBb);
    builder.CreateCondBr(
        builder.CreateICmpEQ(kind, builder.getInt64(BRONZE_ABI_ITER_KIND_MAP_ITERATOR_BITS)),
        mapIterLiveBb, slowBb);

    // A record already marked done answers false — through the helper, which is
    // where that answer is written down. It happens once per loop.
    builder.SetInsertPoint(liveBb);
    llvm::Value* donePtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_DONE_OFFSET);
    llvm::Value* doneWord = builder.CreateAlignedLoad(i64Ty, donePtr, llvm::Align(8), "is.done.w");
    llvm::Value* falseBits =
        builder.getInt64((static_cast<uint64_t>(BRONZE_ABI_TAG_BOOL) << BRONZE_ABI_VALUE_TAG_SHIFT));
    llvm::BasicBlock* tgtBb = llvm::BasicBlock::Create(ctx, "is.tgt", fn);
    builder.CreateCondBr(builder.CreateICmpEQ(doneWord, falseBits), tgtBb, slowBb, likelyBranch);

    builder.SetInsertPoint(tgtBb);
    llvm::Value* targetPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_TARGET_OFFSET);
    llvm::Value* target = builder.CreateAlignedLoad(i64Ty, targetPtr, llvm::Align(8), "is.target");
    llvm::Value* targetAddr =
        builder.CreateAnd(target, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* arr = builder.CreateIntToPtr(targetAddr, ptrTy, "is.arr");
    llvm::Value* arrFlagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, arr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* arrFlags =
        builder.CreateAlignedLoad(i16Ty, arrFlagsPtr, llvm::Align(2), "is.arrflags");
    llvm::BasicBlock* boundsBb = llvm::BasicBlock::Create(ctx, "is.bounds", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(arrFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY)), boundsBb,
        slowBb, likelyBranch);

    // `i >= length` is the walk's END, handled INLINE: the whole of the
    // helper's end-of-walk bookkeeping for the Array kind is two stores —
    // done := true, current := undefined (iterator.cpp's stepFast foot) — and
    // paying a helper call for them billed one call per loop, which on
    // three.js's for-in-per-mesh frames was 1.8M calls a run (the largest
    // single iter helper bucket). A NaN cursor fails both compares and still
    // reaches the helper, so nothing unproven is answered here.
    builder.SetInsertPoint(boundsBb);
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, arr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    llvm::Value* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "is.len");
    llvm::Value* cursorPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_CURSOR_OFFSET);
    llvm::Value* cursorBits =
        builder.CreateAlignedLoad(i64Ty, cursorPtr, llvm::Align(8), "is.cursorbits");
    llvm::Value* cursor = builder.CreateBitCast(cursorBits, dblTy, "is.cursor");
    llvm::Value* lenDbl = builder.CreateUIToFP(len, dblTy, "is.lendbl");
    llvm::Value* nonneg =
        builder.CreateFCmpOGE(cursor, llvm::ConstantFP::get(dblTy, 0.0), "is.nonneg");
    llvm::Value* under = builder.CreateFCmpOLT(cursor, lenDbl, "is.under");
    llvm::Value* inRange = builder.CreateAnd(nonneg, under, "is.inrange");
    llvm::BasicBlock* readBb = llvm::BasicBlock::Create(ctx, "is.read", fn);
    llvm::BasicBlock* endSplitBb = llvm::BasicBlock::Create(ctx, "is.end.split", fn);
    llvm::BasicBlock* endBb = llvm::BasicBlock::Create(ctx, "is.end", fn);
    // Selected to a value that certainly converts BEFORE the conversion, not
    // after: `fptoui` of anything outside the destination range is poison, and
    // a poison value the branch was supposed to have excluded is the shape of
    // miscompile that survives every test on one optimiser and not the next.
    llvm::Value* safeCursor =
        builder.CreateSelect(inRange, cursor, llvm::ConstantFP::get(dblTy, 0.0), "is.safecursor");
    builder.CreateCondBr(inRange, readBb, endSplitBb, likelyBranch);

    // Out of range: a non-negative cursor at or past the length is the END
    // (the only way the loop's own arithmetic gets here); anything else —
    // NaN, negative — is not this path's to answer.
    builder.SetInsertPoint(endSplitBb);
    builder.CreateCondBr(nonneg, endBb, slowBb);

    builder.SetInsertPoint(endBb);
    llvm::Value* trueBits = builder.getInt64(
        (static_cast<uint64_t>(BRONZE_ABI_TAG_BOOL) << BRONZE_ABI_VALUE_TAG_SHIFT) | 1u);
    builder.CreateAlignedStore(trueBits, donePtr, llvm::Align(8));
    llvm::Value* endCurrentPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_CURRENT_OFFSET);
    builder.CreateAlignedStore(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), endCurrentPtr,
                               llvm::Align(8));
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(readBb);
    llvm::Value* idx32 = builder.CreateFPToUI(safeCursor, i32Ty, "is.idx");
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, arr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    llvm::Value* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "is.elems");
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::BasicBlock* loadBb = llvm::BasicBlock::Create(ctx, "is.load", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT)), loadBb, slowBb,
        likelyBranch);

    builder.SetInsertPoint(loadBb);
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* headPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, arr, BRONZE_ABI_ARRAY_HEAD_OFFSET);
    llvm::Value* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), "is.head");
    // +1: the elements block's payload begins one i64 past its header.
    llvm::Value* slotIdx = builder.CreateAdd(
        builder.CreateZExt(builder.CreateAdd(idx32, head), i64Ty), builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    llvm::Value* raw = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "is.raw");
    llvm::Value* rawTag = builder.CreateLShr(raw, BRONZE_ABI_VALUE_TAG_SHIFT);
    // 23.1.5.1 reads with Get, so a hole is `undefined` and not a skip.
    llvm::Value* elem = builder.CreateSelect(
        builder.CreateICmpEQ(rawTag, builder.getInt64(BRONZE_ABI_TAG_HOLE)),
        builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), raw, "is.elem");

    llvm::Value* currentPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_CURRENT_OFFSET);
    builder.CreateAlignedStore(elem, currentPtr, llvm::Align(8));
    llvm::Value* next = builder.CreateFAdd(safeCursor, llvm::ConstantFP::get(dblTy, 1.0));
    builder.CreateAlignedStore(builder.CreateBitCast(next, i64Ty), cursorPtr, llvm::Align(8));
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // MapIterator inline step fast path:
    builder.SetInsertPoint(mapIterLiveBb);
    llvm::Value* mapDonePtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_DONE_OFFSET);
    llvm::Value* mapDoneWord =
        builder.CreateAlignedLoad(i64Ty, mapDonePtr, llvm::Align(8), "is.mdone.w");
    llvm::BasicBlock* mapTgtBb = llvm::BasicBlock::Create(ctx, "is.maptgt", fn);
    builder.CreateCondBr(builder.CreateICmpEQ(mapDoneWord, falseBits), mapTgtBb, slowBb,
                         likelyBranch);

    builder.SetInsertPoint(mapTgtBb);
    llvm::Value* mapTargetPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_TARGET_OFFSET);
    llvm::Value* mapTarget =
        builder.CreateAlignedLoad(i64Ty, mapTargetPtr, llvm::Align(8), "is.maptarget");
    llvm::Value* mapTargetTag = builder.CreateLShr(mapTarget, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isMapTargetObj =
        builder.CreateICmpEQ(mapTargetTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::Value* mapTargetAddr =
        builder.CreateAnd(mapTarget, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* itHdr = builder.CreateIntToPtr(mapTargetAddr, ptrTy, "is.ithdr");
    llvm::Value* itFlagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, itHdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* itFlags =
        builder.CreateAlignedLoad(i16Ty, itFlagsPtr, llvm::Align(2), "is.itflags");
    llvm::Value* isItPlain =
        builder.CreateICmpEQ(itFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN));
    llvm::Value* mapTargetOk = builder.CreateAnd(isMapTargetObj, isItPlain);
    llvm::BasicBlock* itMapBb = llvm::BasicBlock::Create(ctx, "is.itmap", fn);
    builder.CreateCondBr(mapTargetOk, itMapBb, slowBb, likelyBranch);

    builder.SetInsertPoint(itMapBb);
    llvm::Value* mapSlotPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, itHdr, BRONZE_ABI_MAP_ITER_SLOT_MAP_OFFSET);
    llvm::Value* iteratedMap =
        builder.CreateAlignedLoad(i64Ty, mapSlotPtr, llvm::Align(8), "is.iteratedmap");
    llvm::Value* mapTag = builder.CreateLShr(iteratedMap, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isMapObj = builder.CreateICmpEQ(mapTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::Value* mapAddr =
        builder.CreateAnd(iteratedMap, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* mapHdr = builder.CreateIntToPtr(mapAddr, ptrTy, "is.maphdr");
    llvm::Value* mapFlagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, mapHdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* mapFlags =
        builder.CreateAlignedLoad(i16Ty, mapFlagsPtr, llvm::Align(2), "is.mapflags");
    llvm::Value* isMap =
        builder.CreateICmpEQ(mapFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_MAP));
    llvm::Value* isSet =
        builder.CreateICmpEQ(mapFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_SET));
    llvm::Value* mapOk = builder.CreateAnd(isMapObj, builder.CreateOr(isMap, isSet));
    llvm::BasicBlock* kindChkBb = llvm::BasicBlock::Create(ctx, "is.kindchk", fn);
    builder.CreateCondBr(mapOk, kindChkBb, slowBb, likelyBranch);

    builder.SetInsertPoint(kindChkBb);
    llvm::Value* iterKindPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, itHdr, BRONZE_ABI_MAP_ITER_SLOT_KIND_OFFSET);
    llvm::Value* iterKindVal =
        builder.CreateAlignedLoad(i64Ty, iterKindPtr, llvm::Align(8), "is.iterkind");
    llvm::Value* isKeys =
        builder.CreateICmpEQ(iterKindVal, builder.getInt64(BRONZE_ABI_MAP_ITER_KIND_KEYS_BITS));
    llvm::Value* isValues =
        builder.CreateICmpEQ(iterKindVal, builder.getInt64(BRONZE_ABI_MAP_ITER_KIND_VALUES_BITS));
    llvm::Value* isKeysOrValues = builder.CreateOr(isKeys, isValues);
    llvm::BasicBlock* idxBb = llvm::BasicBlock::Create(ctx, "is.mapidx", fn);
    builder.CreateCondBr(isKeysOrValues, idxBb, slowBb, likelyBranch);

    builder.SetInsertPoint(idxBb);
    llvm::Value* nextIndexPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, itHdr, BRONZE_ABI_MAP_ITER_SLOT_NEXT_OFFSET);
    llvm::Value* nextIndexVal =
        builder.CreateAlignedLoad(i64Ty, nextIndexPtr, llvm::Align(8), "is.nextindex");
    llvm::Value* nextIndexDbl = builder.CreateBitCast(nextIndexVal, dblTy, "is.nextindex.dbl");
    llvm::Value* nonnegNext =
        builder.CreateFCmpOGE(nextIndexDbl, llvm::ConstantFP::get(dblTy, 0.0), "is.next.nonneg");
    llvm::BasicBlock* usedBb = llvm::BasicBlock::Create(ctx, "is.usedchk", fn);
    builder.CreateCondBr(nonnegNext, usedBb, slowBb, likelyBranch);

    builder.SetInsertPoint(usedBb);
    llvm::Value* atIdx = builder.CreateFPToUI(nextIndexDbl, i32Ty, "is.at");
    llvm::Value* usedPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, mapHdr, BRONZE_ABI_MAP_HEADER_USED_OFFSET);
    llvm::Value* usedVal =
        builder.CreateAlignedLoad(i64Ty, usedPtr, llvm::Align(8), "is.usedval");
    llvm::Value* usedDbl = builder.CreateBitCast(usedVal, dblTy, "is.used.dbl");
    llvm::Value* usedIdx = builder.CreateFPToUI(usedDbl, i32Ty, "is.used");
    llvm::Value* atUnderUsed = builder.CreateICmpULT(atIdx, usedIdx, "is.at.under");
    llvm::BasicBlock* mapReadBb = llvm::BasicBlock::Create(ctx, "is.mapread", fn);
    llvm::BasicBlock* mapEndBb = llvm::BasicBlock::Create(ctx, "is.mapend", fn);
    builder.CreateCondBr(atUnderUsed, mapReadBb, mapEndBb, likelyBranch);

    builder.SetInsertPoint(mapEndBb);
    builder.CreateAlignedStore(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), mapSlotPtr,
                               llvm::Align(8));
    llvm::Value* mapEndTrueBits = builder.getInt64(
        (static_cast<uint64_t>(BRONZE_ABI_TAG_BOOL) << BRONZE_ABI_VALUE_TAG_SHIFT) | 1u);
    builder.CreateAlignedStore(mapEndTrueBits, mapDonePtr, llvm::Align(8));
    llvm::Value* mapEndCurPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_CURRENT_OFFSET);
    builder.CreateAlignedStore(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), mapEndCurPtr,
                               llvm::Align(8));
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(mapReadBb);
    llvm::Value* entriesPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, mapHdr, BRONZE_ABI_MAP_HEADER_ENTRIES_OFFSET);
    llvm::Value* entriesVal =
        builder.CreateAlignedLoad(i64Ty, entriesPtr, llvm::Align(8), "is.entries");
    llvm::Value* entriesTag = builder.CreateLShr(entriesVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isEntriesObj =
        builder.CreateICmpEQ(entriesTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::BasicBlock* entriesOkBb = llvm::BasicBlock::Create(ctx, "is.entriesok", fn);
    builder.CreateCondBr(isEntriesObj, entriesOkBb, slowBb, likelyBranch);

    builder.SetInsertPoint(entriesOkBb);
    llvm::Value* entriesAddr =
        builder.CreateAnd(entriesVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* entriesHdr = builder.CreateIntToPtr(entriesAddr, ptrTy, "is.entrieshdr");

    llvm::Value* at64 = builder.CreateZExt(atIdx, i64Ty);
    llvm::Value* slotOffset =
        builder.CreateAdd(builder.getInt64(8), builder.CreateMul(at64, builder.getInt64(16)));
    llvm::Value* keySlotPtr = builder.CreateInBoundsGEP(i8Ty, entriesHdr, slotOffset);
    llvm::Value* keyVal =
        builder.CreateAlignedLoad(i64Ty, keySlotPtr, llvm::Align(8), "is.keyval");

    llvm::Value* keyTag = builder.CreateLShr(keyVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isHole =
        builder.CreateICmpEQ(keyTag, builder.getInt64(BRONZE_ABI_TAG_HOLE));
    llvm::BasicBlock* notHoleBb = llvm::BasicBlock::Create(ctx, "is.nothole", fn);
    builder.CreateCondBr(isHole, slowBb, notHoleBb, unlikelyBranch);

    builder.SetInsertPoint(notHoleBb);
    llvm::Value* valSlotPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, keySlotPtr, 8);
    llvm::Value* valVal =
        builder.CreateAlignedLoad(i64Ty, valSlotPtr, llvm::Align(8), "is.valval");
    llvm::Value* valForKind = builder.CreateSelect(isSet, keyVal, valVal);
    llvm::Value* mapElem = builder.CreateSelect(isKeys, keyVal, valForKind, "is.mapelem");

    llvm::Value* curPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_CURRENT_OFFSET);
    builder.CreateAlignedStore(mapElem, curPtr, llvm::Align(8));

    llvm::Value* nextAt = builder.CreateAdd(atIdx, builder.getInt32(1));
    llvm::Value* nextAtDbl = builder.CreateUIToFP(nextAt, dblTy, "is.nextat.dbl");
    llvm::Value* nextAtBits = builder.CreateBitCast(nextAtDbl, i64Ty);
    builder.CreateAlignedStore(nextAtBits, nextIndexPtr, llvm::Align(8));

    llvm::BasicBlock* mapFastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(abi.bronze_iter_step, {recBits}, "is.slowval");
    llvm::BasicBlock* slowEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(builder.getInt1Ty(), 5, "is.result");
    result->addIncoming(builder.getTrue(), fastEndBb);
    result->addIncoming(slowVal, slowEndBb);
    result->addIncoming(builder.getFalse(), endBb);
    result->addIncoming(builder.getTrue(), mapFastEndBb);
    result->addIncoming(builder.getFalse(), mapEndBb);
    return result;
}

void emitIterClose(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* recBits,
                   bool suppress) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "ic.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "ic.done", fn);

    llvm::BasicBlock* seamBb = llvm::BasicBlock::Create(ctx, "ic.seam", fn);
    builder.CreateCondBr(emitIterFastEnabled(builder, abi), seamBb, slowBb);
    builder.SetInsertPoint(seamBb);

    // The open's classification, once more. A kind below `Protocol` owns its
    // cursor and has no iterator object to hand a `return` — the helper's
    // first line is `if (kind < Protocol) return;` — so the record is simply
    // left behind. Kinds are small non-negative doubles, whose IEEE bits
    // order exactly as the unsigned integers they are, so "< 5.0" is one
    // unsigned compare against the 5.0 bit pattern.
    llvm::Value* rec = emitRecordPtr(builder, recBits, slowBb, "ic.");
    llvm::Value* kindPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_KIND_OFFSET);
    llvm::Value* kind = builder.CreateAlignedLoad(i64Ty, kindPtr, llvm::Align(8), "ic.kind");
    builder.CreateCondBr(
        builder.CreateICmpULT(kind, builder.getInt64(BRONZE_ABI_ITER_KIND_OWNED_LIMIT_BITS),
                              "ic.owned"),
        doneBb, slowBb);

    builder.SetInsertPoint(slowBb);
    builder.CreateCall(abi.bronze_iter_close, {recBits, builder.getInt1(suppress)});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
}

llvm::Value* emitIterValue(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* recBits) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "iv.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "iv.done", fn);

    llvm::BasicBlock* seamBb = llvm::BasicBlock::Create(ctx, "iv.seam", fn);
    builder.CreateCondBr(emitIterFastEnabled(builder, abi), seamBb, slowBb);
    builder.SetInsertPoint(seamBb);

    // Every kind keeps its element in `current`, so unlike the step this needs
    // no kind check: the record IS the answer, whoever wrote it.
    llvm::Value* rec = emitRecordPtr(builder, recBits, slowBb, "iv.");
    llvm::Value* currentPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_CURRENT_OFFSET);
    llvm::Value* current =
        builder.CreateAlignedLoad(i64Ty, currentPtr, llvm::Align(8), "iv.current");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(abi.bronze_iter_value, {recBits}, "iv.slowval");
    llvm::BasicBlock* slowEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "iv.result");
    result->addIncoming(current, fastEndBb);
    result->addIncoming(slowVal, slowEndBb);
    return result;
}

}  // namespace bronze::codegen_llvm
