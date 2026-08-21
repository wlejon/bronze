#include "codegen-llvm/llvm_func.h"

#include <string>
#include <vector>

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
      propGetKey_(func.valueCount, UINT32_MAX),
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

llvm::Value* FunctionEmitter::rootSlotAddrOf(const il::Instruction& inst, size_t index) {
    if (index >= inst.operands.size()) return nullptr;
    const il::ValueId id = inst.operands[index];
    if (id == il::kNoValue || id >= func_.valueCount) return nullptr;
    const uint32_t slot = slotOf_[id];
    if (slot == kNoSlot) return nullptr;
    return slotAddr(slot);
}

void FunctionEmitter::reload(il::ValueId id) {
    if (id == il::kNoValue || id >= func_.valueCount) return;
    uint32_t slot = slotOf_[id];
    if (slot == kNoSlot) return;
    values_[id] = builder_.CreateLoad(i64Ty_, slotAddr(slot));
}

// ---- GC root frame ---------------------------------------------
//
// Every Dynamic-typed value gets a slot in one contiguous array the collector
// walks: defs store into it, uses load out of it. The load is the point — a
// collection inside any helper call moves the object and updates the slot,
// while an SSA register would keep pointing into dead from-space. A function
// with no Dynamic values (proven-f64 code) gets no frame and pays nothing.
//
// Slots are REUSED once the value in them is dead, which is what keeps the
// frame proportional to how many values are live at once rather than to how
// many the function ever computes. Without it a 2000-statement function got
// 6002 slots — a 48 KB alloca, 6002 unrolled initialising stores, and 6002
// stack locations for the register allocator to colour — and that, not anything
// bronze does, was 93% of a three.js compile.
//
// A slot may be reused only where nothing can read the old value again, so
// the eligibility rule is deliberately narrow:
//
//  - A value used OUTSIDE its defining block keeps a slot to itself. So does
//    a block parameter and a function parameter. Deciding those needs real
//    liveness over the CFG — a loop header's parameter is live across the
//    back edge — and a wrong answer here is a use-after-move that only shows
//    up under GC stress, which is the most expensive bug this project has.
//  - Everything else is a temporary whose whole life is inside one block, and
//    a linear scan over that block is exact: the range is [def, last use] in
//    textual order, because within a block a def precedes every use of it.
//
// A freed slot is not cleared. It holds a dead-but-valid Value until the next
// def overwrites it, so a collection in between forwards one object that is
// no longer reachable from the program — the same one cycle of float the
// frame already had, not a new hazard.

void FunctionEmitter::planRootFrame() {
    const uint32_t n = func_.valueCount;
    constexpr uint32_t kNoBlockIdx = UINT32_MAX;

    auto isRooted = [&](il::ValueId id, il::Type ty) {
        return id != il::kNoValue && id < n && ty == il::Type::Dynamic;
    };

    // Where each rooted value is defined, and whether any use is somewhere
    // its defining block's linear scan cannot see.
    std::vector<uint32_t> defBlock(n, kNoBlockIdx);
    std::vector<uint32_t> lastUse(n, 0);
    std::vector<bool> pinned(n, false);

    for (size_t p = 0; p < func_.params.size(); ++p) {
        const auto id = static_cast<il::ValueId>(p);
        if (isRooted(id, func_.params[p].type)) pinned[id] = true;
    }

    uint32_t maxArgc = 0;
    bool hasConstruct = false;
    for (uint32_t b = 0; b < func_.blocks.size(); ++b) {
        const auto& block = func_.blocks[b];
        for (const auto& param : block.params) {
            if (isRooted(param.id, param.type)) pinned[param.id] = true;
        }
        for (uint32_t i = 0; i < block.instructions.size(); ++i) {
            const auto& inst = block.instructions[i];
            if (isRooted(inst.result, inst.type)) {
                defBlock[inst.result] = b;
                lastUse[inst.result] = i;
            }
            // Every field on an instruction that names a value: `operands`,
            // and the two block-argument lists a terminator carries. The
            // argument lists are read HERE, at the branch, so they are
            // ordinary uses at index `i` rather than something wider — but
            // they are read out of `inst.target`, not `inst.operands`, and a
            // scan that forgot them would pool a value the branch still needs
            // and hand its slot to the next def.
            auto noteUse = [&](il::ValueId use) {
                if (use == il::kNoValue || use >= n) return;
                if (defBlock[use] != b) {
                    // Defined in another block, or not yet seen here at all
                    // (a value this block only reads). Either way its life is
                    // wider than this scan.
                    pinned[use] = true;
                } else {
                    lastUse[use] = i;
                }
            };
            for (il::ValueId use : inst.operands) noteUse(use);
            for (il::ValueId use : inst.target.args) noteUse(use);
            for (il::ValueId use : inst.elseTarget.args) noteUse(use);
            if (inst.op == il::Op::DynamicCall && inst.operands.size() >= 2) {
                maxArgc = std::max(maxArgc, static_cast<uint32_t>(inst.operands.size() - 2));
            }
            if (inst.op == il::Op::SuperCall && inst.operands.size() >= 2) {
                maxArgc = std::max(maxArgc, static_cast<uint32_t>(inst.operands.size() - 2));
            }
            if (inst.op == il::Op::Construct && !inst.operands.empty()) {
                maxArgc = std::max(maxArgc, static_cast<uint32_t>(inst.operands.size() - 1));
                hasConstruct = true;
            }
            // console.log with more than one argument builds an argv too,
            // and console.warn/error take the same path to the other stream.
            if ((inst.op == il::Op::Print || inst.op == il::Op::PrintErr) &&
                inst.operands.size() > 1) {
                maxArgc = std::max(maxArgc, static_cast<uint32_t>(inst.operands.size()));
            }
        }
    }

    // The pinned values first, in the order they appear, so a frame's layout
    // stays a function of the IL and nothing else.
    uint32_t pinnedCount = 0;
    auto pin = [&](il::ValueId id, il::Type ty) {
        if (!isRooted(id, ty) || !pinned[id]) return;
        if (slotOf_[id] == kNoSlot) slotOf_[id] = pinnedCount++;
    };
    for (size_t p = 0; p < func_.params.size(); ++p) {
        pin(static_cast<il::ValueId>(p), func_.params[p].type);
    }
    for (const auto& block : func_.blocks) {
        for (const auto& param : block.params) pin(param.id, param.type);
        for (const auto& inst : block.instructions) pin(inst.result, inst.type);
    }

    // Then the block-local temporaries, out of a pool every block reuses:
    // two values in different blocks can never both be live, so the pool only
    // has to be as deep as the worst single block.
    uint32_t poolHighWater = 0;
    std::vector<uint32_t> freeSlots;
    std::vector<std::vector<il::ValueId>> expiringAt;
    for (const auto& block : func_.blocks) {
        uint32_t poolSize = 0;
        freeSlots.clear();
        expiringAt.assign(block.instructions.size(), {});
        for (uint32_t i = 0; i < block.instructions.size(); ++i) {
            const auto& inst = block.instructions[i];
            // Release what died at the PREVIOUS instruction, never at this
            // one: this instruction's operands are loaded out of their slots
            // before its result is stored, and a slot handed to the result
            // here would be read after it had been overwritten.
            if (i > 0) {
                for (il::ValueId dead : expiringAt[i - 1]) freeSlots.push_back(slotOf_[dead]);
                expiringAt[i - 1].clear();
            }
            const il::ValueId res = inst.result;
            if (!isRooted(res, inst.type) || pinned[res]) continue;
            // Absolute from the start — the pool sits immediately above the
            // pinned block, and `pinnedCount` is already final here.
            if (freeSlots.empty()) {
                slotOf_[res] = pinnedCount + poolSize++;
            } else {
                slotOf_[res] = freeSlots.back();
                freeSlots.pop_back();
            }
            expiringAt[lastUse[res]].push_back(res);
        }
        poolHighWater = std::max(poolHighWater, poolSize);
    }

    // Call arguments live in the frame too: they are live across the callee,
    // and the widest call site's worth of slots is enough because IL is flat
    // SSA — an argument list is built immediately before its call and dead
    // immediately after, never nested.
    argvBase_ = pinnedCount + poolHighWater;
    frameSlots_ = argvBase_ + maxArgc;

    // One slot above the argv region for the inline `new` fast path's fresh
    // instance. Shared by every construct site — a site's use of it ends at
    // its own merge, and IL construct sites never nest mid-flight for the
    // same reason argument lists never do.
    if (hasConstruct && !shared_.moduleHasNewTarget) {
        constructSelfSlot_ = frameSlots_++;
    }
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
    builder_.CreateStore(builder_.CreateLoad(ptrTy_, globals_.bronze_gc_frame_top),
                         builder_.CreateStructGEP(frameTy_, framePtr_, 0));
    builder_.CreateStore(framePtr_, globals_.bronze_gc_frame_top);
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

// Everything this object file has to tell the runtime before any of its code
// runs, emitted into the top of `main`.
//
// The span registrations come first: they hand the collector the module's own
// Value-holding data — its global cache, its module-environment cell, and its
// function-singleton slots — and the key registration below is the first thing
// that can allocate, so registering after it would leave one collection's worth
// of cells untraced. (They are all empty at that point, so nothing would have
// been lost today; the ordering is what keeps that from being load-bearing.)
//
// Key registration then INTERNS each string and records the process-wide id in
// the module's remap. The module's own numbering stays 0..n-1 and stays an
// immediate everywhere; only the value handed to a helper is the interned id.
// Two modules that both mention "position" therefore agree on the property, and
// two modules that number the same string differently no longer collide.
// The source text of every function this module compiled, as one read-only
// blob per FILE plus a table of (call wrapper, byte range) pairs into it.
//
// One blob per file rather than one string per function is the whole design:
// the ~3000 functions of the three.js graph are written in 28 files that
// overlap almost entirely — nested closures live inside the text of the
// functions containing them — so per-function strings would emit those 1.6 MB
// many times over, where this emits them once.
//
// The table is DATA, not a call per function: 3000 registration calls in the
// module's entry block would be code the program pays for in size whether or
// not anything ever asks a function for its text. One call per file hands the
// whole array over at once.
void FunctionEmitter::emitFunctionSourceTables() {
    if (shared_.module.sourceTexts.empty()) return;
    llvm::Module& llvmModule = *llvmFunc_->getParent();
    for (uint16_t file = 0; file < shared_.module.sourceTexts.size(); ++file) {
        std::vector<llvm::Constant*> entries;
        for (size_t i = 0; i < shared_.module.functions.size(); ++i) {
            const il::Function& fn = shared_.module.functions[i];
            if (fn.sourceFile != file || fn.sourceEnd <= fn.sourceBegin) continue;
            if (i >= shared_.wrappers.size() || !shared_.wrappers[i]) continue;
            entries.push_back(llvm::ConstantExpr::getPtrToInt(shared_.wrappers[i], i64Ty_));
            entries.push_back(llvm::ConstantInt::get(
                i64Ty_, (static_cast<uint64_t>(fn.sourceBegin) << 32) |
                            static_cast<uint64_t>(fn.sourceEnd - fn.sourceBegin)));
        }
        // A file every function of which was synthesized — or one that
        // declared no function at all — emits neither blob nor table, which
        // is what keeps a build's cost proportional to the source it kept.
        if (entries.empty()) continue;

        const std::string& text = shared_.module.sourceTexts[file];
        llvm::Value* textPtr = builder_.CreateGlobalString(text);
        auto* arrTy = llvm::ArrayType::get(i64Ty_, entries.size());
        auto* table = new llvm::GlobalVariable(llvmModule, arrTy, /*isConstant=*/true,
                                               llvm::GlobalValue::PrivateLinkage,
                                               llvm::ConstantArray::get(arrTy, entries),
                                               "bronze_fn_sources");
        table->setAlignment(llvm::Align(8));
        builder_.CreateCall(
            shared_.abi.bronze_register_fn_sources,
            {textPtr, builder_.getInt32(static_cast<uint32_t>(text.size())),
             builder_.CreateConstInBoundsGEP2_32(arrTy, table, 0, 0),
             builder_.getInt32(static_cast<uint32_t>(entries.size() / 2))});
    }
}

void FunctionEmitter::emitModuleInit() {
    if (func_.name != "main" || blocks_.empty()) return;
    const ModuleTables& tables = shared_.tables;
    builder_.SetInsertPoint(blocks_[0]);

    if (tables.globalCache) {
        builder_.CreateCall(shared_.abi.bronze_register_value_cells,
                            {builder_.CreateConstInBoundsGEP2_32(
                                 tables.globalCache->getValueType(), tables.globalCache, 0, 0),
                             builder_.getInt64(tables.globalCacheCount)});
    }
    builder_.CreateCall(shared_.abi.bronze_register_value_cells,
                        {tables.moduleEnv, builder_.getInt64(1)});
    if (tables.templateSlots) {
        builder_.CreateCall(shared_.abi.bronze_register_value_cells,
                            {builder_.CreateConstInBoundsGEP2_32(
                                 tables.templateSlots->getValueType(), tables.templateSlots, 0, 0),
                             builder_.getInt64(tables.templateSlotCount)});
    }
    if (tables.fnSlots) {
        builder_.CreateCall(shared_.abi.bronze_register_fn_slots,
                            {builder_.CreateConstInBoundsGEP2_32(
                                 tables.fnSlots->getValueType(), tables.fnSlots, 0, 0),
                             builder_.getInt64(tables.fnSlotCount)});
    }
    emitFunctionSourceTables();
    for (size_t k = 0; k < shared_.module.keyConstants.size(); ++k) {
        llvm::Value* text = builder_.CreateGlobalString(shared_.module.keyConstants[k]);
        llvm::Value* id =
            builder_.CreateCall(shared_.abi.bronze_register_key_string, {text});
        builder_.CreateAlignedStore(
            id,
            builder_.CreateConstInBoundsGEP2_32(tables.keyMap->getValueType(), tables.keyMap, 0,
                                                static_cast<uint32_t>(k)),
            llvm::Align(4));
    }
    // AFTER the key loop, and that order is the whole reason this call is here
    // rather than at the top: the family table names fields by the module's own
    // key index, and the runtime turns those into process-wide ids by reading
    // `keyMap` — which is only filled once every key above has been interned.
    if (tables.familyClasses != nullptr) {
        builder_.CreateCall(
            shared_.abi.bronze_register_class_family,
            {builder_.CreateConstInBoundsGEP2_32(tables.familyClasses->getValueType(),
                                                 tables.familyClasses, 0, 0),
             builder_.getInt32(tables.familyClassCount),
             builder_.CreateConstInBoundsGEP2_32(tables.familyFields->getValueType(),
                                                 tables.familyFields, 0, 0),
             builder_.CreateConstInBoundsGEP2_32(tables.keyMap->getValueType(), tables.keyMap, 0,
                                                 0),
             tables.familyBase});
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
    if (!blocks_.empty()) {
        builder_.SetInsertPoint(blocks_[0]);
        globals_ = bindTlsBlock(builder_, shared_.abi);
    }
    emitPrologue();
    createBlockPhis();
    emitModuleInit();

    for (size_t bIdx = 0; bIdx < func_.blocks.size(); ++bIdx) {
        if (!emitBlock(bIdx)) return false;
    }
    return true;
}

// ---- exceptions -------------------------------------------------
//
// Propagation is a `ret`, so there is no unwind ABI: after any instruction
// that can throw, generated code loads the pending cell, compares it against
// the Hole singleton and branches. The not-taken path is the whole cost on a
// program that never throws.

void FunctionEmitter::popRootFrame() {
    if (!framePtr_) return;
    builder_.CreateStore(
        builder_.CreateLoad(ptrTy_, builder_.CreateStructGEP(frameTy_, framePtr_, 0)),
        globals_.bronze_gc_frame_top);
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
    llvm::Value* cell = builder_.CreateLoad(i64Ty_, globals_.bronze_exception_cell);
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
        // value the collector can parse before anything branches away from it.
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
        case il::Op::CmpLe:
        case il::Op::CmpGe:
        case il::Op::CmpEq:
        case il::Op::CmpNe:
        case il::Op::NumTruthy:
        case il::Op::StrictEq:
        case il::Op::LooseEq:
        case il::Op::RelLt:
        case il::Op::RelGt:
        case il::Op::RelLe:
        case il::Op::RelGe:
        case il::Op::Pow:
        case il::Op::ToInt32:
        case il::Op::BitAnd:
        case il::Op::BitOr:
        case il::Op::BitXor:
        case il::Op::Shl:
        case il::Op::Shr:
        case il::Op::UShr:
        case il::Op::BitNot:
        case il::Op::ToNumeric:
        case il::Op::NumericStep:
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
        builder_.CreateStore(thrown, globals_.bronze_exception_cell);
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
