#include "lower/typed_loop_index_proof.h"

#include <algorithm>
#include <cstdint>

namespace bronze::lower {

// The argument `pred` passes for parameter `k` of `to`, or kNoValue when
// it reaches `to` some other way (a handler) or on both edges differently.
il::ValueId incomingArg(const il::Function& fn, il::BlockId pred, il::BlockId to, size_t k) {
    const il::Block& pb = fn.blocks[pred];
    if (pb.instructions.empty()) return il::kNoValue;
    const il::Instruction& t = pb.instructions.back();
    il::ValueId v = il::kNoValue;
    const auto take = [&](const il::BlockTarget& tgt) {
        if (tgt.block != to) return true;
        if (k >= tgt.args.size()) return false;
        if (v != il::kNoValue && v != tgt.args[k]) return false;
        v = tgt.args[k];
        return true;
    };
    if (t.op == il::Op::Jump) {
        if (!take(t.target)) return il::kNoValue;
    } else if (t.op == il::Op::Branch) {
        if (!take(t.target) || !take(t.elseTarget)) return il::kNoValue;
    }
    return v;
}

// The one value every predecessor of `b` passes for its parameter `v`.
il::ValueId soleIncoming(const il::Function& fn, const Cfg& cfg, il::BlockId b, il::ValueId v) {
    const il::Block& blk = fn.blocks[b];
    size_t k = 0;
    while (k < blk.params.size() && blk.params[k].id != v) ++k;
    if (k == blk.params.size() || cfg.preds[b].empty()) return il::kNoValue;
    il::ValueId sole = il::kNoValue;
    for (il::BlockId pred : cfg.preds[b]) {
        const il::ValueId in = incomingArg(fn, pred, b, k);
        if (in == il::kNoValue || (sole != il::kNoValue && in != sole)) return il::kNoValue;
        sole = in;
    }
    return sole;
}

size_t proveIntegralIndices(il::Function& fn, const Cfg& cfg, const std::vector<DefSite>& defs, il::BlockId header,
                            const std::vector<il::ValueId>& lengths,
                            const std::vector<std::pair<il::BlockId, uint32_t>>& sites, bool boxedIndex,
                            std::vector<il::ValueId>* onEntry) {
    constexpr double kStepBound = 2147483648.0;
    const auto defOf = [&](il::ValueId v) -> const il::Instruction* {
        if (v >= defs.size() || defs[v].block == il::kNoBlock || defs[v].index == UINT32_MAX) return nullptr;
        return &fn.blocks[defs[v].block].instructions[defs[v].index];
    };
    const auto constIn = [&](il::ValueId v, double lo, double hi) {
        const il::Instruction* d = defOf(v);
        // The range first: it refuses NaN, and the cast is then defined.
        return d && d->op == il::Op::ConstF64 && d->immF64 >= lo && d->immF64 <= hi &&
               d->immF64 == static_cast<double>(static_cast<int64_t>(d->immF64));
    };
    // `v` with block-parameter copies seen through: a parameter whose
    // every incoming argument is the same value is that value.
    const auto through = [&](il::ValueId v) {
        for (int hops = 0; hops < 8; ++hops) {
            if (v >= defs.size() || defs[v].block == il::kNoBlock || defs[v].index != UINT32_MAX) return v;
            const il::BlockId b = defs[v].block;
            if (b == header) return v;
            const il::ValueId in = soleIncoming(fn, cfg, b, v);
            if (in == il::kNoValue) return v;
            v = in;
        }
        return v;
    };
    // An f64 computed ahead of the loop: the same number on every iteration,
    // and one the copy's entry can test.
    const auto aheadOfLoop = [&](il::ValueId v) {
        const il::Instruction* d = defOf(v);
        return onEntry != nullptr && d != nullptr && d->type == il::Type::F64 && defs[v].block != header &&
               !cfg.dominates(header, defs[v].block);
    };
    // `in` is `p` advanced by non-negative integral constants (`p + 1 + 1`).
    const auto advances = [&](il::ValueId in, il::ValueId p) {
        for (int hops = 0; hops < 16; ++hops) {
            in = through(in);
            if (in == p) return true;
            const il::Instruction* step = defOf(in);
            if (!step || step->op != il::Op::Add || step->type != il::Type::F64 || step->operands.size() != 2) {
                return false;
            }
            if (constIn(step->operands[1], 0, kStepBound)) {
                in = step->operands[0];
            } else if (constIn(step->operands[0], 0, kStepBound)) {
                in = step->operands[1];
            } else {
                return false;
            }
        }
        return false;
    };
    const il::Block& h = fn.blocks[header];
    // p: a header parameter of the counted shape, and where it starts when
    // that is no constant.
    struct Counter {
        il::ValueId p;
        il::ValueId start;
    };
    std::vector<Counter> counters;
    for (size_t k = 0; k < h.params.size(); ++k) {
        const il::ValueId p = h.params[k].id;
        if (h.params[k].type != il::Type::F64) continue;
        bool ok = !cfg.preds[header].empty();
        il::ValueId start = il::kNoValue;
        for (il::BlockId pred : cfg.preds[header]) {
            const il::ValueId in = incomingArg(fn, pred, header, k);
            if (in == il::kNoValue) {
                ok = false;
            } else if (!cfg.dominates(header, pred)) {
                if (constIn(in, -kStepBound, kStepBound)) continue;
                ok &= boxedIndex && aheadOfLoop(in) && (start == il::kNoValue || start == in);
                start = in;
            } else {
                ok &= advances(in, p);
            }
            if (!ok) break;
        }
        if (ok) counters.push_back({p, start});
    }
    const auto counterAt = [&](il::ValueId p) -> const Counter* {
        for (const Counter& c : counters) {
            if (c.p == p) return &c;
        }
        return nullptr;
    };
    size_t flagged = 0;
    if (boxedIndex) {
        // Whether `v` is an integer on every iteration; `needs` gathers the
        // entry values that rests on.
        const auto integral = [&](auto&& self, il::ValueId v, int depth, std::vector<il::ValueId>& needs) -> bool {
            v = through(v);
            if (constIn(v, -kStepBound, kStepBound)) return true;
            if (const Counter* c = counterAt(v)) {
                if (c->start != il::kNoValue) needs.push_back(c->start);
                return true;
            }
            if (aheadOfLoop(v)) {
                needs.push_back(v);
                return true;
            }
            const il::Instruction* d = defOf(v);
            if (depth == 0 || !d || d->type != il::Type::F64 || d->operands.size() != 2) return false;
            if (d->op == il::Op::Add || d->op == il::Op::Sub) {
                return self(self, d->operands[0], depth - 1, needs) && self(self, d->operands[1], depth - 1, needs);
            }
            if (d->op != il::Op::Mul) return false;
            return (constIn(d->operands[0], -kStepBound, kStepBound) && self(self, d->operands[1], depth - 1, needs)) ||
                   (constIn(d->operands[1], -kStepBound, kStepBound) && self(self, d->operands[0], depth - 1, needs));
        };
        for (const auto& [b, i] : sites) {
            il::Instruction& op = fn.blocks[b].instructions[i];
            const il::Instruction* box = defOf(op.operands[1]);
            if (!box || box->op != il::Op::Box || box->boxType != il::Type::F64 || box->operands.size() != 1) continue;
            std::vector<il::ValueId> needs;
            if (!integral(integral, box->operands[0], 6, needs)) continue;
            op.immI32 |= il::kElemFlagProvenIntegralIndex;
            ++flagged;
            for (il::ValueId v : needs) {
                if (std::find(onEntry->begin(), onEntry->end(), v) == onEntry->end()) onEntry->push_back(v);
            }
        }
        return flagged;
    }
    if (counters.empty()) return 0;
    // The bounded edges: `br (cmp.lt|le p, unbox L), T, _` with T entered
    // from nowhere else.
    std::vector<std::pair<il::ValueId, il::BlockId>> bounded;  // (p, T)
    for (const il::Block& blk : fn.blocks) {
        if (blk.instructions.empty() || !cfg.dominates(header, blk.id)) continue;
        const il::Instruction& br = blk.instructions.back();
        if (br.op != il::Op::Branch || cfg.preds[br.target.block].size() != 1) continue;
        const il::Instruction* cmp = defOf(br.operands[0]);
        if (!cmp || (cmp->op != il::Op::CmpLt && cmp->op != il::Op::CmpLe) || cmp->operands.size() != 2) continue;
        const il::Instruction* bound = defOf(cmp->operands[1]);
        if (!bound || bound->op != il::Op::Unbox || bound->operands.size() != 1 ||
            std::find(lengths.begin(), lengths.end(), bound->operands[0]) == lengths.end()) {
            continue;
        }
        const il::ValueId p = through(cmp->operands[0]);
        if (counterAt(p)) bounded.push_back({p, br.target.block});
    }
    for (const auto& [b, i] : sites) {
        il::Instruction& op = fn.blocks[b].instructions[i];
        il::ValueId p = through(op.operands[1]);
        if (const il::Instruction* d = defOf(p);
            d && d->op == il::Op::Add && d->type == il::Type::F64 && d->operands.size() == 2) {
            if (constIn(d->operands[1], -kStepBound, kStepBound)) {
                p = through(d->operands[0]);
            } else if (constIn(d->operands[0], -kStepBound, kStepBound)) {
                p = through(d->operands[1]);
            }
        }
        for (const auto& [q, t] : bounded) {
            if (q == p && cfg.dominates(t, b)) {
                op.immI32 |= il::kElemFlagProvenIntegralIndex;
                ++flagged;
                break;
            }
        }
    }
    return flagged;
}

void guardIntegralOnEntry(il::Function& fn, il::BlockId entry, const il::BlockTarget& miss,
                          const std::vector<il::ValueId>& values) {
    if (values.empty()) return;
    il::Instruction enter = fn.blocks[entry].instructions.back();
    fn.blocks[entry].instructions.pop_back();
    il::BlockId at = entry;
    const auto emit = [&](il::Op opcode, il::Type type, std::vector<il::ValueId> operands) -> il::Instruction& {
        il::Instruction inst = enter;
        inst.op = opcode;
        inst.type = type;
        inst.result = static_cast<il::ValueId>(fn.valueCount++);
        inst.operands = std::move(operands);
        inst.target = {};
        inst.elseTarget = {};
        inst.immI32 = 0;
        fn.blocks[at].instructions.push_back(std::move(inst));
        return fn.blocks[at].instructions.back();
    };
    // `ok` holds or the copy is not entered; the rest goes in a new block.
    const auto require = [&](il::ValueId ok) {
        il::Block next;
        next.id = static_cast<il::BlockId>(fn.blocks.size());
        next.copyClass = fn.blocks[entry].copyClass;
        next.copyRegion = fn.blocks[entry].copyRegion;
        next.handler = fn.blocks[entry].handler;
        il::Instruction br = enter;
        br.op = il::Op::Branch;
        br.operands = {ok};
        br.target = il::BlockTarget{next.id, {}};
        br.elseTarget = miss;
        fn.blocks[at].instructions.push_back(std::move(br));
        at = next.id;
        fn.blocks.push_back(std::move(next));
    };
    for (il::ValueId v : values) {
        // floor(v) == v refuses a fraction and NaN; |v| <= 2^53 refuses the
        // infinities and keeps every sum the proof made finite.
        il::Instruction& floor = emit(il::Op::MathUnary, il::Type::F64, {v});
        floor.immI32 = static_cast<int32_t>(il::MathUnaryFn::Floor);
        const il::ValueId floored = floor.result;
        require(emit(il::Op::CmpEq, il::Type::Bool, {floored, v}).result);
        il::Instruction& abs = emit(il::Op::MathUnary, il::Type::F64, {v});
        abs.immI32 = static_cast<int32_t>(il::MathUnaryFn::Abs);
        const il::ValueId magnitude = abs.result;
        il::Instruction& limit = emit(il::Op::ConstF64, il::Type::F64, {});
        limit.immF64 = 9007199254740992.0;
        const il::ValueId bound = limit.result;
        require(emit(il::Op::CmpLe, il::Type::Bool, {magnitude, bound}).result);
    }
    fn.blocks[at].instructions.push_back(std::move(enter));
}

}  // namespace bronze::lower
