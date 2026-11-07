#include "lower/typed_body_version.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

#include "il/il_pin.h"
#include "il/verifier_proofs.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

// The body, in IL instructions. It is copied whole, so this bounds what the
// pass adds to a function; slerpFlat's fast copy is about 270.
constexpr size_t kMaxBodyInstructions = 400;
// Below this many flagged accesses the entry tests and the second copy cost
// more than the accesses save.
constexpr size_t kMinAccesses = 4;

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

bool isElemAccess(const il::Instruction& inst) {
    return (inst.op == il::Op::ElemGet || inst.op == il::Op::ElemSet) && inst.operands.size() >= 2 &&
           !il::isDefineWrite(inst);
}

class BodyVersioner {
public:
    explicit BodyVersioner(il::Function& fn) : fn_(fn) {}

    const std::vector<il::BlockId>& coldBlocks() const { return cold_; }

    bool run() {
        if (fn_.blocks.empty()) return false;
        size_t candidates = 0;
        for (const auto& blk : fn_.blocks) {
            for (const auto& inst : blk.instructions) candidates += isElemAccess(inst) ? 1 : 0;
        }
        if (candidates < kMinAccesses) return false;
        const il::Function original = fn_;
        if (!version()) {
            fn_ = original;
            return false;
        }
        return true;
    }

private:
    // A straight-line function is all one entry block: its code moves to a
    // block of its own, entered by a jump the versioning can split.
    bool splitEntry() {
        const Cfg cfg = buildCfg(fn_);
        if (!cfg.preds[0].empty()) return false;
        for (uint32_t r : cfg.rpoIndex) {
            if (r == UINT32_MAX) return false;
        }
        il::Block body;
        body.id = static_cast<il::BlockId>(fn_.blocks.size());
        body.copyClass = fn_.blocks[0].copyClass;
        body.copyRegion = fn_.blocks[0].copyRegion;
        body.handler = fn_.blocks[0].handler;
        body.instructions = std::move(fn_.blocks[0].instructions);
        il::Instruction jump;
        jump.op = il::Op::Jump;
        jump.type = il::Type::Void;
        jump.result = il::kNoValue;
        jump.target = il::BlockTarget{body.id, {}};
        if (!body.instructions.empty()) jump.span = body.instructions.front().span;
        fn_.blocks[0].instructions = {std::move(jump)};
        fn_.blocks.push_back(std::move(body));
        return true;
    }

    bool version() {
        const il::Block& first = fn_.blocks[0];
        const bool split = first.instructions.empty() || first.instructions.back().op != il::Op::Jump;
        if (split && !splitEntry()) return false;
        const Cfg cfg = buildCfg(fn_);
        // The body starts where the entry block jumps: a guarded region's
        // fast copy, the code after an entry block of setup, or the whole of
        // a straight-line function (splitEntry).
        const il::Block& b0 = fn_.blocks[0];
        if (!cfg.preds[0].empty()) return false;
        const il::BlockId entry = b0.instructions.back().target.block;
        if (entry == 0 || entry >= fn_.blocks.size() || cfg.preds[entry].size() != 1) return false;

        std::vector<uint8_t> in(fn_.blocks.size(), 0);
        std::vector<il::BlockId> body;
        size_t size = 0;
        for (il::BlockId b = 0; b < fn_.blocks.size(); ++b) {
            if (cfg.rpoIndex[b] == UINT32_MAX || !cfg.dominates(entry, b)) continue;
            if (fn_.blocks[b].copyClass == il::CopyClass::Slow) continue;
            in[b] = 1;
            body.push_back(b);
            size += fn_.blocks[b].instructions.size();
        }
        if (size > kMaxBodyInstructions) return false;
        for (il::BlockId b : body) {
            const il::Block& blk = fn_.blocks[b];
            if (blk.handler != il::kNoBlock) return false;
            if (!blk.instructions.empty() && blk.instructions.front().op == il::Op::ExcTake) return false;
            for (il::BlockId p : cfg.preds[b]) {
                // Entered only through the entry, and no loop: a back edge
                // ends at a block that dominates its source.
                if (b != entry && !in[p]) return false;
                if (in[p] && cfg.dominates(b, p)) return false;
            }
        }

        // Nothing outside the body may read a value it defines, other than
        // through the arguments its own jumps pass: the copy gives each such
        // value a second definition.
        const std::vector<DefSite> defs = computeDefSites(fn_);
        const auto definedIn = [&](il::ValueId v) {
            return v < defs.size() && defs[v].block != il::kNoBlock && in[defs[v].block];
        };
        for (const auto& blk : fn_.blocks) {
            if (in[blk.id]) continue;
            for (const auto& inst : blk.instructions) {
                for (il::ValueId v : inst.operands) {
                    if (definedIn(v)) return false;
                }
                for (il::ValueId v : inst.target.args) {
                    if (definedIn(v)) return false;
                }
                for (il::ValueId v : inst.elseTarget.args) {
                    if (definedIn(v)) return false;
                }
            }
        }

        // The receivers: a dynamic value defined before the body, read or
        // written by index in it, and not already a view some typed op
        // names (its kind is known, and the loop pass or the lowerer typed
        // its accesses).
        const std::vector<il::Type> types = valueTypes(fn_);
        std::vector<il::ValueId> proven;
        for (const auto& blk : fn_.blocks) {
            for (const auto& inst : blk.instructions) {
                if ((inst.op == il::Op::ElemGetTyped || inst.op == il::Op::ElemSetTyped) && !inst.operands.empty()) {
                    proven.push_back(inst.operands[0]);
                }
            }
        }
        const auto receiverOk = [&](il::ValueId r) {
            if (r >= types.size() || types[r] != il::Type::Dynamic || definedIn(r)) return false;
            if (std::find(proven.begin(), proven.end(), r) != proven.end()) return false;
            if (r < defs.size() && defs[r].block != il::kNoBlock && defs[r].index != UINT32_MAX &&
                il::isKnownNonArrayProducer(fn_.blocks[defs[r].block].instructions[defs[r].index])) {
                return false;
            }
            return true;
        };
        std::unordered_map<il::ValueId, uint32_t> useCount;
        for (const auto& blk : fn_.blocks) {
            for (const auto& inst : blk.instructions) {
                for (il::ValueId v : inst.operands) ++useCount[v];
                for (il::ValueId v : inst.target.args) ++useCount[v];
                for (il::ValueId v : inst.elseTarget.args) ++useCount[v];
            }
        }
        // A field copy: a read whose one use is a `prop.set`'s value, or a
        // write of a value a `prop.get` read (il_lowering_elem_run.cpp's runs).
        const auto fieldCopy = [&](const il::Instruction& inst) {
            if (inst.op == il::Op::ElemGet) {
                if (inst.result == il::kNoValue || useCount[inst.result] != 1) return false;
                for (il::BlockId b : body) {
                    for (const auto& u : fn_.blocks[b].instructions) {
                        if (u.op == il::Op::PropSet && u.operands.size() >= 2 && u.operands[1] == inst.result) {
                            return true;
                        }
                    }
                }
                return false;
            }
            const il::ValueId v = inst.operands.size() >= 3 ? inst.operands[2] : il::kNoValue;
            return v < defs.size() && defs[v].block != il::kNoBlock && defs[v].index != UINT32_MAX &&
                   fn_.blocks[defs[v].block].instructions[defs[v].index].op == il::Op::PropGet;
        };
        std::vector<il::ValueId> receivers;
        size_t accesses = 0;
        size_t copies = 0;
        for (il::BlockId b : body) {
            for (const auto& inst : fn_.blocks[b].instructions) {
                if (!isElemAccess(inst) || !receiverOk(inst.operands[0])) continue;
                ++accesses;
                if (fieldCopy(inst)) ++copies;
                if (std::find(receivers.begin(), receivers.end(), inst.operands[0]) == receivers.end()) {
                    receivers.push_back(inst.operands[0]);
                }
            }
        }
        if (accesses < kMinAccesses || accesses == copies) return false;
        std::sort(receivers.begin(), receivers.end());

        // A store of a value a raw unbox dominates stores that unbox's
        // number, boxed again beside it: the value is a Number there
        // (il.h `rawUnbox`), and a store whose value is known boxed from an
        // f64 has no ToNumber to fall back to, so no call.
        std::unordered_map<il::ValueId, std::vector<il::ValueId>> rawUnboxes;
        for (const auto& blk : fn_.blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::Unbox && inst.rawUnbox && inst.type == il::Type::F64 &&
                    inst.operands.size() == 1 && inst.result != il::kNoValue) {
                    rawUnboxes[inst.operands[0]].push_back(inst.result);
                }
            }
        }
        for (il::BlockId b : body) {
            const auto& insts = fn_.blocks[b].instructions;
            for (uint32_t i = 0; i < insts.size(); ++i) {
                const il::Instruction& inst = insts[i];
                if (inst.op != il::Op::ElemSet || !isElemAccess(inst) || inst.operands.size() < 3 ||
                    !std::binary_search(receivers.begin(), receivers.end(), inst.operands[0])) {
                    continue;
                }
                const auto it = rawUnboxes.find(inst.operands[2]);
                if (it == rawUnboxes.end()) continue;
                for (il::ValueId u : it->second) {
                    const DefSite& du = defs[u];
                    if ((du.block == b && du.index < i) || (du.block != b && cfg.dominates(du.block, b))) {
                        setNumbers_[{b, i}] = u;
                        break;
                    }
                }
            }
        }
        const il::BlockId copyEntry = copy(body, in, entry, receivers);
        enterThrough(entry, copyEntry, receivers);
        // The split's body block went last, after the blocks it dominates:
        // every block is put after its dominator, the original body's
        // marked through the renumbering.
        std::vector<uint8_t> original(fn_.blocks.size(), 0);
        for (il::BlockId b : body) original[b] = 1;
        if (split) orderAfterDominators(fn_, original);
        if (!dominanceHolds(fn_)) return false;
        for (il::BlockId b = 0; b < original.size(); ++b) {
            if (original[b]) cold_.push_back(b);
        }
        return true;
    }

    // Appends a copy of `body`, in its order, with every access on one of
    // `receivers` flagged; returns the copy of `entry`.
    il::BlockId copy(const std::vector<il::BlockId>& body, const std::vector<uint8_t>& in, il::BlockId entry,
                     const std::vector<il::ValueId>& receivers) {
        std::vector<il::BlockId> bmap(fn_.blocks.size(), il::kNoBlock);
        const il::BlockId base = static_cast<il::BlockId>(fn_.blocks.size());
        for (size_t k = 0; k < body.size(); ++k) bmap[body[k]] = base + static_cast<il::BlockId>(k);
        std::unordered_map<il::ValueId, il::ValueId> vmap;
        for (il::BlockId b : body) {
            for (const auto& p : fn_.blocks[b].params) vmap[p.id] = static_cast<il::ValueId>(fn_.valueCount++);
            for (const auto& inst : fn_.blocks[b].instructions) {
                if (inst.result != il::kNoValue) vmap[inst.result] = static_cast<il::ValueId>(fn_.valueCount++);
            }
        }
        const auto mapV = [&](il::ValueId v) {
            const auto it = vmap.find(v);
            return it == vmap.end() ? v : it->second;
        };
        const auto mapT = [&](il::BlockTarget t) {
            if (t.block != il::kNoBlock && t.block < in.size() && in[t.block]) t.block = bmap[t.block];
            for (auto& a : t.args) a = mapV(a);
            return t;
        };
        for (il::BlockId b : body) {
            const il::Block& src = fn_.blocks[b];
            il::Block out;
            out.id = bmap[b];
            out.handler = src.handler;
            out.copyClass = src.copyClass;
            out.copyRegion = src.copyRegion;
            out.cold = src.cold;
            for (const auto& p : src.params) out.params.push_back({mapV(p.id), p.type});
            for (uint32_t i = 0; i < src.instructions.size(); ++i) {
                const il::Instruction& orig = src.instructions[i];
                il::Instruction inst = orig;
                for (auto& o : inst.operands) o = mapV(o);
                inst.target = mapT(orig.target);
                inst.elseTarget = mapT(orig.elseTarget);
                if (inst.result != il::kNoValue) inst.result = mapV(orig.result);
                if (isElemAccess(orig) &&
                    std::binary_search(receivers.begin(), receivers.end(), orig.operands[0])) {
                    inst.immI32 |= il::kElemFlagFloatViewReceiver;
                    if (const auto n = setNumbers_.find({b, i}); n != setNumbers_.end()) {
                        il::Instruction box;
                        box.op = il::Op::Box;
                        box.type = il::Type::Dynamic;
                        box.boxType = il::Type::F64;
                        box.result = static_cast<il::ValueId>(fn_.valueCount++);
                        box.operands = {mapV(n->second)};
                        box.span = orig.span;
                        inst.operands[2] = box.result;
                        out.instructions.push_back(std::move(box));
                    }
                }
                out.instructions.push_back(std::move(inst));
            }
            fn_.blocks.push_back(std::move(out));
        }
        return bmap[entry];
    }

    // The entry block's `jump entry(args)` becomes a chain of tests, one per
    // receiver: each float view leads on to the next test and, after the
    // last, into the copy; the first that is not one goes to the original.
    void enterThrough(il::BlockId entry, il::BlockId copyEntry, const std::vector<il::ValueId>& receivers) {
        const il::Instruction jump = fn_.blocks[0].instructions.back();
        fn_.blocks[0].instructions.pop_back();
        std::vector<il::BlockId> tests{0};
        for (size_t r = 1; r < receivers.size(); ++r) {
            il::Block blk;
            blk.id = static_cast<il::BlockId>(fn_.blocks.size());
            blk.copyClass = fn_.blocks[0].copyClass;
            blk.copyRegion = fn_.blocks[0].copyRegion;
            blk.handler = fn_.blocks[0].handler;
            tests.push_back(blk.id);
            fn_.blocks.push_back(std::move(blk));
        }
        for (size_t r = 0; r < receivers.size(); ++r) {
            il::Instruction t = jump;
            t.op = il::Op::IsTypedArray;
            t.type = il::Type::Bool;
            t.result = static_cast<il::ValueId>(fn_.valueCount++);
            t.operands = {receivers[r]};
            t.immI32 = il::kTypedArrayAnyFloat;
            t.target = {};
            t.elseTarget = {};
            il::Instruction br = jump;
            br.op = il::Op::Branch;
            br.operands = {t.result};
            br.target = r + 1 < receivers.size() ? il::BlockTarget{tests[r + 1], {}}
                                                 : il::BlockTarget{copyEntry, jump.target.args};
            br.elseTarget = il::BlockTarget{entry, jump.target.args};
            fn_.blocks[tests[r]].instructions.push_back(std::move(t));
            fn_.blocks[tests[r]].instructions.push_back(std::move(br));
        }
    }

    il::Function& fn_;
    std::vector<il::BlockId> cold_;
    // A flagged store (block, index) -> the raw unbox of its value that
    // dominates it.
    std::map<std::pair<il::BlockId, uint32_t>, il::ValueId> setNumbers_;
};

}  // namespace

bool applyTypedBodyVersions(il::Module& module, ColdBlocks* cold) {
    bool any = false;
    for (il::Function& fn : module.functions) {
        if (fn.isExternal || fn.blocks.empty() || fn.isCoroutineBody()) continue;
        BodyVersioner v(fn);
        if (!v.run()) continue;
        any = true;
        if (cold) {
            auto& blocks = (*cold)[fn.name];
            blocks.insert(blocks.end(), v.coldBlocks().begin(), v.coldBlocks().end());
        }
    }
    return any;
}

}  // namespace bronze::lower
