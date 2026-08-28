#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>

#include "lower/guard_region.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

// The floor, on the arithmetic a region has to hold to be worth duplicating.
//
// ONE, and not the two the design study proposed. `sum = sum + o.k` in a
// counted loop is a single boxed add — the loop counter's `i + 1` is already an
// f64 and is not one — and that loop is `bench/proto_dispatch.js`, which is
// exactly a case this pass exists for: what the promotion removes there is the
// accumulator's GC root slot and the store-and-reload of it around the
// property read, per iteration. A floor of two would have refused it. What
// keeps the floor meaningful is the RATIO below, not the count.
constexpr uint32_t kMinArith = 1;
// The caps. A region past either of these is one whose duplication is a
// compile-time cost proportional to a body nobody proved is a loop kernel.
constexpr size_t kMaxRegionBlocks = 64;
constexpr size_t kMaxRegionInsts = 400;
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

bool isArithOp(il::Op op) {
    return op == il::Op::Add || op == il::Op::Sub || op == il::Op::Mul || op == il::Op::Div ||
           op == il::Op::Mod;
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

// PROMOTABLE ARITHMETIC: one of the five operators, boxed result, boxed
// operands. `Neg` and `Pow` are deliberately absent (a later chunk), and so are
// the relational operators, whose promotion is a different rewrite — `rel.lt`
// becomes `cmp.lt`, not an `f64` version of itself.
bool isPromotableArith(const il::Instruction& inst, const std::vector<il::Type>& types) {
    if (!isArithOp(inst.op) || inst.type != il::Type::Dynamic) return false;
    if (inst.operands.size() != 2) return false;
    for (il::ValueId id : inst.operands) {
        if (id >= types.size() || types[id] != il::Type::Dynamic) return false;
    }
    return inst.result != il::kNoValue;
}

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

}  // namespace

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
                             s.refusedTooFew + s.refusedGrowth + s.refusedPlacement + s.refusedSsa;
    std::fprintf(stderr,
                 "[guard] fns=%u regions=%u dup=%u guards=%u elidedBox=%u elidedPin=%u "
                 "promoted=%u blocks=+%u refused=%u(handler %u, singleEntry %u, nonNumeric %u, "
                 "tooFew %u, growth %u, placement %u, ssa %u)\n",
                 s.functions, s.regions, s.duplicated, s.guards, s.elidedBox, s.elidedPin,
                 s.promoted, s.blocksAdded, refused, s.refusedHandler, s.refusedSingleEntry,
                 s.refusedNonNumeric, s.refusedTooFew, s.refusedGrowth, s.refusedPlacement,
                 s.refusedSsa);
}

// ---------------------------------------------------------------------------

std::vector<RegionPlan> selectRegions(const il::Function& fn, GuardRegionStats& stats,
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

        // 3. A handler inside the region, or a region block that IS a handler.
        // Chunk 1 refuses both: a handler takes no parameters, so there is
        // nowhere for a trampoline's values to arrive, and an edge into the
        // middle of the region from a `throw` is an entry the duplication has
        // no copy for.
        bool handlerSeen = false;
        for (il::BlockId b : plan.blocks) {
            if (fn.blocks[b].handler != il::kNoBlock) handlerSeen = true;
        }
        for (const auto& block : fn.blocks) {
            if (block.handler != il::kNoBlock && plan.inRegion[block.handler]) handlerSeen = true;
        }
        if (handlerSeen) {
            ++stats.refusedHandler;
            continue;
        }

        // 4. SINGLE ENTRY, and exactly one outside predecessor of the header.
        // Two of them is the `for` whose initialiser is itself conditional, and
        // it is REFUSED rather than mis-duplicated: with two entry edges there
        // are two places the promoted values would have to be converted, and
        // one of them is not on a path the pass can reason about without a
        // synthesised preheader.
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
            continue;
        }
        plan.entryPred = entryPred;

        // 5. The caps.
        size_t regionInsts = 0;
        for (il::BlockId b : plan.blocks) regionInsts += fn.blocks[b].instructions.size();
        if (plan.blocks.size() > kMaxRegionBlocks || regionInsts > kMaxRegionInsts) {
            ++stats.refusedGrowth;
            continue;
        }

        // 6. The candidate closure.
        DisjointSet ds(fn.valueCount);
        std::vector<uint8_t> seed(fn.valueCount, 0);
        uint32_t arithCount = 0;
        for (il::BlockId b : plan.blocks) {
            for (const auto& inst : fn.blocks[b].instructions) {
                if (!isPromotableArith(inst, types)) continue;
                ++arithCount;
                seed[inst.result] = 1;
                for (il::ValueId id : inst.operands) {
                    seed[id] = 1;
                    ds.unite(inst.result, id);
                }
            }
        }
        // Closed BOTH WAYS over the block-parameter/argument pairs of every
        // edge into a region block, the entry edge included. That is what makes
        // a loop-carried accumulator one value rather than two, and what lets
        // the entry edge hand over a double.
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

        // Which components hold arithmetic, and which of those hold something
        // that is not a number by construction. A `box.str`, a `to.string` and
        // a BigInt literal are each a STATIC proof that the guard would fail,
        // so the whole component is dropped before a block is copied.
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
        // Arithmetic whose component survived. Counted after the poison pass,
        // because that is the number the profitability floor is about.
        uint32_t liveArith = 0;
        for (il::BlockId b : plan.blocks) {
            for (const auto& inst : fn.blocks[b].instructions) {
                if (isPromotableArith(inst, types) && plan.isCandidate[inst.result]) ++liveArith;
            }
        }
        if (liveArith == 0) {
            ++stats.refusedNonNumeric;
            continue;
        }
        if (liveArith < kMinArith) {
            ++stats.refusedTooFew;
            continue;
        }
        plan.promotedArithCount = liveArith;

        // 7. The promoted uses of each candidate: where a guard would have to
        // have taken effect by.
        std::vector<std::vector<std::pair<il::BlockId, uint32_t>>> promotedUses(fn.valueCount);
        for (il::BlockId b : plan.blocks) {
            const auto& block = fn.blocks[b];
            for (size_t i = 0; i < block.instructions.size(); ++i) {
                const auto& inst = block.instructions[i];
                if (isPromotableArith(inst, types) && plan.isCandidate[inst.result]) {
                    for (il::ValueId id : inst.operands) {
                        promotedUses[id].push_back({b, static_cast<uint32_t>(i)});
                    }
                }
                for (const il::BlockTarget* target : {&inst.target, &inst.elseTarget}) {
                    if (target->block == il::kNoBlock || target->block >= fn.blocks.size()) {
                        continue;
                    }
                    if (!plan.inRegion[target->block]) continue;
                    const auto& params = fn.blocks[target->block].params;
                    for (size_t k = 0; k < target->args.size() && k < params.size(); ++k) {
                        if (!plan.isCandidate[params[k].id]) continue;
                        promotedUses[target->args[k]].push_back({b, static_cast<uint32_t>(i)});
                    }
                }
            }
        }

        // 8. Classify every candidate, and place its guard.
        plan.splitsOf.assign(fn.blocks.size(), {});
        bool placementRefused = false;
        for (il::ValueId v = 0; v < fn.valueCount && !placementRefused; ++v) {
            if (!plan.isCandidate[v]) continue;
            Candidate cand;
            cand.value = v;
            const DefSite& def = defs[v];
            const bool insideRegion = def.block != il::kNoBlock && plan.inRegion[def.block];
            const il::Instruction* defInst =
                (def.block != il::kNoBlock && def.index != UINT32_MAX)
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
                (def.index == UINT32_MAX ||
                 (defInst != nullptr && isPromotableArith(*defInst, types)))) {
                cand.kind = CandidateKind::Promoted;
                plan.candidates.push_back(cand);
                continue;
            }

            // Everything left needs a test. Where it goes is the only question,
            // and §2.7's answer is: at the FIRST PROMOTED USE and never at the
            // def. A guard right after the def would cut a block after every
            // read, and `Matrix4.multiplyMatrices` is thirty-two adjacent reads
            // feeding arithmetic — the receiver proof (llvm_recv_proof.h) stops
            // at a block end, so splitting there would trade the whole
            // 146 ns -> 15 ns proof for the promotion.
            if (!insideRegion) {
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

            uint32_t first = UINT32_MAX;
            bool reachable = true;
            for (const auto& [useBlock, useIndex] : promotedUses[v]) {
                if (useBlock == def.block) {
                    first = std::min(first, useIndex);
                } else if (!cfg.dominates(def.block, useBlock)) {
                    reachable = false;
                }
            }
            if (first == UINT32_MAX || !reachable) {
                // Chunk 1 places a guard only in the block that defines the
                // value: a guard that has to dominate uses in several blocks
                // needs a placement search this chunk does not have.
                placementRefused = true;
                break;
            }
            cand.guardBlock = def.block;
            cand.guardIndex = first;
            cand.kind = pinProvesNumberAt(fn.blocks[def.block], first, v) ? CandidateKind::PinElided
                                                                         : CandidateKind::UseGuard;
            if (cand.kind == CandidateKind::UseGuard) {
                plan.splitsOf[def.block].push_back(first);
                ++plan.guardCount;
            }
            plan.candidates.push_back(cand);
        }
        if (placementRefused) {
            ++stats.refusedPlacement;
            continue;
        }

        // The RATIO is over the guards inside the region only. An entry guard
        // is paid once per region ENTRY and an in-region one once per
        // iteration, so counting the first against per-iteration arithmetic
        // compares two different things — and it is what would refuse the inner
        // loop of every nest, whose accumulator arrives boxed from the outer
        // one.
        const uint32_t inRegionGuards = plan.guardCount;
        for (const Candidate& cand : plan.candidates) {
            if (cand.kind == CandidateKind::EntryGuard) ++plan.guardCount;
        }
        // A region with nothing to test proves nothing: every candidate was
        // already an f64 behind a box, and the values are unrooted today.
        // Guards outnumbering the arithmetic they license is the other side of
        // the same floor — a test per operation is what the backend already
        // emits inline, so hoisting it has bought nothing.
        if (plan.guardCount == 0 || inRegionGuards > plan.promotedArithCount) {
            ++stats.refusedTooFew;
            continue;
        }

        for (auto& splits : plan.splitsOf) {
            std::sort(splits.begin(), splits.end());
            splits.erase(std::unique(splits.begin(), splits.end()), splits.end());
        }
        plans.push_back(std::move(plan));
    }

    return plans;
}

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
        size_t originalInsts = 0;
        for (const auto& block : fn.blocks) originalInsts += block.instructions.size();
        size_t added = 0;
        std::vector<uint8_t> alreadyBuilt(fn.blocks.size(), 0);

        for (int attempt = 0; attempt < kMaxRegionsPerFunction; ++attempt) {
            // Counted on the FIRST look at a function only. Every later look
            // re-derives the same answers for the regions it did not take, and
            // counting those again would report one loop as several.
            GuardRegionStats discard;
            std::vector<RegionPlan> plans =
                selectRegions(fn, attempt == 0 ? stats : discard, alreadyBuilt);
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
            stats.blocksAdded += static_cast<uint32_t>(fn.blocks.size() - blocksBefore);
            changed = true;
        }
    }
    return changed;
}

}  // namespace bronze::lower
