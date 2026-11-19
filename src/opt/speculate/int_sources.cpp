// INT SOURCES: which int-form arithmetic plans keep their int form.
//
// A boxed Number is the double's bits (runtime/value.h). The int form of
// `+ - * ++ --`, of a compare and of ToBoolean begins with `is.i32` and
// `unbox.i32.unchecked` on each Dynamic operand: a convert, a convert back and
// three compares when the value really is a double box, and nothing once
// representation selection (opt/repr) has paired the unbox with the
// `box.i32` that made the value. And its result is boxed again as a double,
// a conversion of its own wherever it is stored. So the form pays only where
// its operands come from int-producing operations of the same function: a
// loop counter, a sum of counters, a length. On a value read from the heap
// (a property, an element, a closure's variable, a call's result, a
// parameter) the double form costs one compare and a move per operand, and
// the evidence that the value happened to be an int32 on the harvested run
// buys nothing but conversions.
//
// So a Dynamic value is an INT SOURCE when it is
//   - typed I32, or the box of an I32, or the box of an int32 constant;
//   - the result of a site planned in the Int or Bits form (not `>>>`, a
//     uint32), or a planned `array.length`, or a planned ToNumeric of one;
//   - a block parameter at least one of whose incoming values is one (a loop
//     counter entered from a parameter is unboxed once per loop entry and is
//     an i32 on every iteration after).
// An Int-form plan of `+`, `-`, `*`, `++`/`--`, a compare or ToBoolean whose
// Dynamic operands are not all int sources becomes Double. `%` keeps its int
// form: its double form is a library remainder, dearer than the test.
// The sources are a greatest fixpoint: everything planned Int starts as a
// source and is demoted until nothing changes, so a counter whose only int
// input is its own increment stays int.
//
// An INDEX keeps its int form whatever its operands are: a sum or product
// whose result is the index of a planned element access (`v[i + 3]`). The
// access converts its index to an int32 anyway, so the int form moves that
// test onto the operands, where a value used in several indices (a
// parameter `i` in `v[i]`, `v[i + 1]`, ...) is unboxed once.

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "opt/speculate/evidence.h"
#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

namespace {

bool demotable(il::Op op) {
    switch (op) {
        case il::Op::Add: case il::Op::Sub: case il::Op::Mul: case il::Op::NumericStep:
        case il::Op::RelLt: case il::Op::RelGt: case il::Op::RelLe: case il::Op::RelGe:
        case il::Op::StrictEq: case il::Op::LooseEq:
        case il::Op::Unbox:
            return true;
        default:
            return false;
    }
}

bool int32Constant(double d) {
    return d >= -2147483648.0 && d <= 2147483647.0 && static_cast<double>(static_cast<int32_t>(d)) == d &&
           !(d == 0 && std::signbit(d));
}

}  // namespace

void demoteUnsourcedInts(const il::Function& fn, FunctionPlan& plan) {
    struct Def {
        BlockId block = il::kNoBlock;
        uint32_t index = 0;  // instruction; UINT32_MAX for a block parameter
        uint32_t param = 0;
    };
    std::unordered_map<ValueId, Def> defs;
    std::unordered_map<uint64_t, size_t> planAt;  // (block, index) -> plan.sites index
    auto at = [](BlockId b, uint32_t i) { return (uint64_t{b} << 32) | i; };
    for (size_t s = 0; s < plan.sites.size(); ++s) planAt[at(plan.sites[s].block, plan.sites[s].index)] = s;

    // Incoming values of each block parameter.
    std::unordered_map<ValueId, std::vector<ValueId>> incoming;
    std::unordered_map<ValueId, il::Type> types;
    for (BlockId b = 0; b < fn.blocks.size(); ++b) {
        const il::Block& block = fn.blocks[b];
        for (uint32_t p = 0; p < block.params.size(); ++p) {
            defs[block.params[p].id] = Def{b, UINT32_MAX, p};
            types[block.params[p].id] = block.params[p].type;
        }
        for (uint32_t i = 0; i < block.instructions.size(); ++i) {
            const il::Instruction& inst = block.instructions[i];
            if (inst.result != il::kNoValue) {
                defs[inst.result] = Def{b, i, 0};
                types[inst.result] = inst.type;
            }
            for (const il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                if (t->block == il::kNoBlock || t->block >= fn.blocks.size()) continue;
                const auto& params = fn.blocks[t->block].params;
                for (size_t a = 0; a < t->args.size() && a < params.size(); ++a) {
                    incoming[params[a].id].push_back(t->args[a]);
                }
            }
        }
    }

    for (ValueId v = 0; v < fn.params.size(); ++v) types[v] = fn.params[v].type;

    // The indices of the planned element accesses, closed backward through
    // block-parameter incoming edges and int-planned index arithmetic so
    // loop-carried index offsets (e.g. `uvsOffset *= s; uvsOffset += s`) stay
    // int32 instead of being demoted to doubles.
    std::unordered_set<ValueId> indices;
    std::vector<ValueId> indexWork;
    auto pushIndex = [&](ValueId id) {
        if (id != il::kNoValue && indices.insert(id).second) indexWork.push_back(id);
    };
    for (const SitePlan& p : plan.sites) {
        if (p.kind != SiteKind::ElemRead && p.kind != SiteKind::ElemWrite) continue;
        const il::Instruction& inst = fn.blocks[p.block].instructions[p.index];
        if ((inst.op == il::Op::ElemGet || inst.op == il::Op::ElemSet) && inst.operands.size() >= 2) {
            pushIndex(inst.operands[1]);
        }
    }
    for (size_t wi = 0; wi < indexWork.size(); ++wi) {
        const ValueId v = indexWork[wi];
        const auto dIt = defs.find(v);
        if (dIt == defs.end()) continue;
        if (dIt->second.index == UINT32_MAX) {
            if (const auto inIt = incoming.find(v); inIt != incoming.end()) {
                for (ValueId in : inIt->second) pushIndex(in);
            }
            continue;
        }
        const auto pIt = planAt.find(at(dIt->second.block, dIt->second.index));
        if (pIt == planAt.end()) continue;
        const SitePlan& sp = plan.sites[pIt->second];
        if (sp.kind != SiteKind::Arith || sp.form != ArithForm::Int) continue;
        const il::Instruction& dInst = fn.blocks[dIt->second.block].instructions[dIt->second.index];
        if (dInst.op != il::Op::Add && dInst.op != il::Op::Sub && dInst.op != il::Op::Mul &&
            dInst.op != il::Op::NumericStep) {
            continue;
        }
        for (ValueId opd : dInst.operands) {
            const auto opDef = defs.find(opd);
            if (opDef != defs.end() && opDef->second.index == UINT32_MAX) pushIndex(opd);
        }
    }

    std::vector<ValueId> values;
    values.reserve(defs.size());
    for (const auto& [v, d] : defs) values.push_back(v);
    std::sort(values.begin(), values.end());
    // The current answer per value, true until shown otherwise.
    std::unordered_map<ValueId, bool> source;
    auto isSource = [&](ValueId v) {
        if (types[v] == il::Type::I32) return true;
        auto it = source.find(v);
        return it != source.end() && it->second;
    };
    auto siteIsInt = [&](size_t s) {
        const SitePlan& p = plan.sites[s];
        if (p.kind == SiteKind::ArrayLength) return true;
        if (p.kind != SiteKind::Arith) return false;
        const il::Instruction& inst = fn.blocks[p.block].instructions[p.index];
        if (p.form == ArithForm::Bits) return inst.op != il::Op::UShr;
        return p.form == ArithForm::Int;
    };
    auto evaluate = [&](ValueId v) -> bool {
        const il::Type type = types[v];
        if (type == il::Type::I32) return true;
        if (type != il::Type::Dynamic) return false;
        const Def& d = defs.at(v);
        if (d.index == UINT32_MAX) {
            for (ValueId in : incoming[v]) {
                if (isSource(in)) return true;
            }
            return false;
        }
        const il::Instruction& inst = fn.blocks[d.block].instructions[d.index];
        if (inst.op == il::Op::Box) {
            if (inst.boxType == il::Type::I32) return true;
            if (inst.boxType != il::Type::F64 || inst.operands.empty()) return false;
            auto c = defs.find(inst.operands[0]);
            if (c == defs.end() || c->second.index == UINT32_MAX) return false;
            const il::Instruction& k = fn.blocks[c->second.block].instructions[c->second.index];
            return k.op == il::Op::ConstF64 && int32Constant(k.immF64);
        }
        auto p = planAt.find(at(d.block, d.index));
        if (p == planAt.end()) return false;
        if (inst.op == il::Op::ToNumeric && plan.sites[p->second].kind == SiteKind::Arith) {
            return isSource(inst.operands[0]);
        }
        return siteIsInt(p->second);
    };

    for (ValueId v : values) source[v] = true;
    for (bool changed = true; changed;) {
        changed = false;
        for (ValueId v : values) {
            if (!source[v] || evaluate(v)) continue;
            source[v] = false;
            changed = true;
        }
        for (SitePlan& p : plan.sites) {
            if (p.kind != SiteKind::Arith || p.form != ArithForm::Int) continue;
            const il::Instruction& inst = fn.blocks[p.block].instructions[p.index];
            if (!demotable(inst.op)) continue;
            if (inst.result != il::kNoValue && indices.count(inst.result) != 0 &&
                (inst.op == il::Op::Add || inst.op == il::Op::Sub || inst.op == il::Op::Mul ||
                 inst.op == il::Op::NumericStep)) {
                continue;
            }
            for (ValueId v : inst.operands) {
                if (types[v] != il::Type::Dynamic || isSource(v)) continue;
                p.form = ArithForm::Double;
                changed = true;
                break;
            }
        }
    }
}

}  // namespace bronze::opt::speculate
