#include "codegen-llvm/llvm_array_store_proof.h"

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_alias.h"
#include "codegen-llvm/llvm_recv_proof.h"

#include <cstdlib>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

bool arrayStoreProofEnabled() {
    static const bool enabled =
        std::getenv("BRONZE_NO_ARRAY_STORE_PROOF") == nullptr && receiverProofEnabled();
    return enabled;
}

ArrayStoreProof emitArrayStoreProof(llvm::IRBuilder<>& builder, llvm::Value* objBits,
                                    il::ValueId receiver, uint32_t run, uint32_t maxIndex) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    const std::string tag = "astore" + std::to_string(run) + ".";

    // Every test reads a field the test before it proved was there — the header
    // only after the tag says there is one, the array fields only after the
    // kind word says the header has them at all — so the ladder is a chain of
    // blocks rather than one wide `and`. All six failure edges meet the success
    // edge at the same join, because what a member branches on has to be a
    // single i1 and not a jump the caller has to know about.
    llvm::BasicBlock* hdrBb = llvm::BasicBlock::Create(ctx, tag + "hdr", fn);
    llvm::BasicBlock* lenBb = llvm::BasicBlock::Create(ctx, tag + "len", fn);
    llvm::BasicBlock* capBb = llvm::BasicBlock::Create(ctx, tag + "cap", fn);
    llvm::BasicBlock* propsBb = llvm::BasicBlock::Create(ctx, tag + "props", fn);
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
    builder.CreateCondBr(isArray, lenBb, joinBb);

    // ONE length test for the whole run, against its largest index. Strict, so
    // every member writes an index that is already a slot — which is what keeps
    // `length` out of the fast arm entirely (setElem raises it only for
    // `index >= length`).
    builder.SetInsertPoint(lenBb);
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), tag + "len");
    tagArrayHeaderAccess(len, ctx);
    llvm::Value* inBounds = builder.CreateICmpULT(builder.getInt32(maxIndex), len);
    builder.CreateCondBr(inBounds, capBb, joinBb);

    // The head-offset test, in 64 bits. A shifted array (one `shift()` left
    // behind) stores element k at slot `head + k`, and the per-store arm
    // already refuses a write whose slot is past the block; done wide so that a
    // head near 2^32 cannot wrap the add back into range, which the 32-bit form
    // in emitPropSet can.
    builder.SetInsertPoint(capBb);
    llvm::Value* capPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
    auto* cap = builder.CreateAlignedLoad(i32Ty, capPtr, llvm::Align(4), tag + "cap");
    tagArrayHeaderAccess(cap, ctx);
    llvm::Value* headPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_HEAD_OFFSET);
    auto* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), tag + "head");
    tagArrayHeaderAccess(head, ctx);
    llvm::Value* head64 = builder.CreateZExt(head, i64Ty);
    llvm::Value* lastSlot = builder.CreateAdd(head64, builder.getInt64(maxIndex));
    llvm::Value* inCap = builder.CreateICmpULT(lastSlot, builder.CreateZExt(cap, i64Ty));
    builder.CreateCondBr(inCap, propsBb, joinBb);

    // The named-properties side object, and with it the integrity level: an
    // array that has none is open and extensible, so this one test stands for
    // `Object.freeze`, `Object.seal` and `Object.preventExtensions` at once
    // (integrity.h says why the level lives there). It is also what refuses an
    // `arguments` object, whose `callee` is a named property, and a match array.
    builder.SetInsertPoint(propsBb);
    llvm::Value* propsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
    auto* propsVal = builder.CreateAlignedLoad(i64Ty, propsPtr, llvm::Align(8), tag + "props");
    tagArrayHeaderAccess(propsVal, ctx);
    llvm::Value* propsTag = builder.CreateLShr(propsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* noProps =
        builder.CreateICmpEQ(propsTag, builder.getInt64(BRONZE_ABI_TAG_UNDEFINED));
    builder.CreateCondBr(noProps, elemsBb, joinBb);

    builder.SetInsertPoint(elemsBb);
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), tag + "elems");
    tagArrayHeaderAccess(elemsVal, ctx);
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* elemsIsObj =
        builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    builder.CreateCondBr(elemsIsObj, baseBb, joinBb);

    // Element zero's address: the ring head, plus the one slot the elements
    // object carries in front of its payload. The same expression the read
    // proof and emitPropSet's array arm build, so a proven store, a proven read
    // and a ladder store of the same element cannot disagree about where it is.
    builder.SetInsertPoint(baseBb);
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* slot0 = builder.CreateAdd(head64, builder.getInt64(1));
    llvm::Value* base = builder.CreateInBoundsGEP(i64Ty, elemsObj, slot0, tag + "base");
    builder.CreateBr(joinBb);

    builder.SetInsertPoint(joinBb);
    llvm::PHINode* okPhi = builder.CreatePHI(builder.getInt1Ty(), 7, tag + "ok");
    llvm::PHINode* basePhi = builder.CreatePHI(ptrTy, 7, tag + "baseptr");
    llvm::Value* poison = llvm::PoisonValue::get(ptrTy);
    for (llvm::BasicBlock* pred : {entryBb, hdrBb, lenBb, capBb, propsBb, elemsBb}) {
        okPhi->addIncoming(builder.getFalse(), pred);
        basePhi->addIncoming(poison, pred);
    }
    okPhi->addIncoming(builder.getTrue(), baseBb);
    basePhi->addIncoming(base, baseBb);

    ArrayStoreProof proof;
    proof.receiver = receiver;
    proof.run = run;
    proof.ok = okPhi;
    proof.base = basePhi;
    return proof;
}

ProvenArrayStore emitProvenArrayElementStore(llvm::IRBuilder<>& builder,
                                             const ArrayStoreProof& proof, uint32_t index,
                                             llvm::Value* valBits, llvm::BasicBlock* doneBb) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    const std::string tag =
        "astore" + std::to_string(proof.run) + ".e" + std::to_string(index) + ".";
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, tag + "fast", fn);
    llvm::BasicBlock* ladderBb = llvm::BasicBlock::Create(ctx, tag + "ladder", fn);
    builder.CreateCondBr(proof.ok, fastBb, ladderBb);

    // No test on the value: an Array element slot holds a Value and `arr[i] = v`
    // coerces nothing, so the bits arrive as they are. The store is tagged
    // ArrayElementsData, the same family a proven READ of the same array
    // carries, which is what keeps a read of a slot this run has written from
    // being hoisted above the write (llvm_alias.h: the family's noalias list
    // does not name itself).
    builder.SetInsertPoint(fastBb);
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, proof.base, builder.getInt64(index));
    auto* store = builder.CreateAlignedStore(valBits, slotPtr, llvm::Align(8));
    tagArrayElementsAccess(store, ctx);
    llvm::BasicBlock* fastExit = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(ladderBb);
    return ProvenArrayStore{fastExit};
}

void rejoinArrayStoreProof(ArrayStoreProof& proof, llvm::BasicBlock* fastBb,
                           llvm::BasicBlock* doneBb) {
    if (!proof.live()) return;
    if (fastBb == nullptr) {
        proof = ArrayStoreProof{};
        return;
    }
    llvm::LLVMContext& ctx = doneBb->getContext();
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);
    const std::string tag = "astore" + std::to_string(proof.run) + ".";

    proof.ok =
        phiAtJoin(doneBb, fastBb, proof.ok, llvm::ConstantInt::getFalse(ctx), tag + "ok.live");
    proof.base =
        phiAtJoin(doneBb, fastBb, proof.base, llvm::PoisonValue::get(ptrTy), tag + "base.live");
}

}  // namespace bronze::codegen_llvm
