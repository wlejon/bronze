#include <algorithm>
#include <string>
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
// and, given the module's key constants, the tests a self-hosted builtin's
// body makes of what its caller passed once inlining has put the two side by
// side (selfhost_merge.cpp): a callback that is a function literal
//
//   typeof (create.func | func.ref) -> "function"; typeof box.f64|box.i32 ->
//     "number", box.bool -> "boolean", box.str -> "string"
//   strict.eq <either of those>, box.str k -> whether the strings are equal
//   strict.eq box.str k1, box.str k2 -> whether k1 and k2 are the same key
//   cmp.eq | cmp.ne of two const.bool -> the answer
//   rel.lt | rel.gt | rel.le | rel.ge of two box.f64 (const.f64) -> the answer
//
// and, below a branch on `is.plain_array v` or `is.array v` (its true arm,
// entered from that branch alone), what holds of v there because an Array
// stays an Array: `is.array v` -> true, and `$ToObject(v, _)`
// (`bronze_sh_to_object`) -> v itself;
//
// after which a `create.func` every remaining use of which is in one block
// that only its own block reaches (the call's slow path, once the builtin's
// copy takes the literal's body) moves into that block: the closure is made
// only where something still needs it.
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

// The string `v` is known to be: a `box.str`'s key, or the answer a
// `typeof` of a value of known kind gives. Null when not known.
const std::string* knownString(const Defs& d, il::ValueId v, const std::vector<std::string>* keys) {
    static const std::string kFunction = "function", kNumber = "number", kBoolean = "boolean",
                             kString = "string";
    if (keys == nullptr || v >= d.inst.size() || d.inst[v] == nullptr) return nullptr;
    const il::Instruction& def = *d.inst[v];
    if (def.op == il::Op::Box && def.boxType == il::Type::Str && def.operands.empty()) {
        return def.keyIndex < keys->size() ? &(*keys)[def.keyIndex] : nullptr;
    }
    if (def.op != il::Op::TypeOf || def.operands.size() != 1) return nullptr;
    const il::ValueId of = def.operands[0];
    if (of >= d.inst.size() || d.inst[of] == nullptr) return nullptr;
    const il::Instruction& src = *d.inst[of];
    if (src.op == il::Op::CreateFunction || src.op == il::Op::FunctionRef) return &kFunction;
    if (src.op != il::Op::Box || src.operands.size() != 1) return nullptr;
    switch (src.boxType) {
        case il::Type::F64:
        case il::Type::I32:
            return &kNumber;
        case il::Type::Bool:
            return &kBoolean;
        case il::Type::Str:
            return &kString;
        default:
            return nullptr;
    }
}

// The number `v` boxes when it is `box.f64 (const.f64 x)`, else null.
const double* constNumber(const Defs& d, il::ValueId v) {
    const il::Instruction* box = boxDef(d, v);
    if (box == nullptr || box->boxType != il::Type::F64) return nullptr;
    const il::ValueId x = box->operands[0];
    if (x >= d.inst.size() || d.inst[x] == nullptr || d.inst[x]->op != il::Op::ConstF64) return nullptr;
    return &d.inst[x]->immF64;
}

// The value-level rules. Returns how many fired.
uint32_t foldValues(il::Function& fn, const std::vector<std::string>* keys) {
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
                } else if (const std::string* x = knownString(d, inst.operands[0], keys)) {
                    if (const std::string* y = knownString(d, inst.operands[1], keys)) {
                        makeConstBool(inst, *x == *y);
                        ++folds;
                    }
                }
            } else if ((inst.op == il::Op::RelLt || inst.op == il::Op::RelGt || inst.op == il::Op::RelLe ||
                        inst.op == il::Op::RelGe) &&
                       inst.operands.size() == 2 && constNumber(d, inst.operands[0]) &&
                       constNumber(d, inst.operands[1])) {
                // Two boxed constants: reduce's argument count against 1.
                const double x = *constNumber(d, inst.operands[0]);
                const double y = *constNumber(d, inst.operands[1]);
                const bool r = inst.op == il::Op::RelLt   ? x < y
                               : inst.op == il::Op::RelGt ? x > y
                               : inst.op == il::Op::RelLe ? x <= y
                                                          : x >= y;
                makeConstBool(inst, r);
                ++folds;
            } else if ((inst.op == il::Op::CmpEq || inst.op == il::Op::CmpNe) && inst.operands.size() == 2 &&
                       constOp(d, inst.operands[0]) == il::Op::ConstBool &&
                       constOp(d, inst.operands[1]) == il::Op::ConstBool) {
                const bool same = (d.inst[inst.operands[0]]->immI32 != 0) == (d.inst[inst.operands[1]]->immI32 != 0);
                makeConstBool(inst, inst.op == il::Op::CmpEq ? same : !same);
                ++folds;
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

uint32_t foldArrayFacts(il::Function& fn, const il::Module& module) {
    const Defs d = collectDefs(fn);
    const Cfg cfg = buildCfg(fn);
    // (arm, v): the true arm of a test that v is an Array.
    std::vector<std::pair<il::BlockId, il::ValueId>> facts;
    for (const auto& block : fn.blocks) {
        if (block.instructions.empty()) continue;
        const il::Instruction& term = block.instructions.back();
        if (term.op != il::Op::Branch || term.operands.size() != 1) continue;
        const il::ValueId c = term.operands[0];
        if (c >= d.inst.size() || d.inst[c] == nullptr) continue;
        const il::Instruction& test = *d.inst[c];
        if ((test.op != il::Op::IsPlainArray && test.op != il::Op::IsArray) || test.operands.size() != 1) continue;
        const il::BlockId arm = term.target.block;
        if (arm == il::kNoBlock || arm >= fn.blocks.size() || arm == term.elseTarget.block ||
            cfg.preds[arm].size() != 1) {
            continue;
        }
        facts.emplace_back(arm, test.operands[0]);
    }
    if (facts.empty()) return 0;
    const auto isArrayAt = [&](il::ValueId v, il::BlockId at) {
        for (const auto& [arm, of] : facts) {
            if (of == v && cfg.dominates(arm, at)) return true;
        }
        return false;
    };
    std::vector<il::ValueId> to(fn.valueCount);
    for (il::ValueId v = 0; v < to.size(); ++v) to[v] = v;
    bool renamed = false;
    uint32_t folds = 0;
    for (auto& block : fn.blocks) {
        if (cfg.rpoIndex[block.id] == UINT32_MAX) continue;
        std::vector<size_t> erase;
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            il::Instruction& inst = block.instructions[i];
            if (inst.op == il::Op::IsArray && inst.operands.size() == 1 && isArrayAt(inst.operands[0], block.id)) {
                makeConstBool(inst, true);
                ++folds;
            } else if (inst.op == il::Op::Call && inst.operands.size() == 2 && inst.result != il::kNoValue &&
                       inst.result < to.size() && inst.calleeIndex < module.functions.size() &&
                       module.functions[inst.calleeIndex].isExternal &&
                       module.functions[inst.calleeIndex].name == "bronze_sh_to_object" &&
                       isArrayAt(inst.operands[0], block.id)) {
                to[inst.result] = inst.operands[0];
                renamed = true;
                erase.push_back(i);
                ++folds;
            }
        }
        for (size_t k = erase.size(); k-- > 0;) {
            block.instructions.erase(block.instructions.begin() + static_cast<ptrdiff_t>(erase[k]));
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
                   inst.boxType == il::Type::I32 || inst.boxType == il::Type::Str;
        // An allocation nothing reads: the closure has no effect but its
        // existence.
        case il::Op::CreateFunction:
        case il::Op::FunctionRef:
        case il::Op::TypeOf:
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

// A `create.func` whose every use is in one other block U that is reached
// only through a chain of single-predecessor blocks from its own moves to the
// head of U: U runs at most as often as the defining block and only after
// it, so the operands still dominate and no path makes the closure where it
// did not. Handler blocks and blocks under another handler stay put.
uint32_t sinkClosures(il::Function& fn) {
    const size_t n = fn.blocks.size();
    std::vector<uint32_t> predCount(n, 0);
    std::vector<il::BlockId> onlyPred(n, il::kNoBlock);
    std::vector<uint8_t> isHandler(n, 0);
    for (const auto& block : fn.blocks) {
        if (block.handler != il::kNoBlock && block.handler < n) isHandler[block.handler] = 1;
        if (block.instructions.empty()) continue;
        const il::Instruction& term = block.instructions.back();
        il::BlockId seen = il::kNoBlock;
        for (const il::BlockTarget* t : {&term.target, &term.elseTarget}) {
            if (t->block == il::kNoBlock || t->block >= n || t->block == seen) continue;
            seen = t->block;
            ++predCount[t->block];
            onlyPred[t->block] = block.id;
        }
    }
    // Where each value is used: one block, or kNoBlock with `multi` set.
    std::vector<il::BlockId> useBlock(fn.valueCount, il::kNoBlock);
    std::vector<uint8_t> multi(fn.valueCount, 0);
    for (auto& block : fn.blocks) {
        for (auto& inst : block.instructions) {
            forEachUse(inst, [&](il::ValueId& v) {
                if (v >= useBlock.size()) return;
                if (useBlock[v] == il::kNoBlock) useBlock[v] = block.id;
                else if (useBlock[v] != block.id) multi[v] = 1;
            });
        }
    }
    uint32_t sunk = 0;
    for (size_t b = 0; b < n; ++b) {
        auto& insts = fn.blocks[b].instructions;
        for (size_t i = 0; i < insts.size();) {
            const il::Instruction& inst = insts[i];
            const il::ValueId r = inst.result;
            bool move = inst.op == il::Op::CreateFunction && r < useBlock.size() && !multi[r] &&
                        useBlock[r] != il::kNoBlock && useBlock[r] != b;
            const il::BlockId to = move ? useBlock[r] : il::kNoBlock;
            if (move) {
                // The chain back from U to b: single predecessors, no handler
                // block, the same handler as b throughout.
                il::BlockId at = to;
                for (size_t steps = 0; move && at != b; ++steps) {
                    if (steps > n || isHandler[at] || predCount[at] != 1 ||
                        fn.blocks[at].handler != fn.blocks[b].handler) {
                        move = false;
                        break;
                    }
                    at = onlyPred[at];
                }
            }
            if (!move) {
                ++i;
                continue;
            }
            il::Instruction moved = std::move(insts[i]);
            insts.erase(insts.begin() + static_cast<ptrdiff_t>(i));
            auto& dest = fn.blocks[to].instructions;
            dest.insert(dest.begin(), std::move(moved));
            ++sunk;
        }
    }
    return sunk;
}

}  // namespace

uint32_t foldTypedEntry(il::Function& fn, const il::Module* module) {
    const std::vector<std::string>* keys = module != nullptr ? &module->keyConstants : nullptr;
    uint32_t folds = 0;
    for (;;) {
        const uint32_t decided =
            foldValues(fn, keys) + foldBranches(fn) + (module != nullptr ? foldArrayFacts(fn, *module) : 0);
        folds += decided;
        const bool pruned = pruneBlocks(fn);
        const bool forwarded = forwardParams(fn);
        if (decided == 0 && !pruned && !forwarded) break;
    }
    dropDead(fn);
    if (keys != nullptr) folds += sinkClosures(fn);
    return folds;
}

}  // namespace bronze::lower
