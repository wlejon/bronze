#include "lower/typed_loop_version.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "il/il_pin.h"
#include "il/verifier_proofs.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

// The loop and the trampolines it carries, in IL instructions. One copy per
// kind is made, so a loop costs up to twice this again; what this pass is for
// is a vertex or matrix loop of a few dozen.
constexpr size_t kMaxLoopInstructions = 200;
constexpr int32_t kKindFloat32 = BRONZE_ABI_TA_KIND_FLOAT32;
constexpr int32_t kKindFloat64 = BRONZE_ABI_TA_KIND_FLOAT64;

bool disabled() {
    static const bool off = std::getenv("BRONZE_NO_TYPED_LOOP_VERSIONS") != nullptr;
    return off;
}

bool tracing() {
    static const bool on = std::getenv("BRONZE_TYPED_LOOP_VERSION_TRACE") != nullptr;
    return on;
}

std::vector<il::Type> valueTypes(const il::Function& fn) {
    std::vector<il::Type> t(fn.valueCount, il::Type::Void);
    for (size_t i = 0; i < fn.params.size() && i < t.size(); ++i) t[i] = fn.params[i].type;
    for (const auto& b : fn.blocks) {
        for (const auto& p : b.params) {
            if (p.id < t.size()) t[p.id] = p.type;
        }
        for (const auto& inst : b.instructions) {
            if (inst.result != il::kNoValue && inst.result < t.size()) t[inst.result] = inst.type;
        }
    }
    return t;
}

// One use of a value: the instruction it is in, and whether it is an operand
// or an argument of one of the instruction's block targets.
struct Use {
    il::BlockId block = il::kNoBlock;
    uint32_t index = 0;
    enum Where : uint8_t { Operand, ThenArg, ElseArg } where = Operand;
    uint32_t slot = 0;  // which operand
};

std::vector<std::vector<Use>> computeUses(const il::Function& fn) {
    std::vector<std::vector<Use>> uses(fn.valueCount);
    for (const auto& blk : fn.blocks) {
        for (uint32_t i = 0; i < blk.instructions.size(); ++i) {
            const il::Instruction& inst = blk.instructions[i];
            for (uint32_t k = 0; k < inst.operands.size(); ++k) {
                const il::ValueId v = inst.operands[k];
                if (v < uses.size()) uses[v].push_back({blk.id, i, Use::Operand, k});
            }
            for (il::ValueId v : inst.target.args) {
                if (v < uses.size()) uses[v].push_back({blk.id, i, Use::ThenArg, 0});
            }
            for (il::ValueId v : inst.elseTarget.args) {
                if (v < uses.size()) uses[v].push_back({blk.id, i, Use::ElseArg, 0});
            }
        }
    }
    return uses;
}

// The f64 or i32 a dynamic index or value was boxed from, or kNoValue.
il::ValueId numberSource(const il::Function& fn, const std::vector<DefSite>& defs, il::ValueId v, bool allowI32) {
    if (v >= defs.size() || defs[v].block == il::kNoBlock || defs[v].index == UINT32_MAX) return il::kNoValue;
    const il::Instruction& def = fn.blocks[defs[v].block].instructions[defs[v].index];
    if (def.op != il::Op::Box || def.operands.size() != 1) return il::kNoValue;
    if (def.boxType == il::Type::F64 || (allowI32 && def.boxType == il::Type::I32)) return def.operands[0];
    return il::kNoValue;
}

// An instruction that reads an f64 operand and answers the same for NaN as the
// language answers for `undefined` in that position: each of these applies
// ToNumber (or ToNumeric, whose answer for `undefined` is the same NaN) to it,
// and the other operand of a binary one is an f64 too, so no string or BigInt
// branch is reachable. `slot` matters only for the typed store, whose index is
// not such a position: a property key `"undefined"` is a named lookup on the
// view's chain, and `"NaN"` is an index that misses.
bool isCoercingF64Use(const il::Instruction& inst, uint32_t slot) {
    switch (inst.op) {
        case il::Op::Add:
        case il::Op::Sub:
        case il::Op::Mul:
        case il::Op::Div:
        case il::Op::Mod:
        case il::Op::Pow:
        case il::Op::Neg:
            return inst.type == il::Type::F64;
        case il::Op::CmpLt:
        case il::Op::CmpLe:
        case il::Op::CmpGt:
        case il::Op::CmpGe:
        case il::Op::CmpEq:
        case il::Op::CmpNe:
        case il::Op::MathUnary:
            return true;
        case il::Op::ToInt32:
            return inst.boxType == il::Type::F64;
        case il::Op::ElemSetTyped:
            return slot == 2;
        default:
            return false;
    }
}

struct Loop {
    il::BlockId header = il::kNoBlock;
    il::BlockId preheader = il::kNoBlock;
    std::vector<il::BlockId> blocks;  // the natural loop
    std::vector<uint8_t> inLoop;
};

class Versioner {
public:
    Versioner(il::Function& fn, const std::vector<std::string>& keys) : fn_(fn), keys_(keys) {}

    // Versions every innermost loop it can. Returns how many.
    size_t run() {
        size_t done = 0;
        std::vector<uint8_t> tried;
        for (;;) {
            const Cfg cfg = buildCfg(fn_);
            tried.resize(fn_.blocks.size(), 0);
            bool progressed = false;
            for (il::BlockId h = 0; h < fn_.blocks.size(); ++h) {
                if (tried[h]) continue;
                tried[h] = 1;
                Loop loop;
                if (!innermostLoop(cfg, h, loop)) continue;
                const size_t before = fn_.blocks.size();
                if (!version(cfg, loop)) continue;
                // The copies are never versioned again.
                tried.resize(fn_.blocks.size(), 1);
                for (size_t b = before; b < fn_.blocks.size(); ++b) tried[b] = 1;
                ++done;
                progressed = true;
                break;
            }
            if (!progressed) return done;
        }
    }

private:
    bool innermostLoop(const Cfg& cfg, il::BlockId h, Loop& loop) const {
        if (cfg.rpoIndex[h] == UINT32_MAX) return false;
        loop.header = h;
        loop.inLoop.assign(fn_.blocks.size(), 0);
        for (il::BlockId tail : cfg.preds[h]) {
            if (!cfg.dominates(h, tail)) continue;
            for (il::BlockId b : naturalLoop(cfg, h, tail)) {
                if (!loop.inLoop[b]) {
                    loop.inLoop[b] = 1;
                    loop.blocks.push_back(b);
                }
            }
        }
        if (loop.blocks.empty()) return false;
        std::sort(loop.blocks.begin(), loop.blocks.end());
        // Innermost: no other block of it heads a loop.
        for (il::BlockId b : loop.blocks) {
            if (b == h) continue;
            for (il::BlockId p : cfg.preds[b]) {
                if (loop.inLoop[p] && cfg.dominates(b, p)) return false;
            }
        }
        for (il::BlockId p : cfg.preds[h]) {
            if (loop.inLoop[p]) continue;
            if (loop.preheader != il::kNoBlock) return false;
            loop.preheader = p;
        }
        if (loop.preheader == il::kNoBlock) return false;
        const il::Block& pre = fn_.blocks[loop.preheader];
        return !pre.instructions.empty() && pre.instructions.back().op == il::Op::Jump &&
               pre.instructions.back().target.block == h;
    }

    bool version(const Cfg& cfg, const Loop& loop) {
        if (fn_.blocks[loop.header].copyClass == il::CopyClass::Slow) return false;
        size_t size = 0;
        for (il::BlockId b : loop.blocks) {
            const il::Block& blk = fn_.blocks[b];
            // A handler's region would need re-parenting, and a handler block
            // inside the loop is entered from anywhere in it.
            if (blk.handler != il::kNoBlock) return false;
            if (!blk.instructions.empty() && blk.instructions.front().op == il::Op::ExcTake) return false;
            size += blk.instructions.size();
        }
        const std::vector<DefSite> defs = computeDefSites(fn_);
        const std::vector<il::Type> types = valueTypes(fn_);
        const std::vector<std::vector<Use>> uses = computeUses(fn_);

        // The copied set: the loop plus the blocks outside it that read a
        // value it defines — a guarded region's trampolines — each entered
        // only from the loop. Anything else reading a loop value would see
        // two definitions after the copy.
        std::vector<uint8_t> copied = loop.inLoop;
        std::vector<il::BlockId> stubs;
        const auto definedIn = [&](il::ValueId v, const std::vector<uint8_t>& set) {
            return v < defs.size() && defs[v].block != il::kNoBlock && set[defs[v].block];
        };
        for (const auto& blk : fn_.blocks) {
            if (loop.inLoop[blk.id]) continue;
            bool reads = false;
            for (const auto& inst : blk.instructions) {
                for (il::ValueId v : inst.operands) reads |= definedIn(v, loop.inLoop);
                for (il::ValueId v : inst.target.args) reads |= definedIn(v, loop.inLoop);
                for (il::ValueId v : inst.elseTarget.args) reads |= definedIn(v, loop.inLoop);
            }
            if (!reads) continue;
            if (blk.handler != il::kNoBlock || cfg.preds[blk.id].empty()) return false;
            for (il::BlockId p : cfg.preds[blk.id]) {
                if (!loop.inLoop[p]) return false;
            }
            stubs.push_back(blk.id);
            copied[blk.id] = 1;
            size += blk.instructions.size();
        }
        if (size > kMaxLoopInstructions) return false;
        for (il::BlockId s : stubs) {
            for (const auto& inst : fn_.blocks[s].instructions) {
                if (inst.result == il::kNoValue) continue;
                for (const Use& u : uses[inst.result]) {
                    if (!copied[u.block]) return false;
                }
            }
        }

        // The receivers: a dynamic value defined outside the loop, read or
        // written by index inside it.
        const auto invariantReceiver = [&](il::ValueId r) {
            if (r >= types.size() || types[r] != il::Type::Dynamic) return false;
            if (r >= defs.size()) return false;
            if (defs[r].block == il::kNoBlock) return true;  // a parameter
            if (loop.inLoop[defs[r].block]) return false;
            if (defs[r].index == UINT32_MAX) return true;  // a block parameter
            return !il::isKnownNonArrayProducer(fn_.blocks[defs[r].block].instructions[defs[r].index]);
        };
        std::unordered_map<il::ValueId, il::ValueId> getIndex;  // elem.get result -> its index source
        struct SetSite {
            il::BlockId block;
            uint32_t index;
            il::ValueId idx;
            il::ValueId number;  // the f64 the value was boxed from, or kNoValue
        };
        std::vector<SetSite> sets;
        for (il::BlockId b : loop.blocks) {
            const il::Block& blk = fn_.blocks[b];
            for (uint32_t i = 0; i < blk.instructions.size(); ++i) {
                const il::Instruction& inst = blk.instructions[i];
                if (inst.op != il::Op::ElemGet && inst.op != il::Op::ElemSet) continue;
                // A definition is not the store a typed view's element write
                // is: it throws for an index past the view where the Set
                // discards (10.4.5.3 vs 10.4.5.5).
                if (il::isDefineWrite(inst)) continue;
                if (inst.operands.size() < 2 || !invariantReceiver(inst.operands[0])) continue;
                const il::ValueId idx = numberSource(fn_, defs, inst.operands[1], /*allowI32=*/true);
                if (idx == il::kNoValue) continue;
                if (inst.op == il::Op::ElemGet) {
                    if (inst.result != il::kNoValue) getIndex[inst.result] = idx;
                } else if (inst.operands.size() == 3) {
                    sets.push_back({b, i, idx, numberSource(fn_, defs, inst.operands[2], /*allowI32=*/false)});
                }
            }
        }

        // Which of them become typed: a fixpoint, because a store's value may
        // be a read (`b[i] = a[i]`) and each is admissible only if the other is.
        std::unordered_map<il::ValueId, bool> getOk;
        for (const auto& [x, idx] : getIndex) getOk[x] = true;
        std::vector<uint8_t> setOk(sets.size(), 1);
        const auto setAt = [&](il::BlockId b, uint32_t i) -> int {
            for (size_t s = 0; s < sets.size(); ++s) {
                if (sets[s].block == b && sets[s].index == i) return static_cast<int>(s);
            }
            return -1;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t s = 0; s < sets.size(); ++s) {
                if (!setOk[s]) continue;
                const il::ValueId val = fn_.blocks[sets[s].block].instructions[sets[s].index].operands[2];
                const auto g = getOk.find(val);
                if (sets[s].number == il::kNoValue && (g == getOk.end() || !g->second)) {
                    setOk[s] = 0;
                    changed = true;
                }
            }
            for (auto& [x, ok] : getOk) {
                if (!ok) continue;
                if (!readAdmissible(x, loop, uses, [&](const Use& u) {
                        const int s = setAt(u.block, u.index);
                        return u.where == Use::Operand && u.slot == 2 && s >= 0 && setOk[s];
                    })) {
                    ok = false;
                    changed = true;
                }
            }
        }

        // Every receiver with something to type is tested on the entry edge.
        std::vector<il::ValueId> receivers;
        const auto addReceiver = [&](il::ValueId r) {
            if (std::find(receivers.begin(), receivers.end(), r) == receivers.end()) receivers.push_back(r);
        };
        size_t typedOps = 0;
        for (const auto& [x, ok] : getOk) {
            if (!ok) continue;
            addReceiver(fn_.blocks[defs[x].block].instructions[defs[x].index].operands[0]);
            ++typedOps;
        }
        for (size_t s = 0; s < sets.size(); ++s) {
            if (!setOk[s]) continue;
            addReceiver(fn_.blocks[sets[s].block].instructions[sets[s].index].operands[0]);
            ++typedOps;
        }
        if (typedOps == 0) return false;
        std::sort(receivers.begin(), receivers.end());

        // A copy in which nothing can run code or collect — every instruction
        // left in it is a typed op, arithmetic, a test or a `length` read of
        // a tested view — cannot change any view's length, shape or
        // prototype epoch while it runs. There the `length` reads are read
        // once, on the copy's entry, and the entry test also requires the
        // chain those reads answer from to be pristine, so the read it moves
        // is the header's field and runs no getter.
        std::vector<std::pair<il::BlockId, uint32_t>> lengthReads;
        bool callFree = true;
        for (il::BlockId b : loop.blocks) {
            const il::Block& blk = fn_.blocks[b];
            for (uint32_t i = 0; i < blk.instructions.size(); ++i) {
                const il::Instruction& inst = blk.instructions[i];
                if (inst.op == il::Op::PropGet && inst.operands.size() == 1 && inst.result != il::kNoValue &&
                    std::binary_search(receivers.begin(), receivers.end(), inst.operands[0]) &&
                    inst.keyIndex < keys_.size() && keys_[inst.keyIndex] == "length") {
                    lengthReads.push_back({b, i});
                    continue;
                }
                if (inst.op == il::Op::ElemGet && inst.result != il::kNoValue && getOk.count(inst.result) &&
                    getOk.at(inst.result)) {
                    continue;
                }
                if (inst.op == il::Op::ElemSet && setAt(b, i) >= 0 && setOk[setAt(b, i)]) continue;
                // A checked unbox of a typed read is deleted from the copy.
                if (inst.op == il::Op::Unbox && inst.operands.size() == 1 && getOk.count(inst.operands[0]) &&
                    getOk.at(inst.operands[0])) {
                    continue;
                }
                if (il::canCollect(inst)) callFree = false;
            }
        }
        if (!callFree) lengthReads.clear();
        std::vector<il::ValueId> pristine;  // receivers whose `length` is read on entry
        for (const auto& [b, i] : lengthReads) {
            const il::ValueId r = fn_.blocks[b].instructions[i].operands[0];
            if (std::find(pristine.begin(), pristine.end(), r) == pristine.end()) pristine.push_back(r);
        }

        std::vector<il::BlockId> copiedBlocks = loop.blocks;
        copiedBlocks.insert(copiedBlocks.end(), stubs.begin(), stubs.end());
        const il::Function original = fn_;
        std::vector<std::pair<int32_t, il::BlockId>> entries;
        struct Copy {
            il::BlockId header;
            std::vector<il::ValueId> lengths;
            std::vector<std::pair<il::BlockId, uint32_t>> sites;
        };
        std::vector<Copy> copies;
        for (const int32_t kind : {kKindFloat32, kKindFloat64}) {
            // The copy's entry block comes first, so that what it defines
            // precedes its uses in block order (dominanceHolds).
            const il::BlockId entry = appendBlockLike(loop.preheader);
            std::unordered_map<il::ValueId, il::ValueId> replaced;
            std::vector<il::Instruction> reads;
            for (il::ValueId r : pristine) {
                const il::ValueId len = static_cast<il::ValueId>(fn_.valueCount++);
                for (const auto& [b, i] : lengthReads) {
                    const il::Instruction& read = fn_.blocks[b].instructions[i];
                    if (read.operands[0] != r) continue;
                    if (replaced.empty() || reads.back().operands[0] != r) {
                        reads.push_back(read);
                        reads.back().result = len;
                    }
                    replaced[read.result] = len;
                }
            }
            typedSites_.clear();
            const il::BlockId header =
                copy(copiedBlocks, copied, loop.header, kind, getOk, sets, setOk, getIndex, replaced);
            Copy c{header, {}, typedSites_};
            for (const auto& [from, len] : replaced) c.lengths.push_back(len);
            copies.push_back(std::move(c));
            il::Instruction jump = fn_.blocks[loop.preheader].instructions.back();
            fn_.blocks[entry].instructions = std::move(reads);
            jump.target.block = header;
            fn_.blocks[entry].instructions.push_back(std::move(jump));
            entries.push_back({kind, entry});
        }
        enterThrough(loop, receivers, pristine, entries);
        size_t integral = 0;
        if (!copies.front().lengths.empty()) {
            const Cfg after = buildCfg(fn_);
            const std::vector<DefSite> afterDefs = computeDefSites(fn_);
            for (const Copy& c : copies) integral += proveIntegralIndices(after, afterDefs, c.header, c.lengths, c.sites);
        }
        if (!dominanceHolds(fn_)) {
            fn_ = original;
            return false;
        }
        if (tracing()) {
            std::fprintf(stderr,
                         "[typed-loop] %s: b%u, %zu receivers, %zu typed ops, %zu length reads on entry, "
                         "%zu integral indices\n",
                         fn_.name.c_str(), loop.header, receivers.size(), typedOps, lengthReads.size(), integral);
        }
        return true;
    }

    // INTEGRAL INDICES. A typed op whose index is proven a finite safe
    // integer skips the round trip that tests it (il_pin.h,
    // kElemFlagProvenIntegralIndex), and the shape that proves it is the one
    // a counted loop over a view has: the index is `p` or `p + k`, where `p`
    // is the copy header's parameter, enters as an integral constant, is only
    // ever advanced by a non-negative integral constant, and the access is
    // reached only through the true edge of `p < L` with `L` an entry
    // `length` read — a pristine view's length, below 2^32. Then `p` is in
    // [-2^31, 2^32) at the access, and `p + k` with |k| <= 2^31 is integral
    // and exact. Returns how many ops were flagged.
    size_t proveIntegralIndices(const Cfg& cfg, const std::vector<DefSite>& defs, il::BlockId header,
                                const std::vector<il::ValueId>& lengths,
                                const std::vector<std::pair<il::BlockId, uint32_t>>& sites) {
        constexpr double kStepBound = 2147483648.0;
        const auto defOf = [&](il::ValueId v) -> const il::Instruction* {
            if (v >= defs.size() || defs[v].block == il::kNoBlock || defs[v].index == UINT32_MAX) return nullptr;
            return &fn_.blocks[defs[v].block].instructions[defs[v].index];
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
                const il::ValueId in = soleIncoming(cfg, b, v);
                if (in == il::kNoValue) return v;
                v = in;
            }
            return v;
        };
        const il::Block& h = fn_.blocks[header];
        // p: a header parameter of the counted shape.
        std::vector<il::ValueId> counters;
        for (size_t k = 0; k < h.params.size(); ++k) {
            const il::ValueId p = h.params[k].id;
            if (h.params[k].type != il::Type::F64) continue;
            bool ok = !cfg.preds[header].empty();
            for (il::BlockId pred : cfg.preds[header]) {
                const il::ValueId in = incomingArg(pred, header, k);
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
        // The bounded edges: `br (cmp.lt|le p, unbox L), T, _` with T entered
        // from nowhere else.
        std::vector<std::pair<il::ValueId, il::BlockId>> bounded;  // (p, T)
        for (const il::Block& blk : fn_.blocks) {
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
            il::Instruction& op = fn_.blocks[b].instructions[i];
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

    // The argument `pred` passes for parameter `k` of `to`, or kNoValue when
    // it reaches `to` some other way (a handler) or on both edges differently.
    il::ValueId incomingArg(il::BlockId pred, il::BlockId to, size_t k) const {
        const il::Block& pb = fn_.blocks[pred];
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
    il::ValueId soleIncoming(const Cfg& cfg, il::BlockId b, il::ValueId v) const {
        const il::Block& blk = fn_.blocks[b];
        size_t k = 0;
        while (k < blk.params.size() && blk.params[k].id != v) ++k;
        if (k == blk.params.size() || cfg.preds[b].empty()) return il::kNoValue;
        il::ValueId sole = il::kNoValue;
        for (il::BlockId pred : cfg.preds[b]) {
            const il::ValueId in = incomingArg(pred, b, k);
            if (in == il::kNoValue || (sole != il::kNoValue && in != sole)) return il::kNoValue;
            sole = in;
        }
        return sole;
    }

    il::BlockId appendBlockLike(il::BlockId like) {
        il::Block blk;
        blk.id = static_cast<il::BlockId>(fn_.blocks.size());
        blk.copyClass = fn_.blocks[like].copyClass;
        blk.copyRegion = fn_.blocks[like].copyRegion;
        blk.handler = fn_.blocks[like].handler;
        fn_.blocks.push_back(std::move(blk));
        return fn_.blocks.back().id;
    }

    // Whether read `x`'s every use is one where NaN answers as `undefined`
    // would (the header states the set). `storeOk` says whether a use as a
    // store's value is one this copy types.
    template <typename StoreOk>
    bool readAdmissible(il::ValueId x, const Loop& loop, const std::vector<std::vector<Use>>& uses,
                        StoreOk&& storeOk) const {
        for (const Use& u : uses[x]) {
            const il::Instruction& inst = fn_.blocks[u.block].instructions[u.index];
            if (u.where != Use::Operand) {
                if (!isDeadTrampoline(u.block, u.index, x, loop, uses)) return false;
                continue;
            }
            if (inst.op == il::Op::IsNumber) {
                for (const Use& c : uses[inst.result]) {
                    const il::Instruction& br = fn_.blocks[c.block].instructions[c.index];
                    if (br.op != il::Op::Branch || c.where != Use::Operand || c.slot != 0) return false;
                    if (!isTrampolineEdge(c.block, br, loop)) return false;
                }
                continue;
            }
            if (inst.op == il::Op::Unbox && inst.type == il::Type::F64) {
                for (const Use& c : uses[inst.result]) {
                    if (c.where != Use::Operand) return false;
                    if (!isCoercingF64Use(fn_.blocks[c.block].instructions[c.index], c.slot)) return false;
                }
                continue;
            }
            if (inst.op == il::Op::ElemSet && storeOk(u)) continue;
            return false;
        }
        return true;
    }

    // A branch whose false edge is a guarded region's trampoline: a block of
    // one jump out of the loop, entered from nowhere else.
    bool isTrampolineEdge(il::BlockId from, const il::Instruction& br, const Loop& loop) const {
        const il::BlockId t = br.elseTarget.block;
        if (t == il::kNoBlock || t == br.target.block || loop.inLoop[t] || !br.elseTarget.args.empty()) {
            return false;
        }
        const il::Block& tb = fn_.blocks[t];
        if (tb.instructions.size() != 1 || tb.instructions[0].op != il::Op::Jump || !tb.params.empty()) return false;
        if (tb.instructions[0].target.block < loop.inLoop.size() && loop.inLoop[tb.instructions[0].target.block]) {
            return false;
        }
        for (const auto& blk : fn_.blocks) {
            if (blk.id == from) continue;
            for (const auto& inst : blk.instructions) {
                if (inst.target.block == t || inst.elseTarget.block == t) return false;
            }
            if (blk.handler == t) return false;
        }
        return true;
    }

    // Whether the jump at (b, i), which passes `x`, is the trampoline on the
    // false edge of `x`'s own `is.number` test: after the test folds, nothing
    // reaches it.
    bool isDeadTrampoline(il::BlockId b, uint32_t i, il::ValueId x, const Loop& loop,
                          const std::vector<std::vector<Use>>& uses) const {
        if (i != 0 || fn_.blocks[b].instructions.size() != 1) return false;
        for (const Use& u : uses[x]) {
            const il::Instruction& test = fn_.blocks[u.block].instructions[u.index];
            if (u.where != Use::Operand || test.op != il::Op::IsNumber) continue;
            for (const Use& c : uses[test.result]) {
                const il::Instruction& br = fn_.blocks[c.block].instructions[c.index];
                if (br.op == il::Op::Branch && br.elseTarget.block == b && isTrampolineEdge(c.block, br, loop)) {
                    return true;
                }
            }
        }
        return false;
    }

    // Appends a copy of `blocks`, in their order, with `kind`'s typed ops in
    // it; returns the copy of `header`.
    // `replaced` maps a value the loop computes to one the copy reads in its
    // place (an entry `length` read); its instruction is not copied.
    template <typename GetOk, typename Sets, typename SetOk, typename GetIndex>
    il::BlockId copy(const std::vector<il::BlockId>& blocks, const std::vector<uint8_t>& copied,
                     il::BlockId header, int32_t kind, const GetOk& getOk, const Sets& sets, const SetOk& setOk,
                     const GetIndex& getIndex, const std::unordered_map<il::ValueId, il::ValueId>& replaced) {
        std::vector<il::BlockId> bmap(fn_.blocks.size(), il::kNoBlock);
        const il::BlockId base = static_cast<il::BlockId>(fn_.blocks.size());
        for (size_t k = 0; k < blocks.size(); ++k) bmap[blocks[k]] = base + static_cast<il::BlockId>(k);
        std::unordered_map<il::ValueId, il::ValueId> vmap;
        const auto fresh = [&]() { return static_cast<il::ValueId>(fn_.valueCount++); };
        for (il::BlockId b : blocks) {
            for (const auto& p : fn_.blocks[b].params) vmap[p.id] = fresh();
            for (const auto& inst : fn_.blocks[b].instructions) {
                if (inst.result != il::kNoValue) vmap[inst.result] = fresh();
            }
        }
        const auto mapV = [&](il::ValueId v) {
            const auto it = vmap.find(v);
            return it == vmap.end() ? v : it->second;
        };
        // What each typed read's f64 is, by the ORIGINAL read's id: its
        // unboxes and its is.number tests are answered from it.
        std::unordered_map<il::ValueId, il::ValueId> typedOf;
        // Original unbox result -> typed f64, and every other replaced value.
        std::unordered_map<il::ValueId, il::ValueId> unboxOf = replaced;
        for (const auto& [x, ok] : getOk) {
            if (ok) typedOf[x] = fresh();
        }
        for (il::BlockId b : blocks) {
            for (const auto& inst : fn_.blocks[b].instructions) {
                if (inst.op == il::Op::Unbox && inst.operands.size() == 1 && typedOf.count(inst.operands[0])) {
                    unboxOf[inst.result] = typedOf.at(inst.operands[0]);
                }
            }
        }
        const auto remap = [&](il::ValueId v) {
            const auto u = unboxOf.find(v);
            return u != unboxOf.end() ? u->second : mapV(v);
        };
        const auto mapT = [&](il::BlockTarget t) {
            if (t.block != il::kNoBlock && t.block < copied.size() && copied[t.block]) t.block = bmap[t.block];
            for (auto& a : t.args) a = remap(a);
            return t;
        };
        const auto isTypedSet = [&](il::BlockId b, uint32_t i) {
            for (size_t s = 0; s < sets.size(); ++s) {
                if (setOk[s] && sets[s].block == b && sets[s].index == i) return true;
            }
            return false;
        };

        for (il::BlockId b : blocks) {
            il::Block out;
            const il::Block& src = fn_.blocks[b];
            out.id = bmap[b];
            out.handler = src.handler;
            out.copyClass = src.copyClass;
            out.copyRegion = src.copyRegion;
            for (const auto& p : src.params) out.params.push_back({mapV(p.id), p.type});
            for (uint32_t i = 0; i < src.instructions.size(); ++i) {
                const il::Instruction& orig = src.instructions[i];
                if (orig.result != il::kNoValue && unboxOf.count(orig.result)) continue;
                il::Instruction inst = orig;
                for (auto& o : inst.operands) o = remap(o);
                inst.target = mapT(orig.target);
                inst.elseTarget = mapT(orig.elseTarget);
                if (inst.result != il::kNoValue) inst.result = mapV(orig.result);
                if (orig.op == il::Op::ElemGet && typedOf.count(orig.result)) {
                    // %xf = elem.get.typed r, i ; %x = box.f64 %xf, for the
                    // uses the folded test leaves unreachable.
                    il::Instruction get = inst;
                    get.op = il::Op::ElemGetTyped;
                    get.type = il::Type::F64;
                    get.result = typedOf.at(orig.result);
                    get.operands = {inst.operands[0], mapV(getIndex.at(orig.result))};
                    get.immI32 = kind;
                    typedSites_.push_back({out.id, static_cast<uint32_t>(out.instructions.size())});
                    out.instructions.push_back(get);
                    inst.op = il::Op::Box;
                    inst.boxType = il::Type::F64;
                    inst.operands = {get.result};
                    inst.immI32 = 0;
                } else if (orig.op == il::Op::IsNumber && typedOf.count(orig.operands[0])) {
                    inst.op = il::Op::ConstBool;
                    inst.operands.clear();
                    inst.immI32 = 1;
                } else if (orig.op == il::Op::ElemSet && isTypedSet(b, i)) {
                    il::ValueId idx = il::kNoValue;
                    il::ValueId f = il::kNoValue;
                    for (size_t s = 0; s < sets.size(); ++s) {
                        if (sets[s].block == b && sets[s].index == i) {
                            idx = sets[s].idx;
                            f = sets[s].number;
                        }
                    }
                    if (const auto t = typedOf.find(orig.operands[2]); t != typedOf.end()) {
                        f = t->second;
                    } else {
                        f = mapV(f);
                    }
                    typedSites_.push_back({out.id, static_cast<uint32_t>(out.instructions.size())});
                    inst.op = il::Op::ElemSetTyped;
                    inst.operands = {inst.operands[0], mapV(idx), f};
                    inst.immI32 = kind;
                }
                out.instructions.push_back(std::move(inst));
            }
            fn_.blocks.push_back(std::move(out));
        }
        return bmap[header];
    }

    // The preheader's `jump h(args)` becomes a chain of tests, one block per
    // (kind, receiver): every receiver of `kind` leads into that kind's entry
    // block, and the first that is not falls through to the next kind and,
    // last, to the original loop. A receiver in `pristine` is also tested for
    // the chain its entry `length` read answers from.
    void enterThrough(const Loop& loop, const std::vector<il::ValueId>& receivers,
                      const std::vector<il::ValueId>& pristine,
                      const std::vector<std::pair<int32_t, il::BlockId>>& entries) {
        il::Block& pre = fn_.blocks[loop.preheader];
        const il::Instruction jump = pre.instructions.back();
        pre.instructions.pop_back();
        const il::CopyClass cls = pre.copyClass;
        const uint32_t region = pre.copyRegion;
        const il::BlockId handler = pre.handler;

        const size_t tests = entries.size() * receivers.size();
        std::vector<il::BlockId> testBlock(tests);
        testBlock[0] = loop.preheader;
        for (size_t t = 1; t < tests; ++t) {
            il::Block blk;
            blk.id = static_cast<il::BlockId>(fn_.blocks.size());
            blk.copyClass = cls;
            blk.copyRegion = region;
            blk.handler = handler;
            testBlock[t] = blk.id;
            fn_.blocks.push_back(std::move(blk));
        }
        for (size_t e = 0; e < entries.size(); ++e) {
            for (size_t r = 0; r < receivers.size(); ++r) {
                const size_t t = e * receivers.size() + r;
                il::Instruction test = jump;
                test.op = il::Op::IsTypedArray;
                test.type = il::Type::Bool;
                test.result = static_cast<il::ValueId>(fn_.valueCount++);
                test.operands = {receivers[r]};
                test.immI32 = entries[e].first;
                if (std::find(pristine.begin(), pristine.end(), receivers[r]) != pristine.end()) {
                    test.immI32 |= il::kTypedArrayPristineLength;
                }
                test.target = {};
                test.elseTarget = {};
                il::Instruction br = jump;
                br.op = il::Op::Branch;
                br.operands = {test.result};
                br.target = il::BlockTarget{r + 1 < receivers.size() ? testBlock[t + 1] : entries[e].second, {}};
                br.elseTarget = e + 1 < entries.size() ? il::BlockTarget{testBlock[(e + 1) * receivers.size()], {}}
                                                       : il::BlockTarget{loop.header, jump.target.args};
                fn_.blocks[testBlock[t]].instructions.push_back(std::move(test));
                fn_.blocks[testBlock[t]].instructions.push_back(std::move(br));
            }
        }
    }

    il::Function& fn_;
    const std::vector<std::string>& keys_;
    // The typed ops the last copy() made, by position in the copy.
    std::vector<std::pair<il::BlockId, uint32_t>> typedSites_;
};

}  // namespace

bool applyTypedLoopVersions(il::Module& module) {
    if (disabled()) return false;
    bool any = false;
    for (auto& fn : module.functions) {
        if (fn.isExternal || fn.blocks.empty() || fn.isCoroutineBody()) continue;
        Versioner v(fn, module.keyConstants);
        any |= v.run() != 0;
    }
    return any;
}

}  // namespace bronze::lower
