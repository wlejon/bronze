// REPEATED GUARDED LOADS (guarded_load_fold.h).

#include "lower/guarded_load_fold.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

struct Content {
    il::ValueId receiver;
    uint32_t slot;
    il::ValueId value;
    bool operator==(const Content&) const = default;
};

// An `is.shape` that held: a cell, or a family range.
struct Shape {
    il::ValueId receiver;
    uint32_t cell;
    uint32_t familyLo;
    uint32_t familySpan;
    bool operator==(const Shape&) const = default;
    bool family() const { return familyLo != il::Instruction::kNoFamily; }
    // Whether this test holding says `later` holds.
    bool answers(const Shape& later) const {
        if (receiver != later.receiver || family() != later.family()) return false;
        if (!family()) return cell == later.cell;
        return later.familyLo <= familyLo && familyLo + familySpan <= later.familyLo + later.familySpan;
    }
};

struct Facts {
    std::vector<Content> contents;
    std::vector<Shape> shapes;
    std::vector<il::ValueId> numbers;
    std::vector<std::pair<il::ValueId, il::ValueId>> doubles;  // value, its `unbox.f64 raw`
};

template <typename T>
void keepCommon(std::vector<T>& into, const std::vector<T>& other) {
    into.erase(std::remove_if(into.begin(), into.end(),
                              [&](const T& x) { return std::find(other.begin(), other.end(), x) == other.end(); }),
               into.end());
}

bool bareSite(const il::Instruction& inst, il::Op op, size_t operands) {
    return inst.op == op && inst.shapeGuarded && inst.operands.size() == operands &&
           inst.staticSlot != il::Instruction::kNoStaticSlot;
}

bool rawUnbox(const il::Instruction& inst) {
    return inst.op == il::Op::Unbox && inst.rawUnbox && inst.type == il::Type::F64 && inst.operands.size() == 1 &&
           inst.result != il::kNoValue;
}

struct Folder {
    il::Function fn;
    Cfg cfg;
    std::unordered_map<il::ValueId, il::ValueId> rename;
    std::unordered_map<il::ValueId, il::ValueId> boxed;  // a `box.f64`'s result -> its double
    std::unordered_map<il::ValueId, Shape> shapeTest;    // an `is.shape`'s result -> what it asks
    std::unordered_map<il::ValueId, il::ValueId> numberTest;  // an `is.number`'s result -> its operand
    std::unordered_set<il::ValueId> foldedTrue;
    std::vector<Facts> out;
    bool changed = false;
    bool pruned = false;

    explicit Folder(const il::Function& f) : fn(f), cfg(buildCfg(f)), out(f.blocks.size()) {}

    il::ValueId now(il::ValueId v) const {
        const auto it = rename.find(v);
        return it == rename.end() ? v : it->second;
    }

    // The facts at the end of `p` as its edge into `b` carries them.
    Facts along(il::BlockId p, il::BlockId b) const {
        Facts facts = out[p];
        const il::Instruction& term = fn.blocks[p].instructions.back();
        if (term.op != il::Op::Branch || term.operands.size() != 1 || term.target.block != b ||
            term.elseTarget.block == b) {
            return facts;
        }
        if (const auto s = shapeTest.find(term.operands[0]); s != shapeTest.end()) facts.shapes.push_back(s->second);
        if (const auto n = numberTest.find(term.operands[0]); n != numberTest.end()) {
            facts.numbers.push_back(n->second);
        }
        return facts;
    }

    Facts entering(il::BlockId b, const std::vector<uint8_t>& handler) const {
        Facts facts;
        if (handler[b] != 0 || cfg.preds[b].empty()) return facts;
        for (il::BlockId p : cfg.preds[b]) {
            if (cfg.rpoIndex[p] == UINT32_MAX || cfg.rpoIndex[p] >= cfg.rpoIndex[b]) return Facts{};
        }
        bool first = true;
        for (il::BlockId p : cfg.preds[b]) {
            Facts edge = along(p, b);
            if (first) {
                facts = std::move(edge);
                first = false;
                continue;
            }
            keepCommon(facts.contents, edge.contents);
            keepCommon(facts.shapes, edge.shapes);
            keepCommon(facts.numbers, edge.numbers);
            keepCommon(facts.doubles, edge.doubles);
        }
        return facts;
    }

    void makeTrue(il::Instruction& inst) {
        const il::ValueId result = inst.result;
        inst = il::Instruction{};
        inst.op = il::Op::ConstBool;
        inst.type = il::Type::Bool;
        inst.result = result;
        inst.immI32 = 1;
        foldedTrue.insert(result);
        changed = true;
    }

    // False when the instruction is dropped: its result has another name.
    bool visit(il::Instruction& inst, Facts& facts) {
        rewriteUses(inst, [&](il::ValueId v) { return now(v); });
        if (inst.op == il::Op::Box && inst.boxType == il::Type::F64 && inst.operands.size() == 1 &&
            inst.result != il::kNoValue) {
            boxed.emplace(inst.result, inst.operands[0]);
            return true;
        }
        if (bareSite(inst, il::Op::PropGet, 1) && inst.result != il::kNoValue) {
            for (const Content& c : facts.contents) {
                if (c.receiver != inst.operands[0] || c.slot != inst.staticSlot) continue;
                rename[inst.result] = c.value;
                changed = true;
                return false;
            }
            facts.contents.push_back({inst.operands[0], inst.staticSlot, inst.result});
            return true;
        }
        if (bareSite(inst, il::Op::PropSet, 2)) {
            std::erase_if(facts.contents, [&](const Content& c) { return c.slot == inst.staticSlot; });
            facts.contents.push_back({inst.operands[0], inst.staticSlot, inst.operands[1]});
            return true;
        }
        if (inst.op == il::Op::IsShape && inst.operands.size() == 1 && inst.result != il::kNoValue) {
            const Shape asked{inst.operands[0], inst.staticCellIndex, inst.familyLo, inst.familySpan};
            const bool held = std::any_of(facts.shapes.begin(), facts.shapes.end(),
                                          [&](const Shape& s) { return s.answers(asked); });
            if (held) {
                makeTrue(inst);
            } else {
                shapeTest[inst.result] = asked;
            }
            return true;
        }
        if (inst.op == il::Op::IsNumber && inst.operands.size() == 1 && inst.result != il::kNoValue) {
            const il::ValueId v = inst.operands[0];
            if (boxed.count(v) != 0 || std::find(facts.numbers.begin(), facts.numbers.end(), v) != facts.numbers.end()) {
                makeTrue(inst);
            } else {
                numberTest[inst.result] = v;
            }
            return true;
        }
        if (rawUnbox(inst)) {
            const il::ValueId v = inst.operands[0];
            il::ValueId is = il::kNoValue;
            if (const auto b = boxed.find(v); b != boxed.end()) is = now(b->second);
            for (const auto& [value, d] : facts.doubles) {
                if (value == v) is = d;
            }
            if (is != il::kNoValue) {
                rename[inst.result] = is;
                changed = true;
                return false;
            }
            facts.doubles.push_back({v, inst.result});
            return true;
        }
        if (inst.op == il::Op::Branch && inst.operands.size() == 1 && foldedTrue.count(inst.operands[0]) != 0) {
            inst.op = il::Op::Jump;
            inst.operands.clear();
            inst.elseTarget = il::BlockTarget{};
            pruned = true;
            return true;
        }
        if (il::canCollect(inst)) {
            facts.contents.clear();
            facts.shapes.clear();
        }
        return true;
    }

    bool run() {
        std::vector<uint8_t> handler(fn.blocks.size(), 0);
        for (const il::Block& block : fn.blocks) {
            if (block.handler != il::kNoBlock && block.handler < handler.size()) handler[block.handler] = 1;
        }
        for (il::BlockId b : cfg.rpo) {
            Facts facts = entering(b, handler);
            auto& insts = fn.blocks[b].instructions;
            std::vector<il::Instruction> kept;
            kept.reserve(insts.size());
            for (il::Instruction& inst : insts) {
                if (visit(inst, facts)) kept.push_back(std::move(inst));
            }
            insts = std::move(kept);
            out[b] = std::move(facts);
        }
        if (!changed) return false;
        // A use in a block walked before the read it named was dropped (a back
        // edge's argument) takes the read's other name here.
        for (il::Block& block : fn.blocks) {
            for (il::Instruction& inst : block.instructions) {
                rewriteUses(inst, [&](il::ValueId v) { return now(v); });
            }
        }
        if (pruned) {
            std::vector<uint8_t> unused(fn.blocks.size(), 0);
            pruneUnreachable(fn, unused);
        }
        return dominanceHolds(fn);
    }
};

bool worthALook(const il::Function& fn) {
    for (const il::Block& block : fn.blocks) {
        for (const il::Instruction& inst : block.instructions) {
            if (inst.shapeGuarded && il::layoutFactOperand(inst) == il::kNoValue) return true;
        }
    }
    return false;
}

}  // namespace

bool foldGuardedLoads(il::Module& module) {
    bool changed = false;
    for (il::Function& fn : module.functions) {
        if (fn.isExternal || fn.blocks.empty() || !worthALook(fn)) continue;
        Folder folder(fn);
        if (!folder.run()) continue;
        fn = std::move(folder.fn);
        changed = true;
    }
    return changed;
}

}  // namespace bronze::lower
