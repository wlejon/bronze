#include "codegen-llvm/llvm_store_proof.h"

#include "codegen-llvm/llvm_alias.h"
#include "codegen-llvm/llvm_elem.h"
#include "codegen-llvm/llvm_prop_ic.h"
#include "codegen-llvm/llvm_recv_proof.h"

#include <cmath>
#include <cstdlib>
#include <string>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

const il::Instruction* defOf(const il::Block& block, const std::vector<uint32_t>& defIndex,
                            il::ValueId id) {
    if (id == il::kNoValue || id >= defIndex.size()) return nullptr;
    const uint32_t at = defIndex[id];
    if (at == kNoDef || at >= block.instructions.size()) return nullptr;
    return &block.instructions[at];
}

namespace {

// A `const.f64` whose value is a non-negative integer inside the offset
// window, as that integer. Anything else — a fraction, a negative, a NaN, an
// operand that is not a constant at all — is refused, because an offset the
// planner cannot name is an offset the one length test cannot clear.
bool constantOffset(const il::Instruction* def, uint32_t& out) {
    if (def == nullptr || def->op != il::Op::ConstF64) return false;
    const double v = def->immF64;
    if (!(v >= 0.0) || !std::isfinite(v)) return false;
    if (v != std::floor(v)) return false;
    if (v > static_cast<double>(kMaxStoreOffset)) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

}  // namespace

StoreSiteShape classifyStoreSite(const il::Block& block, const std::vector<uint32_t>& defIndex,
                                 size_t instIndex) {
    StoreSiteShape shape;
    if (instIndex >= block.instructions.size()) return shape;
    const il::Instruction& inst = block.instructions[instIndex];
    if (inst.op != il::Op::ElemSet || inst.operands.size() < 3) return shape;

    // The index has to be a boxed double, and its box has to be in this block:
    // a boxed index that arrived from somewhere else is one the planner cannot
    // read the affine form of, and guessing at it is how a run would end up
    // proving the wrong bound.
    const il::Instruction* boxDef = defOf(block, defIndex, inst.operands[1]);
    if (boxDef == nullptr || boxDef->op != il::Op::Box || boxDef->boxType != il::Type::F64 ||
        boxDef->operands.size() != 1) {
        return shape;
    }

    const il::ValueId inner = boxDef->operands[0];
    const il::Instruction* innerDef = defOf(block, defIndex, inner);
    if (innerDef != nullptr && innerDef->op == il::Op::Add && innerDef->type == il::Type::F64 &&
        innerDef->operands.size() == 2) {
        // `B + k`, either way round: the guarded-region pass emits the constant
        // second, but nothing in the IL promises it.
        uint32_t k = 0;
        if (constantOffset(defOf(block, defIndex, innerDef->operands[1]), k)) {
            shape.base = innerDef->operands[0];
            shape.offset = k;
        } else if (constantOffset(defOf(block, defIndex, innerDef->operands[0]), k)) {
            shape.base = innerDef->operands[1];
            shape.offset = k;
        } else {
            return shape;
        }
    } else {
        // The bare base, which is offset zero.
        shape.base = inner;
        shape.offset = 0;
    }

    shape.receiver = inst.operands[0];
    shape.ok = true;
    return shape;
}

bool storeProofEnabled() {
    static const bool enabled =
        std::getenv("BRONZE_NO_STORE_PROOF") == nullptr && receiverProofEnabled();
    return enabled;
}

StoreProof emitStoreProof(llvm::IRBuilder<>& builder, llvm::Value* objBits,
                          llvm::Value* baseDbl, il::ValueId receiver, il::ValueId base,
                          uint32_t run, uint32_t maxOffset) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    // The base has to be a machine double for the round-trip test below to be
    // the test it claims. Anything else is a plan the emitter cannot honour,
    // and a dead proof is the honest answer.
    if (baseDbl == nullptr || !baseDbl->getType()->isDoubleTy()) return StoreProof{};

    const std::string tag = "store" + std::to_string(run) + ".";

    // Each test reads a field the test before it proved was there — the header
    // only after the tag says there is one, the kind only after the flags say
    // the header has that field at all — so the ladder is a chain of blocks
    // rather than one wide `and`. Every failure edge meets the success edge at
    // the same join, because what a member branches on has to be a single i1.
    llvm::BasicBlock* hdrBb = llvm::BasicBlock::Create(ctx, tag + "hdr", fn);
    llvm::BasicBlock* kindBb = llvm::BasicBlock::Create(ctx, tag + "kind", fn);
    llvm::BasicBlock* idxBb = llvm::BasicBlock::Create(ctx, tag + "idx", fn);
    llvm::BasicBlock* lenBb = llvm::BasicBlock::Create(ctx, tag + "len", fn);
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
    markInvariant(flags, ctx);
    llvm::Value* isTyped =
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY));
    builder.CreateCondBr(isTyped, kindBb, joinBb);

    // The kind is fixed for a view's lifetime, so it is read once for the run
    // and carried as the one bit that separates the two arms a member emits.
    // The other seven kinds take the ladder: the integer ones owe ToInt32 and
    // the BigInt ones owe a ToBigInt that THROWS for a Number value, and
    // neither belongs in a store this small.
    builder.SetInsertPoint(kindBb);
    llvm::Value* kindPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_KIND_OFFSET);
    auto* kind = builder.CreateAlignedLoad(i32Ty, kindPtr, llvm::Align(4), tag + "kind");
    markInvariant(kind, ctx);
    llvm::Value* isF32 =
        builder.CreateICmpEQ(kind, builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT32), tag + "isf32");
    llvm::Value* isF64 =
        builder.CreateICmpEQ(kind, builder.getInt32(BRONZE_ABI_TA_KIND_FLOAT64));
    builder.CreateCondBr(builder.CreateOr(isF32, isF64), idxBb, joinBb);

    // The base is a non-negative integral double: an exact round trip through
    // a u32. The saturating conversion comes first and the compare second, the
    // same poison discipline emitElemGuards documents — an out-of-range
    // fptoui is poison where the saturating form is merely UINT32_MAX, and a
    // saturated value cannot round-trip unless it was that value already.
    // NaN saturates to zero and fails the compare; -0 round-trips to +0, which
    // is right, because `ta[-0]` is `ta["0"]` and the -0 that 10.4.5.14
    // refuses comes only from the STRING "-0", which never reaches here.
    builder.SetInsertPoint(idxBb);
    llvm::Value* idx32 = builder.CreateIntrinsic(llvm::Intrinsic::fptoui_sat, {i32Ty, dblTy},
                                                 {baseDbl}, nullptr, tag + "idx");
    llvm::Value* roundTrip = builder.CreateUIToFP(idx32, dblTy);
    builder.CreateCondBr(builder.CreateFCmpOEQ(roundTrip, baseDbl), lenBb, joinBb);

    // ONE length test for the whole run, against its largest offset — the
    // header says why one read covers all of it. Done in 64 bits so a base
    // near 2^32 cannot wrap the add into range.
    builder.SetInsertPoint(lenBb);
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), tag + "len");
    tagViewLengthAccess(len, ctx);
    llvm::Value* index0 = builder.CreateZExt(idx32, i64Ty, tag + "index0");
    llvm::Value* last = builder.CreateAdd(index0, builder.getInt64(maxOffset));
    llvm::Value* inBounds = builder.CreateICmpULT(last, builder.CreateZExt(len, i64Ty));
    builder.CreateCondBr(inBounds, baseBb, joinBb);

    // Element zero's address, byte for byte the address emitTypedArrayElemPtr
    // computes for index zero — buffer, external-storage word, byte offset —
    // so a proven store and a ladder store of the same element cannot disagree
    // about where it is.
    builder.SetInsertPoint(baseBb);
    llvm::Value* data = emitTypedArrayElemPtr(builder, hdr, builder.getInt32(0), 1);
    llvm::Value* shift =
        builder.CreateSelect(isF32, builder.getInt64(2), builder.getInt64(3), tag + "shift");
    builder.CreateBr(joinBb);

    builder.SetInsertPoint(joinBb);
    llvm::PHINode* okPhi = builder.CreatePHI(builder.getInt1Ty(), 6, tag + "ok");
    okPhi->addIncoming(builder.getFalse(), entryBb);
    okPhi->addIncoming(builder.getFalse(), hdrBb);
    okPhi->addIncoming(builder.getFalse(), kindBb);
    okPhi->addIncoming(builder.getFalse(), idxBb);
    okPhi->addIncoming(builder.getFalse(), lenBb);
    okPhi->addIncoming(builder.getTrue(), baseBb);

    llvm::PHINode* dataPhi = builder.CreatePHI(ptrTy, 6, tag + "data");
    llvm::Value* poison = llvm::PoisonValue::get(ptrTy);
    dataPhi->addIncoming(poison, entryBb);
    dataPhi->addIncoming(poison, hdrBb);
    dataPhi->addIncoming(poison, kindBb);
    dataPhi->addIncoming(poison, idxBb);
    dataPhi->addIncoming(poison, lenBb);
    dataPhi->addIncoming(data, baseBb);

    // The three machine values below are NOT phi'd across members, and the
    // header says why: they are integers in registers, computed in a block
    // that dominates every member of the run, and no heap event can change
    // one. They are phi'd HERE only because the ladder's failure edges reach
    // this block too.
    llvm::PHINode* idxPhi = builder.CreatePHI(i64Ty, 6, tag + "base.index");
    llvm::PHINode* shiftPhi = builder.CreatePHI(i64Ty, 6, tag + "base.shift");
    llvm::PHINode* f32Phi = builder.CreatePHI(builder.getInt1Ty(), 6, tag + "base.isf32");
    llvm::Value* zero64 = builder.getInt64(0);
    for (llvm::BasicBlock* pred : {entryBb, hdrBb, kindBb, idxBb, lenBb}) {
        idxPhi->addIncoming(zero64, pred);
        shiftPhi->addIncoming(zero64, pred);
        f32Phi->addIncoming(builder.getFalse(), pred);
    }
    idxPhi->addIncoming(index0, baseBb);
    shiftPhi->addIncoming(shift, baseBb);
    f32Phi->addIncoming(isF32, baseBb);

    StoreProof proof;
    proof.receiver = receiver;
    proof.base = base;
    proof.run = run;
    proof.ok = okPhi;
    proof.data = dataPhi;
    proof.index0 = idxPhi;
    proof.shift = shiftPhi;
    proof.isF32 = f32Phi;
    return proof;
}

llvm::Value* emitStoreValueIsNumber(llvm::IRBuilder<>& builder, llvm::Value* valBits,
                                    const std::string& name) {
    return builder.CreateICmpULE(valBits, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), name);
}

llvm::BasicBlock* emitTypedElementStore(llvm::IRBuilder<>& builder, const StoreProof& proof,
                                        uint32_t offset, llvm::Value* valBits,
                                        const std::string& tag) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);

    llvm::BasicBlock* f32Bb = llvm::BasicBlock::Create(ctx, tag + "f32", fn);
    llvm::BasicBlock* f64Bb = llvm::BasicBlock::Create(ctx, tag + "f64", fn);
    llvm::BasicBlock* endBb = llvm::BasicBlock::Create(ctx, tag + "stored", fn);

    llvm::Value* index = builder.CreateAdd(proof.index0, builder.getInt64(offset));
    llvm::Value* byteOff = builder.CreateShl(index, proof.shift, tag + "off");
    llvm::Value* elemPtr = builder.CreateInBoundsGEP(i8Ty, proof.data, byteOff, tag + "ptr");
    llvm::Value* dbl = builder.CreateBitCast(valBits, dblTy, tag + "val");
    // The two kinds share everything but the width of the store, so they split
    // here and meet again immediately: the proof's join has to be ONE edge for
    // the phis that carry it to the next member.
    builder.CreateCondBr(proof.isF32, f32Bb, f64Bb);

    builder.SetInsertPoint(f32Bb);
    auto* s32 = builder.CreateAlignedStore(builder.CreateFPTrunc(dbl, f32Ty, tag + "narrow"),
                                           elemPtr, llvm::Align(4));
    tagTypedArrayAccess(s32, ctx);
    builder.CreateBr(endBb);

    builder.SetInsertPoint(f64Bb);
    auto* s64 = builder.CreateAlignedStore(dbl, elemPtr, llvm::Align(8));
    tagTypedArrayAccess(s64, ctx);
    builder.CreateBr(endBb);

    builder.SetInsertPoint(endBb);
    return endBb;
}

ProvenStore emitProvenElementStore(llvm::IRBuilder<>& builder, const StoreProof& proof,
                                   uint32_t offset, llvm::Value* valBits,
                                   llvm::BasicBlock* doneBb) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();

    const std::string tag =
        "store" + std::to_string(proof.run) + ".e" + std::to_string(offset) + ".";
    // The arm is built before the test that selects it, so that the blocks land
    // in the function in the order a reader of the IR meets them — the store's
    // own three, then the ladder that stands beside them.
    llvm::BasicBlock* entryBb = builder.GetInsertBlock();
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, tag + "fast", fn);
    builder.SetInsertPoint(fastBb);
    llvm::BasicBlock* fastEndBb = emitTypedElementStore(builder, proof, offset, valBits, tag);
    llvm::BasicBlock* ladderBb = llvm::BasicBlock::Create(ctx, tag + "ladder", fn);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(entryBb);
    llvm::Value* isNum = emitStoreValueIsNumber(builder, valBits, tag + "isnum");
    builder.CreateCondBr(builder.CreateAnd(proof.ok, isNum), fastBb, ladderBb);

    builder.SetInsertPoint(ladderBb);
    return ProvenStore{fastEndBb};
}

void rejoinStoreProof(StoreProof& proof, llvm::BasicBlock* fastBb, llvm::BasicBlock* doneBb) {
    if (!proof.live()) return;
    if (fastBb == nullptr) {
        proof = StoreProof{};
        return;
    }
    llvm::LLVMContext& ctx = doneBb->getContext();
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);
    const std::string tag = "store" + std::to_string(proof.run) + ".";

    proof.ok =
        phiAtJoin(doneBb, fastBb, proof.ok, llvm::ConstantInt::getFalse(ctx), tag + "ok.live");
    proof.data =
        phiAtJoin(doneBb, fastBb, proof.data, llvm::PoisonValue::get(ptrTy), tag + "data.live");
}

}  // namespace bronze::codegen_llvm
