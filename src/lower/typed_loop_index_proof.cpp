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
                            const std::vector<std::pair<il::BlockId, uint32_t>>& sites, bool boxedIndex) {
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
    const il::Block& h = fn.blocks[header];
    // p: a header parameter of the counted shape.
    std::vector<il::ValueId> counters;
    for (size_t k = 0; k < h.params.size(); ++k) {
        const il::ValueId p = h.params[k].id;
        if (h.params[k].type != il::Type::F64) continue;
        bool ok = !cfg.preds[header].empty();
        for (il::BlockId pred : cfg.preds[header]) {
            const il::ValueId in = incomingArg(fn, pred, header, k);
            if (in == il::kNoValue) {
                ok = false;
            } else if (!cfg.dominates(header, pred)) {
                ok &= constIn(in, -kStepBound, kStepBound);
            } else {
                const il::Instruction* step = defOf(in);
                ok &= step && step->op == il::Op::Add && step->type == il::Type::F64 && step->operands.size() == 2 &&
                      ((through(step->operands[0]) == p && constIn(step->operands[1], 0, kStepBound)) ||
                       (through(step->operands[1]) == p && constIn(step->operands[0], 0, kStepBound)));
            }
            if (!ok) break;
        }
        if (ok) counters.push_back(p);
    }
    if (counters.empty()) return 0;
    // The bounded edges (a typed op's only): `br (cmp.lt|le p, unbox L), T, _` with T entered
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
        if (std::find(counters.begin(), counters.end(), p) != counters.end()) bounded.push_back({p, br.target.block});
    }
    size_t flagged = 0;
    for (const auto& [b, i] : sites) {
        il::Instruction& op = fn.blocks[b].instructions[i];
        il::ValueId p = op.operands[1];
        if (boxedIndex) {
            const il::Instruction* box = defOf(p);
            if (!box || box->op != il::Op::Box || box->boxType != il::Type::F64 || box->operands.size() != 1) continue;
            p = box->operands[0];
        }
        p = through(p);
        if (const il::Instruction* d = defOf(p);
            d && d->op == il::Op::Add && d->type == il::Type::F64 && d->operands.size() == 2) {
            if (constIn(d->operands[1], -kStepBound, kStepBound)) {
                p = through(d->operands[0]);
            } else if (constIn(d->operands[0], -kStepBound, kStepBound)) {
                p = through(d->operands[1]);
            }
        }
        if (boxedIndex) {
            if (std::find(counters.begin(), counters.end(), p) != counters.end()) {
                op.immI32 |= il::kElemFlagProvenIntegralIndex;
                ++flagged;
            }
            continue;
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

}  // namespace bronze::lower
