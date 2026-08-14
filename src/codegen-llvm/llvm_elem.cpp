#include "codegen-llvm/llvm_elem.h"

#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

namespace {

// The shared front half of both access forms: is the receiver an object, and
// is the index a non-negative integral number small enough to be an element
// index? Mirrors the guard ladder at the top of bronze_elem_get /
// bronze_elem_set — the fcmp range check comes BEFORE the fptoui because an
// out-of-range fptoui is poison, where the helper's C++ cast is merely wrong.
//
// On success the builder is left in a fresh block with the receiver's header
// pointer, its flags word, and the index as i32 in hand; every failure edge
// branches to `slowBb`.
struct ElemGuards {
    llvm::Value* hdr;
    llvm::Value* flags;
    llvm::Value* idx32;
};

ElemGuards emitElemGuards(llvm::IRBuilder<>& builder, llvm::Value* objBits, llvm::Value* idxBits,
                          llvm::BasicBlock* slowBb, const char* prefix) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    auto guard = [&](llvm::Value* cond, const char* name) {
        llvm::BasicBlock* cont = llvm::BasicBlock::Create(ctx, std::string(prefix) + name, fn);
        builder.CreateCondBr(cond, cont, slowBb);
        builder.SetInsertPoint(cont);
    };

    llvm::Value* tag = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObject = builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::Value* idxIsNum =
        builder.CreateICmpULE(idxBits, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
    guard(builder.CreateAnd(isObject, idxIsNum), "objnum");

    llvm::Value* d = builder.CreateBitCast(idxBits, dblTy);
    llvm::Value* geZero = builder.CreateFCmpOGE(d, llvm::ConstantFP::get(dblTy, 0.0));
    llvm::Value* leMax =
        builder.CreateFCmpOLE(d, llvm::ConstantFP::get(dblTy, 4294967294.0));
    guard(builder.CreateAnd(geZero, leMax), "range");

    llvm::Value* idx32 = builder.CreateFPToUI(d, builder.getInt32Ty(), "elem.idx");
    llvm::Value* roundTrip = builder.CreateUIToFP(idx32, dblTy);
    guard(builder.CreateFCmpOEQ(roundTrip, d), "integral");

    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "elem.hdr");
    llvm::Value* flagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    llvm::Value* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "elem.flags");

    return {hdr, flags, idx32};
}

// The address of element `idx32` of a float typed-array view, computed from
// the view's buffer Value on every access — never cached, per the GC rule the
// header documents. The builder must already be in the view's arm.
llvm::Value* emitTypedArrayElemPtr(llvm::IRBuilder<>& builder, llvm::Value* hdr,
                                   llvm::Value* idx32, uint32_t elemSize) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::Value* bufPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_BUFFER_OFFSET);
    llvm::Value* bufVal = builder.CreateAlignedLoad(i64Ty, bufPtr, llvm::Align(8), "ta.buf");
    llvm::Value* bufAddr =
        builder.CreateAnd(bufVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* bufHdr = builder.CreateIntToPtr(bufAddr, ptrTy);
    llvm::Value* byteOffPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_BYTEOFFSET_OFFSET);
    llvm::Value* byteOff =
        builder.CreateAlignedLoad(i32Ty, byteOffPtr, llvm::Align(4), "ta.byteoff");

    llvm::Value* dataBase =
        builder.CreateConstInBoundsGEP1_32(i8Ty, bufHdr, BRONZE_ABI_BUF_DATA_OFFSET);
    llvm::Value* viewBase =
        builder.CreateInBoundsGEP(i8Ty, dataBase, builder.CreateZExt(byteOff, i64Ty));
    llvm::Value* byteIdx = builder.CreateMul(builder.CreateZExt(idx32, i64Ty),
                                             builder.getInt64(elemSize), "ta.byteidx");
    return builder.CreateInBoundsGEP(i8Ty, viewBase, byteIdx, "ta.elem.ptr");
}

// A double as a NaN-boxed Value: the same NaN-canonicalizing select the Box
// instruction emits, because a Float64Array can hold any NaN bit pattern and
// a non-canonical one would read back as a tagged pointer.
llvm::Value* emitBoxDouble(llvm::IRBuilder<>& builder, llvm::Value* d) {
    llvm::Value* isNan = builder.CreateFCmpUNO(d, d);
    llvm::Value* bits = builder.CreateBitCast(d, builder.getInt64Ty());
    return builder.CreateSelect(isNan, builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS), bits);
}

}  // namespace

llvm::Value* emitElemGet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                         llvm::Value* idxBits) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "eg.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "eg.done", fn);

    ElemGuards g = emitElemGuards(builder, objBits, idxBits, slowBb, "eg.");

    llvm::BasicBlock* arrBb = llvm::BasicBlock::Create(ctx, "eg.arr", fn);
    llvm::BasicBlock* taBb = llvm::BasicBlock::Create(ctx, "eg.ta", fn);
    llvm::Value* isArr =
        builder.CreateICmpEQ(g.flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
    llvm::Value* isTa =
        builder.CreateICmpEQ(g.flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY));
    llvm::BasicBlock* notArrBb = llvm::BasicBlock::Create(ctx, "eg.notarr", fn);
    builder.CreateCondBr(isArr, arrBb, notArrBb);
    builder.SetInsertPoint(notArrBb);
    builder.CreateCondBr(isTa, taBb, slowBb);

    // Array: in bounds, elements block present, hole answers undefined. An
    // out-of-bounds read goes to the helper, whose own fast path answers the
    // undefined — rare enough that the extra call is not worth a fourth arm.
    builder.SetInsertPoint(arrBb);
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    llvm::Value* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "eg.len");
    llvm::BasicBlock* arrLoadBb = llvm::BasicBlock::Create(ctx, "eg.arr.load", fn);
    builder.CreateCondBr(builder.CreateICmpULT(g.idx32, len), arrLoadBb, slowBb);

    builder.SetInsertPoint(arrLoadBb);
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    llvm::Value* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "eg.elems");
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::BasicBlock* arrReadBb = llvm::BasicBlock::Create(ctx, "eg.arr.read", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT)), arrReadBb,
        slowBb);

    builder.SetInsertPoint(arrReadBb);
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    // +1: the elements block's payload begins one i64 past its header.
    llvm::Value* slotIdx = builder.CreateAdd(builder.CreateZExt(g.idx32, i64Ty),
                                             builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    llvm::Value* raw = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "eg.raw");
    llvm::Value* rawTag = builder.CreateLShr(raw, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isHole = builder.CreateICmpEQ(rawTag, builder.getInt64(BRONZE_ABI_TAG_HOLE));
    llvm::Value* arrVal =
        builder.CreateSelect(isHole, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), raw, "eg.arrval");
    llvm::BasicBlock* arrEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // Typed array: the two float kinds, in bounds. Out of bounds answers
    // undefined — via the helper, same trade as the Array arm.
    builder.SetInsertPoint(taBb);
    llvm::Value* taLenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
    llvm::Value* taLen = builder.CreateAlignedLoad(i32Ty, taLenPtr, llvm::Align(4), "eg.talen");
    llvm::BasicBlock* taKindBb = llvm::BasicBlock::Create(ctx, "eg.ta.kind", fn);
    builder.CreateCondBr(builder.CreateICmpULT(g.idx32, taLen), taKindBb, slowBb);

    builder.SetInsertPoint(taKindBb);
    llvm::Value* kindPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_KIND_OFFSET);
    llvm::Value* kind = builder.CreateAlignedLoad(i32Ty, kindPtr, llvm::Align(4), "eg.kind");
    llvm::BasicBlock* f64Bb = llvm::BasicBlock::Create(ctx, "eg.f64", fn);
    llvm::BasicBlock* notF64Bb = llvm::BasicBlock::Create(ctx, "eg.notf64", fn);
    llvm::BasicBlock* f32Bb = llvm::BasicBlock::Create(ctx, "eg.f32", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(kind, builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT64)), f64Bb, notF64Bb);
    builder.SetInsertPoint(notF64Bb);
    builder.CreateCondBr(
        builder.CreateICmpEQ(kind, builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT32)), f32Bb, slowBb);

    builder.SetInsertPoint(f64Bb);
    llvm::Value* p64 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 8);
    llvm::Value* d64 = builder.CreateAlignedLoad(dblTy, p64, llvm::Align(8), "eg.d64");
    llvm::Value* f64Val = emitBoxDouble(builder, d64);
    llvm::BasicBlock* f64EndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(f32Bb);
    llvm::Value* p32 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
    llvm::Value* d32 = builder.CreateAlignedLoad(f32Ty, p32, llvm::Align(4), "eg.d32");
    llvm::Value* f32Val = emitBoxDouble(builder, builder.CreateFPExt(d32, dblTy));
    llvm::BasicBlock* f32EndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(abi.bronze_elem_get, {objBits, idxBits});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 4, "eg.result");
    result->addIncoming(arrVal, arrEndBb);
    result->addIncoming(f64Val, f64EndBb);
    result->addIncoming(f32Val, f32EndBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

void emitElemSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                 llvm::Value* idxBits, llvm::Value* valBits, bool strict) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "es.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "es.done", fn);

    ElemGuards g = emitElemGuards(builder, objBits, idxBits, slowBb, "es.");

    llvm::BasicBlock* arrBb = llvm::BasicBlock::Create(ctx, "es.arr", fn);
    llvm::BasicBlock* taBb = llvm::BasicBlock::Create(ctx, "es.ta", fn);
    llvm::Value* isArr =
        builder.CreateICmpEQ(g.flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
    llvm::Value* isTa =
        builder.CreateICmpEQ(g.flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY));
    llvm::BasicBlock* notArrBb = llvm::BasicBlock::Create(ctx, "es.notarr", fn);
    builder.CreateCondBr(isArr, arrBb, notArrBb);
    builder.SetInsertPoint(notArrBb);
    builder.CreateCondBr(isTa, taBb, slowBb);

    // Array: in bounds, within capacity, and no named-properties side object —
    // the same three conditions the helper's fast path requires before it will
    // store without consulting the property machinery.
    builder.SetInsertPoint(arrBb);
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    llvm::Value* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "es.len");
    llvm::Value* capPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
    llvm::Value* cap = builder.CreateAlignedLoad(i32Ty, capPtr, llvm::Align(4), "es.cap");
    llvm::Value* propsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
    llvm::Value* propsVal =
        builder.CreateAlignedLoad(i64Ty, propsPtr, llvm::Align(8), "es.props");
    llvm::Value* propsTag = builder.CreateLShr(propsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* noProps =
        builder.CreateICmpNE(propsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::Value* inBounds = builder.CreateAnd(builder.CreateICmpULT(g.idx32, len),
                                              builder.CreateICmpULT(g.idx32, cap));
    llvm::BasicBlock* arrElemsBb = llvm::BasicBlock::Create(ctx, "es.arr.elems", fn);
    builder.CreateCondBr(builder.CreateAnd(inBounds, noProps), arrElemsBb, slowBb);

    builder.SetInsertPoint(arrElemsBb);
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    llvm::Value* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "es.elems");
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::BasicBlock* arrStoreBb = llvm::BasicBlock::Create(ctx, "es.arr.store", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT)), arrStoreBb,
        slowBb);

    builder.SetInsertPoint(arrStoreBb);
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* slotIdx = builder.CreateAdd(builder.CreateZExt(g.idx32, i64Ty),
                                             builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    builder.CreateAlignedStore(valBits, slotPtr, llvm::Align(8));
    builder.CreateBr(doneBb);

    // Typed array: a numeric value into a float view. An in-range index
    // stores; an out-of-range one DISCARDS the write, exactly as the helper
    // and 10.4.5.16 do — so that edge completes inline rather than calling.
    builder.SetInsertPoint(taBb);
    llvm::Value* valIsNum =
        builder.CreateICmpULE(valBits, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
    llvm::BasicBlock* taKindBb = llvm::BasicBlock::Create(ctx, "es.ta.kind", fn);
    builder.CreateCondBr(valIsNum, taKindBb, slowBb);

    builder.SetInsertPoint(taKindBb);
    llvm::Value* kindPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_KIND_OFFSET);
    llvm::Value* kind = builder.CreateAlignedLoad(i32Ty, kindPtr, llvm::Align(4), "es.kind");
    llvm::BasicBlock* f64Bb = llvm::BasicBlock::Create(ctx, "es.f64", fn);
    llvm::BasicBlock* notF64Bb = llvm::BasicBlock::Create(ctx, "es.notf64", fn);
    llvm::BasicBlock* f32Bb = llvm::BasicBlock::Create(ctx, "es.f32", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(kind, builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT64)), f64Bb, notF64Bb);
    builder.SetInsertPoint(notF64Bb);
    builder.CreateCondBr(
        builder.CreateICmpEQ(kind, builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT32)), f32Bb, slowBb);

    builder.SetInsertPoint(f64Bb);
    llvm::Value* taLenPtr64 =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
    llvm::Value* taLen64 = builder.CreateAlignedLoad(i32Ty, taLenPtr64, llvm::Align(4), "es.talen");
    llvm::BasicBlock* f64StoreBb = llvm::BasicBlock::Create(ctx, "es.f64.store", fn);
    builder.CreateCondBr(builder.CreateICmpULT(g.idx32, taLen64), f64StoreBb, doneBb);
    builder.SetInsertPoint(f64StoreBb);
    llvm::Value* p64 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 8);
    builder.CreateAlignedStore(builder.CreateBitCast(valBits, dblTy), p64, llvm::Align(8));
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(f32Bb);
    llvm::Value* taLenPtr32 =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
    llvm::Value* taLen32 = builder.CreateAlignedLoad(i32Ty, taLenPtr32, llvm::Align(4), "es.talen32");
    llvm::BasicBlock* f32StoreBb = llvm::BasicBlock::Create(ctx, "es.f32.store", fn);
    builder.CreateCondBr(builder.CreateICmpULT(g.idx32, taLen32), f32StoreBb, doneBb);
    builder.SetInsertPoint(f32StoreBb);
    llvm::Value* p32 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
    llvm::Value* narrowed =
        builder.CreateFPTrunc(builder.CreateBitCast(valBits, dblTy), f32Ty, "es.f32.val");
    builder.CreateAlignedStore(narrowed, p32, llvm::Align(4));
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    builder.CreateCall(abi.bronze_elem_set,
                       {objBits, idxBits, valBits, builder.getInt1(strict)});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
}

}  // namespace bronze::codegen_llvm
