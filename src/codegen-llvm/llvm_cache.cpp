#include "codegen-llvm/llvm_cache.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>

namespace bronze::codegen_llvm {

llvm::Value* emitGlobalGetCached(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                 const ModuleTables& tables, uint32_t keyIndex) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::Value* cellPtr = globalCacheCellPtr(builder, tables, keyIndex);
    if (!cellPtr) {
        // No cell was assigned, which means no `global.get` in this module
        // names this key — so this is not one. Nothing to read; the helper
        // answers, and passing it a null cell tells it not to cache.
        return builder.CreateCall(abi.bronze_global_get,
                                  {emitKeyId(builder, tables, keyIndex),
                                   llvm::ConstantPointerNull::get(ptrTy)});
    }

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "gbl.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "gbl.done", fn);

    // One load at a constant address, one compare. The cell's address is known
    // at compile time because the table is this module's own data, so there is
    // no length to check and no table pointer to chase.
    llvm::Value* cached = builder.CreateAlignedLoad(i64Ty, cellPtr, llvm::Align(8), "gbl.cell");
    // Undefined marks an unfilled cell; anything else is the cached builtin.
    llvm::Value* filled = builder.CreateICmpNE(
        cached, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), "gbl.filled");
    llvm::BasicBlock* fastBb = builder.GetInsertBlock();
    builder.CreateCondBr(filled, doneBb, slowBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(
        abi.bronze_global_get, {emitKeyId(builder, tables, keyIndex), cellPtr});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "gbl.result");
    result->addIncoming(cached, fastBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

llvm::Value* emitFunctionSingletonCached(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                         const ModuleTables& tables, llvm::Function* wrapper,
                                         uint32_t arity, uint32_t length, uint32_t nameKey,
                                         uint32_t fnFlags, uint32_t slot) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::Value* codePtr = fnSlotPtr(builder, tables, slot);
    if (!codePtr) {
        return builder.CreateCall(abi.bronze_function_singleton,
                                  {wrapper, builder.getInt32(arity), builder.getInt32(length),
                                   emitKeyId(builder, tables, nameKey),
                                   builder.getInt32(static_cast<int32_t>(fnFlags)),
                                   llvm::ConstantPointerNull::get(ptrTy)});
    }

    llvm::BasicBlock* valueBb = llvm::BasicBlock::Create(ctx, "fnsingle.value", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "fnsingle.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "fnsingle.done", fn);

    // The entry's address is a compile-time constant, so the guard is the code
    // word alone: it matches this mention's own wrapper, or the helper runs.
    // A zeroed entry — the table's initial state — has a null code word and
    // therefore always misses.
    llvm::Value* code = builder.CreateAlignedLoad(ptrTy, codePtr, llvm::Align(8), "fnsingle.code");
    llvm::Value* codeOk = builder.CreateICmpEQ(code, wrapper, "fnsingle.codeok");
    builder.CreateCondBr(codeOk, valueBb, slowBb);

    builder.SetInsertPoint(valueBb);
    static_assert(BRONZE_ABI_FNSLOT_CODE_OFFSET == 0 && BRONZE_ABI_FNSLOT_VALUE_OFFSET == 8,
                  "the Value is the word after the code pointer");
    llvm::Value* valuePtr = builder.CreateConstInBoundsGEP1_32(i64Ty, codePtr, 1);
    llvm::Value* cached =
        builder.CreateAlignedLoad(i64Ty, valuePtr, llvm::Align(8), "fnsingle.cached");
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(
        abi.bronze_function_singleton,
        {wrapper, builder.getInt32(arity), builder.getInt32(length),
         emitKeyId(builder, tables, nameKey), builder.getInt32(static_cast<int32_t>(fnFlags)),
         codePtr});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "fnsingle.result");
    result->addIncoming(cached, valueBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

}  // namespace bronze::codegen_llvm
