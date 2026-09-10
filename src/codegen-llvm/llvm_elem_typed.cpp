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
    tagViewLengthAccess(bufVal, ctx);
    llvm::Value* bufAddr =
        builder.CreateAnd(bufVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* bufHdr = builder.CreateIntToPtr(bufAddr, ptrTy);
    llvm::Value* byteOffPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_BYTEOFFSET_OFFSET);
    auto* byteOff =
        builder.CreateAlignedLoad(i32Ty, byteOffPtr, llvm::Align(4), "ta.byteoff");
    byteOff->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(ctx, {}));
    tagViewLengthAccess(byteOff, ctx);

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

std::vector<uint8_t> planIntegralNonNegativeValues(const il::Function& func) {
    std::vector<uint8_t> isNonNeg(func.valueCount, 0);
    if (func.blocks.empty()) return isNonNeg;

    struct Edge {
        il::BlockId from;
        const std::vector<il::ValueId>* args;
    };
    std::vector<std::vector<Edge>> preds(func.blocks.size());
    for (size_t b = 0; b < func.blocks.size(); ++b) {
        for (const auto& inst : func.blocks[b].instructions) {
            if (inst.target.block != il::kNoBlock && inst.target.block < func.blocks.size()) {
                preds[inst.target.block].push_back({static_cast<il::BlockId>(b), &inst.target.args});
            }
            if (inst.elseTarget.block != il::kNoBlock && inst.elseTarget.block < func.blocks.size()) {
                preds[inst.elseTarget.block].push_back({static_cast<il::BlockId>(b), &inst.elseTarget.args});
            }
        }
    }

    std::vector<il::ValueId> aliasOf(func.valueCount, il::kNoValue);
    for (size_t b = 0; b < func.blocks.size(); ++b) {
        if (preds[b].size() == 1) {
            const auto& edge = preds[b][0];
            const auto& blk = func.blocks[b];
            for (size_t p = 0; p < blk.params.size(); ++p) {
                if (p < edge.args->size()) {
                    il::ValueId pId = blk.params[p].id;
                    if (pId < func.valueCount) {
                        aliasOf[pId] = (*edge.args)[p];
                    }
                }
            }
        }
    }
    auto resolve = [&](il::ValueId v, auto& self) -> il::ValueId {
        if (v < func.valueCount && aliasOf[v] != il::kNoValue) {
            return self(aliasOf[v], self);
        }
        return v;
    };

    auto isNonNegativeConst = [](double d) {
        return d >= 0.0 && d <= 4294967295.0 && std::trunc(d) == d;
    };

    auto propagate = [&]() -> bool {
        bool changed = false;
        for (size_t b = 0; b < func.blocks.size(); ++b) {
            const auto& blk = func.blocks[b];
            for (const auto& inst : blk.instructions) {
                if (inst.result == il::kNoValue || inst.result >= func.valueCount || isNonNeg[inst.result]) continue;
                bool ok = false;
                switch (inst.op) {
                    case il::Op::ConstF64:
                        if (isNonNegativeConst(inst.immF64)) ok = true;
                        break;
                    case il::Op::ConstI32:
                        if (inst.immI32 >= 0) ok = true;
                        break;
                    case il::Op::UShr:
                        ok = true;
                        break;
                    case il::Op::Add:
                    case il::Op::Mul:
                        if (inst.operands.size() >= 2) {
                            il::ValueId op0 = resolve(inst.operands[0], resolve);
                            il::ValueId op1 = resolve(inst.operands[1], resolve);
                            if (op0 < func.valueCount && isNonNeg[op0] &&
                                op1 < func.valueCount && isNonNeg[op1]) {
                                ok = true;
                            }
                        }
                        break;
                    case il::Op::BitAnd:
                        if (inst.operands.size() >= 2) {
                            il::ValueId op0 = resolve(inst.operands[0], resolve);
                            il::ValueId op1 = resolve(inst.operands[1], resolve);
                            if ((op0 < func.valueCount && isNonNeg[op0]) ||
                                (op1 < func.valueCount && isNonNeg[op1])) {
                                ok = true;
                            }
                        }
                        break;
                    case il::Op::ToInt32:
                        if (!inst.operands.empty()) {
                            il::ValueId op = resolve(inst.operands[0], resolve);
                            if (op < func.valueCount && isNonNeg[op]) ok = true;
                        }
                        break;
                    default:
                        break;
                }
                if (ok) {
                    isNonNeg[inst.result] = 1;
                    changed = true;
                }
            }
            if (preds[b].size() == 1) {
                for (const auto& p : blk.params) {
                    if (p.id < func.valueCount && !isNonNeg[p.id]) {
                        il::ValueId resVal = resolve(p.id, resolve);
                        if (resVal < func.valueCount && isNonNeg[resVal]) {
                            isNonNeg[p.id] = 1;
                            changed = true;
                        }
                    }
                }
            }
        }
        return changed;
    };

    while (propagate()) {}

    for (size_t b = 0; b < func.blocks.size(); ++b) {
        const auto& blk = func.blocks[b];
        if (blk.params.empty() || preds[b].size() < 2) continue;
        for (size_t p = 0; p < blk.params.size(); ++p) {
            il::ValueId pId = blk.params[p].id;
            if (pId == il::kNoValue || pId >= func.valueCount || isNonNeg[pId]) continue;

            bool hasValidEntry = false;
            bool entryFailed = false;
            std::vector<const Edge*> backedges;

            for (const auto& edge : preds[b]) {
                if (p >= edge.args->size()) { entryFailed = true; break; }
                il::ValueId arg = resolve((*edge.args)[p], resolve);
                if (arg < func.valueCount && isNonNeg[arg]) {
                    hasValidEntry = true;
                } else {
                    backedges.push_back(&edge);
                }
            }

            if (!hasValidEntry || entryFailed || backedges.empty()) continue;

            bool allBackedgesOk = true;
            for (const auto* edge : backedges) {
                il::ValueId rawArg = (*edge->args)[p];
                il::ValueId arg = resolve(rawArg, resolve);
                bool stepOk = false;
                for (const auto& blkCheck : func.blocks) {
                    for (const auto& inst : blkCheck.instructions) {
                        if (inst.result == arg) {
                            if (inst.op == il::Op::Add && inst.operands.size() >= 2) {
                                il::ValueId a0 = resolve(inst.operands[0], resolve);
                                il::ValueId a1 = resolve(inst.operands[1], resolve);
                                if ((a0 == pId && a1 < func.valueCount && isNonNeg[a1]) ||
                                    (a1 == pId && a0 < func.valueCount && isNonNeg[a0])) {
                                    stepOk = true;
                                }
                            }
                            break;
                        }
                    }
                    if (stepOk) break;
                }
                if (!stepOk) {
                    allBackedgesOk = false;
                    break;
                }
            }

            if (allBackedgesOk) {
                isNonNeg[pId] = 1;
            }
        }
    }

    while (propagate()) {}

    return isNonNeg;
}

namespace {

struct IndexExtraction {
    llvm::Value* idx32;
    llvm::Value* isIntegral;
};

IndexExtraction extractIndex32(llvm::IRBuilder<>& builder, llvm::Value* idxDbl,
                              bool isKnownIntegralNonNegative = false) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* dblTy = llvm::Type::getDoubleTy(ctx);

    if (isKnownIntegralNonNegative) {
        llvm::Value* idx32 = builder.CreateFPToUI(idxDbl, i32Ty, "tel.idx");
        return {idx32, builder.getTrue()};
    }

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
                                    llvm::Value* idxDbl, TypedArrayCache* cache = nullptr,
                                    bool isKnownIntegralNonNegative = false,
                                    il::ValueId objId = il::kNoValue) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    IndexExtraction ie = extractIndex32(builder, idxDbl, isKnownIntegralNonNegative);
    llvm::Value* idx32 = ie.idx32;
    llvm::Value* isIntegral = ie.isIntegral;

    llvm::Value* hdr = nullptr;
    llvm::Value* len = nullptr;
    llvm::Value* dataPtr = nullptr;

    if (cache != nullptr && cache->dataPtr != nullptr &&
        (cache->obj == objBits || (objId != il::kNoValue && cache->objId == objId))) {
        hdr = cache->hdr;
        len = cache->len;
        dataPtr = cache->dataPtr;
    } else {
        llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
        hdr = builder.CreateIntToPtr(addr, ptrTy, "tel.hdr");
        llvm::Value* lenPtr =
            builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_TA_LENGTH_OFFSET);
        auto* loadedLen = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), "tel.len");
        tagViewLengthAccess(loadedLen, ctx);
        len = loadedLen;
        dataPtr = emitTypedArrayBasePtr(builder, hdr);
        if (cache != nullptr) {
            cache->objId = objId;
            cache->obj = objBits;
            cache->hdr = hdr;
            cache->dataPtr = dataPtr;
            cache->len = len;
        }
    }

    llvm::Value* inLen = builder.CreateICmpULT(idx32, len);
    llvm::Value* ok = (isIntegral == builder.getTrue()) ? inLen : builder.CreateAnd(isIntegral, inLen);
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
                              llvm::Value* idxDbl, uint32_t elemKind,
                              TypedArrayCache* cache,
                              bool isKnownIntegralNonNegative,
                              il::ValueId objId) {
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

    TypedElemGuards g = emitTypedElemGuards(builder, objBits, idxDbl, cache,
                                           isKnownIntegralNonNegative, objId);
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
                      llvm::Value* idxDbl, llvm::Value* valDbl, uint32_t elemKind,
                      TypedArrayCache* cache,
                      bool isKnownIntegralNonNegative,
                      il::ValueId objId) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* f32Ty = llvm::Type::getFloatTy(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

    if (elemKind == static_cast<uint32_t>(il::kElemKindPlainArrayF64)) {
        emitPlainArrayElemSet(builder, abi, objBits, idxDbl, valDbl);
        return;
    }

    TypedElemGuards g = emitTypedElemGuards(builder, objBits, idxDbl, cache,
                                           isKnownIntegralNonNegative, objId);
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
