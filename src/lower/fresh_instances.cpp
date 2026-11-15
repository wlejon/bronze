#include "lower/fresh_instances.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lower/guard_region_cfg.h"
#include "lower/typed_entry.h"

namespace bronze::lower {

namespace {

// One store of the constructor's run: the key and the value it wrote.
struct Site {
    size_t index = 0;
    uint32_t key = 0;
    il::ValueId value = il::kNoValue;
};

// One instance this pass can do without.
struct Instance {
    il::BlockId guardBlock = il::kNoBlock;   // ends `closure.is C, construct` and its branch
    il::BlockId createBlock = il::kNoBlock;  // holds `create.this`, first
    il::BlockId runBlock = il::kNoBlock;     // holds the run
    size_t head = 0;                         // the run's first store
    std::vector<Site> sites;
    uint32_t slowRun = il::Instruction::kNoTransitionRun;
    std::unordered_set<il::ValueId> names;   // the instance and the parameters it is passed as
    // What reads it, by (block, instruction).
    std::vector<std::pair<il::BlockId, size_t>> reads;
    std::vector<std::pair<il::BlockId, size_t>> methodGuards;
    std::vector<std::pair<il::BlockId, size_t>> shapeTests;
    std::vector<il::BlockId> exits;          // trampolines whose jump carries it
};

struct Pass {
    il::Function& fn;
    Cfg cfg;
    std::vector<uint32_t> entries;  // edges and handler entries into each block
    std::vector<uint8_t> handler;

    explicit Pass(il::Function& f) : fn(f), cfg(buildCfg(f)) {
        entries.assign(fn.blocks.size(), 0);
        handler.assign(fn.blocks.size(), 0);
        for (const il::Block& block : fn.blocks) {
            if (block.handler != il::kNoBlock && block.handler < handler.size()) {
                handler[block.handler] = 1;
                ++entries[block.handler];
            }
            if (block.instructions.empty()) continue;
            const il::Instruction& term = block.instructions.back();
            for (const il::BlockTarget* t : {&term.target, &term.elseTarget}) {
                if (t->block != il::kNoBlock && t->block < entries.size()) ++entries[t->block];
            }
        }
    }

    // A fast-copy block whose last instruction jumps into the slow copy of
    // its region: where a guard's miss leaves (guard_region.h, "IC EXITS").
    bool exitTrampoline(il::BlockId b) const {
        const il::Block& block = fn.blocks[b];
        if (block.copyClass != il::CopyClass::Fast || block.instructions.empty()) return false;
        const il::Instruction& term = block.instructions.back();
        if (term.op != il::Op::Jump || term.target.block >= fn.blocks.size()) return false;
        const il::Block& to = fn.blocks[term.target.block];
        return to.copyClass == il::CopyClass::Slow && to.copyRegion == block.copyRegion;
    }

    static bool uses(const il::Instruction& inst, const std::unordered_set<il::ValueId>& names) {
        for (il::ValueId v : inst.operands) {
            if (names.count(v) != 0) return true;
        }
        for (const il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
            for (il::ValueId v : t->args) {
                if (names.count(v) != 0) return true;
            }
        }
        return false;
    }

    // The instance `create.this` at the head of `b` makes, if every use of it
    // is one this pass has an answer for.
    bool find(il::BlockId b, Instance& out) const {
        const il::Block& made = fn.blocks[b];
        if (made.copyClass != il::CopyClass::Fast || made.instructions.empty() || handler[b]) return false;
        const il::Instruction& create = made.instructions.front();
        if (create.op != il::Op::CreateThis || create.operands.size() != 1 || create.result == il::kNoValue) {
            return false;
        }
        const il::ValueId ctor = create.operands[0];
        const il::ValueId self = create.result;

        // The guard: the block's one way in, whose miss leaves the fast copy.
        if (entries[b] != 1 || cfg.preds[b].size() != 1) return false;
        // Through the empty blocks a split leaves between a guard and its hit.
        il::BlockId hit = b;
        il::BlockId g = cfg.preds[b][0];
        while (fn.blocks[g].instructions.size() == 1 && fn.blocks[g].instructions[0].op == il::Op::Jump &&
               fn.blocks[g].instructions[0].target.args.empty() && entries[g] == 1 && cfg.preds[g].size() == 1 &&
               !handler[g]) {
            hit = g;
            g = cfg.preds[g][0];
        }
        const std::vector<il::Instruction>& gi = fn.blocks[g].instructions;
        if (gi.size() < 2) return false;
        const il::Instruction& term = gi.back();
        const il::Instruction& guard = gi[gi.size() - 2];
        if (term.op != il::Op::Branch || term.operands.size() != 1 || term.operands[0] != guard.result ||
            term.target.block != hit || term.elseTarget.block >= fn.blocks.size() ||
            !exitTrampoline(term.elseTarget.block)) {
            return false;
        }
        if (guard.op != il::Op::ClosureIs || guard.operands.size() != 1 || guard.operands[0] != ctor ||
            guard.immI32 != il::Instruction::kClosureIsConstruct ||
            guard.transitionRunCell != il::Instruction::kNoTransitionRun) {
            return false;
        }
        out.guardBlock = g;
        out.createBlock = b;
        out.names = {self};

        // The run: the first use of the instance, down the straight line from
        // its creation, with nothing before it that can collect.
        il::BlockId at = b;
        size_t i = 1;
        for (;;) {
            const std::vector<il::Instruction>& insts = fn.blocks[at].instructions;
            if (i >= insts.size()) return false;
            const il::Instruction& inst = insts[i];
            if (uses(inst, out.names)) break;
            if (inst.op == il::Op::Jump) {
                const il::BlockId next = inst.target.block;
                if (next >= fn.blocks.size() || entries[next] != 1 || !inst.target.args.empty() ||
                    fn.blocks[next].copyClass != il::CopyClass::Fast) {
                    return false;
                }
                at = next;
                i = 0;
                continue;
            }
            if (!runsNoCode(inst) || inst.op == il::Op::Branch) return false;
            ++i;
        }
        const std::vector<il::Instruction>& run = fn.blocks[at].instructions;
        const il::Instruction& first = run[i];
        if (first.op != il::Op::PropSet || first.transitionRunCell == il::Instruction::kNoTransitionRun ||
            first.transitionRunSlot != 0 || i + first.transitionRunLength > run.size()) {
            return false;
        }
        out.runBlock = at;
        out.head = i;
        std::unordered_set<uint32_t> keys;
        for (size_t j = i; j < i + first.transitionRunLength; ++j) {
            const il::Instruction& inst = run[j];
            const bool store = inst.op == il::Op::PropSet && inst.operands.size() == 2 && inst.operands[0] == self &&
                               inst.operands[1] != self && inst.directTarget == il::Instruction::kNoDirectTarget;
            if (store) {
                if (!keys.insert(inst.keyIndex).second) return false;
                out.sites.push_back({j, inst.keyIndex, inst.operands[1]});
            } else if (uses(inst, out.names) || !runsNoCode(inst)) {
                return false;
            }
        }
        if (out.sites.empty()) return false;

        // The slow copy's run of the same constructor: the store that shares
        // the first store's inline-cache site and inlined frame (each `new`
        // of one class inlines the same stores).
        for (const il::Block& block : fn.blocks) {
            if (block.copyClass != il::CopyClass::Slow || block.copyRegion != fn.blocks[at].copyRegion) continue;
            for (const il::Instruction& inst : block.instructions) {
                if (inst.op != il::Op::PropSet || inst.icIndex != first.icIndex || inst.keyIndex != first.keyIndex ||
                    inst.inlineFrame != first.inlineFrame ||
                    inst.transitionRunCell == il::Instruction::kNoTransitionRun) {
                    continue;
                }
                if (out.slowRun != il::Instruction::kNoTransitionRun) return false;
                out.slowRun = inst.transitionRunCell;
            }
        }
        if (out.slowRun == il::Instruction::kNoTransitionRun) return false;

        // The parameters the instance is passed as, down edges that are a
        // block's only way in.
        for (bool grew = true; grew;) {
            grew = false;
            for (const il::Block& block : fn.blocks) {
                if (block.instructions.empty()) continue;
                const il::Instruction& last = block.instructions.back();
                if (last.op != il::Op::Jump || last.target.block >= fn.blocks.size()) continue;
                const il::Block& to = fn.blocks[last.target.block];
                if (entries[to.id] != 1) continue;
                for (size_t p = 0; p < to.params.size() && p < last.target.args.size(); ++p) {
                    if (out.names.count(last.target.args[p]) != 0 && out.names.insert(to.params[p].id).second) {
                        grew = true;
                    }
                }
            }
        }

        // Every use, sorted; anything else holds the instance.
        std::unordered_set<size_t> siteAt;
        for (const Site& s : out.sites) siteAt.insert(s.index);
        for (const il::Block& block : fn.blocks) {
            for (size_t j = 0; j < block.instructions.size(); ++j) {
                const il::Instruction& inst = block.instructions[j];
                if (!uses(inst, out.names)) continue;
                if (block.id == at && siteAt.count(j) != 0) continue;
                bool operand = false;
                for (il::ValueId v : inst.operands) operand = operand || out.names.count(v) != 0;
                if (operand) {
                    if (inst.operands.size() != 1) return false;
                    if (inst.op == il::Op::MethodIcIs) {
                        out.methodGuards.push_back({block.id, j});
                    } else if (inst.op == il::Op::IsShape) {
                        out.shapeTests.push_back({block.id, j});
                    } else if (inst.op == il::Op::PropGet && keys.count(inst.keyIndex) != 0 &&
                               inst.directTarget == il::Instruction::kNoDirectTarget && inst.result != il::kNoValue) {
                        out.reads.push_back({block.id, j});
                    } else {
                        return false;
                    }
                    continue;
                }
                // Passed on: as one of its own names, or out of the fast copy.
                if (inst.op != il::Op::Jump || inst.target.block >= fn.blocks.size()) return false;
                if (entries[inst.target.block] == 1) {
                    const il::Block& to = fn.blocks[inst.target.block];
                    bool all = true;
                    for (size_t p = 0; p < inst.target.args.size(); ++p) {
                        if (out.names.count(inst.target.args[p]) == 0) continue;
                        all = all && p < to.params.size() && out.names.count(to.params[p].id) != 0;
                    }
                    if (all) continue;
                }
                if (!exitTrampoline(block.id)) return false;
                out.exits.push_back(block.id);
            }
        }

        // A read answers the value stored, whatever it is. What read it may
        // have trusted the slot instead: an unchecked unbox stands behind the
        // shape test this pass answers, which said the slot holds a Number.
        // So a value that is not a Number's box must have no such reader.
        std::unordered_set<il::ValueId> boxes;
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.op == il::Op::Box && inst.boxType == il::Type::F64 && inst.result != il::kNoValue) {
                    boxes.insert(inst.result);
                }
            }
        }
        std::unordered_set<il::ValueId> unknown;
        for (const auto& [blk, j] : out.reads) {
            const il::Instruction& read = fn.blocks[blk].instructions[j];
            for (const Site& s : out.sites) {
                if (s.key == read.keyIndex && boxes.count(s.value) == 0) unknown.insert(read.result);
            }
        }
        if (!unknown.empty()) {
            for (const il::Block& block : fn.blocks) {
                for (const il::Instruction& inst : block.instructions) {
                    if (inst.op == il::Op::Unbox && (inst.rawUnbox || inst.nullishUnbox) && uses(inst, unknown)) {
                        return false;
                    }
                }
            }
        }

        // Nothing the instance is live across may collect: then an exit makes
        // the object the `new` would have, and no handler is entered with it.
        const std::vector<std::vector<il::ValueId>> liveIn = computeLiveIn(fn, cfg);
        const auto liveInto = [&](il::BlockId s) {
            for (il::ValueId v : liveIn[s]) {
                if (out.names.count(v) != 0) return true;
            }
            return false;
        };
        std::unordered_set<uint64_t> gone;
        const auto at64 = [](il::BlockId blk, size_t j) { return (uint64_t{blk} << 32) | j; };
        gone.insert(at64(b, 0));
        for (const Site& s : out.sites) gone.insert(at64(at, s.index));
        for (const auto& [blk, j] : out.reads) gone.insert(at64(blk, j));
        for (const il::Block& block : fn.blocks) {
            bool live = false;
            for (il::BlockId s : cfg.succs[block.id]) live = live || liveInto(s);
            bool ever = live;
            for (size_t j = block.instructions.size(); j-- > 0;) {
                const il::Instruction& inst = block.instructions[j];
                live = live || uses(inst, out.names);
                ever = ever || live;
                if (inst.result != il::kNoValue && out.names.count(inst.result) != 0) {
                    live = false;
                    continue;
                }
                if (live && gone.count(at64(block.id, j)) == 0 && !runsNoCode(inst)) return false;
            }
            if (ever && block.handler != il::kNoBlock) return false;
        }
        return true;
    }

    void apply(const Instance& in) {
        const il::Instruction create = fn.blocks[in.createBlock].instructions.front();
        std::vector<il::Instruction> stores;
        for (const Site& s : in.sites) stores.push_back(fn.blocks[in.runBlock].instructions[s.index]);
        const il::Instruction head = fn.blocks[in.runBlock].instructions[in.head];
        std::unordered_map<uint32_t, il::ValueId> stored;
        for (const Site& s : in.sites) stored[s.key] = s.value;

        // The guard asks for the run.
        std::vector<il::Instruction>& gi = fn.blocks[in.guardBlock].instructions;
        gi[gi.size() - 2].transitionRunCell = in.slowRun;

        // Reads are what was stored; guards ask the run's shape.
        std::unordered_map<il::ValueId, il::ValueId> rename;
        for (const auto& [blk, j] : in.reads) {
            const il::Instruction& read = fn.blocks[blk].instructions[j];
            rename[read.result] = stored.at(read.keyIndex);
        }
        for (const auto& [blk, j] : in.methodGuards) {
            il::Instruction& guard = fn.blocks[blk].instructions[j];
            guard.op = il::Op::RunMethodIs;
            guard.operands.clear();
            guard.transitionRunCell = in.slowRun;
        }
        for (const auto& [blk, j] : in.shapeTests) {
            il::Instruction& test = fn.blocks[blk].instructions[j];
            il::Instruction yes;
            yes.op = il::Op::ConstBool;
            yes.type = il::Type::Bool;
            yes.immI32 = 1;
            yes.result = test.result;
            yes.span = test.span;
            test = std::move(yes);
        }

        // An exit makes the object: the creation and the run as one stretch.
        for (il::BlockId t : in.exits) {
            std::vector<il::Instruction>& ti = fn.blocks[t].instructions;
            const il::ValueId made = fn.valueCount++;
            std::vector<il::Instruction> build;
            il::Instruction c = create;
            c.result = made;
            build.push_back(std::move(c));
            for (size_t k = 0; k < stores.size(); ++k) {
                il::Instruction s = stores[k];
                s.operands[0] = made;
                s.transitionRunCell = k == 0 ? head.transitionRunCell : il::Instruction::kNoTransitionRun;
                s.transitionRunLength = k == 0 ? static_cast<uint32_t>(stores.size()) : 0;
                s.transitionRunSlot = k == 0 ? head.transitionRunSlot : 0;
                build.push_back(std::move(s));
            }
            for (il::ValueId& v : ti.back().target.args) {
                if (in.names.count(v) != 0) v = made;
            }
            ti.insert(ti.end() - 1, std::make_move_iterator(build.begin()), std::make_move_iterator(build.end()));
        }

        // The parameters it was passed as go with their arguments.
        for (il::Block& block : fn.blocks) {
            if (block.instructions.empty()) continue;
            il::BlockTarget& edge = block.instructions.back().target;
            if (block.instructions.back().op != il::Op::Jump || edge.block >= fn.blocks.size()) continue;
            il::Block& to = fn.blocks[edge.block];
            for (size_t p = std::min(to.params.size(), edge.args.size()); p-- > 0;) {
                if (in.names.count(edge.args[p]) == 0 || in.names.count(to.params[p].id) == 0) continue;
                edge.args.erase(edge.args.begin() + static_cast<std::ptrdiff_t>(p));
                to.params.erase(to.params.begin() + static_cast<std::ptrdiff_t>(p));
            }
        }

        // The creation, the stores and the reads go; later indices first.
        std::vector<std::pair<il::BlockId, size_t>> dead = in.reads;
        dead.push_back({in.createBlock, 0});
        for (const Site& s : in.sites) dead.push_back({in.runBlock, s.index});
        std::sort(dead.begin(), dead.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first < b.first : a.second > b.second;
        });
        for (const auto& [blk, j] : dead) {
            std::vector<il::Instruction>& insts = fn.blocks[blk].instructions;
            insts.erase(insts.begin() + static_cast<std::ptrdiff_t>(j));
        }

        // A read's value is a Number's box more often than not, and what read
        // it unboxed it: that is the number boxed.
        std::unordered_map<il::ValueId, il::ValueId> boxed;
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.op == il::Op::Box && inst.boxType == il::Type::F64 && inst.operands.size() == 1 &&
                    inst.result != il::kNoValue) {
                    boxed[inst.result] = inst.operands[0];
                }
            }
        }
        const auto resolve = [&](il::ValueId v) {
            for (auto it = rename.find(v); it != rename.end(); it = rename.find(v)) v = it->second;
            return v;
        };
        for (il::Block& block : fn.blocks) {
            std::vector<il::Instruction> kept;
            kept.reserve(block.instructions.size());
            for (il::Instruction& inst : block.instructions) {
                bool renamed = false;
                for (il::ValueId& v : inst.operands) {
                    const il::ValueId to = resolve(v);
                    renamed = renamed || to != v;
                    v = to;
                }
                for (il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                    for (il::ValueId& v : t->args) v = resolve(v);
                }
                if (renamed && inst.op == il::Op::Unbox && inst.type == il::Type::F64 && inst.operands.size() == 1 &&
                    inst.result != il::kNoValue) {
                    const auto number = boxed.find(inst.operands[0]);
                    if (number != boxed.end()) {
                        rename[inst.result] = number->second;
                        continue;
                    }
                }
                kept.push_back(std::move(inst));
            }
            block.instructions = std::move(kept);
        }
    }
};

}  // namespace

bool runsNoCode(const il::Instruction& inst) {
    if (!il::canCollect(inst)) return true;
    if (inst.shapeGuarded && (inst.op == il::Op::PropGet || inst.op == il::Op::PropSet)) {
        // With a layout fact the site is its ordinary self when the fact is
        // stale (layout_facts.h).
        return il::layoutFactOperand(inst) == il::kNoValue;
    }
    return inst.op == il::Op::EnvGet || inst.op == il::Op::ModuleEnvGet;
}

uint32_t sinkFreshInstances(il::Module& module) {
    uint32_t removed = 0;
    for (il::Function& fn : module.functions) {
        if (fn.isExternal) continue;
        bool regions = false;
        for (const il::Block& block : fn.blocks) regions = regions || block.copyClass == il::CopyClass::Fast;
        if (!regions) continue;
        // One at a time, until none is left: an instance made while another
        // is live stands in that one's way until it is gone itself.
        for (bool changed = true; changed;) {
            changed = false;
            Pass pass(fn);
            for (size_t b = 0; b < fn.blocks.size(); ++b) {
                Instance in;
                if (!pass.find(static_cast<il::BlockId>(b), in)) continue;
                pass.apply(in);
                ++removed;
                changed = true;
                break;
            }
            // What the instance's guards decided is now constant: fold it, so
            // a body the inliner kept twice (typed and not) is one again and
            // the instance it makes has one definition.
            if (changed) foldTypedEntry(fn, &module);
        }
    }
    return removed;
}

}  // namespace bronze::lower
