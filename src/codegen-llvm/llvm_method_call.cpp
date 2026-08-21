#include "codegen-llvm/llvm_method_call.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>

#include "abi/bronze_abi.h"

namespace bronze::codegen_llvm {

static constexpr uint32_t kPadSlots = 16;

llvm::Value* emitMethodCallInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                  const AbiGlobals& globals, const ModuleTables& tables,
                                  llvm::Value* thisVal, uint32_t keyIndex, uint32_t icIndex,
                                  uint32_t argc, llvm::Value* argv) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::Value* entry = icEntryPtr(builder, tables.icTable, icIndex);
    llvm::Value* safeArgv = argv ? argv : llvm::ConstantPointerNull::get(ptrTy);

    llvm::BasicBlock* plainBb = llvm::BasicBlock::Create(ctx, "mic.plain", fn);
    llvm::BasicBlock* shapeBb = llvm::BasicBlock::Create(ctx, "mic.shape", fn);
    llvm::BasicBlock* hitBb = llvm::BasicBlock::Create(ctx, "mic.hit", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "mic.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "mic.done", fn);

    // 1. Feature enable check & Object tag check
    llvm::Value* enabled = builder.CreateAlignedLoad(
        i64Ty, globals.bronze_method_call_ic_enabled, llvm::Align(8), "mic.enabled");
    llvm::Value* isEnabled = builder.CreateICmpNE(enabled, builder.getInt64(0), "mic.isenabled");

    llvm::Value* tag = builder.CreateLShr(thisVal, BRONZE_ABI_VALUE_TAG_SHIFT, "mic.tag");
    llvm::Value* isObj =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "mic.isobj");
    llvm::Value* checkOk = builder.CreateAnd(isEnabled, isObj, "mic.checkok");
    builder.CreateCondBr(checkOk, plainBb, slowBb);

    // 2. Plain Object check (flags == BRONZE_ABI_OBJ_FLAGS_PLAIN)
    builder.SetInsertPoint(plainBb);
    llvm::Value* addr = builder.CreateAnd(thisVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "mic.hdr");
    llvm::Value* flags = builder.CreateAlignedLoad(
        i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
        llvm::Align(2), "mic.flags");
    llvm::Value* isPlain =
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN), "mic.isplain");
    builder.CreateCondBr(isPlain, shapeBb, slowBb);

    // 3. Shape comparison (receiver shape == icEntry[0])
    builder.SetInsertPoint(shapeBb);
    llvm::Value* shape = builder.CreateAlignedLoad(
        ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
        llvm::Align(8), "mic.shape");
    llvm::Value* cachedShapeInt =
        builder.CreateAlignedLoad(i64Ty, entry, llvm::Align(8), "mic.cachedshape");
    llvm::Value* cachedShape = builder.CreateIntToPtr(cachedShapeInt, ptrTy, "mic.cachedshapeptr");
    llvm::Value* shapeMatch = builder.CreateICmpEQ(shape, cachedShape, "mic.shapematch");
    builder.CreateCondBr(shapeMatch, hitBb, slowBb);

    // 4. Hit path: load code pointer (icEntry[1]) and arity (icEntry[2])
    builder.SetInsertPoint(hitBb);
    llvm::Value* codeSlot = builder.CreateConstInBoundsGEP1_32(i64Ty, entry, 1);
    llvm::Value* codeInt =
        builder.CreateAlignedLoad(i64Ty, codeSlot, llvm::Align(8), "mic.codeint");
    llvm::Value* codePtr = builder.CreateIntToPtr(codeInt, ptrTy, "mic.codeptr");

    llvm::Value* aritySlot = builder.CreateConstInBoundsGEP1_32(i64Ty, entry, 2);
    llvm::Value* arity64 =
        builder.CreateAlignedLoad(i64Ty, aritySlot, llvm::Align(8), "mic.arity64");
    llvm::Value* arity = builder.CreateTrunc(arity64, i32Ty, "mic.arity");

    llvm::Value* directArityOk =
        builder.CreateICmpULE(arity, builder.getInt32(argc), "mic.dirarityok");

    llvm::BasicBlock* dispatchBb = llvm::BasicBlock::Create(ctx, "mic.dispatch", fn);
    llvm::BasicBlock* underArityCheckBb =
        llvm::BasicBlock::Create(ctx, "mic.underarity.check", fn);

    builder.CreateCondBr(directArityOk, dispatchBb, underArityCheckBb);

    // 4a. Under-arity check
    builder.SetInsertPoint(underArityCheckBb);
    llvm::Value* canPad =
        builder.CreateICmpULE(arity, builder.getInt32(kPadSlots), "mic.canpad");
    llvm::BasicBlock* underArityBb = llvm::BasicBlock::Create(ctx, "mic.underarity", fn);
    builder.CreateCondBr(canPad, underArityBb, slowBb);

    // 4b. Under-arity padding
    builder.SetInsertPoint(underArityBb);
    llvm::IRBuilder<> entryBuilder(&fn->getEntryBlock(), fn->getEntryBlock().begin());
    llvm::Value* padBuf = entryBuilder.CreateAlloca(
        llvm::ArrayType::get(i64Ty, kPadSlots), nullptr, "mic.padbuf");

    if (argc > 0 && argv) {
        for (uint32_t a = 0; a < argc && a < kPadSlots; ++a) {
            llvm::Value* srcPtr = builder.CreateGEP(i64Ty, argv, builder.getInt32(a));
            llvm::Value* val = builder.CreateAlignedLoad(i64Ty, srcPtr, llvm::Align(8));
            llvm::Value* dstPtr = builder.CreateConstInBoundsGEP2_32(
                llvm::ArrayType::get(i64Ty, kPadSlots), padBuf, 0, a);
            builder.CreateAlignedStore(val, dstPtr, llvm::Align(8));
        }
    }
    llvm::Value* undefVal = builder.getInt64(BRONZE_ABI_UNDEFINED_BITS);
    for (uint32_t a = argc; a < kPadSlots; ++a) {
        llvm::Value* dstPtr = builder.CreateConstInBoundsGEP2_32(
            llvm::ArrayType::get(i64Ty, kPadSlots), padBuf, 0, a);
        builder.CreateAlignedStore(undefVal, dstPtr, llvm::Align(8));
    }

    llvm::Value* padArgv = builder.CreateConstInBoundsGEP2_32(
        llvm::ArrayType::get(i64Ty, kPadSlots), padBuf, 0, 0);

    llvm::FunctionType* codeTy =
        llvm::FunctionType::get(i64Ty, {i64Ty, i64Ty, i32Ty, ptrTy}, false);
    llvm::Value* padRes = builder.CreateCall(
        codeTy, codePtr, {undefVal, thisVal, builder.getInt32(argc), padArgv}, "mic.padres");
    llvm::BasicBlock* padEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 4c. Direct dispatch (argc >= arity)
    builder.SetInsertPoint(dispatchBb);
    llvm::Value* fastRes = builder.CreateCall(
        codeTy, codePtr, {undefVal, thisVal, builder.getInt32(argc), safeArgv}, "mic.fastres");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 5. Slow path: runtime helper
    builder.SetInsertPoint(slowBb);
    llvm::Value* keyId = emitKeyId(builder, tables, keyIndex);
    llvm::Value* slowRes = builder.CreateCall(
        abi.bronze_call_method, {thisVal, keyId, builder.getInt32(argc), safeArgv, entry},
        "mic.slowres");
    llvm::BasicBlock* slowEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    // 6. Merge result
    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 3, "mic.result");
    result->addIncoming(fastRes, fastEndBb);
    result->addIncoming(padRes, padEndBb);
    result->addIncoming(slowRes, slowEndBb);
    return result;
}

llvm::Value* emitMethodCallSpreadInline(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                        const AbiGlobals& globals, const ModuleTables& tables,
                                        llvm::Value* thisVal, uint32_t keyIndex, uint32_t icIndex,
                                        llvm::Value* argsArr) {
    (void)globals;
    llvm::Value* entry = icEntryPtr(builder, tables.icTable, icIndex);
    llvm::Value* keyId = emitKeyId(builder, tables, keyIndex);
    return builder.CreateCall(
        abi.bronze_call_method_spread, {thisVal, keyId, argsArr, entry}, "mic.spreadres");
}

}  // namespace bronze::codegen_llvm
