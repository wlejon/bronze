#include "codegen-llvm/llvm_iter.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>

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

llvm::Value* emitIterStep(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* recBits) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "is.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "is.done", fn);

    llvm::BasicBlock* seamBb = llvm::BasicBlock::Create(ctx, "is.seam", fn);
    builder.CreateCondBr(emitIterFastEnabled(builder, abi), seamBb, slowBb);
    builder.SetInsertPoint(seamBb);

    llvm::Value* rec = emitRecordPtr(builder, recBits, slowBb, "is.");

    // The open's answer, and the whole basis of this path: kind == Array. A
    // double's Value is its IEEE bits and `Kind::Array` is 0.0, so this is a
    // compare against zero.
    llvm::Value* kindPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_KIND_OFFSET);
    llvm::Value* kind = builder.CreateAlignedLoad(i64Ty, kindPtr, llvm::Align(8), "is.kind");
    llvm::BasicBlock* liveBb = llvm::BasicBlock::Create(ctx, "is.live", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(kind, builder.getInt64(BRONZE_ABI_ITER_KIND_ARRAY_BITS)), liveBb,
        slowBb);

    // A record already marked done answers false — through the helper, which is
    // where that answer is written down. It happens once per loop.
    builder.SetInsertPoint(liveBb);
    llvm::Value* donePtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, rec, BRONZE_ABI_ITER_DONE_OFFSET);
    llvm::Value* doneWord = builder.CreateAlignedLoad(i64Ty, donePtr, llvm::Align(8), "is.done.w");
    llvm::Value* falseBits =
        builder.getInt64((static_cast<uint64_t>(BRONZE_ABI_TAG_BOOL) << BRONZE_ABI_VALUE_TAG_SHIFT));
    llvm::BasicBlock* tgtBb = llvm::BasicBlock::Create(ctx, "is.tgt", fn);
    builder.CreateCondBr(builder.CreateICmpEQ(doneWord, falseBits), tgtBb, slowBb);

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
        slowBb);

    // `i >= length` is the walk's END, and it goes to the helper: the helper is
    // what marks the record done and clears `current`, and one call per loop is
    // not worth a second copy of that bookkeeping here.
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
    llvm::Value* inRange = builder.CreateAnd(
        builder.CreateFCmpOGE(cursor, llvm::ConstantFP::get(dblTy, 0.0), "is.nonneg"),
        builder.CreateFCmpOLT(cursor, lenDbl, "is.under"), "is.inrange");
    llvm::BasicBlock* readBb = llvm::BasicBlock::Create(ctx, "is.read", fn);
    // Selected to a value that certainly converts BEFORE the conversion, not
    // after: `fptoui` of anything outside the destination range is poison, and
    // a poison value the branch was supposed to have excluded is the shape of
    // miscompile that survives every test on one optimiser and not the next.
    llvm::Value* safeCursor =
        builder.CreateSelect(inRange, cursor, llvm::ConstantFP::get(dblTy, 0.0), "is.safecursor");
    builder.CreateCondBr(inRange, readBb, slowBb);

    builder.SetInsertPoint(readBb);
    llvm::Value* idx32 = builder.CreateFPToUI(safeCursor, i32Ty, "is.idx");
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, arr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    llvm::Value* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "is.elems");
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::BasicBlock* loadBb = llvm::BasicBlock::Create(ctx, "is.load", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT)), loadBb, slowBb);

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

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(abi.bronze_iter_step, {recBits}, "is.slowval");
    llvm::BasicBlock* slowEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(builder.getInt1Ty(), 2, "is.result");
    result->addIncoming(builder.getTrue(), fastEndBb);
    result->addIncoming(slowVal, slowEndBb);
    return result;
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
