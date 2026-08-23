#include "codegen-llvm/llvm_elem.h"
#include "codegen-llvm/llvm_alias.h"

#include <string>

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

// The shared front half of both access forms: is the receiver an object, and
// is the index a non-negative integral number small enough to be an element
// index? Mirrors the guard ladder at the top of bronze_elem_get /
// bronze_elem_set â€” the fcmp range check comes BEFORE the fptoui because an
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

    llvm::Value* tag = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObject = builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::Value* idxIsNum =
        builder.CreateICmpULE(idxBits, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
    llvm::Value* objAndNum = builder.CreateAnd(isObject, idxIsNum);

    llvm::Value* d = builder.CreateBitCast(idxBits, dblTy);
    llvm::Value* ge0 = builder.CreateFCmpOGE(d, llvm::ConstantFP::get(dblTy, 0.0));
    llvm::Value* ltMax = builder.CreateFCmpOLT(d, llvm::ConstantFP::get(dblTy, 4294967296.0));
    llvm::Value* inRange = builder.CreateAnd(ge0, ltMax);

    // The conversion is fed a value the range check has ALREADY accepted, and
    // the select is what makes that true rather than merely likely. `fptoui` of
    // anything outside the destination range is POISON â€” not a wrong number, a
    // value LLVM may assume never happens â€” and the poison flows through
    // `isIntegral` into the branch condition below, where a branch on poison is
    // undefined behaviour. For a key the optimizer can see is constant it
    // folded exactly that way: `o[false]` bit-casts to a NaN, and the guard
    // ladder collapsed into the ARRAY arm, which then read a plain object's
    // words as an elements block. Ordering the checks is not enough when both
    // live in one basic block; the operand has to be safe on every path.
    llvm::Value* inRangeD = builder.CreateSelect(inRange, d, llvm::ConstantFP::get(dblTy, 0.0));
    llvm::Value* idx32 = builder.CreateFPToUI(inRangeD, builder.getInt32Ty(), "elem.idx");
    llvm::Value* roundTrip = builder.CreateUIToFP(idx32, dblTy);
    llvm::Value* isIntegral = builder.CreateFCmpOEQ(roundTrip, d);

    llvm::Value* ok = builder.CreateAnd(objAndNum, builder.CreateAnd(inRange, isIntegral));
    llvm::BasicBlock* cont = llvm::BasicBlock::Create(ctx, std::string(prefix) + "ok", fn);
    builder.CreateCondBr(ok, cont, slowBb);
    builder.SetInsertPoint(cont);

    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "elem.hdr");
    llvm::Value* flagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    auto* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "elem.flags");
    markInvariant(flags, ctx);

    return {hdr, flags, idx32};
}

}  // namespace

// The address of element `idx32` of a typed-array view, computed from
// the view's buffer Value on every access â€” never cached across allocations,
// per the GC rule the header documents. The builder must already be in the
// view's arm.
llvm::Value* emitTypedArrayElemPtr(llvm::IRBuilder<>& builder, llvm::Value* hdr,
                                   llvm::Value* idx32, uint32_t elemSize) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::Value* bufPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_BUFFER_OFFSET);
    auto* bufVal = builder.CreateAlignedLoad(i64Ty, bufPtr, llvm::Align(8), "ta.buf");
    markInvariant(bufVal, ctx);
    llvm::Value* bufAddr =
        builder.CreateAnd(bufVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* bufHdr = builder.CreateIntToPtr(bufAddr, ptrTy);
    llvm::Value* byteOffPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_BYTEOFFSET_OFFSET);
    auto* byteOff =
        builder.CreateAlignedLoad(i32Ty, byteOffPtr, llvm::Align(4), "ta.byteoff");
    markInvariant(byteOff, ctx);

    // The buffer's external-storage word: zero for an ordinary buffer (bytes
    // inline past the header), else the address of a non-moving host store.
    // NOT an invariant load â€” externalizeArrayBuffer flips it once, inside a
    // host call â€” but element stores can never change it, so it carries the
    // view-length alias scope and hoists out of call-free element loops for
    // the same reason `length` does.
    llvm::Value* extPtrPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, bufHdr, BRONZE_ABI_BUF_EXTPTR_OFFSET);
    auto* extBits = builder.CreateAlignedLoad(i64Ty, extPtrPtr, llvm::Align(8), "ta.extbits");
    tagViewLengthAccess(extBits, ctx);
    llvm::Value* inlineBase = builder.CreateAdd(
        bufAddr, builder.getInt64(BRONZE_ABI_BUF_DATA_OFFSET), "ta.inlinebase");
    llvm::Value* isExt = builder.CreateICmpNE(extBits, builder.getInt64(0), "ta.isext");
    llvm::Value* base = builder.CreateSelect(isExt, extBits, inlineBase, "ta.base");

    llvm::Value* byteIdx = (elemSize == 8)
                               ? builder.CreateShl(idx32, builder.getInt32(3), "ta.byteidx")
                               : ((elemSize == 4) ? builder.CreateShl(idx32, builder.getInt32(2), "ta.byteidx")
                                                  : ((elemSize == 2) ? builder.CreateShl(idx32, builder.getInt32(1), "ta.byteidx")
                                                                     : idx32));
    llvm::Value* totalOffset =
        builder.CreateZExt(builder.CreateAdd(byteOff, byteIdx), i64Ty, "ta.totaloff");
    llvm::Value* basePtr = builder.CreateIntToPtr(base, ptrTy, "ta.base.ptr");
    return builder.CreateInBoundsGEP(i8Ty, basePtr, totalOffset, "ta.elem.ptr");
}

// A double as a NaN-boxed Value: the same NaN-canonicalizing select the Box
// instruction emits, because a Float64Array can hold any NaN bit pattern and
// a non-canonical one would read back as a tagged pointer.
llvm::Value* emitBoxDouble(llvm::IRBuilder<>& builder, llvm::Value* d) {
    llvm::Value* isNan = builder.CreateFCmpUNO(d, d);
    llvm::Value* bits = builder.CreateBitCast(d, builder.getInt64Ty());
    return builder.CreateSelect(isNan, builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS), bits);
}

llvm::Value* emitElemGet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                         llvm::Value* idxBits) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    // Two blocks where there used to be one. `cacheBb` is what every fast-path
    // refusal below branches to, and the computed-read cache's inline hit is
    // emitted there; `slowBb` — the helper — is now reached only through it.
    // So the array and typed-array arms are byte for byte what they were, and
    // the receiver kind they cannot answer for (PLAIN) stops costing a call.
    llvm::BasicBlock* cacheBb = llvm::BasicBlock::Create(ctx, "eg.cache", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "eg.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "eg.done", fn);

    ElemGuards g = emitElemGuards(builder, objBits, idxBits, cacheBb, "eg.");

    llvm::BasicBlock* arrBb = llvm::BasicBlock::Create(ctx, "eg.arr", fn);
    llvm::BasicBlock* taBb = llvm::BasicBlock::Create(ctx, "eg.ta", fn);
    llvm::SwitchInst* swFlags = builder.CreateSwitch(g.flags, cacheBb, 2);
    swFlags->addCase(builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY), arrBb);
    swFlags->addCase(builder.getInt16(BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY), taBb);

    // Array: in bounds, elements block present, hole answers undefined. An
    // out-of-bounds read goes to the helper, whose own fast path answers the
    // undefined â€” rare enough that the extra call is not worth a fourth arm.
    builder.SetInsertPoint(arrBb);
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "eg.len");
    tagArrayHeaderAccess(len, ctx);
    llvm::BasicBlock* arrLoadBb = llvm::BasicBlock::Create(ctx, "eg.arr.load", fn);
    builder.CreateCondBr(builder.CreateICmpULT(g.idx32, len), arrLoadBb, cacheBb);

    builder.SetInsertPoint(arrLoadBb);
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "eg.elems");
    tagArrayHeaderAccess(elemsVal, ctx);
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::BasicBlock* arrReadBb = llvm::BasicBlock::Create(ctx, "eg.arr.read", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT)), arrReadBb,
        cacheBb);

    builder.SetInsertPoint(arrReadBb);
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* headPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_HEAD_OFFSET);
    auto* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), "eg.head");
    tagArrayHeaderAccess(head, ctx);
    llvm::Value* actualIdx = builder.CreateAdd(g.idx32, head, "eg.actidx");
    // +1: the elements block's payload begins one i64 past its header.
    llvm::Value* slotIdx = builder.CreateAdd(builder.CreateZExt(actualIdx, i64Ty),
                                             builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    auto* raw = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "eg.raw");
    tagArrayElementsAccess(raw, ctx);
    llvm::Value* rawTag = builder.CreateLShr(raw, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isHole = builder.CreateICmpEQ(rawTag, builder.getInt64(BRONZE_ABI_TAG_HOLE));
    llvm::Value* arrVal =
        builder.CreateSelect(isHole, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), raw, "eg.arrval");
    llvm::BasicBlock* arrEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // Typed array: all element kinds, in bounds. Out of bounds answers
    // undefined directly without helper call.
    builder.SetInsertPoint(taBb);
    llvm::Value* taLenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
    // The length word is MAINTAINED, not fixed: `transfer` and `resize` close
    // and reopen views by rewriting it (closeOrReopenViews). So it is scoped,
    // never invariant â€” hoistable past element and env stores, reloaded past
    // any call, which is the only place a window can move.
    auto* taLen = builder.CreateAlignedLoad(i32Ty, taLenPtr, llvm::Align(4), "eg.talen");
    tagViewLengthAccess(taLen, ctx);
    llvm::BasicBlock* taKindBb = llvm::BasicBlock::Create(ctx, "eg.ta.kind", fn);
    llvm::BasicBlock* taUndefBb = llvm::BasicBlock::Create(ctx, "eg.ta.undef", fn);
    builder.CreateCondBr(builder.CreateICmpULT(g.idx32, taLen), taKindBb, taUndefBb);

    builder.SetInsertPoint(taUndefBb);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(taKindBb);
    llvm::Value* kindPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_KIND_OFFSET);
    auto* kind = builder.CreateAlignedLoad(i32Ty, kindPtr, llvm::Align(4), "eg.kind");
    markInvariant(kind, ctx);

    llvm::BasicBlock* f64Bb = llvm::BasicBlock::Create(ctx, "eg.f64", fn);
    llvm::BasicBlock* f32Bb = llvm::BasicBlock::Create(ctx, "eg.f32", fn);
    llvm::BasicBlock* i32Bb = llvm::BasicBlock::Create(ctx, "eg.i32", fn);
    llvm::BasicBlock* u32Bb = llvm::BasicBlock::Create(ctx, "eg.u32", fn);
    llvm::BasicBlock* i16Bb = llvm::BasicBlock::Create(ctx, "eg.i16", fn);
    llvm::BasicBlock* u16Bb = llvm::BasicBlock::Create(ctx, "eg.u16", fn);
    llvm::BasicBlock* i8Bb = llvm::BasicBlock::Create(ctx, "eg.i8", fn);
    llvm::BasicBlock* u8Bb = llvm::BasicBlock::Create(ctx, "eg.u8", fn);

    llvm::SwitchInst* swKind = builder.CreateSwitch(kind, cacheBb, 9);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT64), f64Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT32), f32Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_INT32), i32Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_UINT32), u32Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_INT16), i16Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_UINT16), u16Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_INT8), i8Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_UINT8), u8Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_UINT8CLAMPED), u8Bb);

    builder.SetInsertPoint(f64Bb);
    llvm::Value* p64 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 8);
    auto* d64 = builder.CreateAlignedLoad(dblTy, p64, llvm::Align(8), "eg.d64");
    tagTypedArrayAccess(d64, ctx);
    llvm::Value* f64Val = emitBoxDouble(builder, d64);
    llvm::BasicBlock* f64EndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(f32Bb);
    llvm::Value* p32 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
    auto* d32 = builder.CreateAlignedLoad(f32Ty, p32, llvm::Align(4), "eg.d32");
    tagTypedArrayAccess(d32, ctx);
    llvm::Value* f32Val = emitBoxDouble(builder, builder.CreateFPExt(d32, dblTy));
    llvm::BasicBlock* f32EndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(i32Bb);
    llvm::Value* pi32 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
    auto* di32 = builder.CreateAlignedLoad(i32Ty, pi32, llvm::Align(4), "eg.i32");
    tagTypedArrayAccess(di32, ctx);
    llvm::Value* i32Val = emitBoxDouble(builder, builder.CreateSIToFP(di32, dblTy));
    llvm::BasicBlock* i32EndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(u32Bb);
    llvm::Value* pu32 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
    auto* du32 = builder.CreateAlignedLoad(i32Ty, pu32, llvm::Align(4), "eg.u32");
    tagTypedArrayAccess(du32, ctx);
    llvm::Value* u32Val = emitBoxDouble(builder, builder.CreateUIToFP(du32, dblTy));
    llvm::BasicBlock* u32EndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(i16Bb);
    llvm::Value* pi16 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 2);
    auto* di16 = builder.CreateAlignedLoad(i16Ty, pi16, llvm::Align(2), "eg.i16");
    tagTypedArrayAccess(di16, ctx);
    llvm::Value* i16Val = emitBoxDouble(builder, builder.CreateSIToFP(di16, dblTy));
    llvm::BasicBlock* i16EndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(u16Bb);
    llvm::Value* pu16 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 2);
    auto* du16 = builder.CreateAlignedLoad(i16Ty, pu16, llvm::Align(2), "eg.u16");
    tagTypedArrayAccess(du16, ctx);
    llvm::Value* u16Val = emitBoxDouble(builder, builder.CreateUIToFP(du16, dblTy));
    llvm::BasicBlock* u16EndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(i8Bb);
    llvm::Value* pi8 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 1);
    auto* di8 = builder.CreateAlignedLoad(i8Ty, pi8, llvm::Align(1), "eg.i8");
    tagTypedArrayAccess(di8, ctx);
    llvm::Value* i8Val = emitBoxDouble(builder, builder.CreateSIToFP(di8, dblTy));
    llvm::BasicBlock* i8EndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(u8Bb);
    llvm::Value* pu8 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 1);
    auto* du8 = builder.CreateAlignedLoad(i8Ty, pu8, llvm::Align(1), "eg.u8");
    tagTypedArrayAccess(du8, ctx);
    llvm::Value* u8Val = emitBoxDouble(builder, builder.CreateUIToFP(du8, dblTy));
    llvm::BasicBlock* u8EndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(cacheBb);
    ElemCacheHit cached = emitElemCacheGet(builder, abi, objBits, idxBits, slowBb, doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(abi.bronze_elem_get, {objBits, idxBits});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 12, "eg.result");
    result->addIncoming(cached.value, cached.hitBb);
    result->addIncoming(arrVal, arrEndBb);
    result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), taUndefBb);
    result->addIncoming(f64Val, f64EndBb);
    result->addIncoming(f32Val, f32EndBb);
    result->addIncoming(i32Val, i32EndBb);
    result->addIncoming(u32Val, u32EndBb);
    result->addIncoming(i16Val, i16EndBb);
    result->addIncoming(u16Val, u16EndBb);
    result->addIncoming(i8Val, i8EndBb);
    result->addIncoming(u8Val, u8EndBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

void emitElemSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                 llvm::Value* idxBits, llvm::Value* valBits, bool strict) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
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
    llvm::SwitchInst* swFlags = builder.CreateSwitch(g.flags, slowBb, 2);
    swFlags->addCase(builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY), arrBb);
    swFlags->addCase(builder.getInt16(BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY), taBb);

    // Array: in bounds, within capacity, and no named-properties side object â€”
    // the same three conditions the helper's fast path requires before it will
    // store without consulting the property machinery.
    builder.SetInsertPoint(arrBb);
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "es.len");
    tagArrayHeaderAccess(len, ctx);
    llvm::Value* capPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
    auto* cap = builder.CreateAlignedLoad(i32Ty, capPtr, llvm::Align(4), "es.cap");
    tagArrayHeaderAccess(cap, ctx);
    llvm::Value* headPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_HEAD_OFFSET);
    auto* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), "es.head");
    tagArrayHeaderAccess(head, ctx);
    llvm::Value* actualIdx = builder.CreateAdd(g.idx32, head, "es.actidx");
    llvm::Value* propsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
    auto* propsVal =
        builder.CreateAlignedLoad(i64Ty, propsPtr, llvm::Align(8), "es.props");
    tagArrayHeaderAccess(propsVal, ctx);
    llvm::Value* propsTag = builder.CreateLShr(propsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* noProps =
        builder.CreateICmpNE(propsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::Value* inBounds = builder.CreateAnd(builder.CreateICmpULT(g.idx32, len),
                                              builder.CreateICmpULT(actualIdx, cap));
    llvm::BasicBlock* arrElemsBb = llvm::BasicBlock::Create(ctx, "es.arr.elems", fn);
    builder.CreateCondBr(builder.CreateAnd(inBounds, noProps), arrElemsBb, slowBb);

    builder.SetInsertPoint(arrElemsBb);
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "es.elems");
    tagArrayHeaderAccess(elemsVal, ctx);
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::BasicBlock* arrStoreBb = llvm::BasicBlock::Create(ctx, "es.arr.store", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT)), arrStoreBb,
        slowBb);

    builder.SetInsertPoint(arrStoreBb);
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* slotIdx = builder.CreateAdd(builder.CreateZExt(actualIdx, i64Ty),
                                             builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    auto* sArr = builder.CreateAlignedStore(valBits, slotPtr, llvm::Align(8));
    tagArrayElementsAccess(sArr, ctx);
    builder.CreateBr(doneBb);

    // Typed array: a numeric value into a typed array view. An in-range index
    // stores; an out-of-range one DISCARDS the write for the Number kinds,
    // exactly as the helper and 10.4.5.16 do (ToNumber of a number is the
    // number, so nothing observable is skipped) â€” but a BIGINT kind still
    // owes the ToBigInt that THROWS for a Number value even when the index is
    // invalid, conversion-before-validity being 10.4.5.16's own order. So the
    // out-of-bounds edge lands on a cold kind test rather than on `done`: at
    // or above BIGINT64 it takes the helper, which converts and throws.
    builder.SetInsertPoint(taBb);
    llvm::Value* valIsNum =
        builder.CreateICmpULE(valBits, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
    llvm::BasicBlock* taLenBb = llvm::BasicBlock::Create(ctx, "es.ta.len", fn);
    builder.CreateCondBr(valIsNum, taLenBb, slowBb);

    builder.SetInsertPoint(taLenBb);
    llvm::Value* taLenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
    auto* taLen = builder.CreateAlignedLoad(i32Ty, taLenPtr, llvm::Align(4), "es.talen");
    tagViewLengthAccess(taLen, ctx);
    llvm::BasicBlock* taKindBb = llvm::BasicBlock::Create(ctx, "es.ta.kind", fn);
    llvm::BasicBlock* taOobBb = llvm::BasicBlock::Create(ctx, "es.ta.oob", fn);
    builder.CreateCondBr(builder.CreateICmpULT(g.idx32, taLen), taKindBb, taOobBb);

    builder.SetInsertPoint(taOobBb);
    llvm::Value* oobKindPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_KIND_OFFSET);
    auto* oobKind = builder.CreateAlignedLoad(i32Ty, oobKindPtr, llvm::Align(4), "es.oob.kind");
    markInvariant(oobKind, ctx);
    llvm::Value* oobIsNumberKind =
        builder.CreateICmpULT(oobKind, builder.getInt32(BRONZE_ABI_TA_KIND_BIGINT64));
    builder.CreateCondBr(oobIsNumberKind, doneBb, slowBb);

    builder.SetInsertPoint(taKindBb);
    llvm::Value* kindPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_KIND_OFFSET);
    auto* kind = builder.CreateAlignedLoad(i32Ty, kindPtr, llvm::Align(4), "es.kind");
    markInvariant(kind, ctx);

    llvm::BasicBlock* f64Bb = llvm::BasicBlock::Create(ctx, "es.f64", fn);
    llvm::BasicBlock* f32Bb = llvm::BasicBlock::Create(ctx, "es.f32", fn);
    llvm::BasicBlock* i32Bb = llvm::BasicBlock::Create(ctx, "es.i32", fn);
    llvm::BasicBlock* i16Bb = llvm::BasicBlock::Create(ctx, "es.i16", fn);
    llvm::BasicBlock* i8Bb = llvm::BasicBlock::Create(ctx, "es.i8", fn);
    llvm::BasicBlock* u8cBb = llvm::BasicBlock::Create(ctx, "es.u8c", fn);

    llvm::SwitchInst* swKind = builder.CreateSwitch(kind, slowBb, 9);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT64), f64Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT32), f32Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_INT32), i32Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_UINT32), i32Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_INT16), i16Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_UINT16), i16Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_INT8), i8Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_UINT8), i8Bb);
    swKind->addCase(builder.getInt32(BRONZE_ABI_TA_KIND_UINT8CLAMPED), u8cBb);

    builder.SetInsertPoint(f64Bb);
    llvm::Value* p64 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 8);
    auto* s64 = builder.CreateAlignedStore(builder.CreateBitCast(valBits, dblTy), p64, llvm::Align(8));
    tagTypedArrayAccess(s64, ctx);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(f32Bb);
    llvm::Value* p32 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
    llvm::Value* narrowed =
        builder.CreateFPTrunc(builder.CreateBitCast(valBits, dblTy), f32Ty, "es.f32.val");
    auto* s32 = builder.CreateAlignedStore(narrowed, p32, llvm::Align(4));
    tagTypedArrayAccess(s32, ctx);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(i32Bb);
    llvm::Value* pi32 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
    llvm::Value* valDbl32 = builder.CreateBitCast(valBits, dblTy);
    llvm::Value* i32Val = builder.CreateCall(abi.bronze_to_int32_f64, {valDbl32}, "es.i32.val");
    auto* si32 = builder.CreateAlignedStore(i32Val, pi32, llvm::Align(4));
    tagTypedArrayAccess(si32, ctx);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(i16Bb);
    llvm::Value* pi16 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 2);
    llvm::Value* valDbl16 = builder.CreateBitCast(valBits, dblTy);
    llvm::Value* i32For16 = builder.CreateCall(abi.bronze_to_int32_f64, {valDbl16}, "es.i16.tmp");
    llvm::Value* i16Val = builder.CreateTrunc(i32For16, i16Ty, "es.i16.val");
    auto* si16 = builder.CreateAlignedStore(i16Val, pi16, llvm::Align(2));
    tagTypedArrayAccess(si16, ctx);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(i8Bb);
    llvm::Value* pi8 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 1);
    llvm::Value* valDbl8 = builder.CreateBitCast(valBits, dblTy);
    llvm::Value* i32For8 = builder.CreateCall(abi.bronze_to_int32_f64, {valDbl8}, "es.i8.tmp");
    llvm::Value* i8Val = builder.CreateTrunc(i32For8, i8Ty, "es.i8.val");
    auto* si8 = builder.CreateAlignedStore(i8Val, pi8, llvm::Align(1));
    tagTypedArrayAccess(si8, ctx);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(u8cBb);
    llvm::Value* pu8c = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 1);
    llvm::Value* valDblU8c = builder.CreateBitCast(valBits, dblTy);
    llvm::Value* u8cTmp = builder.CreateCall(abi.bronze_to_uint8_clamp_f64, {valDblU8c}, "es.u8c.tmp");
    llvm::Value* u8cVal = builder.CreateTrunc(u8cTmp, i8Ty, "es.u8c.val");
    auto* su8c = builder.CreateAlignedStore(u8cVal, pu8c, llvm::Align(1));
    tagTypedArrayAccess(su8c, ctx);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    builder.CreateCall(abi.bronze_elem_set,
                       {objBits, idxBits, valBits, builder.getInt1(strict)});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
}

namespace {

// The shared front half of the two PROVEN forms: validate the index the way
// 23.2 does — an integral, in-range value inside the view — and hand back the
// header pointer and the i32 index. The receiver needs NO guard at all: the
// op only exists where inference proved the view, which is the contract that
// separates these from emitElemGet above. The select-before-fptoui is the
// same poison discipline emitElemGuards documents: the conversion must be fed
// a value the range check has already accepted on EVERY path.
//
// The bounds check is against the view's length, exactly the bound
// bronze_elem_get / _set use, so the two modes answer identically byte for
// byte — including over a detached or shrunk-away buffer, whose views the
// runtime CLOSES by zeroing this very length word (closeOrReopenViews), so
// the one compare below is also the 10.4.5.9 out-of-bounds check. That is
// why the length load is scoped rather than invariant (llvm_alias.h).
struct TypedElemGuards {
    llvm::Value* hdr;
    llvm::Value* idx32;
    llvm::Value* ok;
};

TypedElemGuards emitTypedElemGuards(llvm::IRBuilder<>& builder, llvm::Value* objBits,
                                    llvm::Value* idxDbl) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::Value* ge0 = builder.CreateFCmpOGE(idxDbl, llvm::ConstantFP::get(dblTy, 0.0));
    llvm::Value* ltMax =
        builder.CreateFCmpOLT(idxDbl, llvm::ConstantFP::get(dblTy, 4294967296.0));
    llvm::Value* inRange = builder.CreateAnd(ge0, ltMax);
    llvm::Value* safe = builder.CreateSelect(inRange, idxDbl, llvm::ConstantFP::get(dblTy, 0.0));
    llvm::Value* idx32 = builder.CreateFPToUI(safe, i32Ty, "tel.idx");
    llvm::Value* roundTrip = builder.CreateUIToFP(idx32, dblTy);
    llvm::Value* isIntegral = builder.CreateFCmpOEQ(roundTrip, idxDbl);

    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "tel.hdr");
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "tel.len");
    tagViewLengthAccess(len, ctx);
    llvm::Value* inLen = builder.CreateICmpULT(idx32, len);

    llvm::Value* ok = builder.CreateAnd(builder.CreateAnd(inRange, isIntegral), inLen);
    return {hdr, idx32, ok};
}

}  // namespace

llvm::Value* emitTypedElemGet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                              llvm::Value* idxDbl, uint32_t elemKind) {
    (void)abi;
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

    TypedElemGuards g = emitTypedElemGuards(builder, objBits, idxDbl);
    llvm::BasicBlock* loadBb = llvm::BasicBlock::Create(ctx, "tel.load", fn);
    llvm::BasicBlock* missBb = llvm::BasicBlock::Create(ctx, "tel.miss", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "tel.done", fn);
    builder.CreateCondBr(g.ok, loadBb, missBb);

    builder.SetInsertPoint(loadBb);
    llvm::Value* loaded = nullptr;
    switch (elemKind) {
        case BRONZE_ABI_TA_KIND_FLOAT64: {
            llvm::Value* p = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 8);
            auto* ld = builder.CreateAlignedLoad(dblTy, p, llvm::Align(8), "tel.d64");
            tagTypedArrayAccess(ld, ctx);
            loaded = ld;
            break;
        }
        case BRONZE_ABI_TA_KIND_FLOAT32: {
            llvm::Value* p = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
            auto* ld = builder.CreateAlignedLoad(f32Ty, p, llvm::Align(4), "tel.d32");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateFPExt(ld, dblTy);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT32: {
            llvm::Value* p = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
            auto* ld = builder.CreateAlignedLoad(i32Ty, p, llvm::Align(4), "tel.i32");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateSIToFP(ld, dblTy);
            break;
        }
        case BRONZE_ABI_TA_KIND_UINT32: {
            llvm::Value* p = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
            auto* ld = builder.CreateAlignedLoad(i32Ty, p, llvm::Align(4), "tel.u32");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateUIToFP(ld, dblTy);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT16: {
            llvm::Value* p = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 2);
            auto* ld = builder.CreateAlignedLoad(i16Ty, p, llvm::Align(2), "tel.i16");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateSIToFP(ld, dblTy);
            break;
        }
        case BRONZE_ABI_TA_KIND_UINT16: {
            llvm::Value* p = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 2);
            auto* ld = builder.CreateAlignedLoad(i16Ty, p, llvm::Align(2), "tel.u16");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateUIToFP(ld, dblTy);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT8: {
            llvm::Value* p = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 1);
            auto* ld = builder.CreateAlignedLoad(i8Ty, p, llvm::Align(1), "tel.i8");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateSIToFP(ld, dblTy);
            break;
        }
        case BRONZE_ABI_TA_KIND_UINT8:
        case BRONZE_ABI_TA_KIND_UINT8CLAMPED: {
            llvm::Value* p = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 1);
            auto* ld = builder.CreateAlignedLoad(i8Ty, p, llvm::Align(1), "tel.u8");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateUIToFP(ld, dblTy);
            break;
        }
        default:
            loaded = llvm::ConstantFP::getNaN(dblTy);
            break;
    }
    llvm::BasicBlock* loadEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(missBb);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(dblTy, 2, "tel.result");
    result->addIncoming(loaded, loadEndBb);
    // ToNumber(undefined): the invalid-index answer in a coercing position.
    result->addIncoming(llvm::ConstantFP::getNaN(dblTy), missBb);
    return result;
}

void emitTypedElemSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                      llvm::Value* idxDbl, llvm::Value* valDbl, uint32_t elemKind) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

    TypedElemGuards g = emitTypedElemGuards(builder, objBits, idxDbl);
    llvm::BasicBlock* storeBb = llvm::BasicBlock::Create(ctx, "tes.store", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "tes.done", fn);
    builder.CreateCondBr(g.ok, storeBb, doneBb);

    builder.SetInsertPoint(storeBb);
    switch (elemKind) {
        case BRONZE_ABI_TA_KIND_FLOAT64: {
            llvm::Value* elemPtr = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 8);
            auto* st = builder.CreateAlignedStore(valDbl, elemPtr, llvm::Align(8));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        case BRONZE_ABI_TA_KIND_FLOAT32: {
            llvm::Value* elemPtr = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
            llvm::Value* narrowed =
                builder.CreateFPTrunc(valDbl, f32Ty, "tes.f32");
            auto* st = builder.CreateAlignedStore(narrowed, elemPtr, llvm::Align(4));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT32:
        case BRONZE_ABI_TA_KIND_UINT32: {
            llvm::Value* elemPtr = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
            llvm::Value* i32Val = builder.CreateCall(abi.bronze_to_int32_f64, {valDbl}, "tes.i32");
            auto* st = builder.CreateAlignedStore(i32Val, elemPtr, llvm::Align(4));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT16:
        case BRONZE_ABI_TA_KIND_UINT16: {
            llvm::Value* elemPtr = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 2);
            llvm::Value* i32Tmp = builder.CreateCall(abi.bronze_to_int32_f64, {valDbl}, "tes.i16.tmp");
            llvm::Value* i16Val = builder.CreateTrunc(i32Tmp, i16Ty, "tes.i16");
            auto* st = builder.CreateAlignedStore(i16Val, elemPtr, llvm::Align(2));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT8:
        case BRONZE_ABI_TA_KIND_UINT8: {
            llvm::Value* elemPtr = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 1);
            llvm::Value* i32Tmp = builder.CreateCall(abi.bronze_to_int32_f64, {valDbl}, "tes.i8.tmp");
            llvm::Value* i8Val = builder.CreateTrunc(i32Tmp, i8Ty, "tes.i8");
            auto* st = builder.CreateAlignedStore(i8Val, elemPtr, llvm::Align(1));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        case BRONZE_ABI_TA_KIND_UINT8CLAMPED: {
            llvm::Value* elemPtr = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 1);
            llvm::Value* u8cTmp = builder.CreateCall(abi.bronze_to_uint8_clamp_f64, {valDbl}, "tes.u8c.tmp");
            llvm::Value* u8cVal = builder.CreateTrunc(u8cTmp, i8Ty, "tes.u8c");
            auto* st = builder.CreateAlignedStore(u8cVal, elemPtr, llvm::Align(1));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        default:
            break;
    }
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
}

}  // namespace bronze::codegen_llvm
