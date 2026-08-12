#include "codegen-llvm/llvm_func.h"

#include <string>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Type.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_prop.h"

namespace bronze::codegen_llvm {

llvm::Type* mapILType(il::Type type, llvm::LLVMContext& ctx) {
    switch (type) {
        case il::Type::Void: return llvm::Type::getVoidTy(ctx);
        case il::Type::Bool: return llvm::Type::getInt1Ty(ctx);
        case il::Type::I32: return llvm::Type::getInt32Ty(ctx);
        case il::Type::F64: return llvm::Type::getDoubleTy(ctx);
        case il::Type::Str: return llvm::PointerType::getUnqual(ctx);
        case il::Type::Dynamic: return llvm::Type::getInt64Ty(ctx);
        default: return nullptr;
    }
}

FunctionEmitter::FunctionEmitter(const Context& shared, const il::Function& func,
                                 llvm::Function* llvmFunc)
    : shared_(shared),
      func_(func),
      llvmFunc_(llvmFunc),
      builder_(shared.ctx),
      i64Ty_(llvm::Type::getInt64Ty(shared.ctx)),
      ptrTy_(llvm::PointerType::getUnqual(shared.ctx)),
      values_(func.valueCount, nullptr),
      slotOf_(func.valueCount, kNoSlot) {}

bool FunctionEmitter::require(bool condition, const char* message) {
    if (condition) return true;
    shared_.diags.error(Span{}, message);
    return false;
}

llvm::Value* FunctionEmitter::operand(const il::Instruction& inst, size_t index,
                                      const char* what) {
    llvm::Value* v = values_[inst.operands[index]];
    if (!v) shared_.diags.error(Span{}, what);
    return v;
}

llvm::Value* FunctionEmitter::slotAddr(uint32_t slot) {
    return builder_.CreateGEP(i64Ty_, slotsBase_, builder_.getInt32(slot));
}

void FunctionEmitter::reload(il::ValueId id) {
    if (id == il::kNoValue || id >= func_.valueCount) return;
    uint32_t slot = slotOf_[id];
    if (slot == kNoSlot) return;
    values_[id] = builder_.CreateLoad(i64Ty_, slotAddr(slot));
}

// ---- GC root frame (docs/0006) ---------------------------------------------
//
// Every Dynamic-typed value gets a slot in one contiguous array the collector
// walks: defs store into it, uses load out of it. The load is the point — a
// collection inside any helper call moves the object and updates the slot,
// while an SSA register would keep pointing into dead from-space. A function
// with no Dynamic values (proven-f64 code) gets no frame and pays nothing.

void FunctionEmitter::planRootFrame() {
    uint32_t slotCount = 0;
    auto assignSlot = [&](il::ValueId id, il::Type ty) {
        if (id == il::kNoValue || id >= func_.valueCount) return;
        if (ty != il::Type::Dynamic) return;
        if (slotOf_[id] == kNoSlot) slotOf_[id] = slotCount++;
    };

    uint32_t maxArgc = 0;
    for (size_t p = 0; p < func_.params.size(); ++p) {
        assignSlot(static_cast<il::ValueId>(p), func_.params[p].type);
    }
    for (const auto& block : func_.blocks) {
        for (const auto& param : block.params) assignSlot(param.id, param.type);
        for (const auto& inst : block.instructions) {
            assignSlot(inst.result, inst.type);
            if (inst.op == il::Op::DynamicCall && inst.operands.size() >= 2) {
                maxArgc = std::max(maxArgc, static_cast<uint32_t>(inst.operands.size() - 2));
            }
            if (inst.op == il::Op::Construct && !inst.operands.empty()) {
                maxArgc = std::max(maxArgc, static_cast<uint32_t>(inst.operands.size() - 1));
            }
            // console.log with more than one argument builds an argv too,
            // and console.warn/error take the same path to the other stream.
            if ((inst.op == il::Op::Print || inst.op == il::Op::PrintErr) &&
                inst.operands.size() > 1) {
                maxArgc = std::max(maxArgc, static_cast<uint32_t>(inst.operands.size()));
            }
        }
    }

    // Call arguments live in the frame too: they are live across the callee,
    // and the widest call site's worth of slots is enough because IL is flat
    // SSA — an argument list is built immediately before its call and dead
    // immediately after, never nested.
    argvBase_ = slotCount;
    frameSlots_ = slotCount + maxArgc;
}

void FunctionEmitter::emitPrologue() {
    if (frameSlots_ == 0 || blocks_.empty()) return;

    // The frame mirrors `bronze_gc_frame` from the ABI registry:
    // { prev, count, slots[frameSlots] }, allocated in this function's own
    // stack frame and linked onto the list head inline.
    builder_.SetInsertPoint(blocks_[0]);
    frameTy_ = llvm::StructType::get(
        shared_.ctx, {ptrTy_, i64Ty_, llvm::ArrayType::get(i64Ty_, frameSlots_)});
    framePtr_ = builder_.CreateAlloca(frameTy_, nullptr, "gcframe");
    slotsBase_ = builder_.CreateStructGEP(frameTy_, framePtr_, 2);

    // Every slot must hold a valid Value before the frame is linked: the
    // collector reads all of them, including the ones whose defs have not run.
    llvm::Value* undefBits = builder_.getInt64(BRONZE_ABI_UNDEFINED_BITS);
    for (uint32_t s = 0; s < frameSlots_; ++s) builder_.CreateStore(undefBits, slotAddr(s));
    for (size_t p = 0; p < func_.params.size(); ++p) {
        if (slotOf_[p] == kNoSlot) continue;
        builder_.CreateStore(values_[p], slotAddr(slotOf_[p]));
    }
    builder_.CreateStore(builder_.getInt64(frameSlots_),
                         builder_.CreateStructGEP(frameTy_, framePtr_, 1));
    builder_.CreateStore(builder_.CreateLoad(ptrTy_, shared_.globals.bronze_gc_frame_top),
                         builder_.CreateStructGEP(frameTy_, framePtr_, 0));
    builder_.CreateStore(framePtr_, shared_.globals.bronze_gc_frame_top);
}

void FunctionEmitter::createBlockPhis() {
    blockPhis_.resize(func_.blocks.size());
    for (size_t bIdx = 0; bIdx < func_.blocks.size(); ++bIdx) {
        const auto& block = func_.blocks[bIdx];
        if (block.params.empty()) continue;
        builder_.SetInsertPoint(blocks_[bIdx]);
        blockPhis_[bIdx].reserve(block.params.size());
        for (const auto& param : block.params) {
            llvm::PHINode* phi = builder_.CreatePHI(mapILType(param.type, shared_.ctx), 0,
                                                    "p" + std::to_string(param.id));
            values_[param.id] = phi;
            blockPhis_[bIdx].push_back(phi);
        }
    }
}

// Key strings are registered once, from the entry block of `main`, because the
// property path indexes them by the number lowering assigned.
void FunctionEmitter::emitKeyRegistration() {
    if (func_.name != "main" || blocks_.empty()) return;
    builder_.SetInsertPoint(blocks_[0]);
    for (size_t k = 0; k < shared_.module.keyConstants.size(); ++k) {
        llvm::Value* text = builder_.CreateGlobalStringPtr(shared_.module.keyConstants[k]);
        builder_.CreateCall(shared_.abi.bronze_register_key_string,
                            {builder_.getInt32(static_cast<uint32_t>(k)), text});
    }
}

bool FunctionEmitter::emit() {
    blocks_.reserve(func_.blocks.size());
    for (const auto& block : func_.blocks) {
        blocks_.push_back(
            llvm::BasicBlock::Create(shared_.ctx, "b" + std::to_string(block.id), llvmFunc_));
    }

    size_t argIdx = 0;
    for (auto& arg : llvmFunc_->args()) {
        arg.setName(func_.params[argIdx].name);
        values_[argIdx] = &arg;
        ++argIdx;
    }

    planRootFrame();
    emitPrologue();
    createBlockPhis();
    emitKeyRegistration();

    for (size_t bIdx = 0; bIdx < func_.blocks.size(); ++bIdx) {
        if (!emitBlock(bIdx)) return false;
    }
    return true;
}

// ---- exceptions (docs/0020) -------------------------------------------------
//
// Propagation is a `ret`, so there is no unwind ABI: after any instruction
// that can throw, generated code loads the pending cell, compares it against
// the Hole singleton and branches. The not-taken path is the whole cost on a
// program that never throws.

void FunctionEmitter::popRootFrame() {
    if (!framePtr_) return;
    builder_.CreateStore(
        builder_.CreateLoad(ptrTy_, builder_.CreateStructGEP(frameTy_, framePtr_, 0)),
        shared_.globals.bronze_gc_frame_top);
}

llvm::BasicBlock* FunctionEmitter::functionUnwindBlock() {
    if (unwindBlock_) return unwindBlock_;
    llvm::IRBuilder<>::InsertPointGuard guard(builder_);
    unwindBlock_ = llvm::BasicBlock::Create(shared_.ctx, "unwind", llvmFunc_);
    builder_.SetInsertPoint(unwindBlock_);
    // The entry point has no caller to propagate to: this is where the
    // language runs out of handlers, so the value is reported on stderr and
    // the process exits non-zero. Popping the frame first would be pointless
    // and the helper does not return.
    if (func_.isEntryPoint) {
        builder_.CreateCall(shared_.abi.bronze_uncaught_exception, {});
        builder_.CreateUnreachable();
        return unwindBlock_;
    }
    // The same frame pop an ordinary return emits, and for the same reason:
    // the collector walks the frame chain and cannot tell the two paths
    // apart, which is the argument for this mechanism over `invoke`.
    popRootFrame();
    llvm::Type* retTy = llvmFunc_->getReturnType();
    if (retTy->isVoidTy()) {
        builder_.CreateRetVoid();
    } else if (retTy->isIntegerTy(64)) {
        // A Dynamic return. `undefined`, never garbage: the caller stores
        // this into a GC root slot before it tests the cell, and the
        // collector reads every slot of a linked frame.
        builder_.CreateRet(builder_.getInt64(BRONZE_ABI_UNDEFINED_BITS));
    } else {
        builder_.CreateRet(llvm::Constant::getNullValue(retTy));
    }
    return unwindBlock_;
}

llvm::BasicBlock* FunctionEmitter::unwindTargetFor(size_t blockIndex) {
    const il::BlockId handler = func_.blocks[blockIndex].handler;
    if (handler != il::kNoBlock && handler < blocks_.size()) return blocks_[handler];
    return functionUnwindBlock();
}

void FunctionEmitter::emitExceptionCheck(size_t blockIndex) {
    llvm::Value* cell = builder_.CreateLoad(i64Ty_, shared_.globals.bronze_exception_cell);
    llvm::Value* pending = builder_.CreateICmpNE(
        cell, builder_.getInt64(BRONZE_ABI_NO_EXCEPTION_BITS), "pending");
    llvm::BasicBlock* cont = llvm::BasicBlock::Create(shared_.ctx, "cont", llvmFunc_);
    builder_.CreateCondBr(pending, unwindTargetFor(blockIndex), cont);
    builder_.SetInsertPoint(cont);
}

bool FunctionEmitter::emitBlock(size_t blockIndex) {
    const auto& block = func_.blocks[blockIndex];
    currentILBlock_ = blockIndex;
    builder_.SetInsertPoint(blocks_[blockIndex]);

    // A block parameter is a def like any other: the phi's value has to reach
    // its root slot before anything can collect.
    for (size_t pi = 0; pi < block.params.size(); ++pi) {
        uint32_t slot = slotOf_[block.params[pi].id];
        if (slot == kNoSlot) continue;
        builder_.CreateStore(blockPhis_[blockIndex][pi], slotAddr(slot));
    }

    for (const auto& inst : block.instructions) {
        for (il::ValueId id : inst.operands) reload(id);
        for (il::ValueId id : inst.target.args) reload(id);
        for (il::ValueId id : inst.elseTarget.args) reload(id);

        if (!emitInstruction(inst)) return false;

        if (inst.result != il::kNoValue && inst.result < func_.valueCount &&
            slotOf_[inst.result] != kNoSlot && values_[inst.result]) {
            builder_.CreateStore(values_[inst.result], slotAddr(slotOf_[inst.result]));
        }

        // AFTER the result store, not after the call: the slot must hold a
        // value the collector can parse before anything branches away from
        // it (docs/0020 decision 2).
        if (il::canThrow(inst)) emitExceptionCheck(blockIndex);
    }
    return true;
}

bool FunctionEmitter::emitInstruction(const il::Instruction& inst) {
    switch (inst.op) {
        case il::Op::Jump:
        case il::Op::Branch:
        case il::Op::Ret:
        case il::Op::Throw:
            return emitTerminator(inst);

        case il::Op::Add:
        case il::Op::Sub:
        case il::Op::Neg:
        case il::Op::Mul:
        case il::Op::Div:
        case il::Op::Mod:
        case il::Op::CmpLt:
        case il::Op::CmpGt:
        case il::Op::CmpEq:
        case il::Op::CmpNe:
        case il::Op::NumTruthy:
        case il::Op::StrictEq:
        case il::Op::LooseEq:
        case il::Op::Pow:
        case il::Op::ToInt32:
        case il::Op::BitAnd:
        case il::Op::BitOr:
        case il::Op::BitXor:
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr:
            return emitArithmetic(inst);

        default:
            return emitRuntimeOp(inst);
    }
}

bool FunctionEmitter::emitTerminator(const il::Instruction& inst) {
    // The phi incomings must name the CURRENT insertion block, not the one
    // this IL block started in: an inlined property guard splits the block, so
    // a terminator later in the same IL block has a different LLVM
    // predecessor.
    llvm::BasicBlock* from = builder_.GetInsertBlock();
    auto addIncomings = [&](const il::BlockTarget& target) {
        for (size_t k = 0; k < target.args.size(); ++k) {
            blockPhis_[target.block][k]->addIncoming(values_[target.args[k]], from);
        }
    };

    switch (inst.op) {
        case il::Op::Jump: {
            addIncomings(inst.target);
            builder_.CreateBr(blocks_[inst.target.block]);
            return true;
        }
        case il::Op::Branch: {
            llvm::Value* cond = values_[inst.operands[0]];
            addIncomings(inst.target);
            addIncomings(inst.elseTarget);
            builder_.CreateCondBr(cond, blocks_[inst.target.block],
                                  blocks_[inst.elseTarget.block]);
            return true;
        }
        default: break;
    }

    if (inst.op == il::Op::Throw) {
        // Set the cell and take this block's handler edge. The value is
        // already in a root slot (it is an ordinary Dynamic def), and the
        // cell is a permanent root, so it stays live across every frame it
        // passes through.
        llvm::Value* thrown = operand(inst, 0, "Undefined value in Throw instruction");
        if (!thrown) return false;
        builder_.CreateStore(thrown, shared_.globals.bronze_exception_cell);
        builder_.CreateBr(unwindTargetFor(currentILBlock_));
        return true;
    }
    // Ret: unlink the root frame before leaving, on every return edge.
    popRootFrame();
    if (inst.operands.empty()) {
        builder_.CreateRetVoid();
        return true;
    }
    llvm::Value* retVal = operand(inst, 0, "Undefined return value in Ret instruction");
    if (!retVal) return false;
    if (llvmFunc_->getReturnType()->isDoubleTy() && retVal->getType()->isIntegerTy(1)) {
        retVal = builder_.CreateUIToFP(retVal, builder_.getDoubleTy());
    }
    builder_.CreateRet(retVal);
    return true;
}

llvm::Value* FunctionEmitter::emitArgv(const il::Instruction& inst, size_t first, uint32_t argc,
                                       bool& ok) {
    ok = true;
    if (argc == 0) return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(shared_.ctx));

    // The argv region of this function's root frame, so the arguments stay
    // rooted across the callee (and so a call inside a loop stops alloca'ing a
    // fresh buffer every iteration).
    llvm::Value* argvPtr = slotAddr(argvBase_);
    for (uint32_t a = 0; a < argc; ++a) {
        llvm::Value* argV = operand(inst, first + a, "Undefined argument in a call instruction");
        if (!argV) {
            ok = false;
            return nullptr;
        }
        builder_.CreateStore(argV, builder_.CreateGEP(i64Ty_, argvPtr, builder_.getInt32(a)));
    }
    return argvPtr;
}

}  // namespace bronze::codegen_llvm
