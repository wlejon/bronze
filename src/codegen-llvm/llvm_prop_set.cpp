// Property WRITES in generated code: the inlined own-slot hit, the
// shape-transition hit a constructor body's repeated property add takes, and
// the array / typed-array element stores a constant index opens. Every guard
// miss falls out to bronze_prop_set.
//
// Apart from the reads in llvm_prop_get.cpp because the two are independent
// emitters that share no state and no block — only the cache primitives in
// llvm_prop_ic.h, which is where a guard both must agree on lives.
//
// The one exception is the ARRAY STORE-RUN PROOF's arm, which sits in front of
// everything below and belongs to a RUN of stores rather than to this one
// (llvm_array_store_proof.h). It arrives as a parameter for the reason the read
// twin's does: the proof is established, spent and re-joined by the emitter in
// llvm_ops_access.cpp, and this file only lends it the join it was going to
// build anyway.

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

void emitPropSet(llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
                 const ModuleTables& tables, llvm::Value* objBits, llvm::Value* objSlot,
                 uint32_t keyIndex, llvm::Value* valBits, uint32_t icIndex, bool strict,
                 bool monomorphic, const StaticSite& site, ValueRepr valRepr,
                 std::string_view keyStr, ArrayStoreProof* proof, ProofJoin* join) {
    // See the read twin: an identity proof does not change what is emitted; the
    // LAYOUT proof beside it does.
    (void)monomorphic;
    llvm::Value* entry = icEntryPtr(builder, tables.icTable, icIndex);

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

    // 0. The static-slot fast path, in front of everything. It is a bare store
    //    at a constant offset, which is legal only because the published shape
    //    was checked WRITABLE before the cell was filled. A property that is
    //    not yet installed, or an object of another shape, misses here and
    //    reaches the transition fast path below unchanged — which is the whole
    //    reason this is emitted in front of that path rather than instead of it:
    //    a constructor's repeated `this.x = ...` IS the transition path, and it
    //    is not a case a static slot could serve.
    const StaticSlotGuard staticGuard = emitStaticSlotGuard(
        builder, tables, objBits, site, doneBb, valBits, valRepr, "set");

    auto optIdx = parseIndexKey(keyStr);

    // 0b. The ARRAY STORE-RUN PROOF's arm (llvm_array_store_proof.h). A run of
    //     adjacent constant-index writes into one Array spends the ladder ONCE,
    //     which leaves each member a GEP and an eight-byte store, and everything
    //     below it is the arm taken when the proof was refused or lost. Nothing
    //     is emitted when there is no live proof, which is every site outside a
    //     run.
    ProvenArrayStore proven;
    if (proof != nullptr && proof->live() && optIdx.has_value()) {
        proven = emitProvenArrayElementStore(builder, *proof, *optIdx, valBits, doneBb);
    }

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
        auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "arr.len");
        tagArrayHeaderAccess(len, ctx);
        llvm::Value* capPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
        auto* cap = builder.CreateAlignedLoad(i32Ty, capPtr, llvm::Align(4), "arr.cap");
        tagArrayHeaderAccess(cap, ctx);
        llvm::Value* headPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                 BRONZE_ABI_ARRAY_HEAD_OFFSET);
        auto* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), "arr.head");
        tagArrayHeaderAccess(head, ctx);
        llvm::Value* actualIdx = builder.CreateAdd(head, builder.getInt32(idx), "arr.actidx");
        llvm::Value* propsPtr = builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                                  BRONZE_ABI_ARRAY_PROPS_OFFSET);
        auto* propsVal = builder.CreateAlignedLoad(i64Ty, propsPtr, llvm::Align(8), "arr.props");
        tagArrayHeaderAccess(propsVal, ctx);
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
        auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "arr.elems");
        tagArrayHeaderAccess(elemsVal, ctx);
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
        auto* sArr = builder.CreateAlignedStore(valBits, slotPtr, llvm::Align(8));
        tagArrayElementsAccess(sArr, ctx);
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
        tagViewLengthAccess(taLen, ctx);
        llvm::BasicBlock* taKindBb = llvm::BasicBlock::Create(ctx, "ic.set.ta.kind", fn);
        // Out of bounds discards inline for the Number kinds only; a BigInt
        // kind still owes ToBigInt's throw for a Number value, so it takes
        // the helper (llvm_elem.cpp's es.ta.oob says why in full).
        llvm::BasicBlock* taOobBb = llvm::BasicBlock::Create(ctx, "ic.set.ta.oob", fn);
        builder.CreateCondBr(builder.CreateICmpULT(builder.getInt32(idx), taLen), taKindBb,
                             taOobBb);

        builder.SetInsertPoint(taOobBb);
        llvm::Value* oobKindPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_KIND_OFFSET);
        auto* oobKind =
            builder.CreateAlignedLoad(i32Ty, oobKindPtr, llvm::Align(4), "ic.set.ta.oob.kind");
        markInvariant(oobKind, ctx);
        llvm::Value* oobIsNumberKind =
            builder.CreateICmpULT(oobKind, builder.getInt32(BRONZE_ABI_TA_KIND_BIGINT64));
        builder.CreateCondBr(oobIsNumberKind, doneBb, slowBb);

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

    // The f64-slot arm. An entry may name a slot whose shape says the eight
    // bytes there ARE a double (slot_repr.h), and this store may still take it
    // — the bits of a boxed Number are the double's bits — but only for a
    // Number. Anything else misses to bronze_prop_set, which generalizes the
    // slot back to boxed and refills the entry without the flag, so the miss
    // is taken once per field rather than once per store.
    //
    // Both halves are free of the entry's other states: DOUBLE is only ever
    // set with the rest of the word zero, so masking it out and comparing to
    // zero is the same "own property, depth 0" test the arm already made.
    //
    // Stage R2 (llvm_repr.h) settles the value's half of that at COMPILE time
    // wherever it can. A proven Number needs no test — its box IS the canonical
    // double a double slot promises — so `reprOk` is the constant true and
    // LLVM folds the `and`, the `or` and the compare away, leaving the arm the
    // shape of the one that existed before representations. An `Int32`-tagged
    // value is the store R1 documented as always missing: it keeps the flag
    // test, and takes the arm anyway with the payload converted, which is
    // exactly what `slotReprCanonicalize` would have stored from the helper.
    llvm::Value* isDoubleSlot = builder.CreateICmpNE(
        builder.CreateAnd(depth,
                          builder.getInt64(static_cast<uint64_t>(BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG))),
        builder.getInt64(0), "ic.set.isdouble");
    llvm::Value* reprOk = nullptr;
    llvm::Value* storeBits = valBits;
    if (valRepr == ValueRepr::Number) {
        ++reprStats().rawStores;
        reprOk = builder.getTrue();
    } else if (valRepr == ValueRepr::Int32Boxed) {
        ++reprStats().sitofpStores;
        reprOk = builder.getTrue();
        storeBits = builder.CreateSelect(isDoubleSlot, emitInt32BoxAsDouble(builder, valBits),
                                         valBits, "ic.set.i32store");
    } else {
        llvm::Value* valIsNumber = builder.CreateICmpULE(
            valBits, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "ic.set.valisnum");
        reprOk = builder.CreateOr(builder.CreateNot(isDoubleSlot), valIsNumber, "ic.set.reprok");
    }
    llvm::Value* depthBase = builder.CreateAnd(
        depth, builder.getInt64(~static_cast<uint64_t>(BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG)),
        "ic.set.depthbase");
    llvm::Value* depthOk = builder.CreateAnd(
        builder.CreateICmpEQ(depthBase, builder.getInt64(0)), reprOk, "ic.set.depthok");

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
        // Same f64-slot test as the own-property arm above, for the same
        // reason: the transition arm's store is a bare one too, and the node
        // this entry caches is the node that decides the slot's
        // representation. A constructor's `this.x = x` is this arm, so this is
        // where a pinned field is BORN a double one.
        llvm::Value* transDepth = builder.CreateLShr(slotWord, 32);
        llvm::Value* transDepthBase = builder.CreateAnd(
            transDepth, builder.getInt64(~static_cast<uint64_t>(BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG)),
            "trans.depthbase");
        llvm::Value* transDepthOk = builder.CreateAnd(
            builder.CreateICmpEQ(transDepthBase, builder.getInt64(0)), reprOk, "trans.depthok");
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
        auto* sTrans = builder.CreateAlignedStore(storeBits, transSlotPtr, llvm::Align(8));
        tagObjectSlotAccess(sTrans, ctx);
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
        // `slotIdx` is a WORD index from the block's start — the header word is
        // word 0 and slot k lives at word k - kInlineSlots + 1 — so the exact
        // in-bounds test is `slotIdx < size/8` (total words). The old
        // `slotIdx < size/8 - 1` compared a header-inclusive index against a
        // header-exclusive capacity and permanently refused the block's LAST
        // slot: every store to the final overflow slot of a full block —
        // three.js's `renderItem.group`, 1.8M helper calls a run — missed here.
        llvm::Value* wordCount = builder.CreateLShr(sizeVal, 3, "overflow.words");
        llvm::Value* slotIdx = builder.CreateSub(transSlot32, builder.getInt32(3));
        llvm::Value* withinCap = builder.CreateICmpULT(slotIdx, wordCount, "trans.withincap");
        builder.CreateCondBr(withinCap, transOverflowAccessBb, slowBb);

        builder.SetInsertPoint(transOverflowAccessBb);
        llvm::Value* overflowShapeSlotPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
        builder.CreateAlignedStore(cachedShape, overflowShapeSlotPtr, llvm::Align(8));
        llvm::Value* overflowSlotPtr =
            builder.CreateInBoundsGEP(i64Ty, overflowObj, slotIdx);
        auto* sTransOv = builder.CreateAlignedStore(storeBits, overflowSlotPtr, llvm::Align(8));
        tagObjectSlotAccess(sTransOv, ctx);
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
    auto* sInline = builder.CreateAlignedStore(storeBits, inlineSlotPtr, llvm::Align(8));
    tagObjectSlotAccess(sInline, ctx);
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
    // Word-index bound, exactly as the transition arm above states it: the
    // valid word indices are 1..size/8-1, so `slotIdx < size/8` is the test
    // and the old `- 1` refused the last overflow slot forever.
    llvm::Value* wordCount = builder.CreateLShr(sizeVal, 3, "overflow.words");
    llvm::Value* slotIdx = builder.CreateSub(slot32, builder.getInt32(3));
    llvm::Value* withinCap = builder.CreateICmpULT(slotIdx, wordCount, "set.withincap");
    builder.CreateCondBr(withinCap, overflowAccessBb, slowBb);

    builder.SetInsertPoint(overflowAccessBb);
    llvm::Value* overflowSlotPtr = builder.CreateInBoundsGEP(i64Ty, overflowObj, slotIdx);
    auto* sOv = builder.CreateAlignedStore(storeBits, overflowSlotPtr, llvm::Align(8));
    tagObjectSlotAccess(sOv, ctx);
    builder.CreateBr(doneBb);

    // 5. Fallback call, then the one-shot publish. After the helper, so that a
    //    constructor's FIRST write — which installs the property and is
    //    therefore a shape transition, not a slot store — publishes the shape
    //    the object ends up with rather than the one it arrived with.
    llvm::BasicBlock* slowDoneBb = llvm::BasicBlock::Create(ctx, "ic.set.slow.done", fn);
    builder.SetInsertPoint(slowBb);
    builder.CreateCall(abi.bronze_prop_set,
                       {objBits, emitKeyId(builder, tables, keyIndex), valBits, entry,
                        builder.getInt1(strict)});
    emitStaticSlotPublish(builder, abi, tables, objBits, objSlot, keyIndex, site,
                          /*forWrite=*/true, slowDoneBb, "set");
    builder.SetInsertPoint(slowDoneBb);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);

    // What this site left for anything that has to cross its join — this run's
    // own proof just below, and any OTHER live proof, in the caller.
    if (join != nullptr) {
        join->fastBb = proven.fastBb;
        join->doneBb = doneBb;
    }

    // The proof that reaches the NEXT member of the run: carried forward by the
    // fast arm alone. Every other predecessor of `doneBb` got here through an
    // arm that can call bronze_prop_set and therefore collect, which is what
    // the derived base pointer cannot survive.
    if (proof != nullptr) rejoinArrayStoreProof(*proof, proven.fastBb, doneBb);

    (void)staticGuard;
}

}  // namespace bronze::codegen_llvm
