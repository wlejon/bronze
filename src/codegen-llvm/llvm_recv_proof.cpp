#include "codegen-llvm/llvm_recv_proof.h"

#include "codegen-llvm/llvm_alias.h"
#include "codegen-llvm/llvm_prop_ic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

namespace bronze::codegen_llvm {

namespace {

// A run of one buys nothing: the proof ladder IS the access ladder, and the
// fast arm would only add a branch in front of it. Two is where the arithmetic
// turns — one ladder plus two four-instruction arms against two ladders.
constexpr size_t kMinRunLength = 2;

// The receiver of a constant-index property read, or kNoValue for an
// instruction that is not one.
il::ValueId receiverOf(const il::Instruction& inst) {
    if (inst.op != il::Op::PropGet) return il::kNoValue;
    if (inst.operands.empty()) return il::kNoValue;
    return inst.operands[0];
}

std::optional<uint32_t> indexKeyOf(const il::Module& module, const il::Instruction& inst) {
    if (inst.op != il::Op::PropGet) return std::nullopt;
    if (inst.keyIndex >= module.keyConstants.size()) return std::nullopt;
    return parseIndexKey(module.keyConstants[inst.keyIndex]);
}

// The index a constant-index `prop.set` writes, for the Array store planner —
// `te[3] = v`, which the front end spells as a PropSet against the key "3".
// Read from the KEY and never from an operand: a `prop.set` whose key is not an
// index is a named write and belongs to no run.
std::optional<uint32_t> storeIndexKeyOf(const il::Module& module, const il::Instruction& inst) {
    if (inst.op != il::Op::PropSet) return std::nullopt;
    if (inst.operands.size() < 2) return std::nullopt;
    if (inst.keyIndex >= module.keyConstants.size()) return std::nullopt;
    return parseIndexKey(module.keyConstants[inst.keyIndex]);
}

// A `prop.set` whose key is a NAME rather than an index. Its three bare-store
// arms neither allocate nor call, llvm_prop_set.cpp hands them back as the
// join's proof-preserving edge, and so a run of anything spans it. The index
// case is excluded for the reason the header gives: those arms can grow an
// element block.
bool carriesProofAcross(const il::Module& module, const il::Instruction& inst) {
    if (inst.op != il::Op::PropSet) return false;
    if (inst.keyIndex >= module.keyConstants.size()) return false;
    return !parseIndexKey(module.keyConstants[inst.keyIndex]).has_value();
}

}  // namespace

bool receiverProofEnabled() {
    static const bool enabled = std::getenv("BRONZE_NO_RECV_PROOF") == nullptr;
    return enabled;
}

bool slotStoreCarryEnabled() {
    static const bool enabled = std::getenv("BRONZE_NO_SLOT_STORE_CARRY") == nullptr;
    return enabled && receiverProofEnabled();
}

namespace {

// The straight line of blocks a run may span, flattened into one instruction
// list so that the scan below is written once and reads the same whether the
// chain is one block or nine. `owner` and `local` take a flat index back to the
// block it came from, which is what the typed-array store classifier and the
// final per-block split still need.
struct ChainView {
    const il::Function* func = nullptr;
    std::vector<size_t> blocks;
    std::vector<const il::Instruction*> insts;
    std::vector<uint32_t> owner;
    std::vector<uint32_t> local;
    // Each chain member's own map from a value to its defining instruction
    // INSIDE that member, which is the question `classifyStoreSite` asks.
    std::vector<std::vector<uint32_t>> defIndex;

    size_t size() const { return insts.size(); }
    const il::Block& blockOf(size_t flat) const { return func->blocks[blocks[owner[flat]]]; }
};

// The index a constant-index ARRAY ELEMENT store writes, in whichever of the
// two forms the front end spelled it:
//
//   - a `prop.set` against the key "3", which is how `te[3] = v` lowers when
//     nothing is known about `te` (storeIndexKeyOf above);
//   - an `elem.set.typed` of the PINNED plain-array kind, which is how the same
//     line lowers under a `--pins ... numeric-elements` manifest, with the
//     index a `const.f64` in the same block.
//
// Both write one Value into one element slot of one Array, so both are members
// of the same run and spend the same proof. The pinned form joined no run at
// all until this was written, and paid dearly for it: it derives its element
// address from the receiver itself — a reload from a root slot after every
// intervening read — so `Matrix4.copy` under `Matrix4.elements:
// numeric-elements` re-derived the base eleven instructions at a time, sixteen
// times, where the unpinned build derived it once and spent a GEP per store.
//
// The run is established with the FULL ladder in both cases (llvm_pin.h says
// what the pin licenses; this planner does not spend it). That is what lets the
// two forms share a run without a homogeneity rule: a pinned member whose proof
// did not hold falls back to exactly the unguarded store it emitted before, and
// an unpinned member never skips a test the pin paid for.
std::optional<uint32_t> arrayStoreIndexOf(const il::Module& module, const ChainView& view,
                                          size_t flat) {
    const il::Instruction& inst = *view.insts[flat];
    if (inst.op == il::Op::PropSet) return storeIndexKeyOf(module, inst);
    if (inst.op != il::Op::ElemSetTyped || inst.operands.size() < 3) return std::nullopt;
    if (inst.immI32 != il::kElemKindPlainArrayF64) return std::nullopt;
    const il::Instruction* idxDef =
        defOf(view.blockOf(flat), view.defIndex[view.owner[flat]], inst.operands[1]);
    if (idxDef == nullptr || idxDef->op != il::Op::ConstF64) return std::nullopt;
    const double v = idxDef->immF64;
    if (!(v >= 0.0) || !std::isfinite(v) || v != std::floor(v)) return std::nullopt;
    if (v > static_cast<double>(kMaxStoreOffset)) return std::nullopt;
    return static_cast<uint32_t>(v);
}

// One pass of the joint scan. `transparent[i]` says whether instruction `i` is
// currently believed to be a committed run member — a site whose fast arm
// neither allocates nor calls and whose join re-establishes every live proof.
// Everything else that can collect ends both runs. Indices are FLAT over the
// chain; `planBlockRuns` cuts the result back into per-block plans.
void scanRuns(const il::Module& module, const ChainView& view,
              const std::vector<bool>& transparent, bool wantStores, bool wantArrayStores,
              bool carry, BlockRunPlan& plan) {
    plan = BlockRunPlan{};
    plan.reads.sites.assign(view.size(), ReceiverRunPlan::Site{});
    plan.stores.sites.assign(view.size(), StoreRunPlan::Site{});
    plan.arrayStores.sites.assign(view.size(), ArrayStoreRunPlan::Site{});

    std::vector<size_t> readMembers;
    std::vector<uint32_t> readIndices;
    il::ValueId readRecv = il::kNoValue;
    uint32_t readMax = 0;
    uint32_t nextReadRun = 0;

    std::vector<size_t> storeMembers;
    std::vector<uint32_t> storeOffsets;
    il::ValueId storeRecv = il::kNoValue;
    il::ValueId storeBase = il::kNoValue;
    uint32_t storeMax = 0;
    uint32_t nextStoreRun = 0;

    std::vector<size_t> arrMembers;
    std::vector<uint32_t> arrIndices;
    il::ValueId arrRecv = il::kNoValue;
    uint32_t arrMax = 0;
    uint32_t nextArrRun = 0;

    auto commitReads = [&]() {
        if (readMembers.size() >= kMinRunLength) {
            for (size_t i = 0; i < readMembers.size(); ++i) {
                auto& site = plan.reads.sites[readMembers[i]];
                site.run = nextReadRun;
                site.establishes = (i == 0);
                site.runMaxIndex = readMax;
            }
            ++nextReadRun;
        }
        readMembers.clear();
        readIndices.clear();
        readRecv = il::kNoValue;
        readMax = 0;
    };
    auto commitStores = [&]() {
        if (storeMembers.size() >= kMinRunLength) {
            for (size_t i = 0; i < storeMembers.size(); ++i) {
                auto& site = plan.stores.sites[storeMembers[i]];
                site.run = nextStoreRun;
                site.establishes = (i == 0);
                site.runMaxOffset = storeMax;
                site.offset = storeOffsets[i];
                site.base = storeBase;
            }
            ++nextStoreRun;
        }
        storeMembers.clear();
        storeOffsets.clear();
        storeRecv = il::kNoValue;
        storeBase = il::kNoValue;
        storeMax = 0;
    };
    auto commitArrayStores = [&]() {
        if (arrMembers.size() >= kMinRunLength) {
            for (size_t i = 0; i < arrMembers.size(); ++i) {
                auto& site = plan.arrayStores.sites[arrMembers[i]];
                site.run = nextArrRun;
                site.establishes = (i == 0);
                site.runMaxIndex = arrMax;
                site.index = arrIndices[i];
            }
            ++nextArrRun;
        }
        arrMembers.clear();
        arrIndices.clear();
        arrRecv = il::kNoValue;
        arrMax = 0;
    };

    for (size_t i = 0; i < view.size(); ++i) {
        const auto& inst = *view.insts[i];
        const il::ValueId recv = receiverOf(inst);
        const std::optional<uint32_t> idx = indexKeyOf(module, inst);
        const StoreSiteShape store =
            wantStores ? classifyStoreSite(view.blockOf(i), view.defIndex[view.owner[i]],
                                           view.local[i])
                       : StoreSiteShape{};
        const std::optional<uint32_t> arrIdx =
            wantArrayStores ? arrayStoreIndexOf(module, view, i) : std::nullopt;

        // A run member is transparent to the OTHER runs' proofs; anything else
        // that can move the heap ends them. Asked before this instruction joins
        // a run of its own, because the question is about the proofs it does
        // not carry.
        const bool member = (recv != il::kNoValue && idx.has_value()) || store.ok ||
                            arrIdx.has_value();
        const bool opaque = !member || !transparent[i];

        if (recv != il::kNoValue && idx.has_value()) {
            if (opaque && il::canCollect(inst)) {
                commitStores();
                commitArrayStores();
            }
            if (!readMembers.empty() && recv == readRecv) {
                readMembers.push_back(i);
                readIndices.push_back(*idx);
                readMax = std::max(readMax, *idx);
                continue;
            }
            // A different receiver, or the first candidate in the block: the
            // run in hand is finished and this access opens the next one.
            commitReads();
            readRecv = recv;
            readMax = *idx;
            readMembers.push_back(i);
            readIndices.push_back(*idx);
            continue;
        }

        if (store.ok) {
            if (opaque && il::canCollect(inst)) {
                commitReads();
                commitArrayStores();
            }
            if (!storeMembers.empty() && store.receiver == storeRecv && store.base == storeBase) {
                storeMembers.push_back(i);
                storeOffsets.push_back(store.offset);
                storeMax = std::max(storeMax, store.offset);
                continue;
            }
            commitStores();
            storeRecv = store.receiver;
            storeBase = store.base;
            storeMax = store.offset;
            storeMembers.push_back(i);
            storeOffsets.push_back(store.offset);
            continue;
        }

        if (arrIdx.has_value()) {
            if (opaque && il::canCollect(inst)) {
                commitReads();
                commitStores();
            }
            const il::ValueId target = inst.operands[0];
            if (!arrMembers.empty() && target == arrRecv) {
                arrMembers.push_back(i);
                arrIndices.push_back(*arrIdx);
                arrMax = std::max(arrMax, *arrIdx);
                continue;
            }
            commitArrayStores();
            arrRecv = target;
            arrMax = *arrIdx;
            arrMembers.push_back(i);
            arrIndices.push_back(*arrIdx);
            continue;
        }

        // A named `prop.set` carries every live proof across its own join
        // instead of ending the runs it stands in (see the header). It joins no
        // run of its own — there is nothing to prove about a slot store that
        // the shape guard in front of it does not already ask.
        //
        // With one exception, and it is the run about the store's OWN object. A
        // receiver proof says its receiver is an ARRAY, and a named store to an
        // array is exactly the store that cannot take a bare arm: `isPlain` is
        // false, so it reaches bronze_prop_set to make the side object that
        // holds `arr.foo`, and that allocates. Carrying a proof of that object
        // across it could only ever hand back a proof the join has already
        // killed, while making the one ladder in front of the run test a larger
        // index than the run before the store needed. So that run ends here,
        // exactly as it did before this rule existed.
        if (carry && carriesProofAcross(module, inst)) {
            const il::ValueId target = inst.operands.empty() ? il::kNoValue : inst.operands[0];
            if (target != il::kNoValue) {
                if (target == readRecv) commitReads();
                if (target == storeRecv || target == storeBase) commitStores();
                if (target == arrRecv) commitArrayStores();
            }
            continue;
        }

        // Not a member of any of the three. It ends a run if it can move the
        // heap out from under that run's derived pointer, or if it redefines
        // the receiver — or, for a typed-array store run, the base — the proof
        // was made about.
        if (il::canCollect(inst)) {
            commitReads();
            commitStores();
            commitArrayStores();
            continue;
        }
        if (inst.result != il::kNoValue) {
            if (inst.result == readRecv) commitReads();
            if (inst.result == storeRecv || inst.result == storeBase) commitStores();
            if (inst.result == arrRecv) commitArrayStores();
        }
    }
    commitReads();
    commitStores();
    commitArrayStores();
}

// The single predecessor of every block, or kNoBlock where a block has none or
// more than one. Built from the terminators and the handler edges, which are
// the only edges this IL has.
std::vector<il::BlockId> singlePredecessors(const il::Function& func) {
    const size_t n = func.blocks.size();
    std::vector<il::BlockId> pred(n, il::kNoBlock);
    std::vector<uint8_t> count(n, 0);
    auto edge = [&](size_t from, il::BlockId to) {
        if (to == il::kNoBlock || to >= n) return;
        if (count[to] < 2) ++count[to];
        pred[to] = static_cast<il::BlockId>(from);
    };
    for (size_t b = 0; b < n; ++b) {
        for (const auto& inst : func.blocks[b].instructions) {
            edge(b, inst.target.block);
            edge(b, inst.elseTarget.block);
        }
        edge(b, func.blocks[b].handler);
    }
    for (size_t b = 0; b < n; ++b) {
        if (count[b] != 1) pred[b] = il::kNoBlock;
    }
    return pred;
}

// May a run continue from `from` into `to`? `to` must be reached from `from`
// and from nowhere else, take no parameters — a parameter is a phi, and a phi
// is a value the head does not define — and be the same copy of the same
// region, because the two copies of a region are alternatives and a `Shared`
// block is reached from both.
bool chains(const il::Function& func, const std::vector<il::BlockId>& pred, size_t from,
            il::BlockId to) {
    if (to == il::kNoBlock || to >= func.blocks.size()) return false;
    // The entry block is entered from OUTSIDE the function, and that edge is in
    // no terminator, so the predecessor count below cannot see it. A back edge
    // to block 0 would otherwise read as its only predecessor and the chain
    // would claim a domination that does not exist.
    if (to == 0) return false;
    if (pred[to] != static_cast<il::BlockId>(from)) return false;
    const il::Block& a = func.blocks[from];
    const il::Block& b = func.blocks[to];
    return b.params.empty() && b.copyClass == a.copyClass && b.copyRegion == a.copyRegion &&
           b.handler == a.handler;
}

// The chain `blockIndex` belongs to, head first. Walking BACK to the head
// before walking forward is what makes the answer the same whichever member
// asks, which is what lets the emitter call this once per block and still get
// one consistent plan for the whole chain.
std::vector<size_t> runChain(const il::Function& func, const std::vector<il::BlockId>& pred,
                             size_t blockIndex) {
    size_t head = blockIndex;
    for (size_t guard = 0; guard < func.blocks.size(); ++guard) {
        const il::BlockId p = pred[head];
        if (p == il::kNoBlock || !chains(func, pred, p, static_cast<il::BlockId>(head))) break;
        head = p;
    }
    std::vector<size_t> chain{head};
    for (size_t guard = 0; guard < func.blocks.size(); ++guard) {
        const il::Block& b = func.blocks[chain.back()];
        if (b.instructions.empty()) break;
        const il::Instruction& term = b.instructions.back();
        // The THEN arm first, and the chain takes the first arm it can. Both
        // arms are dominated by the head, so either would be sound; taking the
        // then arm is what makes the answer deterministic, and it is the arm a
        // guarded region's test continues on — `br %ok, next, bail` — which is
        // the shape this exists for. The other arm plans as its own chain head,
        // and the two agree because both walk back to the same head and forward
        // by the same rule.
        size_t next = SIZE_MAX;
        for (il::BlockId to : {term.target.block, term.elseTarget.block}) {
            if (!chains(func, pred, chain.back(), to)) continue;
            next = to;
            break;
        }
        if (next == SIZE_MAX) break;
        chain.push_back(next);
    }
    return chain;
}

}  // namespace

BlockRunPlan planBlockRuns(const il::Module& module, const il::Function& func,
                           size_t blockIndex) {
    return planBlockRuns(module, func, blockIndex, slotStoreCarryEnabled());
}

BlockRunPlan planBlockRuns(const il::Module& module, const il::Function& func, size_t blockIndex,
                           bool carry) {
    BlockRunPlan plan;
    if (!receiverProofEnabled()) return plan;
    if (blockIndex >= func.blocks.size()) return plan;
    const bool wantStores = storeProofEnabled();
    const bool wantArrayStores = arrayStoreProofEnabled();

    ChainView view;
    view.func = &func;
    if (carry) {
        const std::vector<il::BlockId> pred = singlePredecessors(func);
        view.blocks = runChain(func, pred, blockIndex);
    } else {
        view.blocks = {blockIndex};
    }

    for (size_t c = 0; c < view.blocks.size(); ++c) {
        const il::Block& block = func.blocks[view.blocks[c]];
        // A value's defining instruction inside ITS OWN block, so the store
        // planner can look through an index's `box.f64` and `add` without a
        // search per site. A value defined in another block — a parameter, a
        // phi, an earlier member of this chain — has no entry, which is exactly
        // the answer the planner wants for a run base.
        std::vector<uint32_t> defIndex(func.valueCount, kNoDef);
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const il::ValueId result = block.instructions[i].result;
            if (result != il::kNoValue && result < defIndex.size()) {
                defIndex[result] = static_cast<uint32_t>(i);
            }
        }
        view.defIndex.push_back(std::move(defIndex));
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            view.insts.push_back(&block.instructions[i]);
            view.owner.push_back(static_cast<uint32_t>(c));
            view.local.push_back(static_cast<uint32_t>(i));
        }
    }

    // The fixpoint the header describes. Start optimistic — every candidate
    // site is assumed to become a run member — and re-scan whenever a scan
    // disagrees. Membership only ever shrinks (dropping a member can split or
    // shorten a run, never lengthen one), so this terminates, and the bound
    // below is a belt on that argument rather than a policy.
    std::vector<bool> transparent(view.size(), false);
    for (size_t i = 0; i < view.size(); ++i) {
        const auto& inst = *view.insts[i];
        transparent[i] =
            (receiverOf(inst) != il::kNoValue && indexKeyOf(module, inst)) ||
            (wantStores &&
             classifyStoreSite(view.blockOf(i), view.defIndex[view.owner[i]], view.local[i]).ok) ||
            (wantArrayStores && arrayStoreIndexOf(module, view, i).has_value());
    }

    BlockRunPlan flat;
    for (size_t round = 0; round <= view.size(); ++round) {
        scanRuns(module, view, transparent, wantStores, wantArrayStores, carry, flat);
        bool changed = false;
        for (size_t i = 0; i < view.size(); ++i) {
            const bool member = flat.reads.sites[i].run != ReceiverRunPlan::kNoRun ||
                                flat.stores.sites[i].run != StoreRunPlan::kNoRun ||
                                flat.arrayStores.sites[i].run != ArrayStoreRunPlan::kNoRun;
            if (transparent[i] && !member) {
                transparent[i] = false;
                changed = true;
            }
        }
        if (!changed) break;
    }

    // The slice belonging to the block that asked. Everything else in the plan
    // is another member's, and the emitter reaches it when it emits that member.
    size_t base = 0;
    for (size_t c = 0; c < view.blocks.size(); ++c) {
        const size_t count = func.blocks[view.blocks[c]].instructions.size();
        if (view.blocks[c] == blockIndex) {
            plan.reads.sites.assign(flat.reads.sites.begin() + static_cast<ptrdiff_t>(base),
                                    flat.reads.sites.begin() + static_cast<ptrdiff_t>(base + count));
            plan.stores.sites.assign(
                flat.stores.sites.begin() + static_cast<ptrdiff_t>(base),
                flat.stores.sites.begin() + static_cast<ptrdiff_t>(base + count));
            plan.arrayStores.sites.assign(
                flat.arrayStores.sites.begin() + static_cast<ptrdiff_t>(base),
                flat.arrayStores.sites.begin() + static_cast<ptrdiff_t>(base + count));
            // What the emitter has to agree with before it keeps its proofs
            // across the edge into this block.
            plan.continues = c == 0 ? il::kNoBlock : static_cast<il::BlockId>(view.blocks[c - 1]);
            break;
        }
        base += count;
    }
    return plan;
}

ReceiverProof emitReceiverProof(llvm::IRBuilder<>& builder, llvm::Value* objBits,
                                il::ValueId receiver, uint32_t run, uint32_t maxIndex) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* i16Ty = llvm::Type::getInt16Ty(ctx);
    llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);

    const std::string tag = "recv" + std::to_string(run) + ".";

    // Every test below reads a field the test before it proved was there — the
    // header only after the tag says there is one, the elements object only
    // after the kind says the header has that field at all — so the ladder is a
    // chain of blocks rather than one wide `and`. All four failure edges meet
    // at the same join as the success edge, because what a member branches on
    // has to be a single i1 and not a jump the caller has to know about.
    llvm::BasicBlock* hdrBb = llvm::BasicBlock::Create(ctx, tag + "hdr", fn);
    llvm::BasicBlock* kindBb = llvm::BasicBlock::Create(ctx, tag + "kind", fn);
    llvm::BasicBlock* elemsBb = llvm::BasicBlock::Create(ctx, tag + "elems", fn);
    llvm::BasicBlock* baseBb = llvm::BasicBlock::Create(ctx, tag + "base", fn);
    llvm::BasicBlock* joinBb = llvm::BasicBlock::Create(ctx, tag + "join", fn);

    llvm::BasicBlock* entryBb = builder.GetInsertBlock();
    llvm::Value* tagBits = builder.CreateLShr(objBits, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isObject =
        builder.CreateICmpEQ(tagBits, builder.getInt64(BRONZE_ABI_TAG_OBJECT), tag + "isobj");
    builder.CreateCondBr(isObject, hdrBb, joinBb);

    builder.SetInsertPoint(hdrBb);
    llvm::Value* addr = builder.CreateAnd(objBits, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* hdr = builder.CreateIntToPtr(addr, ptrTy, tag + "hdr");
    llvm::Value* flagsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_OBJ_FLAGS_OFFSET);
    auto* flags = builder.CreateAlignedLoad(i16Ty, flagsPtr, llvm::Align(2), tag + "flags");
    llvm::Value* isArray =
        builder.CreateICmpEQ(flags, builder.getInt16(BRONZE_ABI_OBJ_FLAGS_ARRAY));
    builder.CreateCondBr(isArray, kindBb, joinBb);

    // ONE length test for the whole run, against its largest index. Every
    // member is then in bounds by construction, which is what lets each fast
    // arm below be a load with no compare in front of it.
    builder.SetInsertPoint(kindBb);
    llvm::Value* lenPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_LENGTH_OFFSET);
    auto* len = builder.CreateAlignedLoad(i32Ty, lenPtr, llvm::Align(4), tag + "len");
    tagArrayHeaderAccess(len, ctx);
    llvm::Value* inBounds = builder.CreateICmpULT(builder.getInt32(maxIndex), len);
    builder.CreateCondBr(inBounds, elemsBb, joinBb);

    builder.SetInsertPoint(elemsBb);
    llvm::Value* elemsPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_ELEMS_OFFSET);
    auto* elemsVal = builder.CreateAlignedLoad(i64Ty, elemsPtr, llvm::Align(8), tag + "elems");
    tagArrayHeaderAccess(elemsVal, ctx);
    llvm::Value* elemsTag = builder.CreateLShr(elemsVal, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* elemsIsObj =
        builder.CreateICmpEQ(elemsTag, builder.getInt64(BRONZE_ABI_TAG_OBJECT));
    builder.CreateCondBr(elemsIsObj, baseBb, joinBb);

    // Element zero's address, byte for byte the address llvm_prop_get.cpp's
    // array arm computes for index zero: the ring head, plus the one slot the
    // elements object carries in front of its payload. A proven read and a
    // ladder read of the same element cannot disagree about where it is.
    builder.SetInsertPoint(baseBb);
    llvm::Value* elemsAddr =
        builder.CreateAnd(elemsVal, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* elemsObj = builder.CreateIntToPtr(elemsAddr, ptrTy);
    llvm::Value* headPtr =
        builder.CreateConstInBoundsGEP1_32(i8Ty, hdr, BRONZE_ABI_ARRAY_HEAD_OFFSET);
    auto* head = builder.CreateAlignedLoad(i32Ty, headPtr, llvm::Align(4), tag + "head");
    tagArrayHeaderAccess(head, ctx);
    llvm::Value* slot0 = builder.CreateAdd(builder.CreateZExt(head, i64Ty), builder.getInt64(1));
    llvm::Value* base = builder.CreateInBoundsGEP(i64Ty, elemsObj, slot0, tag + "base");
    builder.CreateBr(joinBb);

    builder.SetInsertPoint(joinBb);
    llvm::PHINode* okPhi = builder.CreatePHI(builder.getInt1Ty(), 5, tag + "ok");
    okPhi->addIncoming(builder.getFalse(), entryBb);
    okPhi->addIncoming(builder.getFalse(), hdrBb);
    okPhi->addIncoming(builder.getFalse(), kindBb);
    okPhi->addIncoming(builder.getFalse(), elemsBb);
    okPhi->addIncoming(builder.getTrue(), baseBb);

    llvm::PHINode* basePhi = builder.CreatePHI(ptrTy, 5, tag + "baseptr");
    llvm::Value* poison = llvm::PoisonValue::get(ptrTy);
    basePhi->addIncoming(poison, entryBb);
    basePhi->addIncoming(poison, hdrBb);
    basePhi->addIncoming(poison, kindBb);
    basePhi->addIncoming(poison, elemsBb);
    basePhi->addIncoming(base, baseBb);

    ReceiverProof proof;
    proof.receiver = receiver;
    proof.run = run;
    proof.ok = okPhi;
    proof.base = basePhi;
    return proof;
}

ProvenRead emitProvenElementRead(llvm::IRBuilder<>& builder, const ReceiverProof& proof,
                                 uint32_t index, llvm::BasicBlock* doneBb) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    const std::string tag = "recv" + std::to_string(proof.run) + ".e" + std::to_string(index) + ".";
    llvm::BasicBlock* fastBb = llvm::BasicBlock::Create(ctx, tag + "fast", fn);
    llvm::BasicBlock* ladderBb = llvm::BasicBlock::Create(ctx, tag + "ladder", fn);
    builder.CreateCondBr(proof.ok, fastBb, ladderBb);

    builder.SetInsertPoint(fastBb);
    llvm::Value* slotPtr = builder.CreateInBoundsGEP(i64Ty, proof.base, builder.getInt64(index));
    auto* raw = builder.CreateAlignedLoad(i64Ty, slotPtr, llvm::Align(8), tag + "raw");
    tagArrayElementsAccess(raw, ctx);
    // A hole reads as `undefined`, and that is the only answer a raw load
    // cannot give for itself. The ladder's other answers — absent, an accessor,
    // a hit up the prototype chain — cannot arise here: the length test put the
    // index inside the dense part, and a dense element is a Value or a hole.
    llvm::Value* rawTag = builder.CreateLShr(raw, BRONZE_ABI_VALUE_TAG_SHIFT);
    llvm::Value* isHole = builder.CreateICmpEQ(rawTag, builder.getInt64(BRONZE_ABI_TAG_HOLE));
    llvm::Value* value =
        builder.CreateSelect(isHole, builder.getInt64(BRONZE_ABI_UNDEFINED_BITS), raw, tag + "val");
    llvm::BasicBlock* fastExit = builder.GetInsertBlock();
    builder.CreateBr(doneBb);

    builder.SetInsertPoint(ladderBb);
    return ProvenRead{fastExit, value};
}

llvm::Value* phiAtJoin(llvm::BasicBlock* doneBb, llvm::BasicBlock* fastBb, llvm::Value* live,
                       llvm::Value* dead, const std::string& name) {
    llvm::IRBuilder<> phiBuilder(doneBb, doneBb->getFirstNonPHIIt());
    llvm::PHINode* phi = phiBuilder.CreatePHI(live->getType(), 2, name);
    for (llvm::BasicBlock* pred : llvm::predecessors(doneBb)) {
        phi->addIncoming(pred == fastBb ? live : dead, pred);
    }
    return phi;
}

void rejoinReceiverProof(llvm::IRBuilder<>& builder, ReceiverProof& proof,
                         llvm::BasicBlock* fastBb, llvm::BasicBlock* doneBb) {
    if (!proof.live()) return;
    if (fastBb == nullptr) {
        proof = ReceiverProof{};
        return;
    }
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);
    const std::string tag = "recv" + std::to_string(proof.run) + ".";

    proof.ok = phiAtJoin(doneBb, fastBb, proof.ok, llvm::ConstantInt::getFalse(ctx),
                         tag + "ok.live");
    proof.base =
        phiAtJoin(doneBb, fastBb, proof.base, llvm::PoisonValue::get(ptrTy), tag + "base.live");
    (void)builder;
}

}  // namespace bronze::codegen_llvm
