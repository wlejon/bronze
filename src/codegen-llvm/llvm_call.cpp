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

    // 3. Arity check (argc >= arity) and A/B seam check
    builder.SetInsertPoint(fastBb);
    llvm::Value* arity = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_ARITY_OFFSET),
        llvm::Align(4), "call.arity");
    llvm::Value* arityOk = builder.CreateICmpULE(arity, builder.getInt32(argc), "call.arityok");

    llvm::Value* enabled = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_inline_call_enabled, llvm::Align(8), "call.enabled");
    llvm::Value* enabledOk = builder.CreateICmpNE(enabled, builder.getInt64(0), "call.enabledok");

    llvm::Value* ok = builder.CreateAnd(arityOk, enabledOk, "call.ok");

    llvm::BasicBlock* dispatchBb = llvm::BasicBlock::Create(ctx, "call.dispatch", fn);
    builder.CreateCondBr(ok, dispatchBb, slowBb);

    // 4. Fast path: indirect call to FunctionHeader::code
    builder.SetInsertPoint(dispatchBb);
    llvm::Value* env = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_ENV_OFFSET),
        llvm::Align(8), "call.env");
    llvm::Value* code = builder.CreateAlignedLoad(
        ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_CODE_OFFSET),
        llvm::Align(8), "call.code");

    llvm::FunctionType* codeTy =
        llvm::FunctionType::get(i64Ty, {i64Ty, i64Ty, i32Ty, ptrTy}, false);
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
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "call.result");
    result->addIncoming(fastRes, fastEndBb);
    result->addIncoming(slowRes, slowBb);
    return result;
}

}  // namespace bronze::codegen_llvm
