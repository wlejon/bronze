#include <algorithm>
#include <vector>

#include "lower/guard_region_cfg.h"
#include "lower/typed_entry.h"

namespace bronze::lower {

// THE FOLD A TYPED ENTRY IS BUILT FOR (typed_entry.h). The entry's body reads
// `box.f64 %p` / `box.bool %p` where the generic body read a parameter of any
// type, and every rule here is a fact about such a box and nothing else:
//
//   unbox.f64 (box.f64 x)       -> x          (checked or raw: the box IS a Number)
//   unbox.bool (box.bool b)     -> b
//   is.number (box.f64 | box.i32) -> true;  is.number (box.bool | box.str) -> false
//   is.nullish (box.*)          -> false
//   strict.eq box.* , undefined|null -> false; box.f64 vs box.bool -> false
//   strict.eq undefined, undefined -> true;   is.nullish undefined|null -> true
//     (an omitted parameter reads `const.undefined`, and these are its
//     defaulted-parameter tests)
//   br <const>                  -> jump to the arm it names
//
// then what those decided: blocks nothing reaches any more are dropped, a block
// parameter every remaining edge passes the same value is that value, and pure
// instructions nothing reads are removed. A guarded region's entry tests on a
// parameter are `is.number`s of exactly such a box, so their slow copies are
// what the pruning removes.
//
// Every rewrite here replaces a value by one that dominates it (a box's operand
// dominates the box, an edge-invariant block argument dominates the block), so
// the result is checked with `dominanceHolds` only as the guard on a bug.

namespace {

struct Defs {
    std::vector<const il::Instruction*> inst;  // defining instruction, or null
    std::vector<il::BlockId> block;            // defining block; kNoBlock for a parameter
};

Defs collectDefs(const il::Function& fn) {
    Defs d;
    d.inst.assign(fn.valueCount, nullptr);
    d.block.assign(fn.valueCount, il::kNoBlock);
    for (const auto& block : fn.blocks) {
        for (const auto& bp : block.params) {
            if (bp.id < d.block.size()) d.block[bp.id] = block.id;
        }
        for (const auto& inst : block.instructions) {
            if (inst.result == il::kNoValue || inst.result >= d.inst.size()) continue;
            d.inst[inst.result] = &inst;
            d.block[inst.result] = block.id;
        }
    }
    return d;
}

const il::Instruction* boxDef(const Defs& d, il::ValueId v) {
    if (v >= d.inst.size()) return nullptr;
    const il::Instruction* def = d.inst[v];
    if (def == nullptr || def->op != il::Op::Box || def->operands.size() != 1) return nullptr;
    return def;
}

bool isNullishConst(const Defs& d, il::ValueId v) {
    if (v >= d.inst.size() || d.inst[v] == nullptr) return false;
    return d.inst[v]->op == il::Op::ConstUndefined || d.inst[v]->op == il::Op::ConstNull;
}

il::Op constOp(const Defs& d, il::ValueId v) {
    if (v >= d.inst.size() || d.inst[v] == nullptr) return il::Op::KeepAlive;
    return d.inst[v]->op;
}

void makeConstBool(il::Instruction& inst, bool value) {
    inst.op = il::Op::ConstBool;
    inst.type = il::Type::Bool;
    inst.operands.clear();
    inst.immI32 = value ? 1 : 0;
    inst.boxType = il::Type::Void;
    inst.rawUnbox = false;
    inst.nullishUnbox = false;
}

template <typename Fn>
void forEachUse(il::Instruction& inst, Fn&& fn) {
    for (il::ValueId& v : inst.operands) fn(v);
    for (il::ValueId& v : inst.target.args) fn(v);
    for (il::ValueId& v : inst.elseTarget.args) fn(v);
}

void applyRename(il::Function& fn, std::vector<il::ValueId>& to) {
    auto resolve = [&](il::ValueId v) {
        while (v < to.size() && to[v] != v) v = to[v];
        return v;
    };
    for (auto& block : fn.blocks) {
        for (auto& inst : block.instructions) {
            forEachUse(inst, [&](il::ValueId& v) { v = resolve(v); });
        }
    }
}

// The value-level rules. Returns how many fired.
uint32_t foldValues(il::Function& fn) {
    const Defs d = collectDefs(fn);
    std::vector<il::ValueId> to(fn.valueCount);
    for (il::ValueId v = 0; v < to.size(); ++v) to[v] = v;
    uint32_t folds = 0;
    bool renamed = false;
    // Erased only after the walk: `d` holds pointers into these vectors.
    std::vector<std::vector<size_t>> erase(fn.blocks.size());
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto& insts = fn.blocks[bi].instructions;
        for (size_t i = 0; i < insts.size(); ++i) {
            il::Instruction& inst = insts[i];
            if (inst.op == il::Op::Unbox && inst.operands.size() == 1 && inst.result < to.size()) {
                const il::Instruction* box = boxDef(d, inst.operands[0]);
                if (box != nullptr && box->boxType == inst.type &&
                    (inst.type == il::Type::F64 || inst.type == il::Type::Bool)) {
                    to[inst.result] = box->operands[0];
                    renamed = true;
                    ++folds;
                    erase[bi].push_back(i);
                }
            } else if (inst.op == il::Op::IsNumber && inst.operands.size() == 1) {
                if (const il::Instruction* box = boxDef(d, inst.operands[0])) {
                    if (box->boxType != il::Type::Dynamic && box->boxType != il::Type::Void) {
                        makeConstBool(inst, box->boxType == il::Type::F64 || box->boxType == il::Type::I32);
                        ++folds;
                    }
                }
            } else if (inst.op == il::Op::IsNullish && inst.operands.size() == 1) {
                if (const il::Instruction* box = boxDef(d, inst.operands[0])) {
                    if (box->boxType != il::Type::Dynamic && box->boxType != il::Type::Void) {
                        makeConstBool(inst, false);
                        ++folds;
                    }
                } else if (isNullishConst(d, inst.operands[0])) {
                    makeConstBool(inst, true);
                    ++folds;
                }
            } else if (inst.op == il::Op::StrictEq && inst.operands.size() == 2) {
                const il::Instruction* a = boxDef(d, inst.operands[0]);
                const il::Instruction* b = boxDef(d, inst.operands[1]);
                auto primitiveBox = [](const il::Instruction* box) {
                    return box != nullptr &&
                           (box->boxType == il::Type::F64 || box->boxType == il::Type::Bool);
                };
                const bool boxVsNullish = (primitiveBox(a) && isNullishConst(d, inst.operands[1])) ||
                                          (primitiveBox(b) && isNullishConst(d, inst.operands[0]));
                const bool numberVsBool = primitiveBox(a) && primitiveBox(b) && a->boxType != b->boxType;
                const il::Op la = constOp(d, inst.operands[0]);
                const il::Op lb = constOp(d, inst.operands[1]);
                const bool bothNullish = isNullishConst(d, inst.operands[0]) && isNullishConst(d, inst.operands[1]);
                if (boxVsNullish || numberVsBool) {
                    makeConstBool(inst, false);
                    ++folds;
                } else if (bothNullish) {
                    // `undefined === undefined`, `null === null`; the two differ.
                    makeConstBool(inst, la == lb);
                    ++folds;
                }
            }
        }
    }
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        auto& insts = fn.blocks[bi].instructions;
        for (size_t k = erase[bi].size(); k-- > 0;) {
            insts.erase(insts.begin() + static_cast<ptrdiff_t>(erase[bi][k]));
        }
    }
    if (renamed) applyRename(fn, to);
    return folds;
}

uint32_t foldBranches(il::Function& fn) {
    const Defs d = collectDefs(fn);
    uint32_t folds = 0;
    for (auto& block : fn.blocks) {
        if (block.instructions.empty()) continue;
        il::Instruction& term = block.instructions.back();
        if (term.op != il::Op::Branch || term.operands.size() != 1) continue;
        const il::ValueId c = term.operands[0];
        if (c >= d.inst.size() || d.inst[c] == nullptr || d.inst[c]->op != il::Op::ConstBool) continue;
        const bool taken = d.inst[c]->immI32 != 0;
        il::Instruction jump;
        jump.op = il::Op::Jump;
        jump.type = il::Type::Void;
        jump.span = term.span;
        jump.target = taken ? term.target : term.elseTarget;
        term = std::move(jump);
        ++folds;
    }
    return folds;
}

// Unreachable blocks dropped, the rest renumbered in their existing order, so
// "a definition's block precedes its uses'" survives.
bool pruneBlocks(il::Function& fn) {
    const Cfg cfg = buildCfg(fn);
    std::vector<il::BlockId> remap(fn.blocks.size(), il::kNoBlock);
    il::BlockId next = 0;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (cfg.rpoIndex[b] != UINT32_MAX) remap[b] = next++;
    }
    if (next == fn.blocks.size()) return false;
    std::vector<il::Block> kept;
    kept.reserve(next);
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (remap[b] == il::kNoBlock) continue;
        il::Block block = std::move(fn.blocks[b]);
        block.id = remap[b];
        if (block.handler != il::kNoBlock) block.handler = remap[block.handler];
        for (auto& inst : block.instructions) {
            for (il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                if (t->block != il::kNoBlock && t->block < remap.size()) t->block = remap[t->block];
            }
        }
        kept.push_back(std::move(block));
    }
    fn.blocks = std::move(kept);
    return true;
}

// A block parameter every incoming edge passes the same value is that value,
// when the value may be read where the block is: a guarded region's two copies
// are mutually exclusive and the verifier rejects a value of one read in the
// other, so a value crosses only from a `Shared` block or within one copy.
bool forwardParams(il::Function& fn) {
    const Defs d = collectDefs(fn);
    std::vector<uint8_t> isHandler(fn.blocks.size(), 0);
    for (const auto& block : fn.blocks) {
        if (block.handler != il::kNoBlock && block.handler < isHandler.size()) isHandler[block.handler] = 1;
    }
    struct Edge {
        il::BlockTarget* target;
    };
    std::vector<std::vector<Edge>> incoming(fn.blocks.size());
    for (auto& block : fn.blocks) {
        if (block.instructions.empty()) continue;
        il::Instruction& term = block.instructions.back();
        for (il::BlockTarget* t : {&term.target, &term.elseTarget}) {
            if (t->block != il::kNoBlock && t->block < incoming.size()) incoming[t->block].push_back({t});
        }
    }
    auto readableIn = [&](il::ValueId v, const il::Block& at) {
        if (v < fn.params.size()) return true;
        if (v >= d.block.size() || d.block[v] == il::kNoBlock) return false;
        const il::Block& def = fn.blocks[d.block[v]];
        if (def.copyClass == il::CopyClass::Shared) return true;
        return def.copyClass == at.copyClass && def.copyRegion == at.copyRegion;
    };
    std::vector<il::ValueId> to(fn.valueCount);
    for (il::ValueId v = 0; v < to.size(); ++v) to[v] = v;
    bool changed = false;
    for (size_t b = 1; b < fn.blocks.size(); ++b) {
        il::Block& block = fn.blocks[b];
        if (block.params.empty() || isHandler[b] || incoming[b].empty()) continue;
        for (size_t k = block.params.size(); k-- > 0;) {
            const il::ValueId self = block.params[k].id;
            il::ValueId only = il::kNoValue;
            bool uniform = true;
            for (const Edge& e : incoming[b]) {
                if (k >= e.target->args.size()) {
                    uniform = false;
                    break;
                }
                const il::ValueId v = e.target->args[k];
                if (v == self) continue;
                if (only == il::kNoValue) {
                    only = v;
                } else if (only != v) {
                    uniform = false;
                    break;
                }
            }
            if (!uniform || only == il::kNoValue || !readableIn(only, block)) continue;
            to[self] = only;
            block.params.erase(block.params.begin() + static_cast<ptrdiff_t>(k));
            for (const Edge& e : incoming[b]) e.target->args.erase(e.target->args.begin() + static_cast<ptrdiff_t>(k));
            changed = true;
        }
    }
    if (changed) applyRename(fn, to);
    return changed;
}

bool isPureDroppable(const il::Instruction& inst) {
    switch (inst.op) {
        case il::Op::Box:
            return inst.boxType == il::Type::F64 || inst.boxType == il::Type::Bool ||
                   inst.boxType == il::Type::I32;
        case il::Op::ConstF64:
        case il::Op::ConstI32:
        case il::Op::ConstBool:
        case il::Op::ConstUndefined:
        case il::Op::ConstNull:
        case il::Op::IsNumber:
        case il::Op::IsNullish:
        case il::Op::StrictEq:
            return true;
        case il::Op::Unbox:
            return inst.rawUnbox;
        default:
            return false;
    }
}

bool dropDead(il::Function& fn) {
    bool any = false;
    for (;;) {
        std::vector<uint8_t> used(fn.valueCount, 0);
        for (auto& block : fn.blocks) {
            for (auto& inst : block.instructions) {
                forEachUse(inst, [&](il::ValueId& v) {
                    if (v < used.size()) used[v] = 1;
                });
            }
        }
        bool removed = false;
        for (auto& block : fn.blocks) {
            const size_t before = block.instructions.size();
            std::erase_if(block.instructions, [&](const il::Instruction& inst) {
                return inst.result != il::kNoValue && inst.result < used.size() && !used[inst.result] &&
                       isPureDroppable(inst);
            });
            removed |= block.instructions.size() != before;
        }
        if (!removed) return any;
        any = true;
    }
}

}  // namespace

uint32_t foldTypedEntry(il::Function& fn) {
    uint32_t folds = 0;
    for (;;) {
        const uint32_t decided = foldValues(fn) + foldBranches(fn);
        folds += decided;
        const bool pruned = pruneBlocks(fn);
        const bool forwarded = forwardParams(fn);
        if (decided == 0 && !pruned && !forwarded) break;
    }
    dropDead(fn);
    return folds;
}

}  // namespace bronze::lower
