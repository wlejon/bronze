#include "codegen-llvm/llvm_convert.h"

#include <cstdlib>
#include <cstring>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

bool toInt32InlineEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BRONZE_NO_INLINE_TOINT32");
        return !(env != nullptr && std::strcmp(env, "1") == 0);
    }();
    return enabled;
}

llvm::Value* emitToInt32F64(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* dbl) {
    if (!toInt32InlineEnabled()) {
        return builder.CreateCall(abi.bronze_to_int32_f64, {dbl}, "toi32");
    }

    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i32Ty = builder.getInt32Ty();
    llvm::Type* i64Ty = builder.getInt64Ty();
    llvm::Type* dblTy = builder.getDoubleTy();

    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, "toi32.fast", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "toi32.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "toi32.done", fn);

    // -2^63 and 2^63 exactly; hex float literals so no decimal rounding can
    // put the bound one ulp off the value the reasoning above depends on.
    llvm::Constant* lo = llvm::ConstantFP::get(dblTy, -0x1p63);
    llvm::Constant* hi = llvm::ConstantFP::get(dblTy, 0x1p63);
    llvm::Value* geLo = builder.CreateFCmpOGE(dbl, lo, "toi32.gelo");
    llvm::Value* ltHi = builder.CreateFCmpOLT(dbl, hi, "toi32.lthi");
    auto* br = builder.CreateCondBr(builder.CreateAnd(geLo, ltHi, "toi32.inrange"), fastBb,
                                    slowBb);
    br->setMetadata(llvm::LLVMContext::MD_prof,
                    llvm::MDBuilder(ctx).createBranchWeights(1048576, 1));

    builder.SetInsertPoint(fastBb);
    llvm::Value* wide = builder.CreateFPToSI(dbl, i64Ty, "toi32.wide");
    llvm::Value* narrow = builder.CreateTrunc(wide, i32Ty, "toi32.narrow");
    llvm::BasicBlock* fastEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* slowVal = builder.CreateCall(abi.bronze_to_int32_f64, {dbl}, "toi32.slowres");
    llvm::BasicBlock* slowEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* phi = builder.CreatePHI(i32Ty, 2, "toi32.result");
    phi->addIncoming(narrow, fastEndBb);
    phi->addIncoming(slowVal, slowEndBb);
    return phi;
}

}  // namespace bronze::codegen_llvm
