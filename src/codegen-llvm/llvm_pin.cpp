#include "codegen-llvm/llvm_pin.h"

#include <llvm/IR/MDBuilder.h>

#include "abi/bronze_abi.h"

namespace bronze::codegen_llvm {

namespace {

// The branch weight every barrier carries. A pin is a promise, and the whole
// point of the barrier is that a program keeping its promise pays for the
// compare and nothing else — so the violating edge is as cold as the
// environment tripwire's and the block layout puts it out of line.
llvm::MDNode* keptWeights(llvm::LLVMContext& ctx) {
    return llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);
}

// "Is `bits` the shape the pin promised?", as a value in the current block.
//
// A Number's Value bits are its double's bits and every other tag sits ABOVE
// the number range (bronze_abi.h), so the Number test is one unsigned compare
// and the nullish widening is two more equalities against constants. Nothing
// here reads the heap, which is what lets the whole barrier fold away wherever
// LLVM can already see the value's provenance.
llvm::Value* emitShapeTest(llvm::IRBuilder<>& builder, llvm::Value* bits, il::PinBarrier kind) {
    llvm::Value* isNum =
        builder.CreateICmpULE(bits, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "pin.isnum");
    if (kind == il::PinBarrier::Number) return isNum;
    llvm::Value* isNull =
        builder.CreateICmpEQ(bits, builder.getInt64(BRONZE_ABI_NULL_BITS), "pin.isnull");
    llvm::Value* isUndef =
        builder.CreateICmpEQ(bits, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), "pin.isundef");
    return builder.CreateOr(isNum, builder.CreateOr(isNull, isUndef), "pin.ok");
}

}  // namespace

void emitPinGuard(llvm::IRBuilder<>& builder, const AbiFns& abi, const ModuleTables& tables,
                  llvm::Value* bits, uint32_t keyIndex, il::PinBarrier kind,
                  llvm::BasicBlock* unwind) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();

    // "Is this a plain JS array" has no inline form — it is an object tag, a
    // header read and a class comparison — and the site it guards is a store
    // to a `numeric-elements` FIELD, which the shape of that pin makes a
    // constructor-time write rather than a loop-carried one. So the whole
    // question, raise included, is the helper's.
    if (kind == il::PinBarrier::DenseArray) {
        builder.CreateCall(abi.bronze_pin_check_array, {emitKeyId(builder, tables, keyIndex), bits});
        return;
    }

    llvm::BasicBlock* badBb = llvm::BasicBlock::Create(ctx, "pin.violated", fn);
    llvm::BasicBlock* okBb = llvm::BasicBlock::Create(ctx, "pin.kept", fn);
    auto* br = builder.CreateCondBr(emitShapeTest(builder, bits, kind), okBb, badBb);
    br->setMetadata(llvm::LLVMContext::MD_prof, keptWeights(ctx));

    // The raise and the departure, in that order and in one block. Leaving here
    // rather than merging is the whole of what makes the kept path free (see
    // llvm_pin.h): the store this guard precedes is skipped because control
    // never comes back to it, not because a cell test after it fires.
    builder.SetInsertPoint(badBb);
    builder.CreateCall(abi.bronze_pin_violation, {emitKeyId(builder, tables, keyIndex), bits});
    builder.CreateBr(unwind);

    builder.SetInsertPoint(okBb);
}

llvm::Value* emitPinnedParamUnbox(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                  const ModuleTables& tables, llvm::Value* bits,
                                  uint32_t keyIndex) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* badBb = llvm::BasicBlock::Create(ctx, "pin.arg.violated", fn);
    llvm::BasicBlock* okBb = llvm::BasicBlock::Create(ctx, "pin.arg.kept", fn);
    auto* br = builder.CreateCondBr(emitShapeTest(builder, bits, il::PinBarrier::Number), okBb,
                                    badBb);
    br->setMetadata(llvm::LLVMContext::MD_prof, keptWeights(ctx));

    // The wrapper leaves HERE rather than falling into the call: the typed
    // entry takes an f64 and there is no f64 to give it, so there is nothing
    // for a merge to carry. `undefined` is the raising convention's return
    // value and the pending cell is what the caller reads.
    builder.SetInsertPoint(badBb);
    builder.CreateCall(abi.bronze_pin_violation, {emitKeyId(builder, tables, keyIndex), bits});
    builder.CreateRet(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS));

    builder.SetInsertPoint(okBb);
    // No ToNumber, and that is the semantic change: the claim is that this IS
    // a Number, and the compare above has just established it, so the whole
    // conversion is the bitcast a Number's bits already are.
    return builder.CreateBitCast(bits, llvm::Type::getDoubleTy(ctx), "pin.arg.f64");
}

llvm::Value* emitPinnedArgUnbox(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                const ModuleTables& tables, llvm::Value* bits, uint32_t keyIndex,
                                llvm::BasicBlock* joinBb,
                                std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>>& incoming) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* badBb = llvm::BasicBlock::Create(ctx, "mdc.pin.violated", fn);
    llvm::BasicBlock* okBb = llvm::BasicBlock::Create(ctx, "mdc.pin.kept", fn);
    auto* br = builder.CreateCondBr(emitShapeTest(builder, bits, il::PinBarrier::Number), okBb,
                                    badBb);
    br->setMetadata(llvm::LLVMContext::MD_prof, keptWeights(ctx));

    builder.SetInsertPoint(badBb);
    builder.CreateCall(abi.bronze_pin_violation, {emitKeyId(builder, tables, keyIndex), bits});
    builder.CreateBr(joinBb);
    incoming.emplace_back(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), badBb);

    builder.SetInsertPoint(okBb);
    return builder.CreateBitCast(bits, llvm::Type::getDoubleTy(ctx), "mdc.pin.f64");
}

}  // namespace bronze::codegen_llvm
