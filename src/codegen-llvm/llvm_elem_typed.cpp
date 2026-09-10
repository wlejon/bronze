#include <cmath>

#include "codegen-llvm/llvm_elem_typed.h"
#include "codegen-llvm/llvm_elem.h"
#include "codegen-llvm/llvm_alias.h"
#include "codegen-llvm/llvm_convert.h"
#include "il/il.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

// The address of element `idx32` of a typed-array view, computed from
// the view's buffer Value on every access — never cached across allocations,
// per the GC rule the header documents. The builder must already be in the
// view's arm.
// Computes the typed array data base pointer (inline data or external store + byte offset).
llvm::Value* emitTypedArrayBasePtr(llvm::IRBuilder<>& builder, llvm::Value* hdr) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    llvm::Value* bufPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_BUFFER_OFFSET);
    auto* bufVal = builder.CreateAlignedLoad(i64Ty, bufPtr, llvm::Align(8), "ta.buf");
    bufVal->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(ctx, {}));
    llvm::Value* bufAddr =
        builder.CreateAnd(bufVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* bufHdr = builder.CreateIntToPtr(bufAddr, ptrTy);
    llvm::Value* byteOffPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_BYTEOFFSET_OFFSET);
    auto* byteOff =
        builder.CreateAlignedLoad(i32Ty, byteOffPtr, llvm::Align(4), "ta.byteoff");
    byteOff->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(ctx, {}));

    llvm::Value* extPtrPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, bufHdr, BRONZE_ABI_BUF_EXTPTR_OFFSET);
    auto* extBits = builder.CreateAlignedLoad(i64Ty, extPtrPtr, llvm::Align(8), "ta.extbits");
    extBits->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(ctx, {}));
    tagViewLengthAccess(extBits, ctx);
    llvm::Value* inlineBase = builder.CreateAdd(
        bufAddr, builder.getInt64(BRONZE_ABI_BUF_DATA_OFFSET), "ta.inlinebase");
    llvm::Value* isExt = builder.CreateICmpNE(extBits, builder.getInt64(0), "ta.isext");
    llvm::Value* base = builder.CreateSelect(isExt, extBits, inlineBase, "ta.base");

    llvm::Value* basePtr = builder.CreateIntToPtr(base, ptrTy, "ta.base.ptr");
    llvm::Value* byteOff64 = builder.CreateZExt(byteOff, i64Ty, "ta.byteoff64");
    return builder.CreateInBoundsGEP(i8Ty, basePtr, byteOff64, "ta.data.ptr");
}

// Computes the address of element `idx32` given an already computed data base pointer.
llvm::Value* emitTypedArrayElemPtrFromBase(llvm::IRBuilder<>& builder, llvm::Value* dataPtr,
                                           llvm::Value* idx32, uint32_t elemSize) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Value* idx64 = builder.CreateZExt(idx32, i64Ty, "ta.idx64");
    llvm::Type* elemTy = (elemSize == 8)
                             ? builder.getDoubleTy()
                             : ((elemSize == 4) ? builder.getFloatTy()
                                                : ((elemSize == 2) ? builder.getInt16Ty() : i8Ty));
    return builder.CreateInBoundsGEP(elemTy, dataPtr, idx64, "ta.elem.ptr");
}

// The address of element `idx32` of a typed-array view, computed from
// the view's buffer Value on every access — never cached across allocations,
// per the GC rule the header documents. The builder must already be in the
// view's arm.
llvm::Value* emitTypedArrayElemPtr(llvm::IRBuilder<>& builder, llvm::Value* hdr,
                                   llvm::Value* idx32, uint32_t elemSize) {
    llvm::Value* dataPtr = emitTypedArrayBasePtr(builder, hdr);
    return emitTypedArrayElemPtrFromBase(builder, dataPtr, idx32, elemSize);
}

namespace {

// The shared front half of the two PROVEN forms: validate the index the way
// 23.2 does — an integral, in-range value inside the view — and hand back the
// header pointer and the i32 index. The receiver needs NO guard at all: the
// op only exists where inference proved the view, which is the contract that
// separates these from emitElemGet above. The select-before-fptoui is the
// same poison discipline emitElemGuards documents: the conversion must be fed
// a value the range check has already accepted on EVERY path.
//
// The bounds check is against the view's length, exactly the bound
// bronze_elem_get / _set use, so the two modes answer identically byte for
// byte — including over a detached or shrunk-away buffer, whose views the
// runtime CLOSES by zeroing this very length word (closeOrReopenViews), so
// the one compare below is also the 10.4.5.9 out-of-bounds check. That is
// why the length load is scoped rather than invariant (llvm_alias.h).
struct TypedElemGuards {
    llvm::Value* hdr;
    llvm::Value* dataPtr;
    llvm::Value* idx32;
    llvm::Value* ok;
};

bool isProvenNonNegativeI32(llvm::Value* v, int depth = 3) {
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
        return ci->getSExtValue() >= 0;
    }
    if (llvm::isa<llvm::ZExtInst>(v)) return true;
    if (depth <= 0) return false;
    if (auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(v)) {
        if (bin->getOpcode() == llvm::Instruction::LShr) {
            if (auto* shiftAmt = llvm::dyn_cast<llvm::ConstantInt>(bin->getOperand(1))) {
                if (shiftAmt->getZExtValue() >= 1 && shiftAmt->getZExtValue() < 32) return true;
            }
            return isProvenNonNegativeI32(bin->getOperand(0), depth - 1);
        }
        if (bin->getOpcode() == llvm::Instruction::AShr) {
            return isProvenNonNegativeI32(bin->getOperand(0), depth - 1);
        }
        if (bin->getOpcode() == llvm::Instruction::And) {
            return isProvenNonNegativeI32(bin->getOperand(0), depth - 1) ||
                   isProvenNonNegativeI32(bin->getOperand(1), depth - 1);
        }
        if (bin->getOpcode() == llvm::Instruction::Add ||
            bin->getOpcode() == llvm::Instruction::Mul) {
            return isProvenNonNegativeI32(bin->getOperand(0), depth - 1) &&
                   isProvenNonNegativeI32(bin->getOperand(1), depth - 1);
        }
    }
    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
        if (phi->getNumIncomingValues() > 0 && phi->getNumIncomingValues() <= 4) {
            for (unsigned int i = 0; i < phi->getNumIncomingValues(); ++i) {
                llvm::Value* inc = phi->getIncomingValue(i);
                if (inc == phi) continue;
                if (!isProvenNonNegativeI32(inc, depth - 1)) return false;
            }
            return true;
        }
    }
    return false;
}

bool isProvenIntegralDoubleHelper(llvm::Value* v, llvm::SmallPtrSetImpl<llvm::PHINode*>& visited,
                                  int depth) {
    if (auto* cfp = llvm::dyn_cast<llvm::ConstantFP>(v)) {
        double d = cfp->getValueAPF().convertToDouble();
        return d >= 0.0 && d <= 4294967295.0 && std::trunc(d) == d;
    }
    if (auto* uitofp = llvm::dyn_cast<llvm::UIToFPInst>(v)) {
        return uitofp->getOperand(0)->getType()->isIntegerTy(32);
    }
    if (auto* sitofp = llvm::dyn_cast<llvm::SIToFPInst>(v)) {
        if (sitofp->getOperand(0)->getType()->isIntegerTy(32)) {
            return isProvenNonNegativeI32(sitofp->getOperand(0), depth);
        }
    }
    if (depth <= 0) return false;

    if (auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(v)) {
        if (bin->getOpcode() == llvm::Instruction::FAdd ||
            bin->getOpcode() == llvm::Instruction::FMul) {
            return isProvenIntegralDoubleHelper(bin->getOperand(0), visited, depth - 1) &&
                   isProvenIntegralDoubleHelper(bin->getOperand(1), visited, depth - 1);
        }
        if (bin->getOpcode() == llvm::Instruction::FSub) {
            if (auto* crhs = llvm::dyn_cast<llvm::ConstantFP>(bin->getOperand(1))) {
                double r = crhs->getValueAPF().convertToDouble();
                if (auto* clhs = llvm::dyn_cast<llvm::ConstantFP>(bin->getOperand(0))) {
                    double l = clhs->getValueAPF().convertToDouble();
                    double res = l - r;
                    return res >= 0.0 && res <= 4294967295.0 && std::trunc(res) == res;
                }
            }
            if (isProvenIntegralDoubleHelper(bin->getOperand(0), visited, depth - 1) &&
                isProvenIntegralDoubleHelper(bin->getOperand(1), visited, depth - 1)) {
                return true;
            }
        }
    }
    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
        if (visited.contains(phi)) {
            return true;
        }
        if (phi->getNumIncomingValues() > 0 && phi->getNumIncomingValues() <= 4) {
            visited.insert(phi);
            bool allOk = true;
            bool hasBaseCase = false;
            for (unsigned int i = 0; i < phi->getNumIncomingValues(); ++i) {
                llvm::Value* inc = phi->getIncomingValue(i);
                if (inc == phi) continue;
                if (auto* incPhi = llvm::dyn_cast<llvm::PHINode>(inc)) {
                    if (visited.contains(incPhi)) {
                        continue;
                    }
                }
                if (!isProvenIntegralDoubleHelper(inc, visited, depth - 1)) {
                    allOk = false;
                    break;
                }
                hasBaseCase = true;
            }
            visited.erase(phi);
            return allOk && hasBaseCase;
        }
    }
    return false;
}

bool isProvenNonNegativeDoubleHelper(llvm::Value* v,
                                     llvm::SmallPtrSetImpl<llvm::PHINode*>& visited,
                                     int depth) {
    if (auto* cfp = llvm::dyn_cast<llvm::ConstantFP>(v)) {
        return !cfp->isNegative() && !cfp->isNaN();
    }
    if (llvm::isa<llvm::UIToFPInst>(v)) return true;
    if (auto* sitofp = llvm::dyn_cast<llvm::SIToFPInst>(v)) {
        return isProvenNonNegativeI32(sitofp->getOperand(0));
    }
    if (depth <= 0) return false;
    if (auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(v)) {
        if (bin->getOpcode() == llvm::Instruction::FAdd ||
            bin->getOpcode() == llvm::Instruction::FMul) {
            return isProvenNonNegativeDoubleHelper(bin->getOperand(0), visited, depth - 1) &&
                   isProvenNonNegativeDoubleHelper(bin->getOperand(1), visited, depth - 1);
        }
        if (bin->getOpcode() == llvm::Instruction::FSub) {
            if (auto* crhs = llvm::dyn_cast<llvm::ConstantFP>(bin->getOperand(1))) {
                if (auto* clhs = llvm::dyn_cast<llvm::ConstantFP>(bin->getOperand(0))) {
                    return clhs->getValueAPF().convertToDouble() >= crhs->getValueAPF().convertToDouble();
                }
            }
        }
    }
    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
        if (visited.contains(phi)) {
            return true;
        }
        if (phi->getNumIncomingValues() > 0 && phi->getNumIncomingValues() <= 4) {
            visited.insert(phi);
            bool allOk = true;
            bool hasBaseCase = false;
            for (unsigned int i = 0; i < phi->getNumIncomingValues(); ++i) {
                llvm::Value* inc = phi->getIncomingValue(i);
                if (inc == phi) continue;
                if (auto* incPhi = llvm::dyn_cast<llvm::PHINode>(inc)) {
                    if (visited.contains(incPhi)) {
                        continue;
                    }
                }
                if (!isProvenNonNegativeDoubleHelper(inc, visited, depth - 1)) {
                    allOk = false;
                    break;
                }
                hasBaseCase = true;
            }
            visited.erase(phi);
            return allOk && hasBaseCase;
        }
    }
    return false;
}

}  // namespace

bool isProvenNonNegativeDouble(llvm::Value* v, int depth) {
    llvm::SmallPtrSet<llvm::PHINode*, 8> visited;
    return isProvenNonNegativeDoubleHelper(v, visited, depth);
}

bool isProvenIntegralDouble(llvm::Value* v, int depth) {
    llvm::SmallPtrSet<llvm::PHINode*, 8> visited;
    return isProvenIntegralDoubleHelper(v, visited, depth);
}

llvm::Value* unwrapBoxedDouble(llvm::Value* val) {
    if (val == nullptr) return nullptr;
    if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(val)) {
        if (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(sel->getFalseValue())) {
            if (bc->getSrcTy()->isDoubleTy()) return bc->getOperand(0);
        }
        if (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(sel->getTrueValue())) {
            if (bc->getSrcTy()->isDoubleTy()) return bc->getOperand(0);
        }
    }
    if (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(val)) {
        if (bc->getSrcTy()->isDoubleTy()) return bc->getOperand(0);
    }
    if (auto* call = llvm::dyn_cast<llvm::CallInst>(val)) {
        if (auto* callee = call->getCalledFunction()) {
            if (callee->getName() == "bronze_box_f64" && call->arg_size() == 1) {
                if (call->getArgOperand(0)->getType()->isDoubleTy()) {
                    return call->getArgOperand(0);
                }
            }
        }
    }
    return nullptr;
}

bool isProvenNumberValue(llvm::Value* v, int depth) {
    if (v == nullptr) return false;
    if (unwrapBoxedDouble(v) != nullptr) return true;
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
        return ci->getValue().ule(BRONZE_ABI_NUMBER_MAX_BITS);
    }
    if (auto* call = llvm::dyn_cast<llvm::CallInst>(v)) {
        if (auto* callee = call->getCalledFunction()) {
            llvm::StringRef name = callee->getName();
            if (name == "bronze_box_f64" || name == "bronze_to_number" ||
                name == "bronze_math_imul") {
                return true;
            }
        }
    }
    if (auto* bc = llvm::dyn_cast<llvm::BitCastInst>(v)) {
        if (bc->getSrcTy()->isDoubleTy()) return true;
    }
    if (depth > 0) {
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
            if (phi->getNumIncomingValues() > 0 && phi->getNumIncomingValues() <= 4) {
                for (unsigned int i = 0; i < phi->getNumIncomingValues(); ++i) {
                    llvm::Value* inc = phi->getIncomingValue(i);
                    if (inc == phi) continue;
                    if (!isProvenNumberValue(inc, depth - 1)) return false;
                }
                return true;
            }
        }
        if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(v)) {
            return isProvenNumberValue(sel->getTrueValue(), depth - 1) &&
                   isProvenNumberValue(sel->getFalseValue(), depth - 1);
        }
    }
    return false;
}

namespace {

struct IndexExtraction {
    llvm::Value* idx32;
    llvm::Value* isIntegral;
};

IndexExtraction extractIndex32(llvm::IRBuilder<>& builder, llvm::Value* idxDbl) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);

    if (auto* cfp = llvm::dyn_cast<llvm::ConstantFP>(idxDbl)) {
        double d = cfp->getValueAPF().convertToDouble();
        if (d >= 0.0 && d <= 4294967295.0 && std::trunc(d) == d) {
            return {builder.getInt32(static_cast<uint32_t>(d)), builder.getTrue()};
        }
    } else if (auto* uitofp = llvm::dyn_cast<llvm::UIToFPInst>(idxDbl)) {
        if (uitofp->getOperand(0)->getType()->isIntegerTy(32)) {
            return {uitofp->getOperand(0), builder.getTrue()};
        }
    } else if (auto* sitofp = llvm::dyn_cast<llvm::SIToFPInst>(idxDbl)) {
        if (sitofp->getOperand(0)->getType()->isIntegerTy(32)) {
            llvm::Value* op = sitofp->getOperand(0);
            if (isProvenNonNegativeI32(op)) {
                return {op, builder.getTrue()};
            }
            llvm::Value* nonNeg = builder.CreateICmpSGE(op, builder.getInt32(0));
            return {op, nonNeg};
        }
    } else if (isProvenIntegralDouble(idxDbl)) {
        if (isProvenNonNegativeDouble(idxDbl)) {
            llvm::Value* idx32 = builder.CreateFPToUI(idxDbl, i32Ty, "tel.idx");
            return {idx32, builder.getTrue()};
        }
        llvm::Value* nonNeg = builder.CreateFCmpOGE(
            idxDbl, llvm::ConstantFP::get(dblTy, 0.0), "tel.nonneg");
        llvm::Value* safeDbl = builder.CreateSelect(
            nonNeg, idxDbl, llvm::ConstantFP::get(dblTy, 0.0));
        llvm::Value* idx32 = builder.CreateFPToUI(safeDbl, i32Ty, "tel.idx");
        return {idx32, nonNeg};
    }

    llvm::Value* idx32 = builder.CreateIntrinsic(
        llvm::Intrinsic::fptoui_sat, {i32Ty, dblTy}, {idxDbl}, nullptr, "tel.idx");
    llvm::Value* roundTrip = builder.CreateUIToFP(idx32, dblTy);
    llvm::Value* isIntegral = builder.CreateFCmpOEQ(roundTrip, idxDbl);
    return {idx32, isIntegral};
}

TypedElemGuards emitTypedElemGuards(llvm::IRBuilder<>& builder, llvm::Value* objBits,
                                    llvm::Value* idxDbl) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    IndexExtraction ie = extractIndex32(builder, idxDbl);
    llvm::Value* idx32 = ie.idx32;
    llvm::Value* isIntegral = ie.isIntegral;

    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "tel.hdr");
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "tel.len");
    tagViewLengthAccess(len, ctx);
    llvm::Value* inLen = builder.CreateICmpULT(idx32, len);

    llvm::Value* ok = (isIntegral == builder.getTrue()) ? inLen : builder.CreateAnd(isIntegral, inLen);
    llvm::Value* dataPtr = emitTypedArrayBasePtr(builder, hdr);
    return {hdr, dataPtr, idx32, ok};
}

llvm::Value* emitPlainArrayElemGet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                                   llvm::Value* objBits, llvm::Value* idxDbl) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    IndexExtraction ie = extractIndex32(builder, idxDbl);
    llvm::Value* idx32 = ie.idx32;
    llvm::Value* isIntegral = ie.isIntegral;

    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "pel.hdr");
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "pel.len");
    tagArrayHeaderAccess(len, ctx);
    llvm::Value* inBounds = builder.CreateICmpULT(idx32, len);

    llvm::Value* ok = (isIntegral == builder.getTrue())
                          ? inBounds
                          : builder.CreateAnd(isIntegral, inBounds);

    llvm::BasicBlock* readBb = llvm::BasicBlock::Create(ctx, "pel.read", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "pel.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "pel.done", fn);
    llvm::BasicBlock* entryBb = builder.GetInsertBlock();

    llvm::Value* nanVal = builder.CreateBitCast(
        builder.getInt64(BRONZE_ABI_CANONICAL_NAN_BITS), dblTy, "pel.nan");

    auto* condBr = builder.CreateCondBr(ok, readBb, doneBb);
    condBr->setMetadata(llvm::LLVMContext::MD_prof,
                        llvm::MDBuilder(ctx).createBranchWeights(1048576, 1));

    builder.SetInsertPoint(readBb);
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "pel.elems");
    tagArrayHeaderAccess(elemsVal, ctx);
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* headPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_HEAD_OFFSET);
    auto* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), "pel.head");
    tagArrayHeaderAccess(head, ctx);
    llvm::Value* actualIdx = builder.CreateAdd(idx32, head, "pel.actidx");
    llvm::Value* slotIdx = builder.CreateAdd(
        builder.CreateZExt(actualIdx, i64Ty), builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    auto* raw = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), "pel.raw");
    tagArrayElementsAccess(raw, ctx);

    llvm::Value* isNum = builder.CreateICmpULE(
        raw, builder.getInt64(BRONZE_ABI_NUMBER_MAX_BITS), "pel.isnum");
    llvm::Value* dblVal = builder.CreateBitCast(raw, dblTy, "pel.f64");
    auto* numBr = builder.CreateCondBr(isNum, doneBb, slowBb);
    numBr->setMetadata(llvm::LLVMContext::MD_prof,
                       llvm::MDBuilder(ctx).createBranchWeights(1048576, 1));

    builder.SetInsertPoint(slowBb);
    llvm::Value* isHole = builder.CreateICmpEQ(
        raw, builder.getInt64(BRONZE_ABI_HOLE_BITS), "pel.ishole");
    auto* unboxCall = builder.CreateCall(abi.bronze_unbox_f64, {raw}, "pel.tonum.val");
    unboxCall->addFnAttr(llvm::Attribute::Cold);
    llvm::Value* slowVal = builder.CreateSelect(isHole, nanVal, unboxCall);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* phi = builder.CreatePHI(dblTy, 3, "pel.res");
    phi->addIncoming(nanVal, entryBb);
    phi->addIncoming(dblVal, readBb);
    phi->addIncoming(slowVal, slowBb);
    return phi;
}

void emitPlainArrayElemSet(llvm::IRBuilder<>& builder, const AbiFns& abi,
                           llvm::Value* objBits, llvm::Value* idxDbl, llvm::Value* valDbl) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    IndexExtraction ie = extractIndex32(builder, idxDbl);
    llvm::Value* idx32 = ie.idx32;
    llvm::Value* isIntegral = ie.isIntegral;

    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, "pes.hdr");
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "pes.len");
    tagArrayHeaderAccess(len, ctx);
    llvm::Value* capPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_CAPACITY_OFFSET);
    auto* cap = builder.CreateAlignedLoad(i32Ty, capPtr, llvm::Align(4), "pes.cap");
    tagArrayHeaderAccess(cap, ctx);
    llvm::Value* headPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_HEAD_OFFSET);
    auto* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), "pes.head");
    tagArrayHeaderAccess(head, ctx);
    llvm::Value* propsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_PROPS_OFFSET);
    auto* propsVal = builder.CreateAlignedLoad(i64Ty, propsPtr, llvm::Align(8), "pes.props");
    tagArrayHeaderAccess(propsVal, ctx);

    llvm::Value* propsTag = builder.CreateLShr(propsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* noProps =
        builder.CreateICmpNE(propsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    llvm::Value* actualIdx = builder.CreateAdd(idx32, head, "pes.actidx");
    llvm::Value* inBounds = builder.CreateAnd(
        builder.CreateICmpULT(idx32, len),
        builder.CreateICmpULT(actualIdx, cap));
    llvm::Value* ok = builder.CreateAnd(inBounds, noProps);
    if (isIntegral != builder.getTrue()) {
        ok = builder.CreateAnd(isIntegral, ok);
    }

    llvm::BasicBlock* storeBb = llvm::BasicBlock::Create(ctx, "pes.store", fn);
    llvm::BasicBlock* slowBb = llvm::BasicBlock::Create(ctx, "pes.slow", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "pes.done", fn);

    auto* condBr = builder.CreateCondBr(ok, storeBb, slowBb);
    condBr->setMetadata(llvm::LLVMContext::MD_prof,
                        llvm::MDBuilder(ctx).createBranchWeights(1048576, 1));

    builder.SetInsertPoint(storeBb);
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), "pes.elems");
    tagArrayHeaderAccess(elemsVal, ctx);
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* slotIdx = builder.CreateAdd(
        builder.CreateZExt(actualIdx, i64Ty), builder.getInt64(1));
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, elemsObj, slotIdx);
    llvm::Value* bits = emitBoxDouble(builder, valDbl);
    auto* st = builder.CreateAlignedStore(bits, slotPtr, llvm::Align(8));
    tagArrayElementsAccess(st, ctx);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(slowBb);
    llvm::Value* idxBoxed = emitBoxDouble(builder, idxDbl);
    llvm::Value* valBoxed = emitBoxDouble(builder, valDbl);
    auto* call = builder.CreateCall(abi.bronze_elem_set,
                                    {objBits, idxBoxed, valBoxed, builder.getFalse()});
    call->addFnAttr(llvm::Attribute::Cold);
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
}

}  // namespace

llvm::Value* emitTypedElemGet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                              llvm::Value* idxDbl, uint32_t elemKind) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);
    llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

    if (elemKind == static_cast<uint32_t>(il::kElemKindPlainArrayF64)) {
        return emitPlainArrayElemGet(builder, abi, objBits, idxDbl);
    }

    TypedElemGuards g = emitTypedElemGuards(builder, objBits, idxDbl);
    llvm::BasicBlock* loadBb = llvm::BasicBlock::Create(ctx, "tel.load", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "tel.done", fn);
    llvm::BasicBlock* entryBb = builder.GetInsertBlock();
    auto* condBr = builder.CreateCondBr(g.ok, loadBb, doneBb);
    condBr->setMetadata(llvm::LLVMContext::MD_prof,
                        llvm::MDBuilder(ctx).createBranchWeights(1048576, 1));

    builder.SetInsertPoint(loadBb);
    llvm::Value* loaded = nullptr;
    switch (elemKind) {
        case BRONZE_ABI_TA_KIND_FLOAT64: {
            llvm::Value* p = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 8);
            auto* ld = builder.CreateAlignedLoad(dblTy, p, llvm::Align(8), "tel.d64");
            tagTypedArrayAccess(ld, ctx);
            loaded = ld;
            break;
        }
        case BRONZE_ABI_TA_KIND_FLOAT32: {
            llvm::Value* p = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 4);
            auto* ld = builder.CreateAlignedLoad(f32Ty, p, llvm::Align(4), "tel.d32");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateFPExt(ld, dblTy);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT32: {
            llvm::Value* p = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 4);
            auto* ld = builder.CreateAlignedLoad(i32Ty, p, llvm::Align(4), "tel.i32");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateSIToFP(ld, dblTy);
            break;
        }
        case BRONZE_ABI_TA_KIND_UINT32: {
            llvm::Value* p = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 4);
            auto* ld = builder.CreateAlignedLoad(i32Ty, p, llvm::Align(4), "tel.u32");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateUIToFP(ld, dblTy);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT16: {
            llvm::Value* p = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 2);
            auto* ld = builder.CreateAlignedLoad(i16Ty, p, llvm::Align(2), "tel.i16");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateSIToFP(ld, dblTy);
            break;
        }
        case BRONZE_ABI_TA_KIND_UINT16: {
            llvm::Value* p = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 2);
            auto* ld = builder.CreateAlignedLoad(i16Ty, p, llvm::Align(2), "tel.u16");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateUIToFP(ld, dblTy);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT8: {
            llvm::Value* p = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 1);
            auto* ld = builder.CreateAlignedLoad(i8Ty, p, llvm::Align(1), "tel.i8");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateSIToFP(ld, dblTy);
            break;
        }
        case BRONZE_ABI_TA_KIND_UINT8:
        case BRONZE_ABI_TA_KIND_UINT8CLAMPED: {
            llvm::Value* p = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 1);
            auto* ld = builder.CreateAlignedLoad(i8Ty, p, llvm::Align(1), "tel.u8");
            tagTypedArrayAccess(ld, ctx);
            loaded = builder.CreateUIToFP(ld, dblTy);
            break;
        }
        default:
            loaded = llvm::ConstantFP::getNaN(dblTy);
            break;
    }
    llvm::Value* loadedBits = (elemKind == BRONZE_ABI_TA_KIND_FLOAT64)
                                  ? emitBoxDouble(builder, loaded)
                                  : builder.CreateBitCast(loaded, builder.getInt64Ty());
    llvm::BasicBlock* loadEndBb = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
    llvm::PHINode* resultBits = builder.CreatePHI(builder.getInt64Ty(), 2, "tel.bits");
    resultBits->addIncoming(loadedBits, loadEndBb);
    resultBits->addIncoming(builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), entryBb);
    return builder.CreateBitCast(resultBits, dblTy, "tel.result");
}

void emitTypedElemSet(llvm::IRBuilder<>& builder, const AbiFns& abi, llvm::Value* objBits,
                      llvm::Value* idxDbl, llvm::Value* valDbl, uint32_t elemKind) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

    if (elemKind == static_cast<uint32_t>(il::kElemKindPlainArrayF64)) {
        emitPlainArrayElemSet(builder, abi, objBits, idxDbl, valDbl);
        return;
    }

    TypedElemGuards g = emitTypedElemGuards(builder, objBits, idxDbl);
    llvm::BasicBlock* storeBb = llvm::BasicBlock::Create(ctx, "tes.store", fn);
    llvm::BasicBlock* doneBb = llvm::BasicBlock::Create(ctx, "tes.done", fn);
    auto* condBr = builder.CreateCondBr(g.ok, storeBb, doneBb);
    condBr->setMetadata(llvm::LLVMContext::MD_prof,
                        llvm::MDBuilder(ctx).createBranchWeights(1048576, 1));

    builder.SetInsertPoint(storeBb);
    switch (elemKind) {
        case BRONZE_ABI_TA_KIND_FLOAT64: {
            llvm::Value* elemPtr = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 8);
            auto* st = builder.CreateAlignedStore(valDbl, elemPtr, llvm::Align(8));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        case BRONZE_ABI_TA_KIND_FLOAT32: {
            llvm::Value* elemPtr = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 4);
            llvm::Value* narrowed =
                builder.CreateFPTrunc(valDbl, f32Ty, "tes.f32");
            auto* st = builder.CreateAlignedStore(narrowed, elemPtr, llvm::Align(4));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT32:
        case BRONZE_ABI_TA_KIND_UINT32: {
            llvm::Value* elemPtr = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 4);
            llvm::Value* i32Val = emitToInt32F64(builder, abi, valDbl);
            auto* st = builder.CreateAlignedStore(i32Val, elemPtr, llvm::Align(4));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT16:
        case BRONZE_ABI_TA_KIND_UINT16: {
            llvm::Value* elemPtr = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 2);
            llvm::Value* i32Tmp = emitToInt32F64(builder, abi, valDbl);
            llvm::Value* i16Val = builder.CreateTrunc(i32Tmp, i16Ty, "tes.i16");
            auto* st = builder.CreateAlignedStore(i16Val, elemPtr, llvm::Align(2));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        case BRONZE_ABI_TA_KIND_INT8:
        case BRONZE_ABI_TA_KIND_UINT8: {
            llvm::Value* elemPtr = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 1);
            llvm::Value* i32Tmp = emitToInt32F64(builder, abi, valDbl);
            llvm::Value* i8Val = builder.CreateTrunc(i32Tmp, i8Ty, "tes.i8");
            auto* st = builder.CreateAlignedStore(i8Val, elemPtr, llvm::Align(1));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        case BRONZE_ABI_TA_KIND_UINT8CLAMPED: {
            llvm::Value* elemPtr = emitTypedArrayElemPtrFromBase(builder, g.dataPtr, g.idx32, 1);
            llvm::Value* u8cTmp = builder.CreateCall(abi.bronze_to_uint8_clamp_f64, {valDbl}, "tes.u8c.tmp");
            llvm::Value* u8cVal = builder.CreateTrunc(u8cTmp, i8Ty, "tes.u8c");
            auto* st = builder.CreateAlignedStore(u8cVal, elemPtr, llvm::Align(1));
            tagTypedArrayAccess(st, ctx);
            break;
        }
        default:
            break;
    }
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(doneBb);
}

}  // namespace bronze::codegen_llvm
