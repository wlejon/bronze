#include "opt/repr/rewrite.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include "il/effects.h"
#include "opt/repr/encoding.h"

namespace bronze::opt::repr {

using il::Op;
using il::Type;

std::optional<BoxSource> boxSource(const il::Function& fn, const DefUse& defs, ValueId v) {
    const il::Instruction* d = defs.defInstIs(fn, v, Op::Box);
    if (d == nullptr || d->operands.size() != 1 || d->type != Type::Dynamic) return std::nullopt;
    const ValueId x = d->operands[0];
    if (defs.type(x) != d->boxType) return std::nullopt;
    switch (d->boxType) {
        case Type::I32:
        case Type::F64:
        case Type::Bool:
        case Type::Str:
            return BoxSource{d->boxType, x};
        default:
            return std::nullopt;
    }
}

std::optional<double> constF64(const il::Function& fn, const DefUse& defs, ValueId v) {
    const il::Instruction* d = defs.defInstIs(fn, v, Op::ConstF64);
    if (d == nullptr || d->type != Type::F64) return std::nullopt;
    return d->immF64;
}

std::optional<Int32Source> exactInt32(const il::Function& fn, const DefUse& defs, ValueId f64) {
    if (defs.type(f64) != Type::F64) return std::nullopt;
    if (const il::Instruction* d = defs.defInstIs(fn, f64, Op::I32ToF64)) {
        if (d->operands.size() == 1 && defs.type(d->operands[0]) == Type::I32) {
            return Int32Source{false, 0, d->operands[0]};
        }
        return std::nullopt;
    }
    if (const std::optional<double> c = constF64(fn, defs, f64); c && isInt32Double(*c)) {
        return Int32Source{true, static_cast<int32_t>(*c), kNoValue};
    }
    return std::nullopt;
}

il::Instruction makeBox(il::Function& fn, ValueId x, Type type, const Span& span) {
    il::Instruction inst;
    inst.op = Op::Box;
    inst.type = Type::Dynamic;
    inst.boxType = type;
    inst.result = newValue(fn);
    inst.operands = {x};
    inst.span = span;
    return inst;
}

il::Instruction makeI32ToF64(il::Function& fn, ValueId x, const Span& span) {
    il::Instruction inst;
    inst.op = Op::I32ToF64;
    inst.type = Type::F64;
    inst.result = newValue(fn);
    inst.operands = {x};
    inst.span = span;
    return inst;
}

il::Instruction makeConstI32(il::Function& fn, int32_t value, const Span& span) {
    il::Instruction inst;
    inst.op = Op::ConstI32;
    inst.type = Type::I32;
    inst.result = newValue(fn);
    inst.immI32 = value;
    inst.span = span;
    return inst;
}

void rewriteInPlace(il::Instruction& inst, Op op, std::vector<ValueId> operands, Type boxType) {
    il::Instruction out;
    out.op = op;
    out.type = inst.type;
    out.result = inst.result;
    out.operands = std::move(operands);
    out.boxType = boxType;
    out.span = inst.span;
    inst = std::move(out);
}

void EditPlan::insertBefore(BlockId block, size_t before, il::Instruction inst) {
    inserts_.push_back(Insert{block, before, inserts_.size(), std::move(inst)});
}

void EditPlan::remove(BlockId block, size_t index) { removes_.push_back(Remove{block, index}); }

bool EditPlan::apply(il::Function& fn) {
    if (empty()) return false;
    // Grouped by block, then position, then the order they were planned in,
    // so the result is a function of the plan alone.
    std::stable_sort(inserts_.begin(), inserts_.end(), [](const Insert& a, const Insert& b) {
        if (a.block != b.block) return a.block < b.block;
        if (a.before != b.before) return a.before < b.before;
        return a.order < b.order;
    });
    std::vector<BlockId> touched;
    for (const Insert& i : inserts_) touched.push_back(i.block);
    for (const Remove& r : removes_) touched.push_back(r.block);
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

    size_t next = 0;
    for (BlockId b : touched) {
        auto& insts = fn.blocks[b].instructions;
        std::vector<uint8_t> dead(insts.size(), 0);
        for (const Remove& r : removes_) {
            if (r.block != b) continue;
            assert(r.index < insts.size() && !il::isTerminator(insts[r.index].op));
            dead[r.index] = 1;
        }
        while (next < inserts_.size() && inserts_[next].block < b) ++next;
        std::vector<il::Instruction> out;
        out.reserve(insts.size() + 4);
        for (size_t i = 0; i < insts.size(); ++i) {
            while (next < inserts_.size() && inserts_[next].block == b && inserts_[next].before == i) {
                out.push_back(std::move(inserts_[next].inst));
                ++next;
            }
            if (!dead[i]) out.push_back(std::move(insts[i]));
        }
        // Nothing goes after the terminator.
        assert(next >= inserts_.size() || inserts_[next].block != b);
        insts = std::move(out);
    }
    inserts_.clear();
    removes_.clear();
    return true;
}

size_t removeDeadInstructions(il::Function& fn) {
    size_t removed = 0;
    for (;;) {
        const DefUse defs(fn);
        size_t round = 0;
        for (il::Block& block : fn.blocks) {
            auto& insts = block.instructions;
            const auto end = std::remove_if(insts.begin(), insts.end(), [&](const il::Instruction& inst) {
                if (inst.result == kNoValue || il::isTerminator(inst.op) || !defs.unused(inst.result)) {
                    return false;
                }
                return il::effectsOf(inst).removableIfUnused;
            });
            round += static_cast<size_t>(insts.end() - end);
            insts.erase(end, insts.end());
        }
        removed += round;
        if (round == 0) return removed;
    }
}

}  // namespace bronze::opt::repr
