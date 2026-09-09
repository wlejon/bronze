#include "codegen-llvm/llvm_convert.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

namespace {

int32_t jsToInt32(double d) {
    if (!std::isfinite(d) || d == 0.0) return 0;
    const double truncated = std::trunc(d);
    const double residue = std::fmod(truncated, 4294967296.0);
    return static_cast<int32_t>(static_cast<uint32_t>(
        static_cast<int64_t>(residue < 0 ? residue + 4294967296.0 : residue)));
}

}  // namespace

bool toInt32InlineEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BRONZE_NO_INLINE_TOINT32");
        return !(env != nullptr && std::strcmp(env, "1") == 0);
    }();
    return enabled;
}

bool pureConversionHelpers() {
    static const bool enabled = [] {
        const char* env = std::getenv("BRONZE_NO_PURE_CONVERSIONS");
        return !(env != nullptr && std::strcmp(env, "1") == 0);
    }();
    return enabled;
}

llvm::Value* emitToInt32F64(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* dbl) {
    if (dbl->getType()->isIntegerTy(32)) {
        return dbl;
    }

    while (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(dbl)) {
        if (bc->getType()->isDoubleTy() && bc->getOperand(0)->getType()->isIntegerTy(64)) {
            if (auto* innerBc = llvm::dyn_cast<llvm::BitCastInst>(bc->getOperand(0))) {
                if (innerBc->getType()->isIntegerTy(64) &&
                    innerBc->getOperand(0)->getType()->isDoubleTy()) {
                    dbl = innerBc->getOperand(0);
                    continue;
                }
            }
        }
        break;
    }

    if (auto* cfp = llvm::dyn_cast<llvm::ConstantFP>(dbl)) {
        return builder.getInt32(jsToInt32(cfp->getValueAPF().convertToDouble()));
    }

    if (auto* sitofp = llvm::dyn_cast<llvm::SIToFPInst>(dbl)) {
        if (sitofp->getOperand(0)->getType()->isIntegerTy(32)) {
            return sitofp->getOperand(0);
        }
    }

    if (auto* uitofp = llvm::dyn_cast<llvm::UIToFPInst>(dbl)) {
        if (uitofp->getOperand(0)->getType()->isIntegerTy(32)) {
            return uitofp->getOperand(0);
        }
    }

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
