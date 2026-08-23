#include "codegen-llvm/llvm_call.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/MDBuilder.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_prop_ic.h"

namespace bronze::codegen_llvm {

// How many formals the inline call's under-arity path will pad. See the
// comment at the cap check below for why it is not eight.
static constexpr uint32_t kPadSlots = 16;

llvm::Value* emitDynamicCallInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                   const AbiGlobals& globals, llvm::Value* callee,
                                   llvm::Value* thisVal, uint32_t argc, llvm::Value* argv,
                                   llvm::Function* knownWrapper) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::BasicBlock* fnBb = llvm::BasicBlock::Create(ctx, "call.fn", fn);
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, "call.fast", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "call.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "call.done", fn);

    // 1. Is the callee an object?
    llvm::Value* tag = builder.CreateLShr(callee, BRONZE_ABI_VALUE_TAG_SHIFT, "call.tag");
    llvm::Value* isObj =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "call.isobj");
    auto* brObj = builder.CreateCondBr(isObj, fnBb, slowBb);
    brObj->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 2. Is it a FunctionHeapObject?
    builder.SetInsertPoint(fnBb);
    llvm::Value* calleeAddr =
        builder.CreateAnd(callee, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* fnPtr = builder.CreateIntToPtr(calleeAddr, ptrTy, "call.fnptr");
    auto* flags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "call.flags");
    markInvariant(flags, ctx);
    llvm::Value* isFn = builder.CreateICmpEQ(
        flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_FUNCTION), "call.isfn");
    auto* brFn = builder.CreateCondBr(isFn, fastBb, slowBb);
    brFn->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 3. Arity check and A/B seam check
    builder.SetInsertPoint(fastBb);
    auto* enabled = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_inline_call_enabled, llvm::Align(8), "call.enabled");
    markInvariant(enabled, ctx);
    llvm::Value* enabledOk = builder.CreateICmpNE(enabled, builder.getInt64(0), "call.enabledok");

    auto* arity = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_ARITY_OFFSET),
        llvm::Align(4), "call.arity");
    markInvariant(arity, ctx);
    llvm::Value* directArityOk = builder.CreateICmpULE(arity, builder.getInt32(argc), "call.dirarityok");

    llvm::BasicBlock* dispatchBb = llvm::BasicBlock::Create(ctx, "call.dispatch", fn);
    llvm::BasicBlock* underArityCheckBb = llvm::BasicBlock::Create(ctx, "call.underarity.check", fn);

    llvm::Value* directOk = builder.CreateAnd(directArityOk, enabledOk, "call.directok");
    auto* brDir = builder.CreateCondBr(directOk, dispatchBb, underArityCheckBb);
    brDir->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // If direct arity not ok: pad up to kPadSlots formals with undefined.
    //
    // Sixteen rather than eight, because eight was under the real world: the
    // one call three.js makes 5,000 times a frame that missed this path was
    // `state.setBlending( material.blending )` — one argument into a function
    // declaring TEN formals — and it took the helper for no reason but the
    // cap. The buffer is an entry-block alloca whose live range is this call,
    // so LLVM's stack colouring shares one slot between sites that cannot both
    // be live; the cost of the wider cap is stack bytes in a frame that has
    // them, and the saving is a helper entry per call.
    builder.SetInsertPoint(underArityCheckBb);
    llvm::Value* canPad = builder.CreateAnd(
        enabledOk,
        builder.CreateICmpULE(arity, builder.getInt32(kPadSlots)), "call.canpad");
    llvm::BasicBlock* underArityBb = llvm::BasicBlock::Create(ctx, "call.underarity", fn);
    builder.CreateCondBr(canPad, underArityBb, slowBb);

    // 4a. Under-arity padding path: copy available args and pad with undefined up to 8 slots
    builder.SetInsertPoint(underArityBb);
    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    llvm::Value* padBuf = entryBuilder.CreateAlloca(
        llvm::ArrayType::get(i64Ty, kPadSlots), nullptr, "call.padbuf");

    if (argc > 0 && argv) {
        for (uint32_t a = 0; a < argc && a < kPadSlots; ++a) {
            llvm::Value* srcPtr = builder.CreateGEP(i64Ty, argv, builder.getInt32(a));
            llvm::Value* val = builder.CreateAlignedLoad(i64Ty, srcPtr, llvm::Align(8));
            llvm::Value* dstPtr = builder.CreateConstInBoundsGEP2_32(
                llvm::ArrayType::get(i64Ty, kPadSlots), padBuf, 0, a);
            builder.CreateAlignedStore(val, dstPtr, llvm::Align(8));
        }
    }
    llvm::Value* undefVal = builder.getInt64(BRONZE_ABI_UNDEFINED_BITS);
    for (uint32_t a = argc; a < kPadSlots; ++a) {
        llvm::Value* dstPtr = builder.CreateConstInBoundsGEP2_32(
            llvm::ArrayType::get(i64Ty, kPadSlots), padBuf, 0, a);
        builder.CreateAlignedStore(undefVal, dstPtr, llvm::Align(8));
    }

    llvm::Value* padArgv = builder.CreateConstInBoundsGEP2_32(
        llvm::ArrayType::get(i64Ty, kPadSlots), padBuf, 0, 0);

    auto* padEnv = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_ENV_OFFSET),
        llvm::Align(8), "call.padenv");
    markInvariant(padEnv, ctx);
    llvm::Value* padRes = nullptr;
    if (knownWrapper) {
        padRes = builder.CreateCall(
            knownWrapper, {padEnv, thisVal, builder.getInt32(argc), padArgv}, "call.padres");
    } else {
        auto* padCode = builder.CreateAlignedLoad(
            ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_CODE_OFFSET),
            llvm::Align(8), "call.padcode");
        markInvariant(padCode, ctx);
        llvm::FunctionType* codeTy =
            llvm::FunctionType::get(i64Ty, {i64Ty, i64Ty, i32Ty, ptrTy}, false);
        padRes = builder.CreateCall(
            codeTy, padCode, {padEnv, thisVal, builder.getInt32(argc), padArgv}, "call.padres");
    }
    llvm::BasicBlock* padEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 4b. Fast direct dispatch (argc >= arity)
    builder.SetInsertPoint(dispatchBb);
    auto* env = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_ENV_OFFSET),
        llvm::Align(8), "call.env");
    markInvariant(env, ctx);
    llvm::Value* fastRes = nullptr;
    if (knownWrapper) {
        fastRes = builder.CreateCall(
            knownWrapper, {env, thisVal, builder.getInt32(argc), argv}, "call.fastres");
    } else {
        auto* code = builder.CreateAlignedLoad(
            ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_CODE_OFFSET),
            llvm::Align(8), "call.code");
        markInvariant(code, ctx);
        llvm::FunctionType* codeTy =
            llvm::FunctionType::get(i64Ty, {i64Ty, i64Ty, i32Ty, ptrTy}, false);
        fastRes = builder.CreateCall(
            codeTy, code, {env, thisVal, builder.getInt32(argc), argv}, "call.fastres");
    }
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);


    // 5. Slow path: helper trampoline
    builder.SetInsertPoint(slowBb);
    llvm::Value* slowRes = builder.CreateCall(
        abi.bronze_dynamic_call, {callee, thisVal, builder.getInt32(argc), argv}, "call.slowres");
    builder.CreateBr(doneBb);

    // 6. Merge result
    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 3, "call.result");
    result->addIncoming(fastRes, fastEndBb);
    result->addIncoming(padRes, padEndBb);
    result->addIncoming(slowRes, slowBb);
    return result;
}

llvm::Value* emitArrayPushDirectCall(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                     llvm::Value* calleeBits, llvm::Value* thisBits,
                                     uint32_t argc, llvm::Value* argvPtr,
                                     llvm::Value* argVal) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* thisBb = llvm::BasicBlock::Create(ctx, "push.this", fn);
    llvm::BasicBlock* calleeBb = llvm::BasicBlock::Create(ctx, "push.callee", fn);
    llvm::BasicBlock* capBb = llvm::BasicBlock::Create(ctx, "push.cap", fn);
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, "push.fast", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "push.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "push.done", fn);

    // 1. Guard `this` is an Object
    llvm::Value* thisTag = builder.CreateLShr(thisBits, BRONZE_ABI_VALUE_TAG_SHIFT, "push.thistag");
    llvm::Value* thisIsObj =
        builder.CreateICmpEQ(thisTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "push.thisisobj");
    builder.CreateCondBr(thisIsObj, thisBb, slowBb);

    // 2. Guard `this` is Array with no side properties object
    builder.SetInsertPoint(thisBb);
    llvm::Value* thisAddr =
        builder.CreateAnd(thisBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* thisHdr = builder.CreateIntToPtr(thisAddr, ptrTy, "push.arrhdr");
    llvm::Value* thisFlags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "push.arrflags");
    llvm::Value* isArr =
        builder.CreateICmpEQ(thisFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY), "push.isarr");

    llvm::Value* propsVal = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_PROPS_OFFSET),
        llvm::Align(8), "push.props");
    llvm::Value* propsTag = builder.CreateLShr(propsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* hasNoProps =
        builder.CreateICmpNE(propsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "push.noprops");
    builder.CreateCondBr(builder.CreateAnd(isArr, hasNoProps), calleeBb, slowBb);

    // 3. Guard callee is a function with expected bronze_array_push code pointer
    builder.SetInsertPoint(calleeBb);
    llvm::Value* calleeTag = builder.CreateLShr(calleeBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* calleeIsObj =
        builder.CreateICmpEQ(calleeTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::Value* calleeAddr =
        builder.CreateAnd(calleeBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* calleeHdr = builder.CreateIntToPtr(calleeAddr, ptrTy);
    llvm::Value* calleeFlags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, calleeHdr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2));
    llvm::Value* calleeIsFn =
        builder.CreateICmpEQ(calleeFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_FUNCTION));
    llvm::Value* codePtr = builder.CreateAlignedLoad(
        ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, calleeHdr, BRONZE_ABI_FN_CODE_OFFSET),
        llvm::Align(8));
    llvm::Value* codeOk = builder.CreateICmpEQ(codePtr, abi.bronze_array_push);
    llvm::Value* calleeOk = builder.CreateAnd(calleeIsObj, builder.CreateAnd(calleeIsFn, codeOk));
    builder.CreateCondBr(calleeOk, capBb, slowBb);

    // 4. Capacity check
    builder.SetInsertPoint(capBb);
    llvm::Value* head = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_HEAD_OFFSET),
        llvm::Align(4), "push.head");
    llvm::Value* len = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET),
        llvm::Align(4), "push.len");
    llvm::Value* cap = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET),
        llvm::Align(4), "push.cap");

    llvm::Value* actualSlot = builder.CreateAdd(head, len, "push.actslot");
    llvm::Value* capOk = builder.CreateICmpULT(actualSlot, cap, "push.capok");
    llvm::Value* lenSafe = builder.CreateICmpULT(len, builder.getInt32(0xFFFFFFFEu), "push.lensafe");
    builder.CreateCondBr(builder.CreateAnd(capOk, lenSafe), fastBb, slowBb);

    // 5. Fast path: store element, increment length, return new length
    builder.SetInsertPoint(fastBb);
    llvm::Value* elemsVal = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET),
        llvm::Align(8), "push.elems");
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::BasicBlock* storeBb = llvm::BasicBlock::Create(ctx, "push.store", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT)), storeBb, slowBb);

    builder.SetInsertPoint(storeBb);
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* slotIdx = builder.CreateAdd(builder.CreateZExt(actualSlot, i64Ty),
                                             builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    builder.CreateAlignedStore(argVal, slotPtr, llvm::Align(8));

    llvm::Value* newLen = builder.CreateAdd(len, builder.getInt32(1), "push.newlen");
    builder.CreateAlignedStore(
        newLen, builder.CreateConstInBoundsGEP1_32(i8Ty, thisHdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET),
        llvm::Align(4));

    llvm::Value* newLenDbl = builder.CreateUIToFP(newLen, dblTy, "push.newlendbl");
    llvm::Value* isNan = builder.CreateFCmpUNO(newLenDbl, newLenDbl);
    llvm::Value* rBits = builder.CreateBitCast(newLenDbl, i64Ty);
    llvm::Value* fastRes = builder.CreateSelect(
        isNan, builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS), rBits, "push.fastval");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 6. Slow path: helper trampoline
    builder.SetInsertPoint(slowBb);
    llvm::Value* slowRes = builder.CreateCall(
        abi.bronze_dynamic_call, {calleeBits, thisBits, builder.getInt32(argc), argvPtr}, "push.slowres");
    llvm::BasicBlock* slowEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 7. Merge result
    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "push.result");
    result->addIncoming(fastRes, fastEndBb);
    result->addIncoming(slowRes, slowEndBb);
    return result;
}

}  // namespace bronze::codegen_llvm
