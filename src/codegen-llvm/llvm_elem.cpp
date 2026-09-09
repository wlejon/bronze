#include "codegen-llvm/llvm_elem.h"
#include "codegen-llvm/llvm_elem_typed.h"
#include "codegen-llvm/llvm_alias.h"
#include "codegen-llvm/llvm_convert.h"
#include "il/il.h"

#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

namespace {

static void markInvariant(llvm::LoadInst* load, llvm::LLVMContext& ctx) {
    load->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(ctx, {}));
}

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
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::Value* tag = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObject = builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::Value* idxIsNum =
        builder.CreateICmpULE(idxBits, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
    llvm::Value* objAndNum = builder.CreateAnd(isObject, idxIsNum);

    llvm::BasicBlock* numBb = llvm::BasicBlock::Create(ctx, std::string(prefix) + "num", fn);
    auto* brObj = builder.CreateCondBr(objAndNum, numBb, slowBb);
    brObj->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(numBb);
    llvm::Value* d = builder.CreateBitCast(idxBits, dblTy);
    llvm::Value* idx32 = builder.CreateIntrinsic(
        llvm::Intrinsic::fptoui_sat, {builder.getInt32Ty(), dblTy}, {d}, nullptr, "elem.idx");
    llvm::Value* roundTrip = builder.CreateUIToFP(idx32, dblTy);
    llvm::Value* isIntegral = builder.CreateFCmpOEQ(roundTrip, d);

    llvm::BasicBlock* cont = llvm::BasicBlock::Create(ctx, std::string(prefix) + "ok", fn);
    auto* brInt = builder.CreateCondBr(isIntegral, cont, slowBb);
    brInt->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);
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

// A double as a NaN-boxed Value: the same NaN-canonicalizing select the Box
// instruction emits, because a Float64Array can hold any NaN bit pattern and
// a non-canonical one would read back as a tagged pointer.
llvm::Value* emitBoxDouble(llvm::IRBuilder<>& builder, llvm::Value* d) {
    // Boxing a double that was BITCAST OUT OF A VALUE is the identity, and the
    // Box instruction states the argument in full (llvm_ops.cpp): the two
    // emitters that produce such a double — the raw unbox and the pinned
    // plain-array element read — both carry the claim that the source bits are
    // a Number, and a Number's Value carries a canonical NaN by construction,
    // so the select below could only ever choose the arm the bits came from.
    // `te[i] = me[i]` between two pinned matrices is the shape that reaches
    // here: sixteen loads, each turned into a double and immediately asked
    // whether it is a NaN it cannot be.
    if (auto* cast = llvm::dyn_cast<llvm::BitCastInst>(d);
        cast != nullptr && cast->getSrcTy()->isIntegerTy(64)) {
        return cast->getOperand(0);
    }
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
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

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
    llvm::BasicBlock* notArrBb = llvm::BasicBlock::Create(ctx, "eg.notarr", fn);
    llvm::BasicBlock* taBb = llvm::BasicBlock::Create(ctx, "eg.ta", fn);

    llvm::Value* isArr = builder.CreateICmpEQ(g.flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
    auto* brArr = builder.CreateCondBr(isArr, arrBb, notArrBb);
    brArr->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(notArrBb);
    llvm::Value* isTa = builder.CreateICmpEQ(g.flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY));
    auto* brTa = builder.CreateCondBr(isTa, taBb, cacheBb);
    brTa->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // Array: in bounds, elements block present, hole answers undefined. An
    // out-of-bounds read goes to the helper, whose own fast path answers the
    // undefined — rare enough that the extra call is not worth a fourth arm.
    builder.SetInsertPoint(arrBb);
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "eg.len");
    tagArrayHeaderAccess(len, ctx);
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "eg.elems");
    tagArrayHeaderAccess(elemsVal, ctx);
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* inBounds = builder.CreateICmpULT(g.idx32, len);
    llvm::Value* elemsIsObj =
        builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::BasicBlock* arrReadBb = llvm::BasicBlock::Create(ctx, "eg.arr.read", fn);
    auto* brInBounds = builder.CreateCondBr(builder.CreateAnd(inBounds, elemsIsObj), arrReadBb, cacheBb);
    brInBounds->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

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

    llvm::BasicBlock* arrHoleBb = llvm::BasicBlock::Create(ctx, "eg.arr.hole", fn);
    llvm::Value* notHole = builder.CreateICmpNE(raw, builder.getInt64(BRONZE_ABI_HOLE_BITS), "eg.nothole");
    auto* brNotHole = builder.CreateCondBr(notHole, doneBb, arrHoleBb);
    brNotHole->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(arrHoleBb);
    builder.CreateBr(doneBb);

    // Typed array: all element kinds, in bounds. Out of bounds answers
    // undefined directly without helper call.
    llvm::BasicBlock* taDoneBb = llvm::BasicBlock::Create(ctx, "eg.ta.done", fn);

    builder.SetInsertPoint(taBb);
    llvm::Value* taLenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
    // The length word is MAINTAINED, not fixed: `transfer` and `resize` close
    // and reopen views by rewriting it (closeOrReopenViews). So it is scoped,
    // never invariant — hoistable past element and env stores, reloaded past
    // any call, which is the only place a window can move.
    auto* taLen = builder.CreateAlignedLoad(i32Ty, taLenPtr, llvm::Align(4), "eg.talen");
    tagViewLengthAccess(taLen, ctx);
    llvm::BasicBlock* taKindBb = llvm::BasicBlock::Create(ctx, "eg.ta.kind", fn);
    llvm::BasicBlock* taUndefBb = llvm::BasicBlock::Create(ctx, "eg.ta.undef", fn);
    auto* brTaLen = builder.CreateCondBr(builder.CreateICmpULT(g.idx32, taLen), taKindBb, taUndefBb);
    brTaLen->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(taUndefBb);
    builder.CreateBr(taDoneBb);

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
    builder.CreateBr(taDoneBb);

    builder.SetInsertPoint(f32Bb);
    llvm::Value* p32 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
    auto* d32 = builder.CreateAlignedLoad(f32Ty, p32, llvm::Align(4), "eg.d32");
    tagTypedArrayAccess(d32, ctx);
    llvm::Value* f32Val = emitBoxDouble(builder, builder.CreateFPExt(d32, dblTy));
    llvm::BasicBlock* f32EndBb = builder.GetInsertBlock();
    builder.CreateBr(taDoneBb);

    builder.SetInsertPoint(i32Bb);
    llvm::Value* pi32 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
    auto* di32 = builder.CreateAlignedLoad(i32Ty, pi32, llvm::Align(4), "eg.i32");
    tagTypedArrayAccess(di32, ctx);
    llvm::Value* i32Val = emitBoxDouble(builder, builder.CreateSIToFP(di32, dblTy));
    llvm::BasicBlock* i32EndBb = builder.GetInsertBlock();
    builder.CreateBr(taDoneBb);

    builder.SetInsertPoint(u32Bb);
    llvm::Value* pu32 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 4);
    auto* du32 = builder.CreateAlignedLoad(i32Ty, pu32, llvm::Align(4), "eg.u32");
    tagTypedArrayAccess(du32, ctx);
    llvm::Value* u32Val = emitBoxDouble(builder, builder.CreateUIToFP(du32, dblTy));
    llvm::BasicBlock* u32EndBb = builder.GetInsertBlock();
    builder.CreateBr(taDoneBb);

    builder.SetInsertPoint(i16Bb);
    llvm::Value* pi16 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 2);
    auto* di16 = builder.CreateAlignedLoad(i16Ty, pi16, llvm::Align(2), "eg.i16");
    tagTypedArrayAccess(di16, ctx);
    llvm::Value* i16Val = emitBoxDouble(builder, builder.CreateSIToFP(di16, dblTy));
    llvm::BasicBlock* i16EndBb = builder.GetInsertBlock();
    builder.CreateBr(taDoneBb);

    builder.SetInsertPoint(u16Bb);
    llvm::Value* pu16 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 2);
    auto* du16 = builder.CreateAlignedLoad(i16Ty, pu16, llvm::Align(2), "eg.u16");
    tagTypedArrayAccess(du16, ctx);
    llvm::Value* u16Val = emitBoxDouble(builder, builder.CreateUIToFP(du16, dblTy));
    llvm::BasicBlock* u16EndBb = builder.GetInsertBlock();
    builder.CreateBr(taDoneBb);

    builder.SetInsertPoint(i8Bb);
    llvm::Value* pi8 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 1);
    auto* di8 = builder.CreateAlignedLoad(i8Ty, pi8, llvm::Align(1), "eg.i8");
    tagTypedArrayAccess(di8, ctx);
    llvm::Value* i8Val = emitBoxDouble(builder, builder.CreateSIToFP(di8, dblTy));
    llvm::BasicBlock* i8EndBb = builder.GetInsertBlock();
    builder.CreateBr(taDoneBb);

    builder.SetInsertPoint(u8Bb);
    llvm::Value* pu8 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 1);
    auto* du8 = builder.CreateAlignedLoad(i8Ty, pu8, llvm::Align(1), "eg.u8");
    tagTypedArrayAccess(du8, ctx);
    llvm::Value* u8Val = emitBoxDouble(builder, builder.CreateUIToFP(du8, dblTy));
    llvm::BasicBlock* u8EndBb = builder.GetInsertBlock();
    builder.CreateBr(taDoneBb);

    builder.SetInsertPoint(taDoneBb);
    llvm::PHINode* taVal = builder.CreatePHI(i64Ty, 9, "eg.ta.val");
    taVal->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), taUndefBb);
    taVal->addIncoming(f64Val, f64EndBb);
    taVal->addIncoming(f32Val, f32EndBb);
    taVal->addIncoming(i32Val, i32EndBb);
    taVal->addIncoming(u32Val, u32EndBb);
    taVal->addIncoming(i16Val, i16EndBb);
    taVal->addIncoming(u16Val, u16EndBb);
    taVal->addIncoming(i8Val, i8EndBb);
    taVal->addIncoming(u8Val, u8EndBb);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(cacheBb);
    ElemCacheHit cached = emitElemCacheGet(builder, abi, objBits, idxBits, slowBb, doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(abi.bronze_elem_get, {objBits, idxBits});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 5, "eg.result");
    result->addIncoming(raw, arrReadBb);
    result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), arrHoleBb);
    result->addIncoming(taVal, taDoneBb);
    result->addIncoming(cached.value, cached.hitBb);
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
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "es.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "es.done", fn);

    ElemGuards g = emitElemGuards(builder, objBits, idxBits, slowBb, "es.");

    llvm::BasicBlock* arrBb = llvm::BasicBlock::Create(ctx, "es.arr", fn);
    llvm::BasicBlock* notArrBb = llvm::BasicBlock::Create(ctx, "es.notarr", fn);
    llvm::BasicBlock* taBb = llvm::BasicBlock::Create(ctx, "es.ta", fn);

    llvm::Value* isArr = builder.CreateICmpEQ(g.flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
    auto* brArr = builder.CreateCondBr(isArr, arrBb, notArrBb);
    brArr->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(notArrBb);
    llvm::Value* isTa = builder.CreateICmpEQ(g.flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY));
    auto* brTa = builder.CreateCondBr(isTa, taBb, slowBb);
    brTa->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // Array: in bounds, within capacity, and no named-properties side object —
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
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "es.elems");
    tagArrayHeaderAccess(elemsVal, ctx);
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* elemsIsObj =
        builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));

    llvm::Value* canStore =
        builder.CreateAnd(builder.CreateAnd(inBounds, noProps), elemsIsObj);
    llvm::BasicBlock* arrStoreBb = llvm::BasicBlock::Create(ctx, "es.arr.store", fn);
    auto* canStoreBr = builder.CreateCondBr(canStore, arrStoreBb, slowBb);
    canStoreBr->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

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
    // stores; an out-of-bounds one DISCARDS the write for the Number kinds,
    // exactly as the helper and 10.4.5.16 do (ToNumber of a number is the
    // number, so nothing observable is skipped) — but a BIGINT kind still
    // owes the ToBigInt that THROWS for a Number value even when the index is
    // invalid, conversion-before-validity being 10.4.5.16's own order. So the
    // out-of-bounds edge lands on a cold kind test rather than on `done`: at
    // or above BIGINT64 it takes the helper, which converts and throws.
    builder.SetInsertPoint(taBb);
    llvm::Value* valIsNum =
        builder.CreateICmpULE(valBits, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS));
    llvm::Value* taLenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
    auto* taLen = builder.CreateAlignedLoad(i32Ty, taLenPtr, llvm::Align(4), "es.talen");
    tagViewLengthAccess(taLen, ctx);
    llvm::Value* inLen = builder.CreateICmpULT(g.idx32, taLen);
    llvm::Value* fastOk = builder.CreateAnd(valIsNum, inLen);

    llvm::BasicBlock* taKindBb = llvm::BasicBlock::Create(ctx, "es.ta.kind", fn);
    llvm::BasicBlock* taOobBb = llvm::BasicBlock::Create(ctx, "es.ta.oob", fn);
    auto* brFastOk = builder.CreateCondBr(fastOk, taKindBb, taOobBb);
    brFastOk->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(taOobBb);
    llvm::BasicBlock* oobNumChkBb = llvm::BasicBlock::Create(ctx, "es.ta.oobnum", fn);
    auto* brOobNum = builder.CreateCondBr(valIsNum, oobNumChkBb, slowBb);
    brOobNum->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(oobNumChkBb);
    llvm::Value* oobKindPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, g.hdr, BRONZE_ABI_TA_KIND_OFFSET);
    auto* oobKind = builder.CreateAlignedLoad(i32Ty, oobKindPtr, llvm::Align(4), "es.oob.kind");
    markInvariant(oobKind, ctx);
    llvm::Value* oobIsNumberKind =
        builder.CreateICmpULT(oobKind, builder.getInt32(BRONZE_ABI_TA_KIND_BIGINT64));
    auto* brOobKind = builder.CreateCondBr(oobIsNumberKind, doneBb, slowBb);
    brOobKind->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

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
    llvm::Value* i32Val = emitToInt32F64(builder, abi, valDbl32);
    auto* si32 = builder.CreateAlignedStore(i32Val, pi32, llvm::Align(4));
    tagTypedArrayAccess(si32, ctx);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(i16Bb);
    llvm::Value* pi16 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 2);
    llvm::Value* valDbl16 = builder.CreateBitCast(valBits, dblTy);
    llvm::Value* i32For16 = emitToInt32F64(builder, abi, valDbl16);
    llvm::Value* i16Val = builder.CreateTrunc(i32For16, i16Ty, "es.i16.val");
    auto* si16 = builder.CreateAlignedStore(i16Val, pi16, llvm::Align(2));
    tagTypedArrayAccess(si16, ctx);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(i8Bb);
    llvm::Value* pi8 = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 1);
    llvm::Value* valDbl8 = builder.CreateBitCast(valBits, dblTy);
    llvm::Value* i32For8 = emitToInt32F64(builder, abi, valDbl8);
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

}  // namespace bronze::codegen_llvm
