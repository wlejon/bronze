#include "codegen-llvm/llvm_env.h"
#include "codegen-llvm/llvm_alias.h"

#include <cstdlib>
#include <cstring>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

namespace {

static void markInvariant(llvm::LoadInst* load, llvm::LLVMContext& ctx) {
    load->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(ctx, {}));
}

// The chain walk with the ACCESS GUARDS DROPPED: `depth` parent loads and the
// slot GEP, and nothing else. Every question the guarded form asks — is this
// Value object-tagged, is the object an Env record, is the record long enough
// for slot `index` — was answered by the SCOPE PLAN before the IL existed, and
// a wrong answer here is a bug in lowering rather than a fact about the
// program. See the licence at `envAccessGuardsElided`.
llvm::Value* emitEnvSlotPtrUnguarded(llvm::IRBuilder<>& builder, llvm::Value* envBits,
                                     uint32_t depth, uint32_t index) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    auto untag = [&](llvm::Value* bits) {
        return builder.CreateIntToPtr(
            builder.CreateAnd(bits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK)), ptrTy,
            "env.hdr");
    };

    llvm::Value* hdr = untag(envBits);
    for (uint32_t step = 0; step < depth; ++step) {
        auto* parent = builder.CreateAlignedLoad(
            i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ENV_PARENT_OFFSET),
            llvm::Align(8), "env.parent");
        markInvariant(parent, ctx);
        hdr = untag(parent);
    }
    return builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                              BRONZE_ABI_ENV_SLOTS_OFFSET + index * 8);
}

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

    if (depth == 0) {
        llvm::Value* tag = builder.CreateLShr(envBits, BRONZE_ABI_VALUE_TAG_SHIFT);
        llvm::Value* isObj = builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
        llvm::Value* addr =
            builder.CreateAnd(envBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
        llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "env.hdr");
        llvm::Value* flagsPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
        auto* flags =
            builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "env.flags");
        markInvariant(flags, ctx);
        llvm::Value* isEnv = builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ENV));

        llvm::Value* sizePtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_HDR_SIZE_OFFSET);
        auto* size = builder.CreateAlignedLoad(i32Ty, sizePtr, llvm::Align(4), "env.size");
        markInvariant(size, ctx);
        const uint32_t needed = BRONZE_ABI_ENV_SLOTS_OFFSET + (index + 1) * 8;
        llvm::Value* inRange = builder.CreateICmpUGE(size, builder.getInt32(needed));

        llvm::Value* ok = builder.CreateAnd(builder.CreateAnd(isObj, isEnv), inRange);
        llvm::BasicBlock* cont = llvm::BasicBlock::Create(ctx, "env.ok", fn);
        builder.CreateCondBr(ok, cont, slowBb);
        builder.SetInsertPoint(cont);

        return builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                                  BRONZE_ABI_ENV_SLOTS_OFFSET + index * 8);
    }

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
        auto* flags =
            builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "env.flags");
        markInvariant(flags, ctx);
        guard(builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ENV)),
              "env.isenv");
        return hdr;
    };

    llvm::Value* hdr = resolveRecord(envBits);
    for (uint32_t step = 0; step < depth; ++step) {
        llvm::Value* parentPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ENV_PARENT_OFFSET);
        auto* parent =
            builder.CreateAlignedLoad(i64Ty, parentPtr, llvm::Align(8), "env.parent");
        markInvariant(parent, ctx);
        hdr = resolveRecord(parent);
    }

    // The slot-range tripwire: the record's total size must cover the slot.
    llvm::Value* sizePtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_HDR_SIZE_OFFSET);
    auto* size = builder.CreateAlignedLoad(i32Ty, sizePtr, llvm::Align(4), "env.size");
    markInvariant(size, ctx);
    const uint32_t needed = BRONZE_ABI_ENV_SLOTS_OFFSET + (index + 1) * 8;
    guard(builder.CreateICmpUGE(size, builder.getInt32(needed)), "env.inrange");

    return builder.CreateConstInBoundsGEP1_32(i8Ty, hdr,
                                              BRONZE_ABI_ENV_SLOTS_OFFSET + index * 8);
}

}  // namespace

// WHAT THE ACCESS GUARDS ARE. `emitEnvSlotPtr` tests the object tag, loads and
// compares the Env brand at every link of the chain, and compares the record's
// size against the slot — at EVERY access — and branches to the helper, whose
// fatal names the lowering bug. They are TRIPWIRES, not semantics: the depth
// and the index are compile-time constants the scope plan resolved, an
// environment record's layout is fixed where it is created, and no program
// text can make a resolved binding land on a record of the wrong kind or one
// too short. Nothing in 9.1.1.1 asks these questions. (The TDZ test does
// correspond to a step of it, and is never touched by any of this.)
//
// WHAT LICENSES DROPPING THEM. The invariant this work runs on is that an
// elision is legal when the fact checked is established by a dominating guard,
// a STATIC PLAN, or a pin. This is the static-plan case, and it is the only
// one of the three that owes nothing to the program being compiled. The whole
// correctness suite — oracle, three.js, pixi, cli, embed, runtime, two-module,
// threaded, shared-load, hot-swap — is green with the guards dropped.
//
// WHY IT IS OFF BY DEFAULT ANYWAY, AND WHY IT IS NOT TIED TO `--pins`. Two
// reasons, and the second is the one that decided it:
//
//   - A tripwire's value is that it is armed in the builds where a lowering
//     bug would first be met.
//   - It is NOT A UNIFORM WIN. Measured on this box (medians of 13,
//     interleaved, two-count wall delta), it is worth what the guards cost
//     where environment slots are hot — `env_slot_kernel` 56.8 -> 48.9
//     ns/iter, `typed_array_crunch` 78.3 -> 71.1 ms — and it COSTS about as
//     much where they are not, purely by moving code: `mat4_kernel` 23.6 ->
//     25.5 ns/call, `mesh_churn_2k` +1.4 ms, `object_graph` +1.3 ms, with
//     nothing but block layout between the two shapes. So it is a decision
//     about one program, which makes it a flag rather than a policy, and
//     tying it to a pin manifest would have regressed the kernel this stage
//     is measured on in the configuration that stage ships.
//
// BRONZE_ELIDE_ENV_GUARDS: `1` drops the access guards, anything else (and
// unset) keeps them.
bool envAccessGuardsElided() {
    static const bool elide = [] {
        const char* env = std::getenv("BRONZE_ELIDE_ENV_GUARDS");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return elide;
}

llvm::Value* emitEnvGet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                        const ModuleTables& tables, llvm::Value* envBits, uint32_t depth,
                        uint32_t index, bool tdz, uint32_t keyIndex, bool elideGuards) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "env.get.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "env.get.done", fn);

    // The slow block stays emitted either way: with the access guards dropped
    // the TDZ edge is still a predecessor when `tdz` is set, and when it is not
    // the block is unreachable and LLVM deletes it.
    llvm::Value* slotPtr = elideGuards
                               ? emitEnvSlotPtrUnguarded(builder, envBits, depth, index)
                               : emitEnvSlotPtr(builder, envBits, depth, index, slowBb);
    auto* fastVal = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "env.val");
    tagEnvRecordAccess(fastVal, ctx);
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
                                  emitKeyId(builder, tables, keyIndex)})
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
                uint32_t depth, uint32_t index, llvm::Value* valBits, bool elideGuards) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();

    // With the access guards dropped a write is one store, so there is no
    // failure edge and no block to branch out of.
    if (elideGuards) {
        auto* store = builder.CreateAlignedStore(
            valBits, emitEnvSlotPtrUnguarded(builder, envBits, depth, index), llvm::Align(8));
        tagEnvRecordAccess(store, ctx);
        return;
    }

    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "env.set.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "env.set.done", fn);

    llvm::Value* slotPtr = emitEnvSlotPtr(builder, envBits, depth, index, slowBb);
    auto* storeInst = builder.CreateAlignedStore(valBits, slotPtr, llvm::Align(8));
    tagEnvRecordAccess(storeInst, ctx);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    builder.CreateCall(abi.bronze_env_set, {envBits, builder.getInt32(depth),
                                            builder.getInt32(index), valBits});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
}

}  // namespace bronze::codegen_llvm
