#include "codegen-llvm/llvm_elem_typed.h"
#include "codegen-llvm/llvm_elem.h"
#include "codegen-llvm/llvm_alias.h"
#include "codegen-llvm/llvm_convert.h"
#include "il/il.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

// The address of element `idx32` of a typed-array view, computed from
// the view's buffer Value on every access — never cached across allocations,
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
    bufVal->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(ctx, {}));
    llvm::Value* bufAddr =
        builder.CreateAnd(bufVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* bufHdr = builder.CreateIntToPtr(bufAddr, ptrTy);
    llvm::Value* byteOffPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_BYTEOFFSET_OFFSET);
    auto* byteOff =
        builder.CreateAlignedLoad(i32Ty, byteOffPtr, llvm::Align(4), "ta.byteoff");
    byteOff->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(ctx, {}));

    // The buffer's external-storage word: zero for an ordinary buffer (bytes
    // inline past the header), else the address of a non-moving host store.
    // NOT an invariant load — externalizeArrayBuffer flips it once, inside a
    // host call — but element stores can never change it, so it carries the
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

    llvm::Value* basePtr = builder.CreateIntToPtr(base, ptrTy, "ta.base.ptr");
    llvm::Value* byteOff64 = builder.CreateZExt(byteOff, i64Ty, "ta.byteoff64");
    llvm::Value* dataPtr = builder.CreateInBoundsGEP(i8Ty, basePtr, byteOff64, "ta.data.ptr");
    llvm::Value* idx64 = builder.CreateZExt(idx32, i64Ty, "ta.idx64");
    llvm::Type* elemTy = (elemSize == 8)
                             ? builder.getDoubleTy()
                             : ((elemSize == 4) ? builder.getFloatTy()
                                                : ((elemSize == 2) ? builder.getInt16Ty() : i8Ty));
    return builder.CreateInBoundsGEP(elemTy, dataPtr, idx64, "ta.elem.ptr");
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

    llvm::Value* idx32 = builder.CreateIntrinsic(
        llvm::Intrinsic::fptoui_sat, {i32Ty, dblTy}, {idxDbl}, nullptr, "tel.idx");
    llvm::Value* roundTrip = builder.CreateUIToFP(idx32, dblTy);
    llvm::Value* isIntegral = builder.CreateFCmpOEQ(roundTrip, idxDbl);

    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "tel.hdr");
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "tel.len");
    tagViewLengthAccess(len, ctx);
    llvm::Value* inLen = builder.CreateICmpULT(idx32, len);

    llvm::Value* ok = builder.CreateAnd(isIntegral, inLen);
    return {hdr, idx32, ok};
}

// The pin ceiling probe's element slot address (il::kElemKindPlainArrayF64,
// BRONZE_UNSOUND_PINS): a plain dense array's element `idx`, computed with NO
// guards — the receiver is ASSUMED an array, the index in-bounds, the slot a
// number. This is the code pin-based compilation would emit after its checks
// moved to the write paths; here nothing backs the assumption but the
// fixture's checksum.
llvm::Value* emitUnsoundArrayElemPtr(llvm::IRBuilder<>& builder, llvm::Value* objBits,
                                     llvm::Value* idxDbl) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::Value* addr =
        builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "pel.hdr");
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "pel.elems");
    tagArrayHeaderAccess(elemsVal, ctx);
    llvm::Value* elemsObj = builder.CreateIntToPtr(
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK)), ptrTy);
    llvm::Value* headPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_HEAD_OFFSET);
    auto* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), "pel.head");
    tagArrayHeaderAccess(head, ctx);
    llvm::Value* idx32 = builder.CreateIntrinsic(
        llvm::Intrinsic::fptoui_sat, {i32Ty, builder.getDoubleTy()}, {idxDbl}, nullptr,
        "pel.idx");
    // +1: the elements block's payload begins one i64 past its header.
    llvm::Value* slotIdx = builder.CreateAdd(
        builder.CreateZExt(builder.CreateAdd(idx32, head), i64Ty), builder.getInt64(1));
    return builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx, "pel.slot");
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

    if (elemKind == static_cast<uint32_t>(il::kElemKindPlainArrayF64)) {
        llvm::Value* slotPtr = emitUnsoundArrayElemPtr(builder, objBits, idxDbl);
        auto* raw = builder.CreateAlignedLoad(builder.getInt64Ty(), slotPtr, llvm::Align(8),
                                              "pel.raw");
        tagArrayElementsAccess(raw, ctx);
        return builder.CreateBitCast(raw, dblTy, "pel.f64");
    }

    TypedElemGuards g = emitTypedElemGuards(builder, objBits, idxDbl);
    llvm::BasicBlock* loadBb = llvm::BasicBlock::Create(ctx, "tel.load", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "tel.done", fn);
    llvm::BasicBlock* entryBb = builder.GetInsertBlock();
    auto* condBr = builder.CreateCondBr(g.ok, loadBb, doneBb);
    condBr->setMetadata(llvm::LLVMContext::MD_prof,
                        llvm::MDBuilder(ctx).createBranchWeights(1048576, 1));

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
    llvm::Value* loadedBits = emitBoxDouble(builder, loaded);
    llvm::BasicBlock* loadEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* resultBits = builder.CreatePHI(builder.getInt64Ty(), 2, "tel.bits");
    resultBits->addIncoming(loadedBits, loadEndBb);
    resultBits->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), entryBb);
    return builder.CreateBitCast(resultBits, dblTy, "tel.result");
}

void emitTypedElemSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                      llvm::Value* idxDbl, llvm::Value* valDbl, uint32_t elemKind) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

    if (elemKind == static_cast<uint32_t>(il::kElemKindPlainArrayF64)) {
        llvm::Value* slotPtr = emitUnsoundArrayElemPtr(builder, objBits, idxDbl);
        // The canonicalizing select Box emits: a non-canonical NaN written
        // raw would read back as a tagged pointer.
        llvm::Value* bits = emitBoxDouble(builder, valDbl);
        auto* st = builder.CreateAlignedStore(bits, slotPtr, llvm::Align(8));
        tagArrayElementsAccess(st, ctx);
        return;
    }

    TypedElemGuards g = emitTypedElemGuards(builder, objBits, idxDbl);
    llvm::BasicBlock* storeBb = llvm::BasicBlock::Create(ctx, "tes.store", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "tes.done", fn);
    auto* condBr = builder.CreateCondBr(g.ok, storeBb, doneBb);
    condBr->setMetadata(llvm::LLVMContext::MD_prof,
                        llvm::MDBuilder(ctx).createBranchWeights(1048576, 1));

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
            llvm::Value* i32Val = emitToInt32F64(builder, abi, valDbl);
            auto* st = builder.CreateAlignedStore(i32Val, elemPtr, llvm::Align(4));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT16:
        case BRONZE_ABI_TA_KIND_UINT16: {
            llvm::Value* elemPtr = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 2);
            llvm::Value* i32Tmp = emitToInt32F64(builder, abi, valDbl);
            llvm::Value* i16Val = builder.CreateTrunc(i32Tmp, i16Ty, "tes.i16");
            auto* st = builder.CreateAlignedStore(i16Val, elemPtr, llvm::Align(2));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT8:
        case BRONZE_ABI_TA_KIND_UINT8: {
            llvm::Value* elemPtr = emitTypedArrayElemPtr(builder, g.hdr, g.idx32, 1);
            llvm::Value* i32Tmp = emitToInt32F64(builder, abi, valDbl);
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
