#include <array>
#include <algorithm>
#include <utility>

#include "lower/guard_region.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

using Rename = std::vector<std::pair<il::ValueId, il::ValueId>>;

il::ValueId renamed(const Rename& map, il::ValueId v) {
    for (const auto& [from, to] : map) {
        if (from == v) return to;
    }
    return v;
}

// Every value an instruction READS, in place, so a rewrite touches operands and
// block arguments through one function and cannot forget the second kind.
template <typename Fn>
void rewriteUses(il::Instruction& inst, Fn&& map) {
    for (il::ValueId& id : inst.operands) id = map(id);
    for (il::ValueId& id : inst.target.args) id = map(id);
    for (il::ValueId& id : inst.elseTarget.args) id = map(id);
}

il::Instruction makeIsNumber(il::ValueId result, il::ValueId operand) {
    il::Instruction inst;
    inst.op = il::Op::IsNumber;
    inst.type = il::Type::Bool;
    inst.result = result;
    inst.operands = {operand};
    return inst;
}

il::Instruction makeRawUnbox(il::ValueId result, il::ValueId operand) {
    il::Instruction inst;
    inst.op = il::Op::Unbox;
    inst.type = il::Type::F64;
    inst.result = result;
    inst.operands = {operand};
    inst.rawUnbox = true;
    return inst;
}

il::Instruction makeBoxF64(il::ValueId result, il::ValueId operand) {
    il::Instruction inst;
    inst.op = il::Op::Box;
    inst.type = il::Type::Dynamic;
    inst.boxType = il::Type::F64;
    inst.result = result;
    inst.operands = {operand};
    return inst;
}

il::Instruction makeJump(il::BlockId target, std::vector<il::ValueId> args = {}) {
    il::Instruction inst;
    inst.op = il::Op::Jump;
    inst.type = il::Type::Void;
    inst.target.block = target;
    inst.target.args = std::move(args);
    return inst;
}

il::Instruction makeBranch(il::ValueId cond, il::BlockId thenBlock, il::BlockId elseBlock,
                           std::vector<il::ValueId> elseArgs = {}) {
    il::Instruction inst;
    inst.op = il::Op::Branch;
    inst.type = il::Type::Void;
    inst.operands = {cond};
    inst.target.block = thenBlock;
    inst.elseTarget.block = elseBlock;
    inst.elseTarget.args = std::move(elseArgs);
    return inst;
}

// The rewrite is only correct if every use of a value is reached by that value's
// definition, and the block-parameter form of SSA makes that a DOMINANCE
// question the IL verifier does not ask (it checks types, arity and range).
// This is the check that makes a shape chunk 1 cannot repair a missed
// optimisation rather than a miscompile: the whole rewritten function is
// discarded when it fails.
//
// Two conditions, not one. Dominance is what LLVM requires of the IR; the index
// ordering is what the EMITTER requires — `FunctionEmitter::emit` walks blocks
// in index order and a use whose def has not been emitted reads a null
// `values_` entry (llvm_func.cpp).
bool dominanceHolds(const il::Function& fn) {
    const Cfg cfg = buildCfg(fn);
    const std::vector<DefSite> defs = computeDefSites(fn);
    auto ok = [&](il::ValueId v, il::BlockId useBlock, size_t useIndex) {
        if (v >= defs.size()) return false;
        const DefSite& def = defs[v];
        if (def.block == il::kNoBlock) return true;  // a function parameter
        if (def.block == useBlock) return def.index == UINT32_MAX || def.index < useIndex;
        return def.block < useBlock && cfg.dominates(def.block, useBlock);
    };
    for (const auto& block : fn.blocks) {
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const auto& inst = block.instructions[i];
            for (il::ValueId v : inst.operands) {
                if (!ok(v, block.id, i)) return false;
            }
            for (const il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                for (il::ValueId v : t->args) {
                    if (!ok(v, block.id, i)) return false;
                }
            }
        }
    }
    return true;
}

// A `box.f64` whose result nothing reads. The entry edge is where these appear:
// a candidate whose def is a box promotes to the box's OPERAND, so redirecting
// the edge to the fast copy hands over the double and leaves the box with no
// reader at all. Removing it is what makes the entry edge of a counted loop
// exactly `jump bFast(%double, %i)`.
void dropDeadF64Boxes(il::Function& fn) {
    std::vector<uint8_t> used(fn.valueCount, 0);
    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            for (il::ValueId v : inst.operands) {
                if (v < used.size()) used[v] = 1;
            }
            for (const il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                for (il::ValueId v : t->args) {
                    if (v < used.size()) used[v] = 1;
                }
            }
        }
    }
    for (auto& block : fn.blocks) {
        auto& insts = block.instructions;
        insts.erase(std::remove_if(insts.begin(), insts.end(),
                                   [&](const il::Instruction& inst) {
                                       return inst.op == il::Op::Box &&
                                              inst.boxType == il::Type::F64 &&
                                              inst.result != il::kNoValue &&
                                              inst.result < used.size() && !used[inst.result];
                                   }),
                    insts.end());
    }
}

}  // namespace

bool buildGuardedRegion(il::Function& fn, const RegionPlan& plan, GuardRegionStats& stats,
                        std::vector<uint8_t>& alreadyBuilt) {
    const il::Function& src = fn;
    const Cfg cfg = buildCfg(src);
    const std::vector<DefSite> defs = computeDefSites(src);
    const std::vector<std::vector<il::ValueId>> liveIn = computeLiveIn(src, cfg);
    const size_t blockCount = src.blocks.size();

    // Types, so a fresh definition can be given the type the value it stands in
    // for had.
    std::vector<il::Type> types(src.valueCount, il::Type::Void);
    for (size_t p = 0; p < src.params.size() && p < types.size(); ++p) {
        types[p] = src.params[p].type;
    }
    for (const auto& block : src.blocks) {
        for (const auto& param : block.params) {
            if (param.id < types.size()) types[param.id] = param.type;
        }
        for (const auto& inst : block.instructions) {
            if (inst.result != il::kNoValue && inst.result < types.size()) {
                types[inst.result] = inst.type;
            }
        }
    }

    // The candidates, by value, for O(1) questions during emission.
    std::vector<const Candidate*> candidateOf(src.valueCount, nullptr);
    for (const Candidate& c : plan.candidates) candidateOf[c.value] = &c;
    auto definedInRegion = [&](il::ValueId v) {
        return v < defs.size() && defs[v].block != il::kNoBlock && plan.inRegion[defs[v].block];
    };

    il::ValueId nextValue = src.valueCount;
    auto newValue = [&] { return nextValue++; };
    // Counted locally: a rewrite the dominance check discards must leave the
    // statistics saying nothing happened, because nothing did.
    uint32_t emittedGuards = 0;

    // ---- value identity in the copies ------------------------------------
    // Every value the region defines gets a second name for the fast copy, and
    // every value that needs a test gets a name for the double the test
    // licenses. Assigned before anything is emitted, so a forward reference
    // inside the fast copy is just a lookup.
    std::vector<il::ValueId> fastOf(src.valueCount, il::kNoValue);
    for (il::BlockId b : plan.blocks) {
        for (const auto& param : src.blocks[b].params) fastOf[param.id] = newValue();
        for (const auto& inst : src.blocks[b].instructions) {
            if (inst.result != il::kNoValue) fastOf[inst.result] = newValue();
        }
    }
    std::vector<il::ValueId> rawOf(src.valueCount, il::kNoValue);
    for (const Candidate& c : plan.candidates) {
        if (c.kind == CandidateKind::UseGuard || c.kind == CandidateKind::PinElided ||
            c.kind == CandidateKind::EntryGuard) {
            rawOf[c.value] = newValue();
        }
    }

    // ---- the slow copy's split points ------------------------------------
    // The tail of a split block is a JOIN — the head falls into it and every
    // trampoline jumps to it — so it takes a parameter for each value the
    // region defines that is live there. A value defined OUTSIDE the region
    // needs none: it dominates both copies.
    struct SlowSplit {
        uint32_t index = 0;
        std::vector<il::ValueId> params;    // the original values, ascending
        std::vector<il::ValueId> paramIds;  // their names in the tail
    };
    std::vector<std::vector<SlowSplit>> slowSplits(blockCount);
    for (il::BlockId b : plan.blocks) {
        for (uint32_t index : plan.splitsOf[b]) {
            SlowSplit split;
            split.index = index;
            for (il::ValueId v : liveBefore(src, cfg, liveIn, b, index)) {
                if (!definedInRegion(v)) continue;
                split.params.push_back(v);
                split.paramIds.push_back(newValue());
            }
            slowSplits[b].push_back(std::move(split));
        }
    }

    // The names a split block leaves behind: uses in everything it dominates
    // have to read the LAST tail's parameter, because a trampoline can land in
    // any tail and the head's definition then never ran.
    std::vector<Rename> finalRename(blockCount);
    for (il::BlockId b : plan.blocks) {
        for (const SlowSplit& split : slowSplits[b]) {
            for (size_t i = 0; i < split.params.size(); ++i) {
                bool replaced = false;
                for (auto& entry : finalRename[b]) {
                    if (entry.first == split.params[i]) {
                        entry.second = split.paramIds[i];
                        replaced = true;
                    }
                }
                if (!replaced) finalRename[b].push_back({split.params[i], split.paramIds[i]});
            }
        }
    }
    auto renameAt = [&](il::BlockId block) {
        Rename out;
        for (il::BlockId cur = cfg.idom[block]; cur != il::kNoBlock; cur = cfg.idom[cur]) {
            if (finalRename[cur].empty()) continue;
            for (const auto& [from, to] : finalRename[cur]) {
                bool seen = false;
                for (const auto& entry : out) seen = seen || entry.first == from;
                if (!seen) out.push_back({from, to});
            }
        }
        return out;
    };

    // ---- the guards, grouped by the point they split at -------------------
    struct GuardPoint {
        uint32_t index = 0;
        std::vector<il::ValueId> guarded;  // ascending, one `is.number` each
    };
    std::vector<std::vector<GuardPoint>> guardPoints(blockCount);
    std::vector<std::vector<std::pair<uint32_t, il::ValueId>>> inlineUnbox(blockCount);
    for (il::BlockId b : plan.blocks) {
        for (uint32_t index : plan.splitsOf[b]) guardPoints[b].push_back(GuardPoint{index, {}});
    }
    for (const Candidate& c : plan.candidates) {
        if (c.kind == CandidateKind::UseGuard) {
            for (GuardPoint& point : guardPoints[c.guardBlock]) {
                if (point.index == c.guardIndex) point.guarded.push_back(c.value);
            }
        } else if (c.kind == CandidateKind::PinElided && c.guardBlock != plan.entryPred) {
            inlineUnbox[c.guardBlock].push_back({c.guardIndex, c.value});
        }
    }
    for (auto& points : inlineUnbox) std::sort(points.begin(), points.end());

    // ---- the block layout -------------------------------------------------
    // Ids are handed out in EMISSION ORDER, because the backend walks blocks in
    // index order and a use whose definition has not been emitted reads a null
    // entry. So: the original blocks with their tails immediately after them,
    // then the entry chain (which the fast copy reads), then the fast copy, then
    // the trampolines (which read the fast copy).
    il::BlockId nextBlock = 0;
    std::vector<il::BlockId> slowHeadId(blockCount, il::kNoBlock);
    std::vector<std::vector<il::BlockId>> slowTailId(blockCount);
    for (size_t b = 0; b < blockCount; ++b) {
        slowHeadId[b] = nextBlock++;
        slowTailId[b].resize(slowSplits[b].size());
        for (size_t j = 0; j < slowSplits[b].size(); ++j) slowTailId[b][j] = nextBlock++;
    }

    std::vector<il::ValueId> entryGuarded;
    std::vector<il::ValueId> entryUnboxed;
    for (const Candidate& c : plan.candidates) {
        if (c.kind == CandidateKind::EntryGuard) entryGuarded.push_back(c.value);
        if ((c.kind == CandidateKind::EntryGuard) ||
            (c.kind == CandidateKind::PinElided && c.guardBlock == plan.entryPred)) {
            entryUnboxed.push_back(c.value);
        }
    }
    std::sort(entryGuarded.begin(), entryGuarded.end());
    std::sort(entryUnboxed.begin(), entryUnboxed.end());
    const bool needEntryChain = !entryUnboxed.empty();
    std::vector<il::BlockId> entryTestId(entryGuarded.size(), il::kNoBlock);
    il::BlockId entryConvId = il::kNoBlock;
    if (needEntryChain) {
        for (size_t i = 0; i < entryGuarded.size(); ++i) entryTestId[i] = nextBlock++;
        entryConvId = nextBlock++;
    }

    // The fast copy: one block per part of each region block, plus a block per
    // extra test at a shared split point.
    std::vector<std::vector<il::BlockId>> fastPartId(blockCount);
    std::vector<std::vector<std::vector<il::BlockId>>> fastChainId(blockCount);
    for (il::BlockId b : plan.blocks) {
        const size_t parts = guardPoints[b].size() + 1;
        fastPartId[b].resize(parts);
        fastChainId[b].resize(guardPoints[b].size());
        fastPartId[b][0] = nextBlock++;
        for (size_t j = 0; j < guardPoints[b].size(); ++j) {
            const size_t extra = guardPoints[b][j].guarded.size();
            fastChainId[b][j].resize(extra == 0 ? 0 : extra - 1);
            for (size_t k = 0; k + 1 < extra; ++k) fastChainId[b][j][k] = nextBlock++;
            fastPartId[b][j + 1] = nextBlock++;
        }
    }

    std::vector<std::vector<il::BlockId>> trampolineId(blockCount);
    for (il::BlockId b : plan.blocks) {
        trampolineId[b].resize(guardPoints[b].size());
        for (size_t j = 0; j < guardPoints[b].size(); ++j) trampolineId[b][j] = nextBlock++;
    }
    // One exit trampoline per fast edge LEAVING the region that would otherwise
    // have to box on the hot path. `then` and `else` are counted separately: a
    // `br` can leave the region on both.
    std::vector<std::array<il::BlockId, 2>> exitTrampolineId(
        blockCount, {il::kNoBlock, il::kNoBlock});
    auto needsBoxing = [&](il::ValueId v) {
        return candidateOf[v] != nullptr && candidateOf[v]->kind == CandidateKind::Promoted;
    };
    auto exitNeedsTrampoline = [&](const il::BlockTarget& target) {
        if (target.block == il::kNoBlock || target.block >= blockCount) return false;
        if (plan.inRegion[target.block]) return false;
        for (il::ValueId v : target.args) {
            if (needsBoxing(v)) return true;
        }
        return false;
    };
    for (il::BlockId b : plan.blocks) {
        const auto& term = src.blocks[b].instructions.back();
        if (exitNeedsTrampoline(term.target)) exitTrampolineId[b][0] = nextBlock++;
        if (exitNeedsTrampoline(term.elseTarget)) exitTrampolineId[b][1] = nextBlock++;
    }

    std::vector<il::Block> out(nextBlock);
    for (il::BlockId b = 0; b < nextBlock; ++b) out[b].id = b;

    // ---- the slow copy ----------------------------------------------------
    for (size_t b = 0; b < blockCount; ++b) {
        const il::Block& source = src.blocks[b];
        il::Block& head = out[slowHeadId[b]];
        head.params = source.params;
        head.handler =
            source.handler == il::kNoBlock ? il::kNoBlock : slowHeadId[source.handler];

        Rename names = renameAt(static_cast<il::BlockId>(b));
        const auto& splits = slowSplits[b];
        auto emitRange = [&](il::Block& into, size_t from, size_t to) {
            for (size_t i = from; i < to; ++i) {
                il::Instruction inst = source.instructions[i];
                rewriteUses(inst, [&](il::ValueId v) { return renamed(names, v); });
                for (il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                    if (t->block != il::kNoBlock && t->block < blockCount) {
                        t->block = slowHeadId[t->block];
                    }
                }
                into.instructions.push_back(std::move(inst));
            }
        };

        size_t cursor = 0;
        il::Block* current = &head;
        for (size_t j = 0; j < splits.size(); ++j) {
            emitRange(*current, cursor, splits[j].index);
            std::vector<il::ValueId> args;
            for (il::ValueId v : splits[j].params) args.push_back(renamed(names, v));
            current->instructions.push_back(makeJump(slowTailId[b][j], std::move(args)));
            il::Block& tail = out[slowTailId[b][j]];
            for (size_t i = 0; i < splits[j].params.size(); ++i) {
                tail.params.push_back(
                    il::BlockParam{splits[j].paramIds[i], types[splits[j].params[i]]});
            }
            for (size_t i = 0; i < splits[j].params.size(); ++i) {
                bool replaced = false;
                for (auto& entry : names) {
                    if (entry.first == splits[j].params[i]) {
                        entry.second = splits[j].paramIds[i];
                        replaced = true;
                    }
                }
                if (!replaced) names.push_back({splits[j].params[i], splits[j].paramIds[i]});
            }
            cursor = splits[j].index;
            current = &tail;
        }
        emitRange(*current, cursor, source.instructions.size());
    }

    // The entry edge is the one edge in the whole slow copy that changes: it
    // now enters the fast copy, and the guards it may fail are what send it
    // back to the header it used to jump to.
    {
        il::Block& pred = out[slowHeadId[plan.entryPred]];
        il::Instruction& term = pred.instructions.back();
        const il::BlockId fastHeader = fastPartId[plan.header][0];
        std::vector<il::ValueId> headerArgs;
        for (il::BlockTarget* t : {&term.target, &term.elseTarget}) {
            if (t->block != slowHeadId[plan.header]) continue;
            headerArgs = t->args;
            if (needEntryChain) {
                t->block = entryGuarded.empty() ? entryConvId : entryTestId[0];
                t->args.clear();
            } else {
                t->block = fastHeader;
                for (size_t i = 0; i < t->args.size(); ++i) {
                    const il::ValueId arg = t->args[i];
                    if (candidateOf[arg] == nullptr) continue;
                    // The only promotable form an entry argument can have with
                    // no chain in front of it: a box, whose operand is the
                    // double the fast header wants.
                    t->args[i] = candidateOf[arg]->boxSource;
                }
            }
        }

        if (needEntryChain) {
            for (size_t i = 0; i < entryGuarded.size(); ++i) {
                il::Block& test = out[entryTestId[i]];
                const il::ValueId cond = newValue();
                test.instructions.push_back(makeIsNumber(cond, entryGuarded[i]));
                const il::BlockId nextOk =
                    i + 1 < entryGuarded.size() ? entryTestId[i + 1] : entryConvId;
                test.instructions.push_back(
                    makeBranch(cond, nextOk, slowHeadId[plan.header], headerArgs));
                ++emittedGuards;
            }
            il::Block& conv = out[entryConvId];
            for (il::ValueId v : entryUnboxed) {
                conv.instructions.push_back(makeRawUnbox(rawOf[v], v));
            }
            std::vector<il::ValueId> args;
            for (size_t i = 0; i < headerArgs.size(); ++i) {
                const il::ValueId arg = headerArgs[i];
                const Candidate* c = candidateOf[arg];
                if (c == nullptr) {
                    args.push_back(arg);
                } else if (c->kind == CandidateKind::BoxElide) {
                    args.push_back(c->boxSource);
                } else {
                    args.push_back(rawOf[arg]);
                }
            }
            conv.instructions.push_back(makeJump(fastHeader, std::move(args)));
        }
    }

    // ---- the fast copy ----------------------------------------------------
    // The f64 form of a candidate, wherever the fast copy is standing. It never
    // emits anything: every one of these was either already a double or was
    // unboxed at the guard that licensed it.
    auto fastF64 = [&](il::ValueId v) -> il::ValueId {
        const Candidate* c = candidateOf[v];
        if (c == nullptr) return il::kNoValue;
        switch (c->kind) {
            case CandidateKind::BoxElide:
                return definedInRegion(c->boxSource) ? fastOf[c->boxSource] : c->boxSource;
            case CandidateKind::Promoted:
                return fastOf[v];
            case CandidateKind::UseGuard:
            case CandidateKind::PinElided:
            case CandidateKind::EntryGuard:
                return rawOf[v];
        }
        return il::kNoValue;
    };

    // The BOXED form, which for a promoted value has to be made. `box.f64` of a
    // value that was raw-unboxed out of a box is the identity (the backend's
    // peephole returns the bits it started from), and of an `fadd` result it is
    // bit-identical to what the dynamic add's own fast arm would have produced
    // — so a value leaving through a trampoline is the value the original IL
    // would have had.
    auto fastDyn = [&](il::ValueId v, il::Block& into, std::vector<std::pair<il::ValueId,
                                                                             il::ValueId>>* cache) {
        const Candidate* c = candidateOf[v];
        if (c == nullptr) return definedInRegion(v) ? fastOf[v] : v;
        if (c->kind != CandidateKind::Promoted) {
            return definedInRegion(v) ? fastOf[v] : v;
        }
        if (cache != nullptr) {
            for (const auto& [from, to] : *cache) {
                if (from == v) return to;
            }
        }
        const il::ValueId boxed = newValue();
        into.instructions.push_back(makeBoxF64(boxed, fastF64(v)));
        if (cache != nullptr) cache->push_back({v, boxed});
        return boxed;
    };

    for (il::BlockId b : plan.blocks) {
        const il::Block& source = src.blocks[b];
        // Cleared per region block: a fast block does not dominate its siblings,
        // so a box made in one is not reusable in another.
        std::vector<std::pair<il::ValueId, il::ValueId>> boxCache;

        il::Block& first = out[fastPartId[b][0]];
        for (const auto& param : source.params) {
            const bool promote = candidateOf[param.id] != nullptr;
            first.params.push_back(
                il::BlockParam{fastOf[param.id], promote ? il::Type::F64 : param.type});
        }

        size_t cursor = 0;
        for (size_t j = 0; j <= guardPoints[b].size(); ++j) {
            il::Block& part = out[fastPartId[b][j]];
            // The doubles the guard just before this part licensed.
            if (j > 0) {
                for (il::ValueId v : guardPoints[b][j - 1].guarded) {
                    part.instructions.push_back(makeRawUnbox(rawOf[v], fastOf[v]));
                }
            }
            const size_t limit = j < guardPoints[b].size() ? guardPoints[b][j].index
                                                           : source.instructions.size();
            for (size_t i = cursor; i < limit; ++i) {
                for (const auto& [index, value] : inlineUnbox[b]) {
                    if (index == i) {
                        part.instructions.push_back(makeRawUnbox(rawOf[value], fastOf[value]));
                    }
                }
                il::Instruction inst = source.instructions[i];
                // A candidate whose def is an INSTRUCTION and whose kind is
                // `Promoted` is promotable arithmetic and nothing else: a box
                // is `BoxElide` and everything else in the region needs a
                // guard, so the classification is the whole test.
                const bool promotedArith =
                    inst.result != il::kNoValue && candidateOf[inst.result] != nullptr &&
                    candidateOf[inst.result]->kind == CandidateKind::Promoted;
                if (inst.result != il::kNoValue) inst.result = fastOf[inst.result];

                if (promotedArith) {
                    inst.type = il::Type::F64;
                    for (il::ValueId& id : inst.operands) id = fastF64(id);
                } else {
                    for (il::ValueId& id : inst.operands) id = fastDyn(id, part, &boxCache);
                }

                for (int side = 0; side < 2; ++side) {
                    il::BlockTarget& t = side == 0 ? inst.target : inst.elseTarget;
                    if (t.block == il::kNoBlock || t.block >= blockCount) continue;
                    const il::BlockId target = t.block;
                    if (plan.inRegion[target]) {
                        const auto& params = src.blocks[target].params;
                        for (size_t k = 0; k < t.args.size() && k < params.size(); ++k) {
                            t.args[k] = candidateOf[params[k].id] != nullptr
                                            ? fastF64(t.args[k])
                                            : fastDyn(t.args[k], part, &boxCache);
                        }
                        t.block = fastPartId[target][0];
                    } else if (exitTrampolineId[b][side] != il::kNoBlock) {
                        t.block = exitTrampolineId[b][side];
                        t.args.clear();
                    } else {
                        for (il::ValueId& id : t.args) id = fastDyn(id, part, &boxCache);
                        t.block = slowHeadId[target];
                    }
                }
                part.instructions.push_back(std::move(inst));
            }
            cursor = limit;

            if (j == guardPoints[b].size()) break;
            // The guard itself: one test per value, chained, every failing edge
            // landing on the one trampoline for this point. Nothing has been
            // computed between the tests, so they all leave with the same
            // values.
            const auto& guarded = guardPoints[b][j].guarded;
            il::Block* testBlock = &part;
            for (size_t k = 0; k < guarded.size(); ++k) {
                const il::ValueId cond = newValue();
                testBlock->instructions.push_back(makeIsNumber(cond, fastOf[guarded[k]]));
                const il::BlockId nextOk = k + 1 < guarded.size() ? fastChainId[b][j][k]
                                                                  : fastPartId[b][j + 1];
                testBlock->instructions.push_back(
                    makeBranch(cond, nextOk, trampolineId[b][j]));
                ++emittedGuards;
                if (k + 1 < guarded.size()) testBlock = &out[fastChainId[b][j][k]];
            }

            // The trampoline. Its arguments are the tail's parameters, in the
            // tail's order, each as the boxed value the slow copy expects.
            il::Block& tramp = out[trampolineId[b][j]];
            std::vector<il::ValueId> args;
            for (il::ValueId v : slowSplits[b][j].params) {
                args.push_back(fastDyn(v, tramp, nullptr));
            }
            tramp.instructions.push_back(makeJump(slowTailId[b][j], std::move(args)));
        }

        // The exit trampolines, which is where a promoted value goes back in a
        // box: on the loop's own exit edge, taken once.
        const auto& term = source.instructions.back();
        for (int side = 0; side < 2; ++side) {
            if (exitTrampolineId[b][side] == il::kNoBlock) continue;
            const il::BlockTarget& t = side == 0 ? term.target : term.elseTarget;
            il::Block& tramp = out[exitTrampolineId[b][side]];
            std::vector<il::ValueId> args;
            for (il::ValueId v : t.args) args.push_back(fastDyn(v, tramp, nullptr));
            tramp.instructions.push_back(makeJump(slowHeadId[t.block], std::move(args)));
        }
    }

    il::Function rewritten = fn;
    rewritten.blocks = std::move(out);
    rewritten.valueCount = nextValue;
    dropDeadF64Boxes(rewritten);
    if (!dominanceHolds(rewritten)) {
        ++stats.refusedSsa;
        return false;
    }

    for (const Candidate& c : plan.candidates) {
        if (c.kind == CandidateKind::BoxElide) ++stats.elidedBox;
        if (c.kind == CandidateKind::PinElided) ++stats.elidedPin;
    }
    // Everything this rewrite made, and everything it was made from, in the new
    // numbering: the next look at this function must not find the slow copy —
    // which a trampoline edge has turned into a natural loop with a single
    // outside predecessor — and duplicate the same region again.
    std::vector<uint8_t> built(nextBlock, 1);
    for (size_t b = 0; b < blockCount; ++b) {
        const bool wasBuilt = b < alreadyBuilt.size() && alreadyBuilt[b];
        built[slowHeadId[b]] = (wasBuilt || plan.inRegion[b] || b == plan.entryPred) ? 1 : 0;
    }
    alreadyBuilt = std::move(built);

    stats.promoted += static_cast<uint32_t>(plan.candidates.size());
    stats.guards += emittedGuards;
    ++stats.duplicated;
    fn = std::move(rewritten);
    return true;
}

}  // namespace bronze::lower
