// The committed hit of the COMPUTED-read cache, emitted inline.
//
// Chunk 3 built the (shape, key) table and chunk 4 taught it absence, and by
// the end of chunk 4 it answered 99.9976 % of the computed reads three.js
// makes — but always through `bronze_elem_get`. The chunk-4 sampler priced
// that: `bronze_elem_get` 2.58 % of the `many_meshes` frame and
// `elemCacheProbe` a further 2.07 %, together 2.08 ms, for a table lookup that
// is a hash, four compares and a load. This file is that lookup, at the site.
//
// WHERE IT SITS is the whole design. It is not a new arm in the element
// emitter's flags switch; it is what the emitter's `slowBb` became. Every
// refusal the array and typed-array arms already had — a non-object receiver,
// a key that is not a number, a plain object, an out-of-bounds index, a
// BigInt view — arrives here, and anything this file also refuses goes on to
// the helper it was always going to. So the two existing fast paths do not
// pay one instruction for this one's existence, and the receiver kind the
// table actually speaks for (PLAIN, which is 100 % of the three.js bill) is
// answered without a call.
//
// WHAT IT REFUSES, and why each refusal is a scope decision rather than a gap:
//
//  - a STRING key. The entry's `key` is an ARENA COPY, so the live key string
//    at a read site is never the same object, and confirming a string key is
//    therefore a length compare and a memcmp — a loop, not a guard. The
//    helper owns `StringHeader::equals` and keeps this half.
//  - depth > 0 (a prototype hit). The walk exists — llvm_prop_ic.cpp's
//    `emitProtoChainWalk` — but a computed read that finds its answer up the
//    chain is not what the bill is made of, and every block emitted here is
//    paid for by the reads that do not take it.
//  - an ACCESSOR entry, which is a call, and a call is exactly what a path
//    whose contract is "no allocation, no user code" cannot make.
//
// Seam: BRONZE_NO_ELEM_INLINE=1, and BRONZE_NO_ELEM_IC=1 lowers it too —
// with the table off the probe could only ever miss, and chunk 3's A/B must
// not be charged for that.

#include "codegen-llvm/llvm_elem.h"
#include "codegen-llvm/llvm_prop_ic.h"

#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

namespace {

// splitmix64's finalizer, byte for byte what runtime/elem_ic.cpp's `mix64`
// computes. The constants come from the ABI header so the two cannot drift:
// a probe that hashes differently from the fill does not answer wrongly, it
// simply never hits — a regression with no symptom but a number.
llvm::Value* emitMix64(llvm::IRBuilder<>& builder, llvm::Value* x, const char* name) {
    llvm::Value* v = builder.CreateAdd(x, builder.getInt64(BRONZE_ABI_MIX64_ADD));
    v = builder.CreateMul(builder.CreateXor(v, builder.CreateLShr(v, 30)),
                          builder.getInt64(BRONZE_ABI_MIX64_MUL1));
    v = builder.CreateMul(builder.CreateXor(v, builder.CreateLShr(v, 27)),
                          builder.getInt64(BRONZE_ABI_MIX64_MUL2));
    return builder.CreateXor(v, builder.CreateLShr(v, 31), name);
}

}  // namespace

ElemCacheHit emitElemCacheGet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                              llvm::Value* keyBits, llvm::BasicBlock* slowBb,
                              llvm::BasicBlock* doneBb) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    // The seam and the table, from the thread's ABI block. `bronze_tls_block_addr`
    // is readnone + willreturn, so this call CSEs with the prologue's fetch and
    // a loop hoists it — the llvm_iter.cpp idiom, for a file that has AbiFns
    // and not AbiGlobals.
    llvm::Value* tls = builder.CreateCall(abi.bronze_tls_block_addr, {}, "tls");
    llvm::Value* seamPtr = builder.CreateConstInBoundsGEP1_64(
        i8Ty, tls, BRONZE_TLS_ELEM_INLINE_ENABLED_OFF, "tls.eleminline");
    llvm::Value* seam = builder.CreateAlignedLoad(i64Ty, seamPtr, llvm::Align(8), "ec.seam");
    llvm::Value* basePtrPtr = builder.CreateConstInBoundsGEP1_64(
        i8Ty, tls, BRONZE_TLS_ELEM_CACHE_TBL_OFF, "tls.elemtbl");
    llvm::Value* base = builder.CreateAlignedLoad(ptrTy, basePtrPtr, llvm::Align(8), "ec.base");
    // A null base is a thread whose runtime start-up has not run yet — the
    // publish and the seam are set together in Heap's constructor, so this is
    // the same refusal as the seam and not a second kind of state.
    llvm::Value* armed =
        builder.CreateAnd(builder.CreateICmpNE(seam, builder.getInt64(0), "ec.seam.on"),
                          builder.CreateICmpNE(base, llvm::Constant::getNullValue(ptrTy),
                                               "ec.base.live"),
                          "ec.armed");

    llvm::BasicBlock* recvBb = llvm::BasicBlock::Create(ctx, "ec.recv", fn);
    builder.CreateCondBr(armed, recvBb, slowBb);

    // --- the receiver: an object, PLAIN, with a non-dictionary shape --------
    builder.SetInsertPoint(recvBb);
    llvm::Value* objTag = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT, "ec.objtag");
    llvm::BasicBlock* kindBb = llvm::BasicBlock::Create(ctx, "ec.recvkind", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(objTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "ec.isobj"), kindBb,
        slowBb);

    builder.SetInsertPoint(kindBb);
    llvm::Value* hdr = builder.CreateIntToPtr(
        builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK)), ptrTy,
        "ec.hdr");
    llvm::Value* flagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    auto* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "ec.flags");
    markInvariant(flags, ctx);
    llvm::BasicBlock* shapeBb = llvm::BasicBlock::Create(ctx, "ec.shape", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN), "ec.isplain"),
        shapeBb, slowBb);

    builder.SetInsertPoint(shapeBb);
    llvm::Value* shapePtrPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SHAPE_OFFSET);
    llvm::Value* shape =
        builder.CreateAlignedLoad(ptrTy, shapePtrPtr, llvm::Align(8), "ec.recvshape");
    llvm::BasicBlock* dictBb = llvm::BasicBlock::Create(ctx, "ec.dict", fn);
    builder.CreateCondBr(builder.CreateICmpNE(shape, llvm::Constant::getNullValue(ptrTy),
                                              "ec.hasshape"),
                         dictBb, slowBb);

    // A dictionary object's properties are not in the shape at all, so a slot
    // number cached against one names nothing. Exactly the probe's refusal,
    // read off exactly the word the probe reads.
    builder.SetInsertPoint(dictBb);
    llvm::Value* dictPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, shape, BRONZE_ABI_SHAPE_DICT_OFFSET);
    llvm::Value* dict = builder.CreateAlignedLoad(ptrTy, dictPtr, llvm::Align(8), "ec.dictword");
    llvm::BasicBlock* keyBb = llvm::BasicBlock::Create(ctx, "ec.key", fn);
    builder.CreateCondBr(
        builder.CreateICmpEQ(dict, llvm::Constant::getNullValue(ptrTy), "ec.notdict"), keyBb,
        slowBb);

    // --- the key's witness: a number's bits, or a boolean's 0/1 -------------
    builder.SetInsertPoint(keyBb);
    llvm::BasicBlock* numKeyBb = llvm::BasicBlock::Create(ctx, "ec.key.num", fn);
    llvm::BasicBlock* boolKeyBb = llvm::BasicBlock::Create(ctx, "ec.key.boolchk", fn);
    llvm::BasicBlock* boolOkBb = llvm::BasicBlock::Create(ctx, "ec.key.bool", fn);
    llvm::BasicBlock* bucketBb = llvm::BasicBlock::Create(ctx, "ec.bucket", fn);
    builder.CreateCondBr(builder.CreateICmpULE(keyBits, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS),
                                               "ec.key.isnum"),
                         numKeyBb, boolKeyBb);

    // A NUMBER witness is the double's raw bits, compared bitwise — so -0 and
    // +0 land in different entries even though they name one property, which
    // is a duplicate entry at worst and never a wrong answer. `witnessFor`
    // says the same thing about the same word.
    builder.SetInsertPoint(numKeyBb);
    builder.CreateBr(bucketBb);

    builder.SetInsertPoint(boolKeyBb);
    llvm::Value* keyTag = builder.CreateLShr(keyBits, BRONZE_ABI_VALUE_TAG_SHIFT, "ec.keytag");
    builder.CreateCondBr(
        builder.CreateICmpEQ(keyTag, builder.getInt64(BRONZE_ABI_TAG_BOOL), "ec.key.isbool"),
        boolOkBb, slowBb);

    builder.SetInsertPoint(boolOkBb);
    llvm::Value* boolWitness = builder.CreateAnd(keyBits, builder.getInt64(1), "ec.key.boolwit");
    builder.CreateBr(bucketBb);

    // --- the bucket --------------------------------------------------------
    builder.SetInsertPoint(bucketBb);
    llvm::PHINode* witness = builder.CreatePHI(i64Ty, 2, "ec.witness");
    witness->addIncoming(keyBits, numKeyBb);
    witness->addIncoming(boolWitness, boolOkBb);
    llvm::PHINode* keyKind = builder.CreatePHI(i8Ty, 2, "ec.keykind");
    keyKind->addIncoming(builder.getInt8(BRONZE_ABI_ELEM_KIND_NUMBER), numKeyBb);
    keyKind->addIncoming(builder.getInt8(BRONZE_ABI_ELEM_KIND_BOOL), boolOkBb);

    // `bucketOf` is exactly `mix64(shape ^ mix64(witness)) & (N - 1)`: TWO
    // finalizer rounds, the inner one on the witness alone and the outer on
    // the xor. Three rounds is a different hash, and a different hash is a
    // probe that never hits — which is what the first version of this line
    // did, and the only thing that caught it was the helper count refusing to
    // move.
    llvm::Value* shapeInt = builder.CreatePtrToInt(shape, i64Ty, "ec.shapeint");
    llvm::Value* hashed =
        builder.CreateXor(shapeInt, emitMix64(builder, witness, "ec.mixwit"), "ec.hashin");
    llvm::Value* bucket = builder.CreateAnd(
        emitMix64(builder, hashed, "ec.hash"), builder.getInt64(BRONZE_ABI_ELEM_ENTRIES - 1),
        "ec.bucket.idx");
    llvm::Value* entry = builder.CreateInBoundsGEP(
        i8Ty, base, builder.CreateMul(bucket, builder.getInt64(BRONZE_ABI_ELEM_ENTRY_SIZE)),
        "ec.entry");

    // --- does the entry name THIS (shape, key) pair? ------------------------
    llvm::Value* entKindPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, entry, BRONZE_ABI_ELEM_KIND_OFFSET);
    llvm::Value* entKind = builder.CreateAlignedLoad(i8Ty, entKindPtr, llvm::Align(1), "ec.entkind");
    llvm::Value* entWitPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, entry, BRONZE_ABI_ELEM_WITNESS_OFFSET);
    llvm::Value* entWit = builder.CreateAlignedLoad(i64Ty, entWitPtr, llvm::Align(8), "ec.entwit");
    llvm::Value* entKeyPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, entry, BRONZE_ABI_ELEM_KEY_OFFSET);
    llvm::Value* entKey = builder.CreateAlignedLoad(ptrTy, entKeyPtr, llvm::Align(8), "ec.entkey");
    // `keyMatches` asks all three, and the null-key test is not redundant with
    // the kind test: an entry is zeroed when empty, and Empty is 0 while a
    // filled entry always names a key.
    llvm::Value* pairOk = builder.CreateAnd(
        builder.CreateAnd(builder.CreateICmpEQ(entKind, keyKind, "ec.kindok"),
                          builder.CreateICmpEQ(entWit, witness, "ec.witok")),
        builder.CreateICmpNE(entKey, llvm::Constant::getNullValue(ptrTy), "ec.keyok"), "ec.pairok");

    llvm::BasicBlock* shapeCmpBb = llvm::BasicBlock::Create(ctx, "ec.shapecmp", fn);
    builder.CreateCondBr(pairOk, shapeCmpBb, slowBb);

    // The cached shape against the LIVE one. This subsumes `isRealShape()`:
    // the live shape is non-null (checked above) and can never be the
    // array-method sentinel, which is the integer 1 and not an arena address.
    builder.SetInsertPoint(shapeCmpBb);
    llvm::Value* cachedShapePtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, entry, BRONZE_ABI_ELEM_IC_OFFSET);
    llvm::Value* cachedShape =
        builder.CreateAlignedLoad(ptrTy, cachedShapePtr, llvm::Align(8), "ec.cshape");
    llvm::BasicBlock* depthBb = llvm::BasicBlock::Create(ctx, "ec.depth", fn);
    builder.CreateCondBr(builder.CreateICmpEQ(cachedShape, shape, "ec.shapeok"), depthBb, slowBb);

    // --- the depth word decides which answer this is ------------------------
    builder.SetInsertPoint(depthBb);
    llvm::Value* slotWordPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, entry, BRONZE_ABI_ELEM_IC_OFFSET +
                                                             BRONZE_ABI_IC_SLOTWORD_OFFSET);
    llvm::Value* slotWord =
        builder.CreateAlignedLoad(i64Ty, slotWordPtr, llvm::Align(8), "ec.slotword");
    llvm::Value* depth = builder.CreateLShr(slotWord, 32, "ec.depth.w");
    llvm::Value* slot32 = builder.CreateTrunc(slotWord, i32Ty, "ec.slot");

    llvm::BasicBlock* ownBb = llvm::BasicBlock::Create(ctx, "ec.own", fn);
    llvm::BasicBlock* absentChkBb = llvm::BasicBlock::Create(ctx, "ec.absentchk", fn);
    llvm::BasicBlock* absentBb = llvm::BasicBlock::Create(ctx, "ec.absent", fn);
    llvm::BasicBlock* hitBb = llvm::BasicBlock::Create(ctx, "ec.hit", fn);

    // depth == 0 is an OWN data property and needs no epoch: the receiver's
    // own shape transitions on every own add, which is the whole of what could
    // invalidate it. Testing the whole word at once also excludes both flags,
    // so an accessor entry (0x80000000) and an absent one (0x40000000) fall
    // out here rather than being masked off and forgotten.
    builder.CreateCondBr(builder.CreateICmpEQ(depth, builder.getInt64(0), "ec.isown"), ownBb,
                         absentChkBb);

    builder.SetInsertPoint(absentChkBb);
    builder.CreateCondBr(
        builder.CreateICmpEQ(depth, builder.getInt64(BRONZE_ABI_IC_DEPTH_ABSENT_FLAG),
                             "ec.isabsent"),
        absentBb, slowBb);

    // Absence is valid while the receiver's shape AND the proto epoch both
    // hold — the pair chunk 1 proved for the named negative IC, asked here of
    // the same two words. The shape half is the compare above; this is the
    // other half.
    builder.SetInsertPoint(absentBb);
    llvm::Value* fillEpochPtr = builder.CreateConstInBoundsGEP1_32(
        i8Ty, entry, BRONZE_ABI_ELEM_IC_OFFSET + BRONZE_ABI_IC_EPOCH_OFFSET);
    llvm::Value* fillEpoch =
        builder.CreateAlignedLoad(i64Ty, fillEpochPtr, llvm::Align(8), "ec.fillepoch");
    llvm::Value* epochPtr =
        builder.CreateConstInBoundsGEP1_64(i8Ty, tls, BRONZE_TLS_PROTO_EPOCH_OFF, "tls.epoch");
    llvm::Value* curEpoch =
        builder.CreateAlignedLoad(i64Ty, epochPtr, llvm::Align(8), "ec.curepoch");
    llvm::BasicBlock* absentEndBb = llvm::BasicBlock::Create(ctx, "ec.absent.hit", fn);
    builder.CreateCondBr(builder.CreateICmpEQ(fillEpoch, curEpoch, "ec.epochok"), absentEndBb,
                         slowBb);
    builder.SetInsertPoint(absentEndBb);
    builder.CreateBr(hitBb);

    // The slot load lands in a block of its OWN and not straight in `hitBb`,
    // because `emitObjectSlotLoad` builds a two-way PHI in whichever block it
    // is given: handed the shared join it would leave a PHI with two incoming
    // values and three predecessors, which is not IR.
    llvm::BasicBlock* ownDoneBb = llvm::BasicBlock::Create(ctx, "ec.own.done", fn);
    builder.SetInsertPoint(ownBb);
    llvm::Value* ownVal =
        emitObjectSlotLoad(builder, ctx, fn, hdr, slot32, slowBb, ownDoneBb, "ec.slot");
    builder.CreateBr(hitBb);

    builder.SetInsertPoint(hitBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "ec.result");
    result->addIncoming(ownVal, ownDoneBb);
    result->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), absentEndBb);
    builder.CreateBr(doneBb);

    return {result, hitBb};
}

}  // namespace bronze::codegen_llvm
