// Property READS in generated code: the constant-key fast paths (an array or
// typed-array element, `length`, a function's `prototype`, an Array.prototype
// method), the shape-cache own-slot and proto-slot hits, and the accessor
// getter call. Every guard miss falls out to bronze_prop_get, and every arm
// that produced a value meets at one PHI at the foot.
//
// Apart from the writes in llvm_prop_set.cpp because the two are independent
// emitters that share no state and no block — only the cache primitives in
// llvm_prop_ic.h, which is where a guard both must agree on lives.

#include "codegen-llvm/llvm_prop.h"

#include "codegen-llvm/llvm_static_slot.h"
#include "codegen-llvm/llvm_prop_ic.h"
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

static llvm::Value* emitPropGetCall(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                    llvm::Value* entry, llvm::Value* objBits,
                                    const ModuleTables& tables, uint32_t keyIndex) {
    return builder.CreateCall(abi.bronze_prop_get,
                              {objBits, emitKeyId(builder, tables, keyIndex), entry},
                              "prop.slow");
}

llvm::Value* emitPropGet(llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
                         const ModuleTables& tables, llvm::Value* objBits, uint32_t keyIndex,
                         uint32_t icIndex, bool monomorphic, uint32_t staticSlot,
                         uint32_t staticCellIndex, std::string_view keyStr) {
    // Not branched on here: `monomorphic` is an identity proof, and the
    // sequence below is an inline cache, which is what an unproven site wants
    // too. It travels to the IL text and to --infer-stats, and the LAYOUT proof
    // beside it (staticSlot) is what changes emitted code.
    (void)monomorphic;
    llvm::Value* entry = icEntryPtr(builder, tables.icTable, icIndex);

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

    // 0. The static-slot fast path, in front of everything. Emits nothing when
    //    the site has no proven layout, and leaves the builder where it was.
    const StaticSlotGuard staticGuard = emitStaticSlotGuard(
        builder, tables, objBits, staticSlot, staticCellIndex, doneBb, /*store=*/nullptr,
        "get");

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
        // Answers 0 for a view whose buffer was transferred or shrunk away —
        // the runtime keeps the length word itself current
        // (closeOrReopenViews), which is also why the load is scoped rather
        // than invariant.
        auto* taLen = builder.CreateAlignedLoad(i32Ty, taLenPtr, llvm::Align(4), "ta.len");
        tagViewLengthAccess(taLen, ctx);
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
        tagViewLengthAccess(taLen, ctx);
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

    // 3. Plain object guard, then the site's ways
    builder.SetInsertPoint(plainCheckBb);
    IcWayScanResult way = emitIcWayScan(builder, ctx, fn, entry, hdr, flags,
                                        globals.bronze_poly_ic_enabled, slowBb, "ic.get");
    llvm::Value* shape = way.shape;
    // Every field below is read off the MATCHED way, never off the site: with
    // four ways a site's word 1 is way 0's slot, and reading it after way 2
    // matched would answer with an unrelated shape's slot.
    llvm::Value* wayEntry = way.entry;

    llvm::Value* slotWordPtr = builder.CreateConstInBoundsGEP1_32(
        i64Ty, wayEntry, static_cast<unsigned>(BRONZE_ABI_IC_SLOTWORD_OFFSET / sizeof(uint64_t)));
    llvm::Value* slotWord =
        builder.CreateAlignedLoad(i64Ty, slotWordPtr, llvm::Align(8), "ic.slotword");
    llvm::Value* depth = builder.CreateLShr(slotWord, 32);
    llvm::Value* isAccessor = builder.CreateICmpNE(
        builder.CreateAnd(depth, builder.getInt64(static_cast<uint64_t>(BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG))),
        builder.getInt64(0), "ic.get.isaccessor");
    llvm::Value* realDepth = builder.CreateAnd(
        depth, builder.getInt64(~static_cast<uint64_t>(BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG |
                                                       BRONZE_ABI_IC_DEPTH_ABSENT_FLAG)),
        "ic.get.realdepth");

    // The depth word decides which arm, and the order is by how hot each is:
    // an own-property hit (depth 0) leaves first and pays ONE compare, then the
    // absent answer, then the two that walk. An accessor entry carries the
    // accessor flag and an absent entry the absent flag, so neither can be
    // mistaken for depth 0 — the flags are what make this a three-way split on
    // a single loaded word rather than three separate tests.
    llvm::BasicBlock* nonZeroDepthBb = llvm::BasicBlock::Create(ctx, "ic.get.depth.nonzero", fn);
    builder.CreateCondBr(builder.CreateICmpEQ(depth, builder.getInt64(0), "ic.get.depthzero"),
                         hitBb, nonZeroDepthBb);

    // 3a. The ABSENT answer: the key is on neither the receiver nor its chain.
    // The shape match above covers every own add; this epoch check covers every
    // way the key could have appeared on a prototype since the entry was filled
    // (bronze_abi.h states the pair as the entry's whole validity condition).
    builder.SetInsertPoint(nonZeroDepthBb);
    llvm::BasicBlock* absentEpochBb = llvm::BasicBlock::Create(ctx, "ic.get.absent.epoch", fn);
    llvm::BasicBlock* flaggedDepthBb = llvm::BasicBlock::Create(ctx, "ic.get.depth.flagged", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(depth,
                             builder.getInt64(static_cast<uint64_t>(BRONZE_ABI_IC_DEPTH_ABSENT_FLAG)),
                             "ic.get.isabsent"),
        absentEpochBb, flaggedDepthBb);

    builder.SetInsertPoint(absentEpochBb);
    llvm::Value* absentEpochPtr = builder.CreateConstInBoundsGEP1_32(
        i64Ty, wayEntry, static_cast<unsigned>(BRONZE_ABI_IC_EPOCH_OFFSET / sizeof(uint64_t)));
    llvm::Value* absentFillEpoch =
        builder.CreateAlignedLoad(i64Ty, absentEpochPtr, llvm::Align(8), "ic.absent.fillepoch");
    llvm::Value* absentCurEpoch = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_proto_epoch, llvm::Align(8), "ic.absent.epoch");
    llvm::BasicBlock* absentHitBb = llvm::BasicBlock::Create(ctx, "ic.get.absent.hit", fn);
    builder.CreateCondBr(builder.CreateICmpEQ(absentFillEpoch, absentCurEpoch), absentHitBb,
                         slowBb);

    builder.SetInsertPoint(absentHitBb);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(flaggedDepthBb);
    llvm::BasicBlock* getAccCheckBb = llvm::BasicBlock::Create(ctx, "ic.get.acc.check", fn);
    llvm::BasicBlock* protoCheckBb = llvm::BasicBlock::Create(ctx, "ic.proto.check", fn);
    builder.CreateCondBr(isAccessor, getAccCheckBb, protoCheckBb);

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
        i64Ty, wayEntry, static_cast<unsigned>(BRONZE_ABI_IC_EPOCH_OFFSET / sizeof(uint64_t)));
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

    // 3b. A depth > 0 data entry: a PROTO hit, reached with the accessor and
    // absent arms already taken, so the depth word here is a real link count.
    builder.SetInsertPoint(protoCheckBb);
    llvm::Value* protoSlot32 = builder.CreateTrunc(slotWord, i32Ty, "proto.slot32");
    llvm::Value* epochPtr = builder.CreateConstInBoundsGEP1_32(
        i64Ty, wayEntry, static_cast<unsigned>(BRONZE_ABI_IC_EPOCH_OFFSET / sizeof(uint64_t)));
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

    // 5. Fallback call — and, for a static site, the one-shot publish that
    //    turns the layout claim into a shape pointer the guard above can hit.
    //    It runs AFTER the helper deliberately: on the very first execution the
    //    property may not be installed yet (a constructor's own `this.x = ...`
    //    reaches the write twin of this), and publishing before the helper had
    //    run would pin the pre-transition shape.
    llvm::BasicBlock* slowDoneBb = llvm::BasicBlock::Create(ctx, "ic.slow.done", fn);
    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = emitPropGetCall(builder, abi, entry, objBits, tables, keyIndex);
    emitStaticSlotPublish(builder, abi, tables, objBits, keyIndex, staticSlot, staticCellIndex,
                          /*forWrite=*/false, slowDoneBb, "get");
    builder.SetInsertPoint(slowDoneBb);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    // inlineHitBb, overflowAccessBb, slowBb, protoLoadSuccessBb, getCallBb,
    // getUndefBb, absentHitBb
    unsigned phiCount = 7;
    if (staticGuard.hitBb) phiCount++;
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
    result->addIncoming(slowVal, slowDoneBb);
    if (staticGuard.hitBb) result->addIncoming(staticGuard.value, staticGuard.hitBb);
    result->addIncoming(protoHitVal, protoLoadSuccessBb);
    result->addIncoming(getterRes, getCallBb);
    result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), getUndefBb);
    result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), absentHitBb);
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
