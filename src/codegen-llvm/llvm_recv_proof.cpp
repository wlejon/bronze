#include "codegen-llvm/llvm_recv_proof.h"

#include "codegen-llvm/llvm_alias.h"
#include "codegen-llvm/llvm_prop_ic.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

namespace {

// A run of one buys nothing: the proof ladder IS the access ladder, and the
// fast arm would only add a branch in front of it. Two is where the arithmetic
// turns — one ladder plus two four-instruction arms against two ladders.
constexpr size_t kMinRunLength = 2;

// The receiver of a constant-index property read, or kNoValue for an
// instruction that is not one.
il::ValueId receiverOf(const il::Instruction& inst) {
    if (inst.op != il::Op::PropGet) return il::kNoValue;
    if (inst.operands.empty()) return il::kNoValue;
    return inst.operands[0];
}

std::optional<uint32_t> indexKeyOf(const il::Module& module, const il::Instruction& inst) {
    if (inst.op != il::Op::PropGet) return std::nullopt;
    if (inst.keyIndex >= module.keyConstants.size()) return std::nullopt;
    return parseIndexKey(module.keyConstants[inst.keyIndex]);
}

}  // namespace

bool receiverProofEnabled() {
    static const bool enabled = std::getenv("BRONZE_NO_RECV_PROOF") == nullptr;
    return enabled;
}

namespace {

// One pass of the joint scan. `transparent[i]` says whether instruction `i` is
// currently believed to be a committed run member — a site whose fast arm
// neither allocates nor calls and whose join re-establishes every live proof.
// Everything else that can collect ends both runs.
void scanRuns(const il::Module& module, const il::Block& block,
              const std::vector<uint32_t>& defIndex, const std::vector<bool>& transparent,
              bool wantStores, BlockRunPlan& plan) {
    plan = BlockRunPlan{};
    plan.reads.sites.assign(block.instructions.size(), ReceiverRunPlan::Site{});
    plan.stores.sites.assign(block.instructions.size(), StoreRunPlan::Site{});

    std::vector<size_t> readMembers;
    std::vector<uint32_t> readIndices;
    il::ValueId readRecv = il::kNoValue;
    uint32_t readMax = 0;
    uint32_t nextReadRun = 0;

    std::vector<size_t> storeMembers;
    std::vector<uint32_t> storeOffsets;
    il::ValueId storeRecv = il::kNoValue;
    il::ValueId storeBase = il::kNoValue;
    uint32_t storeMax = 0;
    uint32_t nextStoreRun = 0;

    auto commitReads = [&]() {
        if (readMembers.size() >= kMinRunLength) {
            for (size_t i = 0; i < readMembers.size(); ++i) {
                auto& site = plan.reads.sites[readMembers[i]];
                site.run = nextReadRun;
                site.establishes = (i == 0);
                site.runMaxIndex = readMax;
            }
            ++nextReadRun;
        }
        readMembers.clear();
        readIndices.clear();
        readRecv = il::kNoValue;
        readMax = 0;
    };
    auto commitStores = [&]() {
        if (storeMembers.size() >= kMinRunLength) {
            for (size_t i = 0; i < storeMembers.size(); ++i) {
                auto& site = plan.stores.sites[storeMembers[i]];
                site.run = nextStoreRun;
                site.establishes = (i == 0);
                site.runMaxOffset = storeMax;
                site.offset = storeOffsets[i];
                site.base = storeBase;
            }
            ++nextStoreRun;
        }
        storeMembers.clear();
        storeOffsets.clear();
        storeRecv = il::kNoValue;
        storeBase = il::kNoValue;
        storeMax = 0;
    };

    for (size_t i = 0; i < block.instructions.size(); ++i) {
        const auto& inst = block.instructions[i];
        const il::ValueId recv = receiverOf(inst);
        const std::optional<uint32_t> idx = indexKeyOf(module, inst);
        const StoreSiteShape store =
            wantStores ? classifyStoreSite(block, defIndex, i) : StoreSiteShape{};

        // A run member is transparent to the OTHER run's proof; anything else
        // that can move the heap ends it. Asked before this instruction joins
        // a run of its own, because the question is about the proof it does
        // not carry.
        const bool member = (recv != il::kNoValue && idx.has_value()) || store.ok;
        const bool opaque = !member || !transparent[i];

        if (recv != il::kNoValue && idx.has_value()) {
            if (opaque && il::canCollect(inst)) commitStores();
            if (!readMembers.empty() && recv == readRecv) {
                readMembers.push_back(i);
                readIndices.push_back(*idx);
                readMax = std::max(readMax, *idx);
                continue;
            }
            // A different receiver, or the first candidate in the block: the
            // run in hand is finished and this access opens the next one.
            commitReads();
            readRecv = recv;
            readMax = *idx;
            readMembers.push_back(i);
            readIndices.push_back(*idx);
            continue;
        }

        if (store.ok) {
            if (opaque && il::canCollect(inst)) commitReads();
            if (!storeMembers.empty() && store.receiver == storeRecv && store.base == storeBase) {
                storeMembers.push_back(i);
                storeOffsets.push_back(store.offset);
                storeMax = std::max(storeMax, store.offset);
                continue;
            }
            commitStores();
            storeRecv = store.receiver;
            storeBase = store.base;
            storeMax = store.offset;
            storeMembers.push_back(i);
            storeOffsets.push_back(store.offset);
            continue;
        }

        // Not a member of either. It ends a run if it can move the heap out
        // from under that run's derived pointer, or if it redefines the
        // receiver — or, for a store run, the base — the proof was made about.
        if (il::canCollect(inst)) {
            commitReads();
            commitStores();
            continue;
        }
        if (inst.result != il::kNoValue) {
            if (inst.result == readRecv) commitReads();
            if (inst.result == storeRecv || inst.result == storeBase) commitStores();
        }
    }
    commitReads();
    commitStores();
}

}  // namespace

BlockRunPlan planBlockRuns(const il::Module& module, const il::Function& func,
                           size_t blockIndex) {
    BlockRunPlan plan;
    if (!receiverProofEnabled()) return plan;
    if (blockIndex >= func.blocks.size()) return plan;
    const il::Block& block = func.blocks[blockIndex];
    const bool wantStores = storeProofEnabled();

    // A value's defining instruction inside THIS block, so the store planner
    // can look through an index's `box.f64` and `add` without a search per
    // site. A value defined in another block — a parameter, a phi — has no
    // entry, which is exactly the answer the planner wants for a run base.
    std::vector<uint32_t> defIndex(func.valueCount, kNoDef);
    for (size_t i = 0; i < block.instructions.size(); ++i) {
        const il::ValueId result = block.instructions[i].result;
        if (result != il::kNoValue && result < defIndex.size()) {
            defIndex[result] = static_cast<uint32_t>(i);
        }
    }

    // The fixpoint the header describes. Start optimistic — every candidate
    // site is assumed to become a run member — and re-scan whenever a scan
    // disagrees. Membership only ever shrinks (dropping a member can split or
    // shorten a run, never lengthen one), so this terminates, and the bound
    // below is a belt on that argument rather than a policy.
    std::vector<bool> transparent(block.instructions.size(), false);
    for (size_t i = 0; i < block.instructions.size(); ++i) {
        const auto& inst = block.instructions[i];
        transparent[i] = (receiverOf(inst) != il::kNoValue && indexKeyOf(module, inst)) ||
                         (wantStores && classifyStoreSite(block, defIndex, i).ok);
    }

    for (size_t round = 0; round <= block.instructions.size(); ++round) {
        scanRuns(module, block, defIndex, transparent, wantStores, plan);
        bool changed = false;
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const bool member = plan.reads.sites[i].run != ReceiverRunPlan::kNoRun ||
                                plan.stores.sites[i].run != StoreRunPlan::kNoRun;
            if (transparent[i] && !member) {
                transparent[i] = false;
                changed = true;
            }
        }
        if (!changed) break;
    }
    return plan;
}

ReceiverProof emitReceiverProof(llvm::IRBuilder<>& builder, llvm::Value* objBits,
                                il::ValueId receiver, uint32_t run, uint32_t maxIndex) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    const std::string tag = "recv" + std::to_string(run) + ".";

    // Every test below reads a field the test before it proved was there — the
    // header only after the tag says there is one, the elements object only
    // after the kind says the header has that field at all — so the ladder is a
    // chain of blocks rather than one wide `and`. All four failure edges meet
    // at the same join as the success edge, because what a member branches on
    // has to be a single i1 and not a jump the caller has to know about.
    llvm::BasicBlock* hdrBb = llvm::BasicBlock::Create(ctx, tag + "hdr", fn);
    llvm::BasicBlock* kindBb = llvm::BasicBlock::Create(ctx, tag + "kind", fn);
    llvm::BasicBlock* elemsBb = llvm::BasicBlock::Create(ctx, tag + "elems", fn);
    llvm::BasicBlock* baseBb = llvm::BasicBlock::Create(ctx, tag + "base", fn);
    llvm::BasicBlock* joinBb = llvm::BasicBlock::Create(ctx, tag + "join", fn);

    llvm::BasicBlock* entryBb = builder.GetInsertBlock();
    llvm::Value* tagBits = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObject =
        builder.CreateICmpEQ(tagBits, builder.getInt64(BRONZE_ABI_TAG_OBJECT), tag + "isobj");
    builder.CreateCondBr(isObject, hdrBb, joinBb);

    builder.SetInsertPoint(hdrBb);
    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, tag + "hdr");
    llvm::Value* flagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    auto* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), tag + "flags");
    llvm::Value* isArray =
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
    builder.CreateCondBr(isArray, kindBb, joinBb);

    // ONE length test for the whole run, against its largest index. Every
    // member is then in bounds by construction, which is what lets each fast
    // arm below be a load with no compare in front of it.
    builder.SetInsertPoint(kindBb);
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), tag + "len");
    tagArrayHeaderAccess(len, ctx);
    llvm::Value* inBounds = builder.CreateICmpULT(builder.getInt32(maxIndex), len);
    builder.CreateCondBr(inBounds, elemsBb, joinBb);

    builder.SetInsertPoint(elemsBb);
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), tag + "elems");
    tagArrayHeaderAccess(elemsVal, ctx);
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* elemsIsObj =
        builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    builder.CreateCondBr(elemsIsObj, baseBb, joinBb);

    // Element zero's address, byte for byte the address llvm_prop_get.cpp's
    // array arm computes for index zero: the ring head, plus the one slot the
    // elements object carries in front of its payload. A proven read and a
    // ladder read of the same element cannot disagree about where it is.
    builder.SetInsertPoint(baseBb);
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* headPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_HEAD_OFFSET);
    auto* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), tag + "head");
    tagArrayHeaderAccess(head, ctx);
    llvm::Value* slot0 = builder.CreateAdd(builder.CreateZExt(head, i64Ty), builder.getInt64(1));
    llvm::Value* base = builder.CreateInBoundsGEP(i64Ty, elemsObj, slot0, tag + "base");
    builder.CreateBr(joinBb);

    builder.SetInsertPoint(joinBb);
    llvm::PHINode* okPhi = builder.CreatePHI(builder.getInt1Ty(), 5, tag + "ok");
    okPhi->addIncoming(builder.getFalse(), entryBb);
    okPhi->addIncoming(builder.getFalse(), hdrBb);
    okPhi->addIncoming(builder.getFalse(), kindBb);
    okPhi->addIncoming(builder.getFalse(), elemsBb);
    okPhi->addIncoming(builder.getTrue(), baseBb);

    llvm::PHINode* basePhi = builder.CreatePHI(ptrTy, 5, tag + "baseptr");
    llvm::Value* poison = llvm::PoisonValue::get(ptrTy);
    basePhi->addIncoming(poison, entryBb);
    basePhi->addIncoming(poison, hdrBb);
    basePhi->addIncoming(poison, kindBb);
    basePhi->addIncoming(poison, elemsBb);
    basePhi->addIncoming(base, baseBb);

    ReceiverProof proof;
    proof.receiver = receiver;
    proof.run = run;
    proof.ok = okPhi;
    proof.base = basePhi;
    return proof;
}

ProvenRead emitProvenElementRead(llvm::IRBuilder<>& builder, const ReceiverProof& proof,
                                 uint32_t index, llvm::BasicBlock* doneBb) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    const std::string tag = "recv" + std::to_string(proof.run) + ".e" + std::to_string(index) + ".";
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, tag + "fast", fn);
    llvm::BasicBlock* ladderBb = llvm::BasicBlock::Create(ctx, tag + "ladder", fn);
    builder.CreateCondBr(proof.ok, fastBb, ladderBb);

    builder.SetInsertPoint(fastBb);
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, proof.base, builder.getInt64(index));
    auto* raw = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), tag + "raw");
    tagArrayElementsAccess(raw, ctx);
    // A hole reads as `undefined`, and that is the only answer a raw load
    // cannot give for itself. The ladder's other answers — absent, an accessor,
    // a hit up the prototype chain — cannot arise here: the length test put the
    // index inside the dense part, and a dense element is a Value or a hole.
    llvm::Value* rawTag = builder.CreateLShr(raw, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isHole = builder.CreateICmpEQ(rawTag, builder.getInt64(BRONZE_ABI_TAG_HOLE));
    llvm::Value* value =
        builder.CreateSelect(isHole, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), raw, tag + "val");
    llvm::BasicBlock* fastExit = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(ladderBb);
    return ProvenRead{fastExit, value};
}

llvm::Value* phiAtJoin(llvm::BasicBlock* doneBb, llvm::BasicBlock* fastBb, llvm::Value* live,
                       llvm::Value* dead, const std::string& name) {
    llvm::IRBuilder<> phiBuilder(doneBb, doneBb->getFirstNonPHIIt());
    llvm::PHINode* phi = phiBuilder.CreatePHI(live->getType(), 2, name);
    for (llvm::BasicBlock* pred : llvm::predecessors(doneBb)) {
        phi->addIncoming(pred == fastBb ? live : dead, pred);
    }
    return phi;
}

void rejoinReceiverProof(llvm::IRBuilder<>& builder, ReceiverProof& proof,
                         llvm::BasicBlock* fastBb, llvm::BasicBlock* doneBb) {
    if (!proof.live()) return;
    if (fastBb == nullptr) {
        proof = ReceiverProof{};
        return;
    }
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);
    const std::string tag = "recv" + std::to_string(proof.run) + ".";

    proof.ok = phiAtJoin(doneBb, fastBb, proof.ok, llvm::ConstantInt::getFalse(ctx),
                         tag + "ok.live");
    proof.base =
        phiAtJoin(doneBb, fastBb, proof.base, llvm::PoisonValue::get(ptrTy), tag + "base.live");
    (void)builder;
}

}  // namespace bronze::codegen_llvm
