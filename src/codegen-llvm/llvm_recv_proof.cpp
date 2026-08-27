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

ReceiverRunPlan planReceiverRuns(const il::Module& module, const il::Function& func,
                                 size_t blockIndex) {
    ReceiverRunPlan plan;
    if (!receiverProofEnabled()) return plan;
    if (blockIndex >= func.blocks.size()) return plan;
    const auto& block = func.blocks[blockIndex];

    std::vector<size_t> members;
    il::ValueId receiver = il::kNoValue;
    uint32_t maxIndex = 0;
    uint32_t nextRun = 0;

    auto commit = [&]() {
        if (members.size() >= kMinRunLength) {
            if (plan.sites.size() < block.instructions.size()) {
                plan.sites.resize(block.instructions.size());
            }
            for (size_t i = 0; i < members.size(); ++i) {
                auto& site = plan.sites[members[i]];
                site.run = nextRun;
                site.establishes = (i == 0);
                site.runMaxIndex = maxIndex;
            }
            ++nextRun;
        }
        members.clear();
        receiver = il::kNoValue;
        maxIndex = 0;
    };

    for (size_t i = 0; i < block.instructions.size(); ++i) {
        const auto& inst = block.instructions[i];
        const il::ValueId recv = receiverOf(inst);
        const std::optional<uint32_t> idx = indexKeyOf(module, inst);

        if (recv != il::kNoValue && idx.has_value()) {
            if (!members.empty() && recv == receiver) {
                members.push_back(i);
                maxIndex = std::max(maxIndex, *idx);
                continue;
            }
            // A different receiver, or the first candidate in the block: the
            // run in hand is finished and this access opens the next one.
            commit();
            receiver = recv;
            maxIndex = *idx;
            members.push_back(i);
            continue;
        }

        // Not a member. It ends the run if it can move the heap out from under
        // the derived pointer, or if it redefines the receiver the proof was
        // made about.
        if (members.empty()) continue;
        if (il::canCollect(inst) || inst.result == receiver) commit();
    }
    commit();
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

void rejoinReceiverProof(llvm::IRBuilder<>& builder, ReceiverProof& proof,
                         llvm::BasicBlock* fastBb, llvm::BasicBlock* doneBb) {
    if (!proof.live()) return;
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::IRBuilder<> phiBuilder(doneBb, doneBb->getFirstNonPHIIt());
    const std::string tag = "recv" + std::to_string(proof.run) + ".";
    llvm::PHINode* okPhi = phiBuilder.CreatePHI(phiBuilder.getInt1Ty(), 2, tag + "ok.live");
    llvm::PHINode* basePhi = phiBuilder.CreatePHI(ptrTy, 2, tag + "base.live");
    llvm::Value* poison = llvm::PoisonValue::get(ptrTy);

    for (llvm::BasicBlock* pred : llvm::predecessors(doneBb)) {
        const bool viaFast = pred == fastBb;
        okPhi->addIncoming(viaFast ? proof.ok : phiBuilder.getFalse(), pred);
        basePhi->addIncoming(viaFast ? proof.base : poison, pred);
    }

    proof.ok = okPhi;
    proof.base = basePhi;
    (void)builder;
}

}  // namespace bronze::codegen_llvm
