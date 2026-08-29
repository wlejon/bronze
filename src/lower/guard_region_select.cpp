#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>

#include "lower/guard_region.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

// The floor, on the arithmetic a LOOP region has to hold to be worth
// duplicating.
//
// ONE, and not the two the design study proposed. `sum = sum + o.k` in a
// counted loop is a single boxed add — the loop counter's `i + 1` is already an
// f64 and is not one — and that loop is `bench/proto_dispatch.js`, which is
// exactly a case this pass exists for: what the promotion removes there is the
// accumulator's GC root slot and the store-and-reload of it around the
// property read, per iteration. A floor of two would have refused it. What
// keeps the floor meaningful is the RATIO below, not the count.
constexpr uint32_t kMinArith = 1;
// The floor for an ENTRY region, which is a different bargain. A loop region
// duplicates a body whose arithmetic runs once per ITERATION and whose guards
// run once per entry, so one operation is enough to pay for the copy. A
// straight-line region runs both exactly once, so what it buys is bounded by
// the ratio alone — and it duplicates the WHOLE FUNCTION rather than a loop
// body. Two says: a copy has to answer at least two coercions.
constexpr uint32_t kMinEntryUses = 2;
// AN ENTRY REGION HAS NO SPLIT BUDGET, and that is a measured decision rather
// than an omission.
//
// Each split is a join in the slow copy that takes a parameter for every value
// the region defines and is live there, plus a trampoline that fills them in.
// On the kernel this pass is for that costs nothing: `multiplyMatrices` reads
// thirty-two elements up front, so ONE point covers all thirty-two and the
// trampoline carries what the reads produced and nothing else. A function
// shaped `te[0] *= s; te[4] *= s; ...` is the opposite — each read is consumed
// immediately, so coalescing has nothing to coalesce and there is one point per
// read. `Matrix4.multiplyScalar` is sixteen points: fifty instructions become
// two hundred and fifty-five and five GC root slots become fifty-three.
//
// Bounding that was tried (2026-08-28) two ways, a flat cap of two points and a
// ratio of four coercions per point. Both refuse `multiplyScalar` and both also
// refuse `Vector3.applyMatrix4`, `Vector3.project` and `Matrix4.extractRotation`,
// which have the same shape and are hot: over twelve interleaved rounds of
// `bench/three_math.js` the bounded builds measured 26.28 ms against 26.34 for
// no pass at all, while the unbounded one measured 25.18. The whole benefit on
// that benchmark lives in the functions a split budget refuses. So the size
// bound is the REGION CAP alone (`kMaxRegionInsts` / `kMaxRegionBlocks`), and
// what it costs — +41% GC root slots and +894 blocks on the three.js graph — is
// reported rather than traded away. The named follow-up is `planFrame`: the
// fast and slow copies are mutually exclusive, so their pinned values could
// share slots, which is where most of that 41% is.
// The caps. A region past either of these is one whose duplication is a
// compile-time cost proportional to a body nobody proved is a loop kernel. For
// an entry region they are the only size bound there is — the per-function
// growth budget below cannot apply to a whole-function copy, which exceeds it
// by construction.
constexpr size_t kMaxRegionBlocks = 64;
// 700 and not 400 since the frame overlay landed (codegen-llvm/llvm_frame.h).
// What the old cap was paying for was the DUPLICATED GC ROOT SLOTS: the fast
// and slow copies each got a full set of pinned slots although only one of them
// can run, so a big region cost twice its roots. Overlaying the two copies took
// that back — the three.js graph went from 11,448 root slots to 9,830, and
// `Matrix4.multiplyMatrices` from a 71-slot frame to 38 — and what a bigger
// region now costs is instructions, which is what the per-function growth
// budget below already bounds for a loop region.
//
// The number is `Matrix4.invert` (677 instructions, 292 checked unboxes): the
// sampler's #1 on `bench/three_math.js` at 22%, refused by the old cap, and
// worth 292 of the 1214 folded coercions on the whole graph on its own. Raising
// the cap to reach it moved that benchmark from 24.6 ms to 22.0 (best of six
// interleaved rounds) and cost no measurable compile time on the graph (four
// interleaved rounds: 33.8 s mean at 700 against 33.8 s at 400).
//
// The next function up is `Quaternion.setFromEuler` at 406, which this cap
// admits and which the placement rules now reach: 150 of the graph's folded
// coercions are its. Above it there is nothing but module top level, which runs
// once.
constexpr size_t kMaxRegionInsts = 700;
// The per-function growth budget is "no more than the function's own size", and
// this is the floor under it. Without one the budget refuses exactly the case
// the pass exists for: in `sumProps` the loop IS the function, so a copy of it
// plus two guards and three trampolines is more instructions than the whole
// function had. Doubling something this small is not a code-size problem, and
// the cap that matters — a region of 400 instructions — is the one above.
constexpr size_t kGrowthFloor = 64;
// A function is rewritten at most this many times. Each successful rewrite
// makes its own region unselectable — the fast copy's arithmetic is `f64` and
// the slow copy has gained edges into blocks that are not its header — so this
// is a belt on a loop that already terminates.
constexpr int kMaxRegionsPerFunction = 8;

bool isBinaryArithOp(il::Op op) {
    return op == il::Op::Add || op == il::Op::Sub || op == il::Op::Mul || op == il::Op::Div ||
           op == il::Op::Mod || op == il::Op::Pow;
}

// The type of every value in the function, by id.
std::vector<il::Type> valueTypes(const il::Function& fn) {
    std::vector<il::Type> types(fn.valueCount, il::Type::Void);
    for (size_t p = 0; p < fn.params.size() && p < types.size(); ++p) {
        types[p] = fn.params[p].type;
    }
    for (const auto& block : fn.blocks) {
        for (const auto& param : block.params) {
            if (param.id < types.size()) types[param.id] = param.type;
        }
        for (const auto& inst : block.instructions) {
            if (inst.result != il::kNoValue && inst.result < types.size()) {
                types[inst.result] = inst.type;
            }
        }
    }
    return types;
}

}  // namespace

// PROMOTABLE ARITHMETIC: boxed result, boxed operands. `Pow` is here because
// its promoted form is `bronze_pow` over two doubles rather than the dynamic
// helper that has to run 13.15.3 first, and `Neg` because it is the one UNARY
// member of the family — one operand, and `fneg` on the true edge. The
// relational operators are still absent: their promotion is a different
// rewrite, `rel.lt` becoming `cmp.lt` rather than an `f64` version of itself.
bool isPromotableArith(const il::Instruction& inst, const std::vector<il::Type>& types) {
    const bool unary = inst.op == il::Op::Neg;
    if (!unary && !isBinaryArithOp(inst.op)) return false;
    if (inst.type != il::Type::Dynamic) return false;
    if (inst.operands.size() != (unary ? 1u : 2u)) return false;
    for (il::ValueId id : inst.operands) {
        if (id >= types.size() || types[id] != il::Type::Dynamic) return false;
    }
    return inst.result != il::kNoValue;
}

namespace {

// Union-find over value ids: the candidate closure is a connected-components
// problem, and a refusal drops a whole component because the reason to refuse
// (a string, a BigInt) is a fact about what the values in it can hold.
struct DisjointSet {
    std::vector<il::ValueId> parent;
    explicit DisjointSet(uint32_t n) : parent(n) {
        std::iota(parent.begin(), parent.end(), 0u);
    }
    il::ValueId find(il::ValueId a) {
        while (parent[a] != a) {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    }
    void unite(il::ValueId a, il::ValueId b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[b] = a;
    }
};

// A `pin.guard %v, number` standing immediately in front of a position has
// already made the test this pass would make, with one instruction of reach —
// the same reach `storeValueRepr` reads a pin over (llvm_repr.cpp).
bool pinProvesNumberAt(const il::Block& block, uint32_t index, il::ValueId value) {
    if (index == 0) return false;
    const auto& prev = block.instructions[index - 1];
    return prev.op == il::Op::PinGuard && !prev.operands.empty() && prev.operands[0] == value &&
           prev.immI32 == static_cast<int32_t>(il::PinBarrier::Number);
}

// Does `from` dominate every region block reachable from it? The question an
// entry region's split placement turns on: `renameAt` (guard_region_build.cpp)
// finds a split's renames by walking a block's DOMINATOR chain, so a block that
// reads the split block's orphaned definitions without being dominated by it
// would read a name the pruning removed.
bool dominatesReachable(const Cfg& cfg, const RegionPlan& plan, il::BlockId from) {
    std::vector<uint8_t> seen(plan.inRegion.size(), 0);
    std::vector<il::BlockId> stack{from};
    seen[from] = 1;
    while (!stack.empty()) {
        const il::BlockId b = stack.back();
        stack.pop_back();
        for (il::BlockId s : cfg.succs[b]) {
            if (s >= seen.size() || seen[s] || !plan.inRegion[s]) continue;
            if (!cfg.dominates(from, s)) return false;
            seen[s] = 1;
            stack.push_back(s);
        }
    }
    return true;
}

// A candidate waiting to be assigned to a coalesced guard point in its own
// block. `definedAt` is the first instruction index at which the value EXISTS
// — one past its def — so "already defined at point p" is `definedAt <= p`.
struct Pending {
    il::ValueId value = il::kNoValue;
    uint32_t definedAt = 0;
    uint32_t firstUse = 0;
};

// ---------------------------------------------------------------------------
// The analysis both region kinds share. `plan` arrives with `header`, `blocks`,
// `inRegion` and `entryRegion` filled in; everything else is decided here.
// False means refused, with the reason counted.
// ---------------------------------------------------------------------------
bool analyzeRegion(const il::Function& fn, const std::vector<std::string>& keys, const Cfg& cfg,
                   const std::vector<il::Type>& types, const std::vector<DefSite>& defs,
                   RegionPlan& plan, GuardRegionStats& stats) {
    // 1. A handler inside the region, or a region block that IS a handler.
    // Refused both ways: a handler takes no parameters, so there is nowhere for
    // a trampoline's values to arrive, and an edge into the middle of the
    // region from a `throw` is an entry the duplication has no copy for.
    bool handlerSeen = false;
    for (il::BlockId b : plan.blocks) {
        if (fn.blocks[b].handler != il::kNoBlock) handlerSeen = true;
    }
    for (const auto& block : fn.blocks) {
        if (block.handler != il::kNoBlock && plan.inRegion[block.handler]) handlerSeen = true;
    }
    if (handlerSeen) {
        ++stats.refusedHandler;
        return false;
    }

    // 2. SINGLE ENTRY, and exactly one outside predecessor of the header. Two
    // of them is the `for` whose initialiser is itself conditional, and it is
    // REFUSED rather than mis-duplicated: with two entry edges there are two
    // places the promoted values would have to be converted, and one of them is
    // not on a path the pass can reason about without a synthesised preheader.
    // An entry region always passes this — its preheader IS the synthesis.
    bool singleEntry = true;
    il::BlockId entryPred = il::kNoBlock;
    uint32_t outsidePreds = 0;
    for (il::BlockId b : plan.blocks) {
        for (il::BlockId p : cfg.preds[b]) {
            if (plan.inRegion[p]) continue;
            if (b != plan.header) {
                singleEntry = false;
            } else {
                ++outsidePreds;
                entryPred = p;
            }
        }
    }
    if (!singleEntry || outsidePreds != 1) {
        ++stats.refusedSingleEntry;
        return false;
    }
    plan.entryPred = entryPred;

    // 3. The caps.
    size_t regionInsts = 0;
    for (il::BlockId b : plan.blocks) regionInsts += fn.blocks[b].instructions.size();
    if (plan.blocks.size() > kMaxRegionBlocks || regionInsts > kMaxRegionInsts) {
        ++stats.refusedGrowth;
        return false;
    }

    // 4. The candidate closure. Two kinds of seed: a value that IS boxed
    // arithmetic, and a value a checked coercion reads.
    DisjointSet ds(fn.valueCount);
    std::vector<uint8_t> seed(fn.valueCount, 0);
    for (il::BlockId b : plan.blocks) {
        for (const auto& inst : fn.blocks[b].instructions) {
            if (isPromotableArith(inst, types)) {
                seed[inst.result] = 1;
                for (il::ValueId id : inst.operands) {
                    seed[id] = 1;
                    ds.unite(inst.result, id);
                }
            } else if (isCheckedUnboxOf(inst, types)) {
                // Not united with the result: an `f64` is not a candidate for
                // anything, so the operand is a component of its own unless
                // arithmetic joins it to something.
                seed[inst.operands[0]] = 1;
            }
        }
    }
    // Closed BOTH WAYS over the block-parameter/argument pairs of every edge
    // into a region block, the entry edge included. That is what makes a
    // loop-carried accumulator one value rather than two, and what lets the
    // entry edge hand over a double.
    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            for (const il::BlockTarget* target : {&inst.target, &inst.elseTarget}) {
                if (target->block == il::kNoBlock || target->block >= fn.blocks.size()) {
                    continue;
                }
                if (!plan.inRegion[target->block]) continue;
                const auto& params = fn.blocks[target->block].params;
                for (size_t i = 0; i < target->args.size() && i < params.size(); ++i) {
                    if (params[i].type != il::Type::Dynamic) continue;
                    const il::ValueId arg = target->args[i];
                    if (arg >= types.size() || types[arg] != il::Type::Dynamic) continue;
                    ds.unite(params[i].id, arg);
                }
            }
        }
    }

    // Which components hold a seed, and which of those hold something that is
    // not a number by construction. A `box.str`, a `to.string` and a BigInt
    // literal are each a STATIC proof that the guard would fail, so the whole
    // component is dropped before a block is copied.
    std::vector<uint8_t> componentSeeded(fn.valueCount, 0);
    std::vector<uint8_t> componentPoisoned(fn.valueCount, 0);
    for (il::ValueId v = 0; v < fn.valueCount; ++v) {
        if (seed[v]) componentSeeded[ds.find(v)] = 1;
    }
    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            if (inst.result == il::kNoValue || inst.result >= fn.valueCount) continue;
            const bool poison = (inst.op == il::Op::Box && inst.boxType == il::Type::Str) ||
                                inst.op == il::Op::ToStr || inst.op == il::Op::ConstBigInt;
            if (poison) componentPoisoned[ds.find(inst.result)] = 1;
        }
    }

    plan.isCandidate.assign(fn.valueCount, 0);
    for (il::ValueId v = 0; v < fn.valueCount; ++v) {
        if (types[v] != il::Type::Dynamic) continue;
        const il::ValueId root = ds.find(v);
        if (!componentSeeded[root] || componentPoisoned[root]) continue;
        plan.isCandidate[v] = 1;
    }

    // 5. Everything a guard would license, counted after the poison pass
    // because that is the number the profitability floor is about — and
    // collected as USE SITES, because the promoted uses are also where the
    // guards have to have taken effect by.
    std::vector<std::vector<std::pair<il::BlockId, uint32_t>>> promotedUses(fn.valueCount);
    uint32_t useCount = 0;
    for (il::BlockId b : plan.blocks) {
        const auto& block = fn.blocks[b];
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const auto& inst = block.instructions[i];
            const auto here = std::pair<il::BlockId, uint32_t>{b, static_cast<uint32_t>(i)};
            if (isPromotableArith(inst, types) && plan.isCandidate[inst.result]) {
                ++useCount;
                for (il::ValueId id : inst.operands) promotedUses[id].push_back(here);
            } else if (isCheckedUnboxOf(inst, types) && plan.isCandidate[inst.operands[0]]) {
                ++useCount;
                promotedUses[inst.operands[0]].push_back(here);
            }
            for (const il::BlockTarget* target : {&inst.target, &inst.elseTarget}) {
                if (target->block == il::kNoBlock || target->block >= fn.blocks.size()) continue;
                if (!plan.inRegion[target->block]) continue;
                const auto& params = fn.blocks[target->block].params;
                for (size_t k = 0; k < target->args.size() && k < params.size(); ++k) {
                    if (!plan.isCandidate[params[k].id]) continue;
                    promotedUses[target->args[k]].push_back(here);
                }
            }
        }
    }
    if (useCount == 0) {
        ++stats.refusedNonNumeric;
        return false;
    }
    if (useCount < (plan.entryRegion ? kMinEntryUses : kMinArith)) {
        ++stats.refusedTooFew;
        return false;
    }
    plan.promotedUseCount = useCount;

    // 6. Classify every candidate, and collect the ones that need a test.
    plan.splitsOf.assign(fn.blocks.size(), {});
    std::vector<std::vector<Pending>> pendingOf(fn.blocks.size());
    for (il::ValueId v = 0; v < fn.valueCount; ++v) {
        if (!plan.isCandidate[v]) continue;
        Candidate cand;
        cand.value = v;
        const DefSite& def = defs[v];
        const bool insideRegion = def.block != il::kNoBlock && plan.inRegion[def.block];
        const il::Instruction* defInst = (def.block != il::kNoBlock && def.index != UINT32_MAX)
                                             ? &fn.blocks[def.block].instructions[def.index]
                                             : nullptr;

        if (defInst != nullptr && defInst->op == il::Op::Box &&
            defInst->boxType == il::Type::F64 && !defInst->operands.empty()) {
            cand.kind = CandidateKind::BoxElide;
            cand.boxSource = defInst->operands[0];
            plan.candidates.push_back(cand);
            continue;
        }
        if (insideRegion &&
            (def.index == UINT32_MAX || (defInst != nullptr && isPromotableArith(*defInst, types)))) {
            cand.kind = CandidateKind::Promoted;
            plan.candidates.push_back(cand);
            continue;
        }

        if (!insideRegion) {
            // Defined outside, so it dominates both copies: one test on the
            // entry edge covers every iteration. For an entry region "outside"
            // is a function parameter and the entry edge is the preheader.
            cand.kind = CandidateKind::EntryGuard;
            cand.guardBlock = plan.entryPred;
            cand.guardIndex =
                static_cast<uint32_t>(fn.blocks[plan.entryPred].instructions.size()) - 1;
            if (pinProvesNumberAt(fn.blocks[plan.entryPred], cand.guardIndex, v)) {
                cand.kind = CandidateKind::PinElided;
            }
            plan.candidates.push_back(cand);
            continue;
        }

        // WHERE the guard may go. It has to reach every promoted use, so the
        // only sites that serve a use in another block are sites in a block
        // that DOMINATES that use — and the definition's own block is the only
        // block on offer, since a guard before the definition has nothing to
        // test. A use the definition's block does not dominate therefore has
        // no site at all, and the region is refused.
        uint32_t first = UINT32_MAX;
        bool dominatesUses = true;
        bool anyUse = false;
        for (const auto& [useBlock, useIndex] : promotedUses[v]) {
            anyUse = true;
            if (useBlock == def.block) {
                first = std::min(first, useIndex);
            } else if (!cfg.dominates(def.block, useBlock)) {
                dominatesUses = false;
            }
        }
        // A candidate is seeded by arithmetic or by a coercion and both of
        // those are uses, so a candidate with no promoted use is not a shape
        // the closure can produce. It is refused rather than placed anywhere,
        // because "the latest point that reaches every use" is not defined over
        // an empty set.
        if (!dominatesUses || !anyUse) {
            ++stats.refusedPlacement;
            return false;
        }
        if (first == UINT32_MAX) {
            // Every promoted use is in a block this one dominates: the guard
            // goes at the END of the definition's block, in front of the
            // terminator. `Quaternion.setFromEuler` is this case and nothing
            // else — six sines and cosines computed in the header, every
            // product of them in a `switch` arm.
            //
            // That point reaches all of those uses because the fast copy of one
            // block is a CHAIN of parts whose only exits are the guard branches
            // themselves, and a failed guard leaves for the slow copy and never
            // comes back. So the last part of this block's chain dominates the
            // fast copy of every block this block dominates, which is where the
            // uses are.
            //
            // Always at or after the point the value exists at: a terminator is
            // `ret`/`jump`/`br`/`throw` (il effects.cpp) and none of those
            // carries a result, so a definition is never the last instruction.
            first = static_cast<uint32_t>(fn.blocks[def.block].instructions.size()) - 1;
        }
        // The PIN question is asked at the candidate's own first use and not at
        // whatever coalesced point it lands on: a barrier reaches one
        // instruction, and a point another candidate chose is not that one.
        if (pinProvesNumberAt(fn.blocks[def.block], first, v)) {
            cand.kind = CandidateKind::PinElided;
            cand.guardBlock = def.block;
            cand.guardIndex = first;
            plan.candidates.push_back(cand);
            continue;
        }
        pendingOf[def.block].push_back(Pending{v, def.index + 1, first});
    }

    // 7. COALESCING. Within a block the guard point is the first promoted use
    // of ANY unassigned candidate, and every candidate already defined by then
    // is guarded there — one chain of tests, one trampoline. See the header for
    // why guarding a value earlier than its own first use is free and why
    // guarding each at its own use is not.
    for (il::BlockId b : plan.blocks) {
        auto& pending = pendingOf[b];
        std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& c) {
            return a.firstUse != c.firstUse ? a.firstUse < c.firstUse : a.value < c.value;
        });
        size_t done = 0;
        std::vector<uint8_t> taken(pending.size(), 0);
        while (done < pending.size()) {
            uint32_t point = UINT32_MAX;
            for (size_t i = 0; i < pending.size(); ++i) {
                if (!taken[i]) {
                    point = pending[i].firstUse;
                    break;
                }
            }
            plan.splitsOf[b].push_back(point);
            for (size_t i = 0; i < pending.size(); ++i) {
                if (taken[i] || pending[i].definedAt > point) continue;
                taken[i] = 1;
                ++done;
                Candidate cand;
                cand.value = pending[i].value;
                cand.kind = CandidateKind::UseGuard;
                cand.guardBlock = b;
                cand.guardIndex = point;
                plan.candidates.push_back(cand);
                ++plan.guardCount;
            }
        }
    }
    // An entry region's slow copy has NO ENTRY of its own — the fast copy is
    // the function's — so the only way into it is a trampoline, and the only
    // block a trampoline lands in is the tail of a split. Everything the slow
    // copy defines before a split is therefore orphaned and pruned, and what
    // re-supplies it is the tail's PARAMETERS plus a rename in the blocks that
    // read them.
    //
    // Those two halves have to meet, and only the second is a condition. The
    // first is free: a value defined before a split and read after it is live
    // at the split by definition, and the tail takes a parameter for exactly
    // the values `liveBefore` reports there. So is the case of a value defined
    // in a block the prefix orphans WHOLE — a block that cannot reach the split
    // but whose definition is read past it dominates the split block, so that
    // value is live at the split too.
    //
    // The second is this check. `renameAt` finds a split's renames by walking a
    // block's DOMINATOR chain, so the split block must dominate every region
    // block reachable from it — otherwise a surviving reader keeps the name of
    // a definition the pruning removed, and the rewrite is discarded by
    // `dominanceHolds` at best.
    //
    // The header satisfies that by the region's own definition, which is why it
    // used to be the whole rule. It is not the only block that can: the join
    // after a DEFAULTED PARAMETER dominates everything below it too, and that
    // is the shape of `Quaternion.setFromEuler` — `(euler, update = true)`, so
    // its six sines and cosines are computed one block under the header and
    // every product of them is a `switch` arm below that.
    //
    // A loop region is not exposed to any of this: its slow copy keeps its own
    // back edge, so its header stays reachable and nothing is orphaned.
    if (plan.entryRegion) {
        for (il::BlockId b : plan.blocks) {
            if (b == plan.header || plan.splitsOf[b].empty()) continue;
            if (!dominatesReachable(cfg, plan, b)) {
                ++stats.refusedEntrySplit;
                return false;
            }
        }
    }

    // The rewrite reads candidates in value order for the raw-unbox names and
    // in guard-point order for the chains; sorting once here is what keeps both
    // deterministic after the coalescing pass appended out of order.
    std::sort(plan.candidates.begin(), plan.candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.value < b.value; });

    // The RATIO is over the guards inside the region only. An entry guard is
    // paid once per region ENTRY and an in-region one once per iteration, so
    // counting the first against per-iteration work compares two different
    // things — and it is what would refuse the inner loop of every nest, whose
    // accumulator arrives boxed from the outer one.
    const uint32_t inRegionGuards = plan.guardCount;
    for (const Candidate& cand : plan.candidates) {
        if (cand.kind == CandidateKind::EntryGuard) ++plan.guardCount;
    }
    // A region with nothing to test proves nothing: every candidate was already
    // an f64 behind a box, and the values are unrooted today. Guards
    // outnumbering the work they license is the other side of the same floor —
    // a test per operation is what the backend already emits inline, so
    // hoisting it has bought nothing.
    if (plan.guardCount == 0 || inRegionGuards > plan.promotedUseCount) {
        ++stats.refusedTooFew;
        return false;
    }

    for (auto& splits : plan.splitsOf) {
        std::sort(splits.begin(), splits.end());
        splits.erase(std::unique(splits.begin(), splits.end()), splits.end());
    }

    // READ RUNS, last: it rewrites the points coalescing settled and the
    // candidates that sit at them, so it wants both finished and sorted. The
    // candidate list is re-sorted afterwards because nothing here reorders it —
    // only `guardIndex` changes — but the guard chains are read in point order
    // and a point that moved has to find its members again.
    plan.runGuardsOf.assign(fn.blocks.size(), {});
    for (il::BlockId b : plan.blocks) {
        mergeReadRunGuards(fn, keys, types, b, plan.candidates, plan);
    }
    return true;
}

// The aggregate counters say how often a reason fired; they cannot say WHERE,
// and "where" is what decides whether a reason is worth a rule — fifty refusals
// spread over cold constructors are not the same fact as five over the
// functions a benchmark spends its time in. This is the per-function view, off
// unless asked for, and deterministic because it walks the module's functions
// in order and prints one line each.
bool guardRegionTraceOn() {
    static const bool on = [] {
        const char* env = std::getenv("BRONZE_GUARDED_REGION_TRACE");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return on;
}

// The counters that moved while one function was examined, as `reason=n` pairs.
// Empty when nothing moved, which is the common case and the one worth not
// printing.
std::string traceDelta(const GuardRegionStats& before, const GuardRegionStats& after) {
    const std::pair<const char*, std::pair<uint32_t, uint32_t>> fields[] = {
        {"dup", {before.duplicated, after.duplicated}},
        {"guards", {before.guards, after.guards}},
        {"points", {before.guardPoints, after.guardPoints}},
        {"runGuards", {before.runGuards, after.runGuards}},
        {"unboxFolded", {before.unboxFolded, after.unboxFolded}},
        {"handler", {before.refusedHandler, after.refusedHandler}},
        {"singleEntry", {before.refusedSingleEntry, after.refusedSingleEntry}},
        {"nonNumeric", {before.refusedNonNumeric, after.refusedNonNumeric}},
        {"tooFew", {before.refusedTooFew, after.refusedTooFew}},
        {"growth", {before.refusedGrowth, after.refusedGrowth}},
        {"placement", {before.refusedPlacement, after.refusedPlacement}},
        {"entrySplit", {before.refusedEntrySplit, after.refusedEntrySplit}},
        {"ssa", {before.refusedSsa, after.refusedSsa}},
        {"machine", {before.refusedMachine, after.refusedMachine}},
        {"copyPred", {before.refusedCopyPred, after.refusedCopyPred}},
    };
    std::string out;
    for (const auto& [label, counts] : fields) {
        if (counts.second == counts.first) continue;
        if (!out.empty()) out += ' ';
        out += label;
        out += '=';
        out += std::to_string(counts.second - counts.first);
    }
    return out;
}

}  // namespace

bool isCheckedUnboxOf(const il::Instruction& inst, const std::vector<il::Type>& types) {
    if (inst.op != il::Op::Unbox || inst.type != il::Type::F64) return false;
    if (inst.rawUnbox || inst.nullishUnbox) return false;
    if (inst.operands.size() != 1 || inst.result == il::kNoValue) return false;
    const il::ValueId src = inst.operands[0];
    return src < types.size() && types[src] == il::Type::Dynamic;
}

bool guardedRegionsDisabled() {
    static const bool off = [] {
        const char* env = std::getenv("BRONZE_NO_GUARDED_REGIONS");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return off;
}

void guardRegionStatsReport(const GuardRegionStats& s) {
    const char* env = std::getenv("BRONZE_GUARDED_REGION_STATS");
    if (env == nullptr || std::strcmp(env, "1") != 0) return;
    const uint32_t refused = s.refusedHandler + s.refusedSingleEntry + s.refusedNonNumeric +
                             s.refusedTooFew + s.refusedGrowth + s.refusedPlacement +
                             s.refusedEntrySplit + s.refusedSsa + s.refusedMachine +
                             s.refusedCopyPred;
    std::fprintf(stderr,
                 "[guard] fns=%u regions=%u entry=%u dup=%u guards=%u points=%u runGuards=%u "
                 "elidedBox=%u "
                 "elidedPin=%u unboxFolded=%u promoted=%u blocks=+%u pruned=%u "
                 "refused=%u(handler %u, singleEntry %u, nonNumeric %u, tooFew %u, growth %u, "
                 "placement %u, entrySplit %u, ssa %u, machine %u, copyPred %u)\n",
                 s.functions, s.regions, s.entryRegions, s.duplicated, s.guards, s.guardPoints,
                 s.runGuards, s.elidedBox, s.elidedPin, s.unboxFolded, s.promoted, s.blocksAdded,
                 s.blocksPruned, refused, s.refusedHandler, s.refusedSingleEntry,
                 s.refusedNonNumeric, s.refusedTooFew, s.refusedGrowth, s.refusedPlacement,
                 s.refusedEntrySplit, s.refusedSsa, s.refusedMachine, s.refusedCopyPred);
}

// ---------------------------------------------------------------------------

std::vector<RegionPlan> selectRegions(const il::Function& fn, const std::vector<std::string>& keys,
                                      GuardRegionStats& stats,
                                      const std::vector<uint8_t>& alreadyBuilt) {
    std::vector<RegionPlan> plans;
    if (fn.blocks.empty()) return plans;

    const Cfg cfg = buildCfg(fn);
    const std::vector<il::Type> types = valueTypes(fn);
    const std::vector<DefSite> defs = computeDefSites(fn);

    // 1. Natural loops, one per header (a header reached by two back edges owns
    // the union of both, which is what makes `continue` inside a loop one
    // region rather than two).
    std::vector<std::vector<il::BlockId>> loopBlocks;
    std::vector<il::BlockId> loopHeaders;
    for (size_t t = 0; t < fn.blocks.size(); ++t) {
        const auto tail = static_cast<il::BlockId>(t);
        if (cfg.rpoIndex[tail] == UINT32_MAX) continue;
        for (il::BlockId h : cfg.succs[tail]) {
            if (!cfg.dominates(h, tail)) continue;
            std::vector<il::BlockId> body = naturalLoop(cfg, h, tail);
            auto it = std::find(loopHeaders.begin(), loopHeaders.end(), h);
            if (it == loopHeaders.end()) {
                loopHeaders.push_back(h);
                loopBlocks.push_back(std::move(body));
            } else {
                auto& existing = loopBlocks[static_cast<size_t>(it - loopHeaders.begin())];
                existing.insert(existing.end(), body.begin(), body.end());
                std::sort(existing.begin(), existing.end());
                existing.erase(std::unique(existing.begin(), existing.end()), existing.end());
            }
        }
    }

    // 2. INNERMOST ONLY. A region properly containing another is refused here
    // rather than nested: one duplication per loop nest is the growth budget,
    // and the inner loop is where the iterations are.
    std::vector<bool> innermost(loopHeaders.size(), true);
    for (size_t a = 0; a < loopHeaders.size(); ++a) {
        for (size_t b = 0; b < loopHeaders.size(); ++b) {
            if (a == b) continue;
            const bool contains = std::includes(loopBlocks[a].begin(), loopBlocks[a].end(),
                                                loopBlocks[b].begin(), loopBlocks[b].end());
            if (contains && loopBlocks[b].size() < loopBlocks[a].size()) innermost[a] = false;
        }
    }

    for (size_t li = 0; li < loopHeaders.size(); ++li) {
        if (!innermost[li]) continue;
        RegionPlan plan;
        plan.header = loopHeaders[li];
        plan.blocks = loopBlocks[li];
        plan.inRegion.assign(fn.blocks.size(), false);
        for (il::BlockId b : plan.blocks) plan.inRegion[b] = true;

        // A region this pass already built, or built out of. Skipped BEFORE it
        // is counted: it is not an opportunity the pass declined, it is the
        // pass's own output looking like one.
        bool overlapsBuilt = false;
        for (il::BlockId b : plan.blocks) {
            if (b < alreadyBuilt.size() && alreadyBuilt[b]) overlapsBuilt = true;
        }
        if (overlapsBuilt) continue;
        ++stats.regions;

        if (!analyzeRegion(fn, keys, cfg, types, defs, plan, stats)) continue;
        // The region's single outside predecessor is where the entry chain is
        // spliced, and the chain RE-PASSES that edge's arguments — on the fast
        // edge as doubles, on a failed guard as the boxes they were. A
        // predecessor that is itself one copy of an earlier region would make
        // those re-passes read that copy's values from a block belonging to
        // this region's fast copy, which is the one thing the copy-class
        // invariant forbids (il.h `CopyClass`). It is reachable — a loop whose
        // exit edge lands directly on the next loop's header — and it is cheap
        // to decline.
        if (plan.entryPred < fn.blocks.size() &&
            fn.blocks[plan.entryPred].copyClass != il::CopyClass::Shared) {
            ++stats.refusedCopyPred;
            continue;
        }
        plans.push_back(std::move(plan));
    }

    return plans;
}

// ---------------------------------------------------------------------------
// The entry region.
// ---------------------------------------------------------------------------

il::Function withPreheader(const il::Function& fn) {
    il::Function out = fn;
    for (auto& block : out.blocks) {
        block.id += 1;
        if (block.handler != il::kNoBlock) block.handler += 1;
        for (auto& inst : block.instructions) {
            for (il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                if (t->block != il::kNoBlock) t->block += 1;
            }
        }
    }
    il::Block pre;
    pre.id = 0;
    il::Instruction jump;
    jump.op = il::Op::Jump;
    jump.type = il::Type::Void;
    jump.target.block = 1;
    pre.instructions.push_back(std::move(jump));
    out.blocks.insert(out.blocks.begin(), std::move(pre));
    return out;
}

bool selectEntryRegion(const il::Function& prepped, const std::vector<std::string>& keys,
                       GuardRegionStats& stats, RegionPlan& plan) {
    if (prepped.blocks.size() < 2) return false;
    // Refused by NAME and not by shape: a resume machine's entry dispatches on
    // an index held in the frame, so every one of its blocks is reachable from
    // `b0` and the region definition is satisfied. What is wrong with copying
    // it is that its live values cross suspensions in the frame rather than in
    // SSA, so the promotion has nothing to carry.
    if (prepped.isResumeBody) {
        ++stats.refusedMachine;
        return false;
    }

    const Cfg cfg = buildCfg(prepped);
    // A function with a loop is a loop function. Not "a loop region was built":
    // a loop that was refused was refused for a reason, and a whole-function
    // copy would contain that same loop and duplicate the shape the refusal was
    // about. Silent, because a loop function is not an entry-region opportunity
    // the pass declined — it is a different question entirely.
    for (size_t t = 0; t < prepped.blocks.size(); ++t) {
        const auto tail = static_cast<il::BlockId>(t);
        if (cfg.rpoIndex[tail] == UINT32_MAX) continue;
        for (il::BlockId h : cfg.succs[tail]) {
            if (cfg.dominates(h, tail)) return false;
        }
    }

    plan = RegionPlan{};
    plan.entryRegion = true;
    plan.header = 1;
    plan.inRegion.assign(prepped.blocks.size(), true);
    plan.inRegion[0] = false;
    for (size_t b = 1; b < prepped.blocks.size(); ++b) {
        plan.blocks.push_back(static_cast<il::BlockId>(b));
    }
    ++stats.entryRegions;

    const std::vector<il::Type> types = valueTypes(prepped);
    const std::vector<DefSite> defs = computeDefSites(prepped);
    return analyzeRegion(prepped, keys, cfg, types, defs, plan, stats);
}

namespace {

// One function's whole examination: its loop regions until the growth budget or
// the attempt limit stops it, and the entry region when none of them was taken.
// A function of its own so that the module loop above it can bracket it — the
// per-function trace is the difference between the counters before and after
// this call, which a body full of `continue` could not report.
bool examineFunction(il::Function& fn, const std::vector<std::string>& keys,
                     GuardRegionStats& stats) {
    size_t originalInsts = 0;
    for (const auto& block : fn.blocks) originalInsts += block.instructions.size();
    size_t added = 0;
    std::vector<uint8_t> alreadyBuilt(fn.blocks.size(), 0);
    bool changed = false;

    for (int attempt = 0; attempt < kMaxRegionsPerFunction; ++attempt) {
        // Counted on the FIRST look at a function only. Every later look
        // re-derives the same answers for the regions it did not take, and
        // counting those again would report one loop as several.
        GuardRegionStats discard;
        std::vector<RegionPlan> plans =
            selectRegions(fn, keys, attempt == 0 ? stats : discard, alreadyBuilt);
        if (plans.empty()) break;

        const RegionPlan& plan = plans.front();
        size_t regionInsts = 0;
        for (il::BlockId b : plan.blocks) regionInsts += fn.blocks[b].instructions.size();
        // The per-FUNCTION budget: what the duplication adds may not exceed
        // what was there, so no function can be more than doubled however
        // many loops it has — with the small-function floor above under it.
        const size_t estimate = regionInsts + 4 * plan.guardCount + 2 * plan.blocks.size();
        if (added + estimate > std::max(originalInsts, kGrowthFloor)) {
            if (attempt == 0) ++stats.refusedGrowth;
            break;
        }

        const size_t blocksBefore = fn.blocks.size();
        if (!buildGuardedRegion(fn, plan, stats, alreadyBuilt)) break;
        added += estimate;
        if (fn.blocks.size() > blocksBefore) {
            stats.blocksAdded += static_cast<uint32_t>(fn.blocks.size() - blocksBefore);
        }
        changed = true;
    }

    if (changed) return true;
    // The entry region, on a copy with a preheader in front of it. There is no
    // growth budget to meet here and there cannot be: a whole-function copy
    // exceeds "adds no more than the function had" by construction. The region
    // caps in `analyzeRegion` are the size bound instead.
    il::Function prepped = withPreheader(fn);
    RegionPlan plan;
    if (!selectEntryRegion(prepped, keys, stats, plan)) return false;
    std::vector<uint8_t> entryBuilt(prepped.blocks.size(), 0);
    const size_t blocksBefore = prepped.blocks.size();
    if (!buildGuardedRegion(prepped, plan, stats, entryBuilt)) return false;
    if (prepped.blocks.size() > blocksBefore) {
        stats.blocksAdded += static_cast<uint32_t>(prepped.blocks.size() - blocksBefore);
    }
    fn = std::move(prepped);
    return true;
}

}  // namespace

bool applyGuardedRegions(il::Module& module, GuardRegionStats* statsOut) {
    GuardRegionStats local;
    GuardRegionStats& stats = statsOut != nullptr ? *statsOut : local;
    if (guardedRegionsDisabled()) return false;
    // A `--census` build is an INSTRUMENT: what it measures is where lowering
    // ran out of static answers, and a pass that changes which values reach an
    // operator changes the readings. It is off there for the same reason a
    // census run is counts and never times.
    if (!module.censusOutPath.empty()) return false;

    bool changed = false;
    for (il::Function& fn : module.functions) {
        ++stats.functions;
        const GuardRegionStats before = stats;
        if (examineFunction(fn, module.keyConstants, stats)) changed = true;
        if (!guardRegionTraceOn()) continue;
        const std::string delta = traceDelta(before, stats);
        if (!delta.empty()) {
            std::fprintf(stderr, "[guard-fn] %s %s\n", fn.name.c_str(), delta.c_str());
        }
    }
    return changed;
}

}  // namespace bronze::lower
