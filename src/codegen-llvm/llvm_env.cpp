#include "codegen-llvm/llvm_env.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

namespace {

// Resolves the record `depth` parent links up from `envBits` and returns the
// address of slot `index`, mirroring resolveEnv + ancestor + the slot-range
// check in rt_object.cpp: brand-checks every link of the chain and bounds-
// checks the slot against the record's own size, so a lowering bug still
// lands in the helper's fatal instead of a load past the object. The builder
// is left in a fresh block on the success path; every failure edge branches
// to `slowBb`.
llvm::Value* emitEnvSlotPtr(llvm::IRBuilder<>& builder, llvm::Value* envBits, uint32_t depth,
                            uint32_t index, llvm::BasicBlock* slowBb) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    auto guard = [&](llvm::Value* cond, const char* name) {
        llvm::BasicBlock* cont = llvm::BasicBlock::Create(ctx, name, fn);
        builder.CreateCondBr(cond, cont, slowBb);
        builder.SetInsertPoint(cont);
    };

    // One record: object tag, then the Env brand.
    auto resolveRecord = [&](llvm::Value* bits) -> llvm::Value* {
        llvm::Value* tag = builder.CreateLShr(bits, BRONZE_ABI_VALUE_TAG_SHIFT);
        guard(builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT)), "env.isobj");
        llvm::Value* addr =
            builder.CreateAnd(bits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
        llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "env.hdr");
        llvm::Value* flagsPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
        llvm::Value* flags =
            builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "env.flags");
        guard(builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ENV)),
              "env.isenv");
        return hdr;
    };

    llvm::Value* hdr = resolveRecord(envBits);
    for (uint32_t step = 0; step < depth; ++step) {
        llvm::Value* parentPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ENV_PARENT_OFFSET);
        llvm::Value* parent =
            builder.CreateAlignedLoad(i64Ty, parentPtr, llvm::Align(8), "env.parent");
        hdr = resolveRecord(parent);
    }

    // The slot-range tripwire: the record's total size must cover the slot.
    llvm::Value* sizePtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_HDR_SIZE_OFFSET);
    llvm::Value* size = builder.CreateAlignedLoad(i32Ty, sizePtr, llvm::Align(4), "env.size");
    const uint32_t needed = BRONZE_ABI_ENV_SLOTS_OFFSET + (index + 1) * 8;
    guard(builder.CreateICmpUGE(size, builder.getInt32(needed)), "env.inrange");

    return builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                              BRONZE_ABI_ENV_SLOTS_OFFSET + index * 8);
}

}  // namespace

llvm::Value* emitEnvGet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* envBits,
                        uint32_t depth, uint32_t index, bool tdz, uint32_t keyIndex) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "env.get.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "env.get.done", fn);

    llvm::Value* slotPtr = emitEnvSlotPtr(builder, envBits, depth, index, slowBb);
    llvm::Value* fastVal = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "env.val");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    if (tdz) {
        // The one compare 9.1.1.1.6 is: the marker means the ReferenceError,
        // and the helper owns raising it.
        llvm::Value* isUninit = builder.CreateICmpEQ(
            fastVal, builder.getInt64(BRONZE_ABI_UNINITIALIZED_BITS), "env.tdz");
        builder.CreateCondBr(isUninit, slowBb, doneBb);
    } else {
        builder.CreateBr(doneBb);
    }

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal =
        tdz ? builder.CreateCall(abi.bronze_env_get_tdz,
                                 {envBits, builder.getInt32(depth), builder.getInt32(index),
                                  builder.getInt32(keyIndex)})
            : builder.CreateCall(abi.bronze_env_get,
                                 {envBits, builder.getInt32(depth), builder.getInt32(index)});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "env.get.result");
    result->addIncoming(fastVal, fastEndBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

void emitEnvSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* envBits,
                uint32_t depth, uint32_t index, llvm::Value* valBits) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "env.set.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "env.set.done", fn);

    llvm::Value* slotPtr = emitEnvSlotPtr(builder, envBits, depth, index, slowBb);
    builder.CreateAlignedStore(valBits, slotPtr, llvm::Align(8));
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    builder.CreateCall(abi.bronze_env_set, {envBits, builder.getInt32(depth),
                                            builder.getInt32(index), valBits});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
}

}  // namespace bronze::codegen_llvm
