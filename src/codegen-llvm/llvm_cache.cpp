#include "codegen-llvm/llvm_cache.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>

namespace bronze::codegen_llvm {

llvm::Value* emitGlobalGetCached(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                 const AbiGlobals& globals, uint32_t keyIndex) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* loadBb = llvm::BasicBlock::Create(ctx, "gbl.load", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "gbl.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "gbl.done", fn);

    llvm::Value* len = builder.CreateAlignedLoad(i64Ty, globals.bronze_global_cache_len,
                                                 llvm::Align(8), "gbl.len");
    llvm::Value* inRange = builder.CreateICmpUGT(len, builder.getInt64(keyIndex), "gbl.inrange");
    builder.CreateCondBr(inRange, loadBb, slowBb);

    builder.SetInsertPoint(loadBb);
    llvm::Value* tbl = builder.CreateAlignedLoad(ptrTy, globals.bronze_global_cache_tbl,
                                                 llvm::Align(8), "gbl.tbl");
    llvm::Value* cellPtr = builder.CreateConstInBoundsGEP1_32(i64Ty, tbl, keyIndex);
    llvm::Value* cached = builder.CreateAlignedLoad(i64Ty, cellPtr, llvm::Align(8), "gbl.cell");
    // Undefined marks an unfilled cell; anything else is the cached builtin.
    llvm::Value* filled = builder.CreateICmpNE(
        cached, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), "gbl.filled");
    builder.CreateCondBr(filled, doneBb, slowBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal =
        builder.CreateCall(abi.bronze_global_get, {builder.getInt32(keyIndex)});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "gbl.result");
    result->addIncoming(cached, loadBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

llvm::Value* emitFunctionSingletonCached(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                         const AbiGlobals& globals, llvm::Function* wrapper,
                                         uint32_t arity, uint32_t length, uint32_t nameKey,
                                         uint32_t slot) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* entryBb = llvm::BasicBlock::Create(ctx, "fnsingle.entry", fn);
    llvm::BasicBlock* valueBb = llvm::BasicBlock::Create(ctx, "fnsingle.value", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "fnsingle.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "fnsingle.done", fn);

    llvm::Value* len = builder.CreateAlignedLoad(i64Ty, globals.bronze_fn_singleton_len,
                                                 llvm::Align(8), "fnsingle.len");
    llvm::Value* inRange = builder.CreateICmpUGT(len, builder.getInt64(slot), "fnsingle.inrange");
    builder.CreateCondBr(inRange, entryBb, slowBb);

    builder.SetInsertPoint(entryBb);
    llvm::Value* tbl = builder.CreateAlignedLoad(ptrTy, globals.bronze_fn_singleton_tbl,
                                                 llvm::Align(8), "fnsingle.tbl");
    static_assert(BRONZE_ABI_FNSLOT_SIZE == 2 * sizeof(uint64_t) &&
                  BRONZE_ABI_FNSLOT_CODE_OFFSET == 0 &&
                  BRONZE_ABI_FNSLOT_VALUE_OFFSET == 8);
    llvm::Value* codePtr = builder.CreateConstInBoundsGEP1_32(i64Ty, tbl, slot * 2);
    llvm::Value* code = builder.CreateAlignedLoad(ptrTy, codePtr, llvm::Align(8), "fnsingle.code");
    llvm::Value* codeOk = builder.CreateICmpEQ(code, wrapper, "fnsingle.codeok");
    builder.CreateCondBr(codeOk, valueBb, slowBb);

    builder.SetInsertPoint(valueBb);
    llvm::Value* valuePtr = builder.CreateConstInBoundsGEP1_32(i64Ty, tbl, slot * 2 + 1);
    llvm::Value* cached =
        builder.CreateAlignedLoad(i64Ty, valuePtr, llvm::Align(8), "fnsingle.cached");
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(
        abi.bronze_function_singleton,
        {wrapper, builder.getInt32(arity), builder.getInt32(length), builder.getInt32(nameKey),
         builder.getInt32(slot)});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "fnsingle.result");
    result->addIncoming(cached, valueBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

}  // namespace bronze::codegen_llvm
