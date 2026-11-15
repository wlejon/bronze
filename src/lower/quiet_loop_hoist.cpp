#include "lower/quiet_loop_hoist.h"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lower/fresh_instances.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

// A fast-copy block that is one edge out of its copy: a single predecessor,
// and a jump into another copy class at its end.
bool exitTrampoline(const il::Function& fn, const Cfg& cfg, il::BlockId b) {
    const il::Block& block = fn.blocks[b];
    if (block.copyClass != il::CopyClass::Fast || block.instructions.empty() || cfg.preds[b].size() != 1) return false;
    const il::Instruction& term = block.instructions.back();
    if (term.op != il::Op::Jump || term.target.block >= fn.blocks.size()) return false;
    return fn.blocks[term.target.block].copyClass != il::CopyClass::Fast;
}

// The test a shape-guarded load stands behind, as the fields that make two
// tests one question.
struct Claim {
    il::ValueId receiver;
    uint32_t cell;
    uint32_t familyLo;
    uint32_t familySpan;
    bool operator==(const Claim& o) const {
        return receiver == o.receiver && cell == o.cell && familyLo == o.familyLo && familySpan == o.familySpan;
    }
};
Claim claimOf(const il::Instruction& inst, il::ValueId receiver) {
    const bool family = inst.familyLo != il::Instruction::kNoFamily;
    return {receiver, family ? 0 : inst.staticCellIndex, inst.familyLo, family ? inst.familySpan : 0};
}

// Whether two instructions that move as they are compute one thing.
bool sameQuestion(const il::Instruction& a, const il::Instruction& b) {
    if (a.op != b.op || a.operands != b.operands || a.type != b.type) return false;
    switch (a.op) {
        case il::Op::ModuleEnvGet:
            return true;
        case il::Op::EnvGet:
            return a.envDepth == b.envDepth && a.envIndex == b.envIndex;
        case il::Op::IsShape:
            return claimOf(a, a.operands[0]) == claimOf(b, b.operands[0]);
        case il::Op::IsNumber:
            return true;
        case il::Op::ClosureIs:
            return a.calleeIndex == b.calleeIndex && a.immI32 == b.immI32 && a.transitionRunCell == b.transitionRunCell;
        case il::Op::PropGet:
            return a.staticSlot == b.staticSlot && a.numberBits == b.numberBits &&
                   claimOf(a, a.operands[0]) == claimOf(b, b.operands[0]);
        case il::Op::Unbox:
            return a.rawUnbox == b.rawUnbox && a.nullishUnbox == b.nullishUnbox;
        default:
            return false;
    }
}

// One loop's invariants moved in front of it, or 0 when no loop had any.
uint32_t hoistOne(il::Function& fn) {
    const Cfg cfg = buildCfg(fn);
    const std::vector<DefSite> defs = computeDefSites(fn);
    for (il::BlockId header = 0; header < fn.blocks.size(); ++header) {
        if (fn.blocks[header].copyClass != il::CopyClass::Fast) continue;
        std::vector<uint8_t> inLoop(fn.blocks.size(), 0);
        bool any = false;
        for (il::BlockId pred : cfg.preds[header]) {
            if (!cfg.dominates(header, pred)) continue;
            for (il::BlockId b : naturalLoop(cfg, header, pred)) {
                if (b < inLoop.size()) inLoop[b] = 1;
                any = true;
            }
        }
        if (!any) continue;

        // Quiet throughout, and entered from one block outside it.
        bool quiet = true;
        bool storesEnv = false;
        std::vector<uint32_t> storedSlots;
        for (il::BlockId b = 0; b < fn.blocks.size() && quiet; ++b) {
            if (!inLoop[b]) continue;
            const il::Block& block = fn.blocks[b];
            quiet = block.copyClass == il::CopyClass::Fast && block.handler == il::kNoBlock;
            for (const il::Instruction& inst : block.instructions) {
                quiet = quiet && runsNoCode(inst);
                storesEnv = storesEnv || inst.op == il::Op::EnvSet;
                if (inst.op == il::Op::PropSet) storedSlots.push_back(inst.staticSlot);
            }
        }
        if (!quiet) continue;
        il::BlockId outside = il::kNoBlock;
        bool unique = true;
        for (il::BlockId pred : cfg.preds[header]) {
            if (inLoop[pred]) continue;
            if (outside != il::kNoBlock) unique = false;
            outside = pred;
        }
        if (!unique || outside == il::kNoBlock || fn.blocks[outside].handler != il::kNoBlock) continue;

        // What moves, in an order in which a definition precedes its uses.
        // `plain` moves as it is; `behind[i]` is the index in `plain` of the
        // test a load (or the unboxing of one) moves behind.
        std::vector<il::Instruction> plain;
        std::vector<il::Instruction> loads;
        std::vector<size_t> behind;
        Rename same;  // a question asked again -> the first asking
        std::vector<std::pair<il::BlockId, size_t>> taken;
        const auto canon = [&](il::ValueId v) { return renamed(same, v); };
        const auto moved = [&](il::ValueId v, const std::vector<il::Instruction>& list) {
            for (size_t k = 0; k < list.size(); ++k) {
                if (list[k].result == v) return k;
            }
            return SIZE_MAX;
        };
        const auto invariant = [&](il::ValueId v) {
            if (v >= defs.size()) return false;
            if (defs[v].block == il::kNoBlock || !inLoop[defs[v].block]) return true;
            return moved(v, plain) != SIZE_MAX;
        };
        // The moved `is.shape` whose true edge every path to `b` takes and
        // which admits `load`'s claim.
        const auto guardOf = [&](il::BlockId b, const il::Instruction& load) {
            const Claim want = claimOf(load, load.operands[0]);
            for (il::BlockId at = b; at != il::kNoBlock && inLoop[at];) {
                const il::BlockId up = cfg.idom[at];
                if (up == il::kNoBlock || !inLoop[up] || fn.blocks[up].instructions.empty()) break;
                const il::Instruction& term = fn.blocks[up].instructions.back();
                if (term.op == il::Op::Branch && term.operands.size() == 1 && term.target.block == at &&
                    term.elseTarget.block != at && cfg.preds[at].size() == 1) {
                    const size_t k = moved(canon(term.operands[0]), plain);
                    if (k != SIZE_MAX && plain[k].op == il::Op::IsShape && claimOf(plain[k], plain[k].operands[0]) == want) {
                        return k;
                    }
                }
                at = up;
            }
            return SIZE_MAX;
        };
        for (il::BlockId b : cfg.rpo) {
            if (!inLoop[b]) continue;
            const std::vector<il::Instruction>& insts = fn.blocks[b].instructions;
            for (size_t i = 0; i < insts.size(); ++i) {
                il::Instruction inst = insts[i];
                if (inst.result == il::kNoValue) continue;
                for (il::ValueId& v : inst.operands) v = canon(v);
                bool asIs = false;
                size_t guard = SIZE_MAX;
                switch (inst.op) {
                    case il::Op::ModuleEnvGet:
                    case il::Op::RunMethodIs:
                        asIs = inst.operands.empty();
                        break;
                    case il::Op::EnvGet:
                        asIs = !storesEnv && inst.operands.size() == 1 && invariant(inst.operands[0]);
                        break;
                    case il::Op::IsShape:
                        asIs = inst.staticSlot != il::Instruction::kNoStaticSlot && inst.operands.size() == 1 &&
                               invariant(inst.operands[0]);
                        break;
                    case il::Op::ClosureIs:
                    case il::Op::MethodIcIs:
                    case il::Op::IsNumber:
                        asIs = inst.operands.size() == 1 && invariant(inst.operands[0]);
                        break;
                    case il::Op::PropGet:
                        if (inst.shapeGuarded && inst.operands.size() == 1 && invariant(inst.operands[0]) &&
                            (inst.type == il::Type::Dynamic || inst.type == il::Type::F64) &&
                            std::find(storedSlots.begin(), storedSlots.end(), inst.staticSlot) == storedSlots.end()) {
                            guard = guardOf(b, inst);
                        }
                        break;
                    case il::Op::Unbox:
                        if (inst.type == il::Type::F64 && inst.operands.size() == 1 && !il::canCollect(inst)) {
                            const size_t k = moved(inst.operands[0], loads);
                            if (k != SIZE_MAX) guard = behind[k];
                        }
                        break;
                    default:
                        break;
                }
                if (!asIs && guard == SIZE_MAX) continue;
                taken.emplace_back(b, i);
                std::vector<il::Instruction>& list = asIs ? plain : loads;
                size_t first = SIZE_MAX;
                for (size_t k = 0; k < list.size() && first == SIZE_MAX; ++k) {
                    if (sameQuestion(list[k], inst) && (asIs || behind[k] == guard)) first = k;
                }
                if (first != SIZE_MAX) {
                    same.emplace_back(inst.result, list[first].result);
                    continue;
                }
                list.push_back(std::move(inst));
                if (!asIs) behind.push_back(guard);
            }
        }
        if (taken.empty()) continue;

        // Out of the loop, last first so the indices stand.
        for (size_t k = taken.size(); k-- > 0;) {
            std::vector<il::Instruction>& insts = fn.blocks[taken[k].first].instructions;
            insts.erase(insts.begin() + static_cast<std::ptrdiff_t>(taken[k].second));
        }
        // A load the loop still reads is a parameter of the join behind its
        // arm; one only its own unboxing read needs none.
        std::vector<uint8_t> read(fn.valueCount, 0);
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                il::Instruction copy = inst;
                rewriteUses(copy, [&](il::ValueId id) {
                    const il::ValueId v = canon(id);
                    if (v < read.size()) read[v] = 1;
                    return id;
                });
            }
        }
        Rename rename = same;
        const auto fresh = [&] { return fn.valueCount++; };
        const auto newBlock = [&] {
            il::Block block;
            block.id = static_cast<il::BlockId>(fn.blocks.size());
            block.copyClass = il::CopyClass::Fast;
            block.copyRegion = fn.blocks[header].copyRegion;
            fn.blocks.push_back(std::move(block));
            return static_cast<il::BlockId>(fn.blocks.size() - 1);
        };
        const Span span = fn.blocks[header].instructions.empty() ? Span{}
                                                                 : fn.blocks[header].instructions.front().span;
        const auto jumpTo = [&](il::BlockId to, std::vector<il::ValueId> args) {
            il::Instruction jump;
            jump.op = il::Op::Jump;
            jump.type = il::Type::Void;
            jump.span = span;
            jump.target.block = to;
            jump.target.args = std::move(args);
            return jump;
        };

        // The block in front of the loop: the header's parameters under new
        // names, handed on at the end.
        il::BlockId at = newBlock();
        std::vector<il::BlockId> arms;
        std::vector<il::ValueId> pass;
        for (const il::BlockParam& p : fn.blocks[header].params) {
            const il::ValueId id = fresh();
            fn.blocks[at].params.push_back({id, p.type});
            pass.push_back(id);
        }
        {
            il::Instruction& term = fn.blocks[outside].instructions.back();
            if (term.target.block == header) term.target.block = at;
            if (term.op == il::Op::Branch && term.elseTarget.block == header) term.elseTarget.block = at;
        }
        for (il::Instruction& inst : plain) fn.blocks[at].instructions.push_back(inst);
        for (size_t g = 0; g < plain.size(); ++g) {
            std::vector<size_t> group;
            for (size_t k = 0; k < loads.size(); ++k) {
                if (behind[k] == g) group.push_back(k);
            }
            if (group.empty()) continue;
            const il::BlockId arm = newBlock();
            arms.push_back(arm);
            const il::BlockId join = newBlock();
            std::vector<il::ValueId> hit;
            std::vector<il::ValueId> miss;
            for (size_t k : group) {
                const il::Instruction& load = loads[k];
                fn.blocks[arm].instructions.push_back(load);
                if (load.result >= read.size() || !read[load.result]) continue;
                // On the arm the test refused: a value of the type, unread.
                il::Instruction none;
                none.op = load.type == il::Type::F64 ? il::Op::ConstF64 : il::Op::ConstUndefined;
                none.type = load.type;
                none.span = load.span;
                none.result = fresh();
                miss.push_back(none.result);
                fn.blocks[at].instructions.push_back(std::move(none));
                hit.push_back(load.result);
                const il::ValueId param = fresh();
                fn.blocks[join].params.push_back({param, load.type});
                rename.emplace_back(load.result, param);
            }
            fn.blocks[arm].instructions.push_back(jumpTo(join, std::move(hit)));
            il::Instruction branch;
            branch.op = il::Op::Branch;
            branch.type = il::Type::Void;
            branch.span = span;
            branch.operands = {plain[g].result};
            branch.target.block = arm;
            branch.elseTarget.block = join;
            branch.elseTarget.args = std::move(miss);
            fn.blocks[at].instructions.push_back(std::move(branch));
            at = join;
        }
        fn.blocks[at].instructions.push_back(jumpTo(header, std::move(pass)));

        // The loop reads the first asking of each question and the joins'
        // parameters; the arms keep the names they load under.
        for (il::Block& block : fn.blocks) {
            if (std::find(arms.begin(), arms.end(), block.id) != arms.end()) continue;
            for (il::Instruction& inst : block.instructions) {
                rewriteUses(inst, [&](il::ValueId id) { return renamed(rename, renamed(same, id)); });
            }
        }
        return static_cast<uint32_t>(taken.size());
    }
    return 0;
}

// A `box.f64` of the fast copy that only exit trampolines read is made in
// each of them. Returns the boxes moved.
uint32_t sinkExitBoxes(il::Function& fn) {
    const Cfg cfg = buildCfg(fn);
    struct Use {
        il::BlockId block;
        bool exit;
    };
    std::unordered_map<il::ValueId, std::vector<Use>> uses;
    for (const il::Block& block : fn.blocks) {
        const bool exit = exitTrampoline(fn, cfg, block.id);
        for (const il::Instruction& inst : block.instructions) {
            il::Instruction copy = inst;
            rewriteUses(copy, [&](il::ValueId id) {
                uses[id].push_back({block.id, exit});
                return id;
            });
        }
    }
    uint32_t sunk = 0;
    for (il::BlockId b = 0; b < fn.blocks.size(); ++b) {
        if (fn.blocks[b].copyClass != il::CopyClass::Fast || exitTrampoline(fn, cfg, b)) continue;
        std::vector<il::Instruction>& insts = fn.blocks[b].instructions;
        for (size_t i = 0; i < insts.size();) {
            const il::Instruction box = insts[i];
            const auto found = uses.find(box.result);
            bool only = box.op == il::Op::Box && box.boxType == il::Type::F64 && box.operands.size() == 1 &&
                        found != uses.end() && !found->second.empty();
            if (only) {
                for (const Use& use : found->second) only = only && use.exit && use.block != b;
            }
            if (!only) {
                ++i;
                continue;
            }
            std::vector<il::BlockId> exits;
            for (const Use& use : found->second) {
                if (std::find(exits.begin(), exits.end(), use.block) == exits.end()) exits.push_back(use.block);
            }
            for (il::BlockId e : exits) {
                il::Instruction copy = box;
                copy.result = fn.valueCount++;
                std::vector<il::Instruction>& out = fn.blocks[e].instructions;
                for (il::Instruction& inst : out) {
                    rewriteUses(inst, [&](il::ValueId id) { return id == box.result ? copy.result : id; });
                }
                out.insert(out.begin(), std::move(copy));
            }
            insts.erase(insts.begin() + static_cast<std::ptrdiff_t>(i));
            ++sunk;
        }
    }
    return sunk;
}

}  // namespace

uint32_t hoistQuietLoops(il::Module& module) {
    uint32_t total = 0;
    for (il::Function& fn : module.functions) {
        if (fn.isExternal) continue;
        bool regions = false;
        for (const il::Block& block : fn.blocks) regions = regions || block.copyClass == il::CopyClass::Fast;
        if (!regions) continue;
        il::Function before = fn;
        uint32_t moved = 0;
        // A loop's invariants, then those of the loops inside it that it did
        // not take (one that is not quiet has quiet loops in it).
        for (uint32_t n = hoistOne(fn), rounds = 0; n != 0 && rounds < 64; n = hoistOne(fn), ++rounds) moved += n;
        if (moved != 0) {
            std::vector<uint8_t> none(fn.blocks.size(), 0);
            pruneUnreachable(fn, none);
            orderAfterDominators(fn, none);
        }
        moved += sinkExitBoxes(fn);
        if (moved == 0) continue;
        if (!dominanceHolds(fn)) {
            fn = std::move(before);
            continue;
        }
        total += moved;
    }
    return total;
}

}  // namespace bronze::lower
