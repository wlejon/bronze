#include "codegen-llvm/llvm_math.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>

#include "codegen-llvm/llvm_convert.h"
#include "codegen-llvm/llvm_elem_typed.h"
#include "codegen-llvm/llvm_prop_ic.h"

namespace bronze::codegen_llvm {

std::optional<MathIntrinsic> mathIntrinsicFor(std::string_view keyStr, uint32_t argc) {
    if (argc == 1) {
        if (keyStr == "sqrt") return MathIntrinsic::Sqrt;
        if (keyStr == "sin") return MathIntrinsic::Sin;
        if (keyStr == "cos") return MathIntrinsic::Cos;
        if (keyStr == "abs") return MathIntrinsic::Abs;
        if (keyStr == "floor") return MathIntrinsic::Floor;
        if (keyStr == "ceil") return MathIntrinsic::Ceil;
        if (keyStr == "round") return MathIntrinsic::Round;
    } else if (argc == 2) {
        if (keyStr == "min") return MathIntrinsic::Min;
        if (keyStr == "max") return MathIntrinsic::Max;
        if (keyStr == "imul") return MathIntrinsic::Imul;
    }
    return std::nullopt;
}

static llvm::Function* mathExpectedCode(const AbiFns& abi, MathIntrinsic kind) {
    switch (kind) {
        case MathIntrinsic::Sqrt: return abi.bronze_math_sqrt;
        case MathIntrinsic::Sin: return abi.bronze_math_sin;
        case MathIntrinsic::Cos: return abi.bronze_math_cos;
        case MathIntrinsic::Abs: return abi.bronze_math_abs;
        case MathIntrinsic::Min: return abi.bronze_math_min;
        case MathIntrinsic::Max: return abi.bronze_math_max;
        case MathIntrinsic::Imul: return abi.bronze_math_imul;
        case MathIntrinsic::Floor: return abi.bronze_math_floor;
        case MathIntrinsic::Ceil: return abi.bronze_math_ceil;
        case MathIntrinsic::Round: return abi.bronze_math_round;
    }
    return nullptr;
}

llvm::Value* emitMathComputeRaw(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                MathIntrinsic kind, llvm::ArrayRef<llvm::Value*> args) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);

    if (kind == MathIntrinsic::Imul) {
        llvm::Value* x = builder.CreateBitCast(args[0], dblTy, "math.x");
        llvm::Value* y = builder.CreateBitCast(args[1], dblTy, "math.y");
        llvm::Value* xi = emitToInt32F64(builder, abi, x);
        llvm::Value* yi = emitToInt32F64(builder, abi, y);
        llvm::Value* mul = builder.CreateMul(xi, yi, "math.imul");
        return builder.CreateSIToFP(mul, dblTy, "math.imul.dbl");
    }

    llvm::Value* x = builder.CreateBitCast(args[0], dblTy, "math.x");
    llvm::Value* r = nullptr;
    switch (kind) {
        case MathIntrinsic::Sqrt:
            r = builder.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, x);
            break;
        case MathIntrinsic::Abs:
            r = builder.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, x);
            break;
        case MathIntrinsic::Sin:
            r = builder.CreateUnaryIntrinsic(llvm::Intrinsic::sin, x);
            break;
        case MathIntrinsic::Cos:
            r = builder.CreateUnaryIntrinsic(llvm::Intrinsic::cos, x);
            break;
        case MathIntrinsic::Floor:
            r = builder.CreateUnaryIntrinsic(llvm::Intrinsic::floor, x);
            break;
        case MathIntrinsic::Ceil:
            r = builder.CreateUnaryIntrinsic(llvm::Intrinsic::ceil, x);
            break;
        case MathIntrinsic::Round:
            r = builder.CreateUnaryIntrinsic(llvm::Intrinsic::round, x);
            break;
        case MathIntrinsic::Min:
        case MathIntrinsic::Max: {
            llvm::Value* y = builder.CreateBitCast(args[1], dblTy, "math.y");
            llvm::Function* kernel = kind == MathIntrinsic::Min ? abi.bronze_math_min2_f64
                                                                : abi.bronze_math_max2_f64;
            r = builder.CreateCall(kernel, {x, y});
            break;
        }
        case MathIntrinsic::Imul:
            llvm_unreachable("handled above");
    }
    return r;
}

llvm::Value* emitMathCompute(llvm::IRBuilder<>& builder, const AbiFns& abi,
                            MathIntrinsic kind, llvm::ArrayRef<llvm::Value*> args) {
    llvm::Value* r = emitMathComputeRaw(builder, abi, kind, args);
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    // The same re-box Value::fromDouble performs: NaN canonicalized, anything
    // else its own bits.
    llvm::Value* isNan = builder.CreateFCmpUNO(r, r, "math.isnan");
    llvm::Value* rBits = builder.CreateBitCast(r, i64Ty);
    return builder.CreateSelect(
        isNan, builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS), rBits, "math.fastval");
}

llvm::Value* emitMathDirectCall(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                MathIntrinsic kind, llvm::Value* calleeBits,
                                llvm::Value* thisBits, uint32_t argc, llvm::Value* argvPtr,
                                llvm::ArrayRef<llvm::Value*> args) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

    llvm::Function* expectedCode = mathExpectedCode(abi, kind);

    llvm::BasicBlock* flagsBb = llvm::BasicBlock::Create(ctx, "math.flags", fn);
    llvm::BasicBlock* codeBb = llvm::BasicBlock::Create(ctx, "math.code", fn);
    llvm::BasicBlock* argsBb = llvm::BasicBlock::Create(ctx, "math.args", fn);
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, "math.fast", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "math.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "math.done", fn);

    // 1. The callee is a function object...
    llvm::Value* tag = builder.CreateLShr(calleeBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObj =
        builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "math.isobj");
    auto* brObj = builder.CreateCondBr(isObj, flagsBb, slowBb);
    brObj->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(flagsBb);
    llvm::Value* addr =
        builder.CreateAnd(calleeBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "math.hdr");
    llvm::Value* flagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    auto* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), "math.kind");
    markInvariant(flags, ctx);
    llvm::Value* isFn =
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_FUNCTION));
    auto* brFn = builder.CreateCondBr(isFn, codeBb, slowBb);
    brFn->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 2. ...whose code pointer IS the intrinsic — the identity the collector
    // can never move and an overwrite can never fake.
    builder.SetInsertPoint(codeBb);
    llvm::Value* codePtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_FN_CODE_OFFSET);
    auto* code = builder.CreateAlignedLoad(ptrTy, codePtr, llvm::Align(8), "math.codeptr");
    markInvariant(code, ctx);
    llvm::Value* codeOk = builder.CreateICmpEQ(code, expectedCode, "math.codeok");
    auto* brCode = builder.CreateCondBr(codeOk, argsBb, slowBb);
    brCode->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    // 3. ...called with numbers, so the helper's ToNumber ladder (which can
    // run user code) has nothing to do.
    builder.SetInsertPoint(argsBb);
    llvm::Value* argsOk = builder.getInt1(true);
    for (llvm::Value* arg : args) {
        llvm::Value* isNum = builder.CreateICmpULE(
            arg, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "math.argnum");
        argsOk = builder.CreateAnd(argsOk, isNum);
    }
    auto* brArgs = builder.CreateCondBr(argsOk, fastBb, slowBb);
    brArgs->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(fastBb);
    llvm::Value* fastVal = emitMathCompute(builder, abi, kind, args);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(
        abi.bronze_dynamic_call, {calleeBits, thisBits, builder.getInt32(argc), argvPtr});
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* result = builder.CreatePHI(i64Ty, 2, "math.result");
    result->addIncoming(fastVal, fastBb);
    result->addIncoming(slowVal, slowBb);
    return result;
}

llvm::Value* emitMethodCallMathDirect(
    llvm::IRBuilder<>& builder, const AbiFns& abi, const AbiGlobals& globals,
    const ModuleTables& tables, MathIntrinsic kind, llvm::Value* thisVal,
    uint32_t keyIndex, uint32_t icIndex, uint32_t argc,
    llvm::ArrayRef<llvm::Value*> args,
    llvm::function_ref<llvm::Value*()> missEmit,
    llvm::Value** lastGuardedMathRecv,
    bool resultAsF64) {
    (void)keyIndex;
    (void)argc;
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::BasicBlock* argsBb = llvm::BasicBlock::Create(ctx, "math.m.args", fn);
    llvm::BasicBlock* missBb = llvm::BasicBlock::Create(ctx, "math.m.miss", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "math.m.done", fn);

    if (lastGuardedMathRecv != nullptr && *lastGuardedMathRecv == thisVal) {
        builder.CreateBr(argsBb);
    } else {
        llvm::Function* expectedCode = mathExpectedCode(abi, kind);
        llvm::Value* entry = icEntryPtr(builder, tables.icTable, icIndex);
        llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);

        llvm::BasicBlock* plainBb = llvm::BasicBlock::Create(ctx, "math.m.plain", fn);

        auto* enabled = builder.CreateAlignedLoad(
            i64Ty, globals.bronze_method_call_ic_enabled, llvm::Align(8), "math.m.enabled");
        markInvariant(enabled, ctx);
        llvm::Value* isEnabled = builder.CreateICmpNE(enabled, builder.getInt64(0), "math.m.isenabled");
        llvm::Value* tag = builder.CreateLShr(thisVal, BRONZE_ABI_VALUE_TAG_SHIFT, "math.m.tag");
        llvm::Value* isObj =
            builder.CreateICmpEQ(tag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "math.m.isobj");
        auto* brGuard = builder.CreateCondBr(builder.CreateAnd(isEnabled, isObj), plainBb, missBb);
        brGuard->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

        builder.SetInsertPoint(plainBb);
        llvm::Value* addr = builder.CreateAnd(thisVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
        llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "math.m.hdr");
        auto* flags = builder.CreateAlignedLoad(
            i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
            llvm::Align(2), "math.m.flags");
        markInvariant(flags, ctx);
        llvm::Value* isPlain =
            builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_PLAIN), "math.m.isplain");
        llvm::BasicBlock* shapeBb = llvm::BasicBlock::Create(ctx, "math.m.shape", fn);
        auto* brPlain = builder.CreateCondBr(isPlain, shapeBb, missBb);
        brPlain->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

        builder.SetInsertPoint(shapeBb);
        llvm::Value* shape = builder.CreateAlignedLoad(
            ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SHAPE_OFFSET),
            llvm::Align(8), "math.m.shape");
        llvm::Value* cachedShapeInt =
            builder.CreateAlignedLoad(i64Ty, entry, llvm::Align(8), "math.m.cachedshape");
        llvm::Value* cachedShape = builder.CreateIntToPtr(cachedShapeInt, ptrTy, "math.m.cachedshapeptr");
        llvm::Value* shapeMatch = builder.CreateICmpEQ(shape, cachedShape, "math.m.shapematch");
        llvm::BasicBlock* formBb = llvm::BasicBlock::Create(ctx, "math.m.form", fn);
        auto* brShape = builder.CreateCondBr(shapeMatch, formBb, missBb);
        brShape->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

        builder.SetInsertPoint(formBb);
        llvm::Value* arityWord = builder.CreateAlignedLoad(
            i64Ty, builder.CreateConstInBoundsGEP1_32(i64Ty, entry, BRONZE_ABI_METHOD_IC_ARITY_WORD),
            llvm::Align(8), "math.m.arityword");
        llvm::Value* formBits =
            builder.CreateLShr(arityWord, BRONZE_ABI_METHOD_IC_SLOT_SHIFT, "math.m.formbits");
        llvm::Value* isSlotForm =
            builder.CreateICmpNE(formBits, builder.getInt64(0), "math.m.isslotform");
        llvm::BasicBlock* slotBb = llvm::BasicBlock::Create(ctx, "math.m.slot", fn);
        auto* brSlot = builder.CreateCondBr(isSlotForm, slotBb, missBb);
        brSlot->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

        builder.SetInsertPoint(slotBb);
        llvm::Value* slotIdx = builder.CreateSub(formBits, builder.getInt64(1), "math.m.slotidx");
        llvm::Value* isInline = builder.CreateICmpULT(
            slotIdx, builder.getInt64(BRONZE_ABI_OBJ_INLINE_SLOTS), "math.m.isinline");
        llvm::BasicBlock* slotInlBb = llvm::BasicBlock::Create(ctx, "math.m.slot.inl", fn);
        llvm::BasicBlock* slotOvBb = llvm::BasicBlock::Create(ctx, "math.m.slot.ov", fn);
        llvm::BasicBlock* slotLoadBb = llvm::BasicBlock::Create(ctx, "math.m.slot.load", fn);
        auto* brInl = builder.CreateCondBr(isInline, slotInlBb, slotOvBb);
        brInl->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

        builder.SetInsertPoint(slotInlBb);
        llvm::Value* inlBase =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_SLOTS_OFFSET);
        llvm::Value* inlPtr = builder.CreateGEP(i64Ty, inlBase, slotIdx, "math.m.slot.inlptr");
        builder.CreateBr(slotLoadBb);

        builder.SetInsertPoint(slotOvBb);
        llvm::Value* ovBits = builder.CreateAlignedLoad(
            i64Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_OVERFLOW_OFFSET),
            llvm::Align(8), "math.m.slot.ovbits");
        llvm::Value* ovAddr =
            builder.CreateAnd(ovBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
        llvm::Value* ovHdr = builder.CreateIntToPtr(ovAddr, ptrTy, "math.m.slot.ovhdr");
        llvm::Value* ovBase = builder.CreateConstInBoundsGEP1_32(i8Ty, ovHdr, BRONZE_ABI_HDR_BYTES);
        llvm::Value* ovIdx = builder.CreateSub(
            slotIdx, builder.getInt64(BRONZE_ABI_OBJ_INLINE_SLOTS), "math.m.slot.ovidx");
        llvm::Value* ovPtr = builder.CreateGEP(i64Ty, ovBase, ovIdx, "math.m.slot.ovptr");
        builder.CreateBr(slotLoadBb);

        builder.SetInsertPoint(slotLoadBb);
        llvm::PHINode* slotPtr = builder.CreatePHI(ptrTy, 2, "math.m.slot.ptr");
        slotPtr->addIncoming(inlPtr, slotInlBb);
        slotPtr->addIncoming(ovPtr, slotOvBb);
        llvm::Value* slotVal =
            builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "math.m.slot.val");

        llvm::Value* fnTag = builder.CreateLShr(slotVal, BRONZE_ABI_VALUE_TAG_SHIFT, "math.m.fntag");
        llvm::Value* fnIsObj =
            builder.CreateICmpEQ(fnTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT), "math.m.fnisobj");
        llvm::BasicBlock* fnHdrBb = llvm::BasicBlock::Create(ctx, "math.m.fnhdr", fn);
        auto* brFnObj = builder.CreateCondBr(fnIsObj, fnHdrBb, missBb);
        brFnObj->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

        builder.SetInsertPoint(fnHdrBb);
        llvm::Value* fnAddr = builder.CreateAnd(slotVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
        llvm::Value* fnPtr = builder.CreateIntToPtr(fnAddr, ptrTy, "math.m.fnptr");
        auto* fnFlags = builder.CreateAlignedLoad(
            i16Ty, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_OBJ_FLAGS_OFFSET),
            llvm::Align(2), "math.m.fnflags");
        markInvariant(fnFlags, ctx);
        llvm::Value* isFn =
            builder.CreateICmpEQ(fnFlags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_FUNCTION), "math.m.isfn");
        llvm::BasicBlock* codeBb = llvm::BasicBlock::Create(ctx, "math.m.code", fn);
        auto* brFn = builder.CreateCondBr(isFn, codeBb, missBb);
        brFn->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

        builder.SetInsertPoint(codeBb);
        auto* code = builder.CreateAlignedLoad(
            ptrTy, builder.CreateConstInBoundsGEP1_32(i8Ty, fnPtr, BRONZE_ABI_FN_CODE_OFFSET),
            llvm::Align(8), "math.m.codeptr");
        markInvariant(code, ctx);
        llvm::Value* codeOk = builder.CreateICmpEQ(code, expectedCode, "math.m.codeok");
        auto* brCode = builder.CreateCondBr(codeOk, argsBb, missBb);
        brCode->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);
    }

    builder.SetInsertPoint(argsBb);
    llvm::MDNode* likelyBranch = llvm::MDBuilder(ctx).createBranchWeights(1048576, 1);
    llvm::Value* argsOk = builder.getInt1(true);
    for (llvm::Value* arg : args) {
        if (isProvenNumberValue(arg)) continue;
        llvm::Value* isNum = builder.CreateICmpULE(
            arg, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "math.m.argnum");
        argsOk = builder.CreateAnd(argsOk, isNum);
    }
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, "math.m.fast", fn);
    auto* brArgs = builder.CreateCondBr(argsOk, fastBb, missBb);
    brArgs->setMetadata(llvm::LLVMContext::MD_prof, likelyBranch);

    builder.SetInsertPoint(fastBb);
    llvm::Value* fastVal = resultAsF64 ? emitMathComputeRaw(builder, abi, kind, args)
                                       : emitMathCompute(builder, abi, kind, args);
    if (lastGuardedMathRecv != nullptr) {
        *lastGuardedMathRecv = thisVal;
    }
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(missBb);
    if (lastGuardedMathRecv != nullptr) {
        *lastGuardedMathRecv = nullptr;
    }
    llvm::Value* missVal = missEmit();
    if (resultAsF64) {
        missVal = builder.CreateBitCast(missVal, dblTy, "math.m.miss.f64");
    }
    llvm::BasicBlock* missEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::Type* resTy = resultAsF64 ? dblTy : i64Ty;
    llvm::PHINode* result = builder.CreatePHI(resTy, 2, "math.m.result");
    result->addIncoming(fastVal, fastEndBb);
    result->addIncoming(missVal, missEndBb);
    return result;
}

}  // namespace bronze::codegen_llvm
