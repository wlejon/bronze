#include "codegen-llvm/llvm_math.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>

namespace bronze::codegen_llvm {

std::optional<MathIntrinsic> mathIntrinsicFor(std::string_view keyStr, uint32_t argc) {
    if (argc == 1) {
        if (keyStr == "sqrt") return MathIntrinsic::Sqrt;
        if (keyStr == "sin") return MathIntrinsic::Sin;
        if (keyStr == "cos") return MathIntrinsic::Cos;
        if (keyStr == "abs") return MathIntrinsic::Abs;
    } else if (argc == 2) {
        if (keyStr == "min") return MathIntrinsic::Min;
        if (keyStr == "max") return MathIntrinsic::Max;
    }
    return std::nullopt;
}

static void markInvariant(llvm::LoadInst* load, llvm::LLVMContext& ctx) {
    load->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(ctx, {}));
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
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::Function* expectedCode = nullptr;
    switch (kind) {
        case MathIntrinsic::Sqrt: expectedCode = abi.bronze_math_sqrt; break;
        case MathIntrinsic::Sin: expectedCode = abi.bronze_math_sin; break;
        case MathIntrinsic::Cos: expectedCode = abi.bronze_math_cos; break;
        case MathIntrinsic::Abs: expectedCode = abi.bronze_math_abs; break;
        case MathIntrinsic::Min: expectedCode = abi.bronze_math_min; break;
        case MathIntrinsic::Max: expectedCode = abi.bronze_math_max; break;
    }

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
    builder.CreateCondBr(isObj, flagsBb, slowBb);

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
    builder.CreateCondBr(isFn, codeBb, slowBb);

    // 2. ...whose code pointer IS the intrinsic — the identity the collector
    // can never move and an overwrite can never fake.
    builder.SetInsertPoint(codeBb);
    llvm::Value* codePtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_FN_CODE_OFFSET);
    auto* code = builder.CreateAlignedLoad(ptrTy, codePtr, llvm::Align(8), "math.codeptr");
    markInvariant(code, ctx);
    llvm::Value* codeOk = builder.CreateICmpEQ(code, expectedCode, "math.codeok");
    builder.CreateCondBr(codeOk, argsBb, slowBb);

    // 3. ...called with numbers, so the helper's ToNumber ladder (which can
    // run user code) has nothing to do.
    builder.SetInsertPoint(argsBb);
    llvm::Value* argsOk = builder.getInt1(true);
    for (llvm::Value* arg : args) {
        llvm::Value* isNum = builder.CreateICmpULE(
            arg, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "math.argnum");
        argsOk = builder.CreateAnd(argsOk, isNum);
    }
    builder.CreateCondBr(argsOk, fastBb, slowBb);

    builder.SetInsertPoint(fastBb);
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
            r = builder.CreateCall(abi.bronze_math_sin_f64, {x});
            break;
        case MathIntrinsic::Cos:
            r = builder.CreateCall(abi.bronze_math_cos_f64, {x});
            break;
        case MathIntrinsic::Min:
        case MathIntrinsic::Max: {
            llvm::Value* y = builder.CreateBitCast(args[1], dblTy, "math.y");
            llvm::Function* kernel = kind == MathIntrinsic::Min ? abi.bronze_math_min2_f64
                                                                : abi.bronze_math_max2_f64;
            r = builder.CreateCall(kernel, {x, y});
            break;
        }
    }
    // The same re-box Value::fromDouble performs: NaN canonicalized, anything
    // else its own bits.
    llvm::Value* isNan = builder.CreateFCmpUNO(r, r, "math.isnan");
    llvm::Value* rBits = builder.CreateBitCast(r, i64Ty);
    llvm::Value* fastVal = builder.CreateSelect(
        isNan, builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS), rBits, "math.fastval");
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

}  // namespace bronze::codegen_llvm
