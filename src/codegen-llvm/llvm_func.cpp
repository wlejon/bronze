#include "codegen-llvm/llvm_func.h"

#include <algorithm>
#include <string>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Type.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_env.h"
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

FunctionEmitter::FunctionEmitter(const Context& shared, uint32_t funcIndex,
                                 llvm::Function* llvmFunc, bool frameless)
    : shared_(shared),
      func_(shared.module.functions[funcIndex]),
      llvmFunc_(llvmFunc),
      builder_(shared.ctx),
      i64Ty_(llvm::Type::getInt64Ty(shared.ctx)),
      ptrTy_(llvm::PointerType::getUnqual(shared.ctx)),
      values_(shared.module.functions[funcIndex].valueCount, nullptr),
      propGetKey_(shared.module.functions[funcIndex].valueCount, UINT32_MAX),
      funcRefIndex_(shared.module.functions[funcIndex].valueCount, UINT32_MAX),
      holeRawSlot_(shared.module.functions[funcIndex].valueCount, 0),
      // Only when the seam asks for it: the scan is linear in the function and
      // the answer is unread otherwise (llvm_recv_proof.h, `holeRawSlotEnabled`).
      holeRawPays_(holeRawSlotEnabled()
                       ? planHoleRawSlots(shared.module.functions[funcIndex])
                       : std::vector<uint8_t>(shared.module.functions[funcIndex].valueCount, 0)),
      regBlock_(shared.module.functions[funcIndex].valueCount, il::kNoBlock),
      slotOf_(shared.plans[funcIndex].slotOf),
      ownSlots_(shared.plans[funcIndex].ownSlots),
      // A frameless variant fills a region the CALLER already sized and
      // linked, so it allocates nothing of its own and its slot count is its
      // own. Anything else allocates the whole region under it.
      frameSlots_(frameless ? shared.plans[funcIndex].ownSlots
                            : shared.regions.totalSlots[funcIndex]),
      frameless_(frameless),
      funcIndex_(funcIndex),
      repr_(shared.reprPlans[funcIndex]),
      live_(shared.livePlans[funcIndex]),
      envGuardsElided_(envAccessGuardsElided()) {
    argvBase_ = shared.plans[funcIndex].argvBase;
    constructSelfSlot_ = shared.plans[funcIndex].constructSelfSlot;
}

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

void FunctionEmitter::reload(il::ValueId id, bool holeInsensitive, uint32_t anchor) {
    if (id == il::kNoValue || id >= func_.valueCount) return;
    uint32_t slot = slotOf_[id];
    if (slot == kNoSlot) return;
    // Nothing has collected since the register was written, and the register
    // was written where the plan says it was — so it still names the object the
    // slot names, and the load is the load this stage exists to remove.
    //
    // A HOLE-RAW slot is the one value the register and the slot disagree
    // about on purpose: the slot holds the element's own bits and the reload is
    // where the correction is spent, so a use that can tell a hole from
    // `undefined` reads through the slot however current the register is.
    if (anchor != LiveRootPlan::kReload && regBlock_[id] == anchor && values_[id] != nullptr &&
        (holeRawSlot_[id] == 0 || holeInsensitive)) {
        return;
    }
    llvm::Value* loaded = builder_.CreateLoad(i64Ty_, slotAddr(slot));
    regBlock_[id] = static_cast<uint32_t>(currentILBlock_);
    // A hole-raw slot holds the element's own bits, so the correction the read
    // arm did not spend is spent here — once per USE rather than once per read,
    // which is what puts it on the edge that leaves a fast copy and off the
    // straight line that reads sixteen elements in a row. The use that cannot
    // tell the two apart pays nothing at all, which is the case a guarded
    // element run is made of.
    values_[id] = holeRawSlot_[id] != 0 && !holeInsensitive
                      ? emitHoleCorrection(builder_, loaded, "hole.raw")
                      : loaded;
}

llvm::Value* FunctionEmitter::calleeRegionBase() {
    // No frame at all means every merged callee under this one has an empty
    // region too — `planRegions` sizes a caller for the deepest region beneath
    // it, so a caller with nothing to allocate has nothing to hand over. The
    // pointer is then never dereferenced and null says so.
    if (!slotsBase_) return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy_));
    return slotAddr(ownSlots_);
}

void FunctionEmitter::emitPrologue() {
    if (blocks_.empty()) return;
    builder_.SetInsertPoint(blocks_[0]);

    // The frameless case: the region base arrived as a parameter and the
    // caller's prologue already wrote a valid Value into every slot of it, so
    // all that is left is rooting the parameters. Re-initialising the region
    // would be wrong in no way and useless in every way — a slot holding a
    // dead-but-valid Value from an earlier call through the same region is
    // exactly the float the pool already has.
    if (frameless_) {
        slotsBase_ = parentSlots_;
        for (size_t p = 0; p < func_.params.size(); ++p) {
            if (slotOf_[p] == kNoSlot) continue;
            builder_.CreateStore(values_[p], slotAddr(slotOf_[p]));
        }
        return;
    }
    if (frameSlots_ == 0) return;

    // The frame mirrors `bronze_gc_frame` from the ABI registry:
    // { prev, count, slots[frameSlots] }, allocated in this function's own
    // stack frame and linked onto the list head inline.
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
            regBlock_[param.id] = static_cast<uint32_t>(bIdx);
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
    // The method-call sites' env words, registered as value cells so a latched
    // direct-form env — a heap Value in the module's .bss — is forwarded in
    // place at every collection (bronze_abi.h's METHOD-CALL site contract).
    // The site list is DATA for the same reason the source tables are: one
    // call hands the whole array over, instead of a call per site in the
    // entry block. The IL is scanned here rather than carried in ModuleTables
    // because the site numbers are already in it and nothing else needs them.
    if (tables.icTable) {
        std::vector<uint64_t> methodSites;
        for (const il::Function& ilFn : shared_.module.functions) {
            for (const il::Block& block : ilFn.blocks) {
                for (const il::Instruction& inst : block.instructions) {
                    if (inst.op == il::Op::MethodCall || inst.op == il::Op::MethodCallSpread) {
                        methodSites.push_back(inst.icIndex);
                    }
                }
            }
        }
        std::sort(methodSites.begin(), methodSites.end());
        methodSites.erase(std::unique(methodSites.begin(), methodSites.end()),
                          methodSites.end());
        if (!methodSites.empty()) {
            llvm::Module& llvmModule = *llvmFunc_->getParent();
            auto* arrTy = llvm::ArrayType::get(i64Ty_, methodSites.size());
            auto* sitesTable = new llvm::GlobalVariable(
                llvmModule, arrTy, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
                llvm::ConstantDataArray::get(shared_.ctx, llvm::ArrayRef<uint64_t>(methodSites)),
                "__bronze_method_ic_sites");
            sitesTable->setAlignment(llvm::Align(8));
            builder_.CreateCall(
                shared_.abi.bronze_register_method_ic_cells,
                {builder_.CreateConstInBoundsGEP2_32(tables.icTable->getValueType(),
                                                     tables.icTable, 0, 0),
                 builder_.CreateConstInBoundsGEP2_32(arrTy, sitesTable, 0, 0),
                 builder_.getInt64(methodSites.size())});
        }
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
    // The slot-representation eligibility list, here for the family table's
    // reason: it names fields by the module's own key index, and the runtime
    // resolves those through `keyMap`, which the loop above has just filled.
    if (tables.slotReprFields != nullptr) {
        builder_.CreateCall(
            shared_.abi.bronze_register_slot_repr,
            {builder_.CreateConstInBoundsGEP2_32(tables.slotReprFields->getValueType(),
                                                 tables.slotReprFields, 0, 0),
             builder_.getInt32(tables.slotReprFieldCount),
             builder_.CreateConstInBoundsGEP2_32(tables.keyMap->getValueType(), tables.keyMap, 0,
                                                 0)});
    }
    // The PIN CENSUS site table, and it is here for exactly the reason the
    // family table is: it names sites by the module's own key index and the
    // runtime turns those into process-wide ids through `keyMap`, which is not
    // filled until the loop above has run.
    //
    // The WHOLE table goes over, not only the sites that execute. A site the
    // run never reaches is a fact — "never observed" is a different answer from
    // "not a site" — and a statically refused entry has to be refused on a run
    // that never touches it.
    if (!shared_.module.censusSites.empty() && tables.keyMap != nullptr) {
        std::vector<uint32_t> flat;
        flat.reserve(shared_.module.censusSites.size() * 2);
        for (const auto& site : shared_.module.censusSites) {
            flat.push_back(site.keyIndex);
            flat.push_back(site.info);
        }
        llvm::Module& llvmModule = *llvmFunc_->getParent();
        auto* arrTy = llvm::ArrayType::get(builder_.getInt32Ty(), flat.size());
        auto* table = new llvm::GlobalVariable(
            llvmModule, arrTy, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantDataArray::get(shared_.ctx, llvm::ArrayRef<uint32_t>(flat)),
            "__bronze_census_sites");
        table->setAlignment(llvm::Align(4));
        builder_.CreateCall(
            shared_.abi.bronze_census_register,
            {builder_.CreateGlobalString(shared_.module.censusOutPath),
             builder_.CreateConstInBoundsGEP2_32(arrTy, table, 0, 0),
             builder_.getInt32(static_cast<uint32_t>(shared_.module.censusSites.size())),
             builder_.CreateConstInBoundsGEP2_32(tables.keyMap->getValueType(), tables.keyMap, 0,
                                                 0)});
    }
}

// ---- the framed entry of a merge target -------------------------------

void emitFrameForwarder(const FunctionEmitter::Context& shared, uint32_t funcIndex,
                        llvm::Function* entry, llvm::Function* variant) {
    llvm::LLVMContext& ctx = shared.ctx;
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::PointerType* ptrTy = llvm::PointerType::getUnqual(ctx);
    const uint32_t slots = shared.regions.totalSlots[funcIndex];

    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", entry));
    AbiGlobals globals = bindTlsBlock(b, shared.abi);

    std::vector<llvm::Value*> args;
    for (llvm::Argument& a : entry->args()) args.push_back(&a);

    llvm::Value* slotsBase = llvm::ConstantPointerNull::get(ptrTy);
    llvm::StructType* frameTy = nullptr;
    llvm::Value* frame = nullptr;
    if (slots > 0) {
        frameTy = llvm::StructType::get(ctx, {ptrTy, i64Ty, llvm::ArrayType::get(i64Ty, slots)});
        frame = b.CreateAlloca(frameTy, nullptr, "gcframe");
        slotsBase = b.CreateStructGEP(frameTy, frame, 2);
        // The whole region, this function's slots and every merged callee's
        // alike, holds a valid Value before the frame is linked. Once, here,
        // rather than once per call into the region: a slot still holding an
        // earlier call's value is the same dead-but-valid float the slot pool
        // already has, and it is never garbage.
        llvm::Value* undefBits = b.getInt64(BRONZE_ABI_UNDEFINED_BITS);
        for (uint32_t s = 0; s < slots; ++s) {
            b.CreateStore(undefBits, b.CreateGEP(i64Ty, slotsBase, b.getInt32(s)));
        }
        b.CreateStore(b.getInt64(slots), b.CreateStructGEP(frameTy, frame, 1));
        b.CreateStore(b.CreateLoad(ptrTy, globals.bronze_gc_frame_top),
                      b.CreateStructGEP(frameTy, frame, 0));
        b.CreateStore(frame, globals.bronze_gc_frame_top);
    }
    args.push_back(slotsBase);
    args.push_back(globals.block_base);
    llvm::CallInst* call = b.CreateCall(variant, args);
    // Unconditional, and not the budgeted ask a direct edge makes: this call
    // exists only because the body was split out of the function it belongs
    // to, and leaving it out of line would be a boundary the source never had.
    call->addFnAttr(llvm::Attribute::AlwaysInline);
    if (slots > 0) {
        b.CreateStore(b.CreateLoad(ptrTy, b.CreateStructGEP(frameTy, frame, 0)),
                      globals.bronze_gc_frame_top);
    }
    if (entry->getReturnType()->isVoidTy()) {
        b.CreateRetVoid();
    } else {
        b.CreateRet(call);
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
        if (argIdx < func_.params.size()) {
            arg.setName(func_.params[argIdx].name);
            values_[argIdx] = &arg;
            // An argument dominates the whole function, so the plan may name
            // block 0 as a parameter's anchor and be right about it.
            regBlock_[argIdx] = 0;
        } else if (argIdx == func_.params.size()) {
            arg.setName("__region");
            parentSlots_ = &arg;
        } else {
            arg.setName("__tls");
            tlsBase_ = &arg;
        }
        ++argIdx;
    }

    if (!blocks_.empty()) {
        builder_.SetInsertPoint(blocks_[0]);
        if (frameless_) {
            globals_ = bindTlsBlockAt(builder_, tlsBase_);
        } else {
            globals_ = bindTlsBlock(builder_, shared_.abi);
            tlsBase_ = globals_.block_base;
        }
    }
    emitPrologue();
    createBlockPhis();
    emitModuleInit();

    for (size_t bIdx = 0; bIdx < func_.blocks.size(); ++bIdx) {
        if (!emitBlock(bIdx)) return false;
    }

    // A frameless variant fetches nothing — its view of the thread block is
    // the caller's, and it arrives as a parameter. But the seam words the
    // instruction families emit (llvm_iter.cpp, llvm_arith.cpp,
    // llvm_elem_cache.cpp, llvm_ops.cpp) still ASK for a fetch of their own,
    // on the standing promise that a `readnone` call CSEs with the prologue's.
    // There is no prologue fetch here for them to CSE with, and
    // `cacheTlsFetches` skips a function whose entry block has none — so every
    // one of them would stay a real cross-module call, in the loop. They are
    // answered from the parameter instead, which is the same answer the
    // prologue's fetch would have given.
    if (frameless_ && tlsBase_ != nullptr) {
        llvm::SmallVector<llvm::CallInst*, 8> fetches;
        for (llvm::BasicBlock& bb : *llvmFunc_) {
            for (llvm::Instruction& inst : bb) {
                auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                if (call != nullptr &&
                    call->getCalledFunction() == shared_.abi.bronze_tls_block_addr) {
                    fetches.push_back(call);
                }
            }
        }
        for (llvm::CallInst* call : fetches) {
            call->replaceAllUsesWith(tlsBase_);
            call->eraseFromParent();
        }
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
    // A frameless variant links nothing, so it unlinks nothing: the frame its
    // slots live in belongs to a caller that is still running.
    if (frameless_ || !framePtr_) return;
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

    // A receiver proof is an LLVM value, and a value must dominate its use. It
    // usually starts again here — but a run may span a straight-line CHAIN of
    // blocks (llvm_recv_proof.h), and when this block continues the chain the
    // last one started, the proof from the last one dominates every use in this
    // one and needs no phi: this block's only predecessor is the block that
    // just branched here, and it takes no parameters.
    //
    // The emitter checks that itself rather than trusting the plan, because the
    // plan is about the IL and this is about what was actually emitted. Blocks
    // are emitted in index order, so the check is nearly always satisfied; when
    // it is not, the proofs die and every site in the block emits its own
    // ladder, which is slower and not wrong.
    runPlan_ = planBlockRuns(shared_.module, func_, blockIndex);
    const bool continuesChain = runPlan_.continues != il::kNoBlock &&
                                runPlan_.continues == lastEmittedBlock_;
    if (!continuesChain) {
        recvProof_ = ReceiverProof{};
        storeProof_ = StoreProof{};
        arrayStoreProof_ = ArrayStoreProof{};
    }
    lastEmittedBlock_ = static_cast<il::BlockId>(blockIndex);

    // A block parameter is a def like any other: the phi's value has to reach
    // its root slot before anything can collect.
    for (size_t pi = 0; pi < block.params.size(); ++pi) {
        uint32_t slot = slotOf_[block.params[pi].id];
        if (slot == kNoSlot) continue;
        builder_.CreateStore(blockPhis_[blockIndex][pi], slotAddr(slot));
    }

    for (size_t instIndex = 0; instIndex < block.instructions.size(); ++instIndex) {
        const auto& inst = block.instructions[instIndex];
        // Where this instruction sits, for the one emitter that needs its
        // POSITION and not only its fields: a store spends a `pin.guard`
        // standing immediately in front of it (llvm_repr.h, `storeValueRepr`).
        currentILInst_ = instIndex;
        // The uses in the order the live-root plan enumerated them
        // (llvm_live_roots.h): operands, then the two block-argument lists.
        // The two walks must agree position for position, because the plan's
        // answer is indexed by position and nothing else.
        size_t useIndex = 0;
        for (size_t i = 0; i < inst.operands.size(); ++i, ++useIndex) {
            reload(inst.operands[i], i == 0 && holeInsensitiveUse(inst, inst.operands[0]),
                   live_.anchor(blockIndex, instIndex, useIndex));
        }
        for (il::ValueId id : inst.target.args) {
            reload(id, false, live_.anchor(blockIndex, instIndex, useIndex++));
        }
        for (il::ValueId id : inst.elseTarget.args) {
            reload(id, false, live_.anchor(blockIndex, instIndex, useIndex++));
        }

        proofsCarried_ = false;
        if (!emitInstruction(inst)) return false;

        // A proof holds a pointer DERIVED into the heap, which a collection
        // leaves dangling. A run member carries the live proofs across a join
        // of its own (llvm_recv_proof.h); anything else that can collect ends
        // them here, so the rule is enforced at the instruction rather than
        // resting on the plan being the only thing that says so.
        if (il::canCollect(inst) && !proofsCarried_) {
            recvProof_ = ReceiverProof{};
            storeProof_ = StoreProof{};
            arrayStoreProof_ = ArrayStoreProof{};
        }

        if (inst.result != il::kNoValue && inst.result < func_.valueCount &&
            values_[inst.result]) {
            regBlock_[inst.result] = static_cast<uint32_t>(blockIndex);
            if (slotOf_[inst.result] != kNoSlot) {
                builder_.CreateStore(values_[inst.result], slotAddr(slotOf_[inst.result]));
            }
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
