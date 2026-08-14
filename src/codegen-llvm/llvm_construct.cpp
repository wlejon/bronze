#include "codegen-llvm/llvm_construct.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>

#include "abi/bronze_abi.h"

namespace bronze::codegen_llvm {

// The layout facts this path writes and reads, restated where they are used.
// The header word is tag | flags << 16 | size << 32 as one little-endian u64:
// Object tag, Plain kind, and the one size the window ever hands out.
static_assert(BRONZE_ABI_OBJ_FLAGS_PLAIN == 0,
              "the header word below bakes flags == Plain into its constant");
static_assert(BRONZE_ABI_OBJ_FLAGS_OFFSET == 2 && BRONZE_ABI_HDR_SIZE_OFFSET == 4,
              "the header word below assumes {u16 tag, u16 flags, u32 size}");
static_assert(BRONZE_ABI_OBJ_SHAPE_OFFSET == 8 && BRONZE_ABI_OBJ_OVERFLOW_OFFSET == 16 &&
              BRONZE_ABI_OBJ_SLOTS_OFFSET == 24 && BRONZE_ABI_OBJ_INLINE_SLOTS == 4,
              "the store sequence below spells out this exact plain-object layout");
static_assert(BRONZE_ABI_PLAIN_OBJECT_BYTES ==
                  BRONZE_ABI_OBJ_SLOTS_OFFSET + BRONZE_ABI_OBJ_INLINE_SLOTS * 8,
              "the plain-object size is the slots offset plus the inline slots");

llvm::Value* emitConstructInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                 const AbiGlobals& globals, llvm::Value* ctor, uint32_t argc,
                                 llvm::Value* argv, llvm::Value* selfSlotAddr) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* fnBb = llvm::BasicBlock::Create(ctx, "new.fn", fn);
    llvm::BasicBlock* allocBb = llvm::BasicBlock::Create(ctx, "new.alloc", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "new.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "new.done", fn);

    // Guard: an Object-tagged value. Nothing may be dereferenced before this
    // — a double's payload is not a pointer.
    llvm::Value* tag = builder.CreateLShr(ctor, BRONZE_ABI_VALUE_TAG_SHIFT, "new.tag");
    llvm::Value* isObj =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "new.isobj");
    builder.CreateCondBr(isObj, fnBb, slowBb);

    // Guard: the Function heap kind. The flags word is within the minimum
    // payload every Object-tagged allocation has, so this load is safe before
    // the kind is known; every FunctionHeader field below needs this compare
    // to have passed first.
    builder.SetInsertPoint(fnBb);
    llvm::Value* fnPtr = builder.CreateIntToPtr(
        builder.CreateAnd(ctor, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK)), ptrTy,
        "new.fnptr");
    llvm::Value* flags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "new.flags");
    llvm::Value* isFn = builder.CreateICmpEQ(
        flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_FUNCTION), "new.isfn");
    builder.CreateCondBr(isFn, allocBb, slowBb);

    // The remaining guards, all loads off the (now known) FunctionHeader and
    // the window globals, folded into one branch:
    //  - the vet byte: the helper's ordinary path has run for this function,
    //    so it is not bound, not a wrapper, and its instance_shape exists;
    //  - arity <= argc: the exact condition under which FunctionHeader::call
    //    passes argv through unpadded — arity 0 satisfies it for any argc;
    //  - a non-null instance_shape, pure insurance against future drift (the
    //    vet byte already implies it);
    //  - one object of headroom in the window. The unsigned subtraction makes
    //    the dormant 0/0 window miss, and the inline path NEVER collects:
    //    when the object does not fit, the helper — which can — is the path.
    builder.SetInsertPoint(allocBb);
    llvm::Value* vet = builder.CreateAlignedLoad(
        i8Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_CTOR_VETTED_OFFSET),
        llvm::Align(1), "new.vet");
    llvm::Value* vetOk = builder.CreateICmpNE(vet, builder.getInt8(0), "new.vetok");
    llvm::Value* arity = builder.CreateAlignedLoad(
        i32Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_ARITY_OFFSET),
        llvm::Align(4), "new.arity");
    llvm::Value* arityOk = builder.CreateICmpULE(arity, builder.getInt32(argc), "new.arityok");
    llvm::Value* shape = builder.CreateAlignedLoad(
        i64Ty,
        builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_INSTANCE_SHAPE_OFFSET),
        llvm::Align(8), "new.shape");
    llvm::Value* shapeOk =
        builder.CreateICmpNE(shape, builder.getInt64(0), "new.shapeok");
    llvm::Value* cursor = builder.CreateAlignedLoad(i64Ty, globals.bronze_alloc_cursor,
                                                    llvm::Align(8), "new.cursor");
    llvm::Value* limit = builder.CreateAlignedLoad(i64Ty, globals.bronze_alloc_limit,
                                                   llvm::Align(8), "new.limit");
    llvm::Value* headroom = builder.CreateSub(limit, cursor, "new.headroom");
    llvm::Value* fits = builder.CreateICmpUGE(
        headroom, builder.getInt64(BRONZE_ABI_PLAIN_OBJECT_BYTES), "new.fits");
    llvm::Value* ok = builder.CreateAnd(builder.CreateAnd(vetOk, arityOk),
                                        builder.CreateAnd(shapeOk, fits), "new.ok");

    llvm::BasicBlock* buildBb = llvm::BasicBlock::Create(ctx, "new.build", fn);
    builder.CreateCondBr(ok, buildBb, slowBb);

    // The instance, spelled as the stores ObjectHeader::create performs:
    // header word, shape, undefined overflow, undefined inline slots. Pure
    // pointer arithmetic — no call, no collection — so nothing here can move
    // anything.
    builder.SetInsertPoint(buildBb);
    builder.CreateAlignedStore(
        builder.CreateAdd(cursor, builder.getInt64(BRONZE_ABI_PLAIN_OBJECT_BYTES)),
        globals.bronze_alloc_cursor, llvm::Align(8));
    llvm::Value* objPtr = builder.CreateIntToPtr(cursor, ptrTy, "new.obj");
    constexpr uint64_t kHeaderWord =
        static_cast<uint64_t>(BRONZE_ABI_TAG_OBJECT) |
        (static_cast<uint64_t>(BRONZE_ABI_PLAIN_OBJECT_BYTES) << 32);
    builder.CreateAlignedStore(builder.getInt64(kHeaderWord), objPtr, llvm::Align(8));
    auto storeWord = [&](unsigned byteOffset, llvm::Value* word) {
        builder.CreateAlignedStore(
            word, builder.CreateConstInBoundsGEP1_32(i8Ty, objPtr, byteOffset), llvm::Align(8));
    };
    storeWord(BRONZE_ABI_OBJ_SHAPE_OFFSET, shape);
    llvm::Value* undef = builder.getInt64(BRONZE_ABI_UNDEFINED_BITS);
    storeWord(BRONZE_ABI_OBJ_OVERFLOW_OFFSET, undef);
    for (unsigned s = 0; s < BRONZE_ABI_OBJ_INLINE_SLOTS; ++s) {
        storeWord(BRONZE_ABI_OBJ_SLOTS_OFFSET + s * 8, undef);
    }

    // Root the instance, then run the constructor exactly as
    // FunctionHeader::call's unpadded branch does: code(env, this, argc,
    // argv). The reload after the call is the point of the root slot — the
    // constructor's own allocations may relocate the instance, and the slot
    // is what the collector forwards.
    llvm::Value* instBits = builder.CreateOr(
        cursor,
        builder.getInt64(static_cast<uint64_t>(BRONZE_ABI_TAG_OBJECT)
                         << BRONZE_ABI_VALUE_TAG_SHIFT),
        "new.inst");
    builder.CreateStore(instBits, selfSlotAddr);
    llvm::Value* env = builder.CreateAlignedLoad(
        i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_ENV_OFFSET),
        llvm::Align(8), "new.env");
    llvm::Value* code = builder.CreateAlignedLoad(
        ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_CODE_OFFSET),
        llvm::Align(8), "new.code");
    llvm::FunctionType* codeTy =
        llvm::FunctionType::get(i64Ty, {i64Ty, i64Ty, i32Ty, ptrTy}, false);
    llvm::Value* callRes = builder.CreateCall(
        codeTy, code, {env, instBits, builder.getInt32(argc), argv}, "new.callres");
    llvm::Value* self = builder.CreateLoad(i64Ty, selfSlotAddr, "new.self");
    // A constructor returning an object replaces the instance; any other
    // return value (including the undefined a pending exception rides out
    // on) leaves the instance as the answer — the helper's exact foot rule.
    llvm::Value* resIsObj = builder.CreateICmpEQ(
        builder.CreateLShr(callRes, BRONZE_ABI_VALUE_TAG_SHIFT),
        builder.getInt64(BRONZE_ABI_TAG_OBJECT), "new.resisobj");
    llvm::Value* fastVal = builder.CreateSelect(resIsObj, callRes, self, "new.fastval");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(
        abi.bronze_construct, {ctor, builder.getInt32(argc), argv}, "new.slowval");
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "new.result");
    result->addIncoming(fastVal, fastEndBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

}  // namespace bronze::codegen_llvm
