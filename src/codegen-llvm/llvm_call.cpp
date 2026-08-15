#include "codegen-llvm/llvm_call.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>

#include "abi/bronze_abi.h"

namespace bronze::codegen_llvm {

llvm::Value* emitDynamicCallInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                   const AbiGlobals& globals, llvm::Value* callee,
                                   llvm::Value* thisVal, uint32_t argc, llvm::Value* argv) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* fnBb = llvm::BasicBlock::Create(ctx, "call.fn", fn);
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, "call.fast", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "call.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "call.done", fn);

    // 1. Is the callee an object?
    llvm::Value* tag = builder.CreateLShr(callee, BRONZE_ABI_VALUE_TAG_SHIFT, "call.tag");
    llvm::Value* isObj =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "call.isobj");
    builder.CreateCondBr(isObj, fnBb, slowBb);

    // 2. Is it a FunctionHeapObject?
    builder.SetInsertPoint(fnBb);
    llvm::Value* calleeAddr =
        builder.CreateAnd(callee, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* fnPtr = builder.CreateIntToPtr(calleeAddr, ptrTy, "call.fnptr");
    llvm::Value* flags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "call.flags");
    llvm::Value* isFn = builder.CreateICmpEQ(
        flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_FUNCTION), "call.isfn");
    builder.CreateCondBr(isFn, fastBb, slowBb);

    // 3. Arity check and A/B seam check
    builder.SetInsertPoint(fastBb);
    llvm::Value* enabled = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_inline_call_enabled, llvm::Align(8), "call.enabled");
    llvm::Value* enabledOk = builder.CreateICmpNE(enabled, builder.getInt64(0), "call.enabledok");

    llvm::Value* arity = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_ARITY_OFFSET),
        llvm::Align(4), "call.arity");
    llvm::Value* directArityOk = builder.CreateICmpULE(arity, builder.getInt32(argc), "call.dirarityok");

    llvm::BasicBlock* dispatchBb = llvm::BasicBlock::Create(ctx, "call.dispatch", fn);
    llvm::BasicBlock* underArityCheckBb = llvm::BasicBlock::Create(ctx, "call.underarity.check", fn);

    llvm::Value* directOk = builder.CreateAnd(directArityOk, enabledOk, "call.directok");
    builder.CreateCondBr(directOk, dispatchBb, underArityCheckBb);

    // If direct arity not ok: check if enabled and arity <= 8 (for under-arity stack padding)
    builder.SetInsertPoint(underArityCheckBb);
    llvm::Value* canPad = builder.CreateAnd(
        enabledOk,
        builder.CreateICmpULE(arity, builder.getInt32(8)), "call.canpad");
    llvm::BasicBlock* underArityBb = llvm::BasicBlock::Create(ctx, "call.underarity", fn);
    builder.CreateCondBr(canPad, underArityBb, slowBb);

    // 4a. Under-arity padding path: copy available args and pad with undefined up to 8 slots
    builder.SetInsertPoint(underArityBb);
    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    llvm::Value* padBuf = entryBuilder.CreateAlloca(
        llvm::ArrayType::get(i64Ty, 8), nullptr, "call.padbuf");

    if (argc > 0 && argv) {
        for (uint32_t a = 0; a < argc && a < 8; ++a) {
            llvm::Value* srcPtr = builder.CreateGEP(i64Ty, argv, builder.getInt32(a));
            llvm::Value* val = builder.CreateAlignedLoad(i64Ty, srcPtr, llvm::Align(8));
            llvm::Value* dstPtr = builder.CreateConstInBoundsGEP2_32(
                llvm::ArrayType::get(i64Ty, 8), padBuf, 0, a);
            builder.CreateAlignedStore(val, dstPtr, llvm::Align(8));
        }
    }
    llvm::Value* undefVal = builder.getInt64(BRONZE_ABI_UNDEFINED_BITS);
    for (uint32_t a = argc; a < 8; ++a) {
        llvm::Value* dstPtr = builder.CreateConstInBoundsGEP2_32(
            llvm::ArrayType::get(i64Ty, 8), padBuf, 0, a);
        builder.CreateAlignedStore(undefVal, dstPtr, llvm::Align(8));
    }

    llvm::Value* padArgv = builder.CreateConstInBoundsGEP2_32(
        llvm::ArrayType::get(i64Ty, 8), padBuf, 0, 0);

    llvm::Value* padEnv = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_ENV_OFFSET),
        llvm::Align(8), "call.padenv");
    llvm::Value* padCode = builder.CreateAlignedLoad(
        ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_CODE_OFFSET),
        llvm::Align(8), "call.padcode");

    llvm::FunctionType* codeTy =
        llvm::FunctionType::get(i64Ty, {i64Ty, i64Ty, i32Ty, ptrTy}, false);
    llvm::Value* padRes = builder.CreateCall(
        codeTy, padCode, {padEnv, thisVal, builder.getInt32(argc), padArgv}, "call.padres");
    llvm::BasicBlock* padEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 4b. Fast direct dispatch (argc >= arity)
    builder.SetInsertPoint(dispatchBb);
    llvm::Value* env = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_ENV_OFFSET),
        llvm::Align(8), "call.env");
    llvm::Value* code = builder.CreateAlignedLoad(
        ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_CODE_OFFSET),
        llvm::Align(8), "call.code");

    llvm::Value* fastRes = builder.CreateCall(
        codeTy, code, {env, thisVal, builder.getInt32(argc), argv}, "call.fastres");
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

}  // namespace bronze::codegen_llvm
