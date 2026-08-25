#include "codegen-llvm/llvm_method_call.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/MDBuilder.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_prop_ic.h"

namespace bronze::codegen_llvm {

static constexpr uint32_t kPadSlots = 16;

llvm::Value* emitMethodCallInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                  const AbiGlobals& globals, const ModuleTables& tables,
                                  llvm::Value* thisVal, uint32_t keyIndex, uint32_t icIndex,
                                  uint32_t argc, llvm::Value* argv) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::Value* entry = icEntryPtr(builder, tables.icTable, icIndex);
    llvm::Value* safeArgv = argv ? argv : llvm::ConstantPointerNull::get(ptrTy);
    llvm::Value* undefVal = builder.getInt64(BRONZE_ABI_UNDEFINED_BITS);

    llvm::BasicBlock* plainBb = llvm::BasicBlock::Create(ctx, "mic.plain", fn);
    llvm::BasicBlock* shapeBb = llvm::BasicBlock::Create(ctx, "mic.shape", fn);
    llvm::BasicBlock* exoticBb = llvm::BasicBlock::Create(ctx, "mic.exotic", fn);
    llvm::BasicBlock* exoticBoxBb = llvm::BasicBlock::Create(ctx, "mic.exotic.box", fn);
    llvm::BasicBlock* hitBb = llvm::BasicBlock::Create(ctx, "mic.hit", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "mic.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "mic.done", fn);

    // 1. Feature enable check & Object tag check
    auto* enabled = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_method_call_ic_enabled, llvm::Align(8), "mic.enabled");
    markInvariant(enabled, ctx);
    llvm::Value* isEnabled = builder.CreateICmpNE(enabled, builder.getInt64(0), "mic.isenabled");

    llvm::Value* tag = builder.CreateLShr(thisVal, BRONZE_ABI_VALUE_TAG_SHIFT, "mic.tag");
    llvm::Value* isObj =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "mic.isobj");
    llvm::Value* checkOk = builder.CreateAnd(isEnabled, isObj, "mic.checkok");
    auto* brCheck = builder.CreateCondBr(checkOk, plainBb, slowBb);
    brCheck->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 2. Plain Object check (flags == BRONZE_ABI_OBJ_FLAGS_PLAIN)
    builder.SetInsertPoint(plainBb);
    llvm::Value* addr = builder.CreateAnd(thisVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "mic.hdr");
    auto* flags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "mic.flags");
    markInvariant(flags, ctx);
    llvm::Value* isPlain =
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN), "mic.isplain");
    builder.CreateCondBr(isPlain, shapeBb, exoticBb);


    // 2b. EXOTIC receiver (Array, collection, typed-array view, or a global
    // constructor): the entry may hold the kind-guarded direct form
    // (bronze_abi.h's method-site contract). Word 0's low half, its guard-kind
    // bit masked off, must be ((kind << 2) | 1) for the LIVE receiver's kind —
    // an odd word can never be a Shape*, and a never-latched or plain-latched
    // entry can never be odd, so this one compare is the whole form dispatch.
    builder.SetInsertPoint(exoticBb);
    llvm::Value* exoWord0 = builder.CreateAlignedLoad(i64Ty, entry, llvm::Align(8), "mic.exo.word0");
    llvm::Value* exoLow = builder.CreateAnd(
        exoWord0, builder.getInt64(0xFFFFFFFFull & ~BRONZE_ABI_METHOD_IC_CODE_GUARD_BIT),
        "mic.exo.low");
    llvm::Value* flags64 = builder.CreateZExt(flags, i64Ty, "mic.exo.flags64");
    llvm::Value* exoExpect = builder.CreateOr(
        builder.CreateShl(flags64, BRONZE_ABI_METHOD_IC_KIND_SHIFT),
        builder.getInt64(BRONZE_ABI_METHOD_IC_EXOTIC_BIT), "mic.exo.expect");
    llvm::Value* exoKindOk = builder.CreateICmpEQ(exoLow, exoExpect, "mic.exo.kindok");
    builder.CreateCondBr(exoKindOk, exoticBoxBb, slowBb);

    // The guard's second clause: load the u64 at the aux offset word 0
    // carries in its high half, then ask the question bit 1 selects.
    //   BOX guard (bit clear): the receiver's named-property box — the ONLY
    //   thing that can shadow a table method (an own property, or a subclass
    //   prototype chain hanging off it) — must not be Object-tagged; a
    //   receiver carrying one takes the helper, which walks the box first.
    //   CODE guard (bit set): the word — the receiver Function's code
    //   pointer — must equal the site's aux word, pinning the receiver to
    //   the one global constructor the entry was latched against.
    builder.SetInsertPoint(exoticBoxBb);
    llvm::Value* auxOff = builder.CreateLShr(
        exoWord0, builder.getInt64(BRONZE_ABI_METHOD_IC_BOX_SHIFT), "mic.exo.auxoff");
    llvm::Value* auxPtr = builder.CreateGEP(i8Ty, hdr, auxOff, "mic.exo.auxptr");
    llvm::Value* auxVal =
        builder.CreateAlignedLoad(i64Ty, auxPtr, llvm::Align(8), "mic.exo.auxval");
    llvm::Value* guardKind = builder.CreateAnd(
        exoWord0, builder.getInt64(BRONZE_ABI_METHOD_IC_CODE_GUARD_BIT), "mic.exo.guardkind");
    llvm::Value* isCodeGuard = builder.CreateICmpNE(
        guardKind, builder.getInt64(0), "mic.exo.iscodeguard");

    llvm::BasicBlock* exoCodeBb = llvm::BasicBlock::Create(ctx, "mic.exotic.code", fn);
    llvm::BasicBlock* exoBoxChkBb = llvm::BasicBlock::Create(ctx, "mic.exotic.boxchk", fn);
    builder.CreateCondBr(isCodeGuard, exoCodeBb, exoBoxChkBb);

    builder.SetInsertPoint(exoCodeBb);
    llvm::Value* auxWord = builder.CreateAlignedLoad(
        i64Ty,
        builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_AUX_WORD),
        llvm::Align(8), "mic.exo.auxword");
    llvm::Value* codeMatch = builder.CreateICmpEQ(auxVal, auxWord, "mic.exo.codematch");
    // An exotic entry is always the DIRECT form (word 2's high half is zero),
    // so the shared hit dispatch reads code/arity/env exactly as a shape hit
    // would.
    builder.CreateCondBr(codeMatch, hitBb, slowBb);

    builder.SetInsertPoint(exoBoxChkBb);
    llvm::Value* boxTag =
        builder.CreateLShr(auxVal, BRONZE_ABI_VALUE_TAG_SHIFT, "mic.exo.boxtag");
    llvm::Value* boxIsObj = builder.CreateICmpEQ(
        boxTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "mic.exo.boxisobj");
    builder.CreateCondBr(boxIsObj, slowBb, hitBb);

    // 3. Shape comparison (receiver shape == icEntry[0]); a way-0 miss falls
    // to the site's WAY 1 (words 6-9, bronze_abi.h's site contract) before
    // the helper — the polymorphic-site arm, plain DIRECT entries only.
    builder.SetInsertPoint(shapeBb);
    llvm::Value* shape = builder.CreateAlignedLoad(
        ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
        llvm::Align(8), "mic.shape");
    llvm::Value* cachedShapeInt =
        builder.CreateAlignedLoad(i64Ty, entry, llvm::Align(8), "mic.cachedshape");
    llvm::Value* cachedShape = builder.CreateIntToPtr(cachedShapeInt, ptrTy, "mic.cachedshapeptr");
    llvm::Value* shapeMatch = builder.CreateICmpEQ(shape, cachedShape, "mic.shapematch");
    llvm::BasicBlock* way1Bb = llvm::BasicBlock::Create(ctx, "mic.way1", fn);
    auto* brShape = builder.CreateCondBr(shapeMatch, hitBb, way1Bb);
    brShape->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 3b. WAY 1: shape against word 6; a hit reads the direct triple from
    // words 7-9 and joins the dispatch below with no form split — displacement
    // only ever writes a plain DIRECT resident here. A never-filled way 1 is
    // word 6 == 0, which no real shape equals.
    builder.SetInsertPoint(way1Bb);
    llvm::Value* w1ShapeInt = builder.CreateAlignedLoad(
        i64Ty,
        builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD),
        llvm::Align(8), "mic.w1.shape");
    llvm::Value* w1Shape = builder.CreateIntToPtr(w1ShapeInt, ptrTy, "mic.w1.shapeptr");
    llvm::Value* w1Match = builder.CreateICmpEQ(shape, w1Shape, "mic.w1.match");
    llvm::BasicBlock* way1HitBb = llvm::BasicBlock::Create(ctx, "mic.way1.hit", fn);
    builder.CreateCondBr(w1Match, way1HitBb, slowBb);

    builder.SetInsertPoint(way1HitBb);
    llvm::Value* w1Code = builder.CreateAlignedLoad(
        i64Ty,
        builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_WAY1_CODE_WORD),
        llvm::Align(8), "mic.w1.code");
    llvm::Value* w1ArityWord = builder.CreateAlignedLoad(
        i64Ty,
        builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_WAY1_ARITY_WORD),
        llvm::Align(8), "mic.w1.arityword");
    llvm::Value* w1Env = builder.CreateAlignedLoad(
        i64Ty,
        builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD),
        llvm::Align(8), "mic.w1.env");
    llvm::Value* w1Arity = builder.CreateTrunc(w1ArityWord, i32Ty, "mic.w1.arity");
    // Falls through to the join built after the way-0 hit dispatch below;
    // the branch is created there once the join block exists.

    // 4. Hit: the entry's word 2 selects the form (bronze_abi.h's method-site
    // contract). High half zero is the DIRECT form — cached code, cached env,
    // cached arity. Nonzero is the SLOT form carrying the receiver's own slot
    // index plus one: the callee lives in that slot NOW, so code, env and
    // arity are all read from the function object found there, never cached.
    builder.SetInsertPoint(hitBb);
    llvm::Value* arityWord = builder.CreateAlignedLoad(
        i64Ty,
        builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_ARITY_WORD),
        llvm::Align(8), "mic.arityword");
    llvm::Value* formBits = builder.CreateLShr(
        arityWord, BRONZE_ABI_METHOD_IC_SLOT_SHIFT, "mic.formbits");
    llvm::Value* isSlotForm =
        builder.CreateICmpNE(formBits, builder.getInt64(0), "mic.isslotform");

    llvm::BasicBlock* directBb = llvm::BasicBlock::Create(ctx, "mic.direct", fn);
    llvm::BasicBlock* slotBb = llvm::BasicBlock::Create(ctx, "mic.slot", fn);
    llvm::BasicBlock* joinBb = llvm::BasicBlock::Create(ctx, "mic.join", fn);
    builder.CreateCondBr(isSlotForm, slotBb, directBb);

    // 4a. Direct form: everything the call needs is in the entry.
    builder.SetInsertPoint(directBb);
    llvm::Value* directCode = builder.CreateAlignedLoad(
        i64Ty,
        builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_CODE_WORD),
        llvm::Align(8), "mic.dircode");
    llvm::Value* directEnv = builder.CreateAlignedLoad(
        i64Ty,
        builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_ENV_WORD),
        llvm::Align(8), "mic.direnv");
    llvm::Value* directArity = builder.CreateTrunc(arityWord, i32Ty, "mic.dirarity");
    builder.CreateBr(joinBb);

    // 4b. Slot form: load the receiver's own slot, inline or overflow.
    builder.SetInsertPoint(slotBb);
    llvm::Value* slotIdx =
        builder.CreateSub(formBits, builder.getInt64(1), "mic.slotidx");
    llvm::Value* isInline = builder.CreateICmpULT(
        slotIdx, builder.getInt64(BRONZE_ABI_OBJ_INLINE_SLOTS), "mic.isinline");

    llvm::BasicBlock* slotInlBb = llvm::BasicBlock::Create(ctx, "mic.slot.inl", fn);
    llvm::BasicBlock* slotOvBb = llvm::BasicBlock::Create(ctx, "mic.slot.ov", fn);
    llvm::BasicBlock* slotLoadBb = llvm::BasicBlock::Create(ctx, "mic.slot.load", fn);
    builder.CreateCondBr(isInline, slotInlBb, slotOvBb);

    builder.SetInsertPoint(slotInlBb);
    llvm::Value* inlBase =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SLOTS_OFFSET);
    llvm::Value* inlPtr = builder.CreateGEP(i64Ty, inlBase, slotIdx, "mic.slot.inlptr");
    builder.CreateBr(slotLoadBb);

    // The overflow block is a heap object whose payload is a Value array; the
    // shape that just matched guarantees the block exists and covers the
    // cached index (ObjectHeader::getSlot states the invariant).
    builder.SetInsertPoint(slotOvBb);
    llvm::Value* ovBits = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_OVERFLOW_OFFSET),
        llvm::Align(8), "mic.slot.ovbits");
    llvm::Value* ovAddr =
        builder.CreateAnd(ovBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* ovHdr = builder.CreateIntToPtr(ovAddr, ptrTy, "mic.slot.ovhdr");
    llvm::Value* ovBase = builder.CreateConstInBoundsGEP1_32(i8Ty, ovHdr, BRONZE_ABI_HDR_BYTES);
    llvm::Value* ovIdx = builder.CreateSub(
        slotIdx, builder.getInt64(BRONZE_ABI_OBJ_INLINE_SLOTS), "mic.slot.ovidx");
    llvm::Value* ovPtr = builder.CreateGEP(i64Ty, ovBase, ovIdx, "mic.slot.ovptr");
    builder.CreateBr(slotLoadBb);

    builder.SetInsertPoint(slotLoadBb);
    llvm::PHINode* slotPtr = builder.CreatePHI(ptrTy, 2, "mic.slot.ptr");
    slotPtr->addIncoming(inlPtr, slotInlBb);
    slotPtr->addIncoming(ovPtr, slotOvBb);
    llvm::Value* slotVal =
        builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "mic.slot.val");

    // The slot may hold ANYTHING a same-shape write put there; only a Function
    // heap object dispatches directly, everything else takes the helper and
    // its TypeError — the same split bronze_dynamic_call performs.
    llvm::Value* vTag = builder.CreateLShr(slotVal, BRONZE_ABI_VALUE_TAG_SHIFT, "mic.slot.tag");
    llvm::Value* vIsObj =
        builder.CreateICmpEQ(vTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "mic.slot.isobj");
    llvm::BasicBlock* slotFnBb = llvm::BasicBlock::Create(ctx, "mic.slot.fn", fn);
    builder.CreateCondBr(vIsObj, slotFnBb, slowBb);

    builder.SetInsertPoint(slotFnBb);
    llvm::Value* fnAddr =
        builder.CreateAnd(slotVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* fnPtr = builder.CreateIntToPtr(fnAddr, ptrTy, "mic.slot.fnptr");
    auto* fnFlags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "mic.slot.fnflags");
    markInvariant(fnFlags, ctx);
    llvm::Value* isFn = builder.CreateICmpEQ(
        fnFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_FUNCTION), "mic.slot.isfn");
    llvm::BasicBlock* slotOkBb = llvm::BasicBlock::Create(ctx, "mic.slot.ok", fn);
    builder.CreateCondBr(isFn, slotOkBb, slowBb);

    builder.SetInsertPoint(slotOkBb);
    auto* slotCode = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_CODE_OFFSET),
        llvm::Align(8), "mic.slot.code");
    markInvariant(slotCode, ctx);
    auto* slotEnv = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_ENV_OFFSET),
        llvm::Align(8), "mic.slot.env");
    markInvariant(slotEnv, ctx);
    auto* slotArity = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_ARITY_OFFSET),
        llvm::Align(4), "mic.slot.arity");
    markInvariant(slotArity, ctx);
    builder.CreateBr(joinBb);

    // The way-1 hit joins here too — its branch is created now that the join
    // block exists (the loads were emitted in 3b, before the hit dispatch).
    builder.SetInsertPoint(way1HitBb);
    builder.CreateBr(joinBb);

    // 4c. All three ways meet with (code, env, arity) resolved.
    builder.SetInsertPoint(joinBb);
    llvm::PHINode* codeInt = builder.CreatePHI(i64Ty, 3, "mic.codeint");
    codeInt->addIncoming(directCode, directBb);
    codeInt->addIncoming(slotCode, slotOkBb);
    codeInt->addIncoming(w1Code, way1HitBb);
    llvm::PHINode* envVal = builder.CreatePHI(i64Ty, 3, "mic.env");
    envVal->addIncoming(directEnv, directBb);
    envVal->addIncoming(slotEnv, slotOkBb);
    envVal->addIncoming(w1Env, way1HitBb);
    llvm::PHINode* arity = builder.CreatePHI(i32Ty, 3, "mic.arity");
    arity->addIncoming(directArity, directBb);
    arity->addIncoming(slotArity, slotOkBb);
    arity->addIncoming(w1Arity, way1HitBb);
    llvm::Value* codePtr = builder.CreateIntToPtr(codeInt, ptrTy, "mic.codeptr");

    llvm::Value* directArityOk =
        builder.CreateICmpULE(arity, builder.getInt32(argc), "mic.dirarityok");

    llvm::BasicBlock* dispatchBb = llvm::BasicBlock::Create(ctx, "mic.dispatch", fn);
    llvm::BasicBlock* underArityCheckBb =
        llvm::BasicBlock::Create(ctx, "mic.underarity.check", fn);

    builder.CreateCondBr(directArityOk, dispatchBb, underArityCheckBb);

    // 4d. Under-arity check
    builder.SetInsertPoint(underArityCheckBb);
    llvm::Value* canPad =
        builder.CreateICmpULE(arity, builder.getInt32(kPadSlots), "mic.canpad");
    llvm::BasicBlock* underArityBb = llvm::BasicBlock::Create(ctx, "mic.underarity", fn);
    builder.CreateCondBr(canPad, underArityBb, slowBb);

    // 4e. Under-arity padding
    builder.SetInsertPoint(underArityBb);
    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    llvm::Value* padBuf = entryBuilder.CreateAlloca(
        llvm::ArrayType::get(i64Ty, kPadSlots), nullptr, "mic.padbuf");

    if (argc > 0 && argv) {
        for (uint32_t a = 0; a < argc && a < kPadSlots; ++a) {
            llvm::Value* srcPtr = builder.CreateGEP(i64Ty, argv, builder.getInt32(a));
            llvm::Value* val = builder.CreateAlignedLoad(i64Ty, srcPtr, llvm::Align(8));
            llvm::Value* dstPtr = builder.CreateConstInBoundsGEP2_32(
                llvm::ArrayType::get(i64Ty, kPadSlots), padBuf, 0, a);
            builder.CreateAlignedStore(val, dstPtr, llvm::Align(8));
        }
    }
    for (uint32_t a = argc; a < kPadSlots; ++a) {
        llvm::Value* dstPtr = builder.CreateConstInBoundsGEP2_32(
            llvm::ArrayType::get(i64Ty, kPadSlots), padBuf, 0, a);
        builder.CreateAlignedStore(undefVal, dstPtr, llvm::Align(8));
    }

    llvm::Value* padArgv = builder.CreateConstInBoundsGEP2_32(
        llvm::ArrayType::get(i64Ty, kPadSlots), padBuf, 0, 0);

    llvm::FunctionType* codeTy =
        llvm::FunctionType::get(i64Ty, {i64Ty, i64Ty, i32Ty, ptrTy}, false);
    llvm::Value* padRes = builder.CreateCall(
        codeTy, codePtr, {envVal, thisVal, builder.getInt32(argc), padArgv}, "mic.padres");
    llvm::BasicBlock* padEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 4f. Direct dispatch (argc >= arity)
    builder.SetInsertPoint(dispatchBb);
    llvm::Value* fastRes = builder.CreateCall(
        codeTy, codePtr, {envVal, thisVal, builder.getInt32(argc), safeArgv}, "mic.fastres");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 5. Slow path: runtime helper
    builder.SetInsertPoint(slowBb);
    llvm::Value* keyId = emitKeyId(builder, tables, keyIndex);
    llvm::Value* slowRes = builder.CreateCall(
        abi.bronze_call_method, {thisVal, keyId, builder.getInt32(argc), safeArgv, entry},
        "mic.slowres");
    llvm::BasicBlock* slowEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 6. Merge result
    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 3, "mic.result");
    result->addIncoming(fastRes, fastEndBb);
    result->addIncoming(padRes, padEndBb);
    result->addIncoming(slowRes, slowEndBb);
    return result;
}

MethodDirectGuard emitMethodDirectGuard(llvm::IRBuilder<>& builder, const AbiGlobals& globals,
                                        const ModuleTables& tables, llvm::Value* thisVal,
                                        uint32_t icIndex, llvm::Function* wrapper,
                                        bool needsEnv) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::MDNode* likely = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::Value* entry = icEntryPtr(builder, tables.icTable, icIndex);

    llvm::BasicBlock* plainBb = llvm::BasicBlock::Create(ctx, "mdc.plain", fn);
    llvm::BasicBlock* shapeBb = llvm::BasicBlock::Create(ctx, "mdc.shape", fn);
    llvm::BasicBlock* formBb = llvm::BasicBlock::Create(ctx, "mdc.form", fn);
    llvm::BasicBlock* codeBb = llvm::BasicBlock::Create(ctx, "mdc.code", fn);
    llvm::BasicBlock* hitBb = llvm::BasicBlock::Create(ctx, "mdc.hit", fn);
    llvm::BasicBlock* missBb = llvm::BasicBlock::Create(ctx, "mdc.miss", fn);

    auto* enabled = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_method_call_ic_enabled, llvm::Align(8), "mdc.enabled");
    markInvariant(enabled, ctx);
    llvm::Value* isEnabled = builder.CreateICmpNE(enabled, builder.getInt64(0), "mdc.isenabled");
    llvm::Value* tag = builder.CreateLShr(thisVal, BRONZE_ABI_VALUE_TAG_SHIFT, "mdc.tag");
    llvm::Value* isObj =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "mdc.isobj");
    auto* br0 = builder.CreateCondBr(builder.CreateAnd(isEnabled, isObj), plainBb, missBb);
    br0->setMetadata(llvm::LLVMContext::MD_prof, likely);

    // A non-plain heap object has no shape word at that offset, so the flags
    // test is what makes the load below a load of a shape at all — the same
    // rule llvm_static_slot.cpp states.
    builder.SetInsertPoint(plainBb);
    llvm::Value* addr = builder.CreateAnd(thisVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "mdc.hdr");
    auto* flags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "mdc.flags");
    markInvariant(flags, ctx);
    auto* br1 = builder.CreateCondBr(
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN)), shapeBb, missBb);
    br1->setMetadata(llvm::LLVMContext::MD_prof, likely);

    builder.SetInsertPoint(shapeBb);
    llvm::Value* shape = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
        llvm::Align(8), "mdc.shape");
    llvm::Value* cachedShape =
        builder.CreateAlignedLoad(i64Ty, entry, llvm::Align(8), "mdc.cachedshape");
    auto* br2 =
        builder.CreateCondBr(builder.CreateICmpEQ(shape, cachedShape), formBb, missBb);
    br2->setMetadata(llvm::LLVMContext::MD_prof, likely);

    // Only the DIRECT form (word 2's high half zero) caches a code pointer. The
    // SLOT form says the callee lives in one of the receiver's OWN slots right
    // now — a per-instance closure or a host function, never a prototype method
    // this module compiled — so there is nothing here to pin.
    builder.SetInsertPoint(formBb);
    llvm::Value* arityWord = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_ARITY_WORD),
        llvm::Align(8), "mdc.arityword");
    llvm::Value* formBits =
        builder.CreateLShr(arityWord, BRONZE_ABI_METHOD_IC_SLOT_SHIFT, "mdc.formbits");
    auto* br3 = builder.CreateCondBr(
        builder.CreateICmpEQ(formBits, builder.getInt64(0)), codeBb, missBb);
    br3->setMetadata(llvm::LLVMContext::MD_prof, likely);

    // The one question the cache does not already answer for itself, and the
    // whole of what makes the guessed callee sound.
    builder.SetInsertPoint(codeBb);
    llvm::Value* code = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_CODE_WORD),
        llvm::Align(8), "mdc.code");
    llvm::Value* want = builder.CreatePtrToInt(wrapper, i64Ty, "mdc.want");
    auto* br4 = builder.CreateCondBr(builder.CreateICmpEQ(code, want), hitBb, missBb);
    br4->setMetadata(llvm::LLVMContext::MD_prof, likely);

    MethodDirectGuard out;
    out.hit = hitBb;
    out.miss = missBb;
    if (needsEnv) {
        builder.SetInsertPoint(hitBb);
        out.env = builder.CreateAlignedLoad(
            i64Ty, builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_ENV_WORD),
            llvm::Align(8), "mdc.env");
    }
    builder.SetInsertPoint(missBb);
    return out;
}

llvm::Value* emitMethodCallSpreadInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                        const AbiGlobals& globals, const ModuleTables& tables,
                                        llvm::Value* thisVal, uint32_t keyIndex, uint32_t icIndex,
                                        llvm::Value* argsArr) {
    (void)globals;
    llvm::Value* entry = icEntryPtr(builder, tables.icTable, icIndex);
    llvm::Value* keyId = emitKeyId(builder, tables, keyIndex);
    return builder.CreateCall(
        abi.bronze_call_method_spread, {thisVal, keyId, argsArr, entry}, "mic.spreadres");
}

}  // namespace bronze::codegen_llvm
