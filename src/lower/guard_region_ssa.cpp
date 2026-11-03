// The slow copy's names for an entry region whose split blocks do not dominate
// what they reach (guard_region.h, `RegionPlan::repairNames`).
//
// The rewrite names a value in the slow copy by the dominator chain: a use reads
// the tail parameter of the nearest split block above it that re-defines the
// value, or the original definition. That is the whole answer when every split
// block dominates the region blocks it reaches. When one does not —
// `Quaternion.slerpFlat`, whose `Math` calls sit in an `if` whose join every
// other path reaches too — a join below it has two kinds of predecessor: the
// path from a trampoline, which carries the tail's parameter, and the path from
// the head of the slow copy, which carries the original definition. The join
// needs a parameter of its own for the value, and the dominator chain has no
// block to find it in.
//
// So the names are re-derived over the PRUNED function, one value at a time, by
// the on-demand construction of Braun et al. ("Simple and Efficient
// Construction of Static Single Assignment Form"): a use reads the value's last
// definition earlier in its block, else the value at the block's entry, which
// is the one predecessor's value at its end, or a new block parameter fed by
// every predecessor's. A parameter whose arguments are all one value (or the
// parameter itself) is that value and is removed again. Pruning comes first
// because it is what makes the question answerable: the head of the slow copy
// a trampoline skipped is gone, so its definitions are not on offer.

#include <algorithm>
#include <cstdint>
#include <utility>

#include "lower/guard_region.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

struct Repair {
    il::Function& fn;
    const Cfg& cfg;
    const std::vector<uint8_t>& slow;     // by block: a block of this region's slow copy
    const std::vector<uint8_t>& handler;  // by block: some block's handler
    std::vector<uint8_t> inClass;         // by value: one of the names being re-derived
    std::vector<il::ValueId> entryValue;  // by block, memoised
    std::vector<il::ValueId> created;     // parameters this pass added
    il::ValueId nextValue = 0;
    il::Type type = il::Type::Dynamic;
    bool failed = false;

    Repair(il::Function& f, const Cfg& c, const std::vector<uint8_t>& s, const std::vector<uint8_t>& h)
        : fn(f), cfg(c), slow(s), handler(h), nextValue(f.valueCount) {}

    // The last name of the value that block `b` defines before instruction
    // `index` (all of the block for `index == SIZE_MAX`), or kNoValue.
    il::ValueId lastDefBefore(il::BlockId b, size_t index) const {
        const il::Block& block = fn.blocks[b];
        const size_t end = std::min(index, block.instructions.size());
        for (size_t i = end; i-- > 0;) {
            const il::ValueId r = block.instructions[i].result;
            if (r != il::kNoValue && r < inClass.size() && inClass[r]) return r;
        }
        for (size_t p = block.params.size(); p-- > 0;) {
            const il::ValueId id = block.params[p].id;
            if (id < inClass.size() && inClass[id]) return id;
        }
        return il::kNoValue;
    }

    il::ValueId atEnd(il::BlockId b) {
        const il::ValueId v = lastDefBefore(b, SIZE_MAX);
        return v != il::kNoValue ? v : atEntry(b);
    }

    il::ValueId atEntry(il::BlockId b) {
        if (failed) return il::kNoValue;
        if (entryValue[b] != il::kNoValue) return entryValue[b];
        // Only the slow copy is re-derived. A walk that leaves it found a path
        // on which nothing defines the value, which is not a function the
        // rewrite may produce.
        if (!slow[b] || cfg.preds[b].empty()) {
            failed = true;
            return il::kNoValue;
        }
        std::vector<il::BlockId> preds = cfg.preds[b];
        std::sort(preds.begin(), preds.end());
        preds.erase(std::unique(preds.begin(), preds.end()), preds.end());
        if (preds.size() == 1) {
            const il::ValueId v = atEnd(preds[0]);
            entryValue[b] = v;
            return v;
        }
        // A join. A handler is entered from inside a block, with no edge to
        // carry an argument on, so it cannot take the parameter.
        if (handler[b]) {
            failed = true;
            return il::kNoValue;
        }
        const il::ValueId param = nextValue++;
        if (inClass.size() <= param) inClass.resize(param + 1, 0);
        inClass[param] = 1;
        entryValue[b] = param;
        fn.blocks[b].params.push_back(il::BlockParam{param, type});
        created.push_back(param);
        for (il::BlockId p : preds) {
            const il::ValueId arg = atEnd(p);
            if (failed) return il::kNoValue;
            il::Instruction& term = fn.blocks[p].instructions.back();
            for (il::BlockTarget* t : {&term.target, &term.elseTarget}) {
                if (t->block == b) t->args.push_back(arg);
            }
        }
        return param;
    }
};

// Every value an instruction reads.
template <typename Fn>
void forEachUse(il::Instruction& inst, Fn&& fn) {
    for (il::ValueId& id : inst.operands) fn(id);
    for (il::ValueId& id : inst.target.args) fn(id);
    for (il::ValueId& id : inst.elseTarget.args) fn(id);
}

// The parameter's position in its block, or SIZE_MAX.
size_t paramIndex(const il::Block& block, il::ValueId id) {
    for (size_t p = 0; p < block.params.size(); ++p) {
        if (block.params[p].id == id) return p;
    }
    return SIZE_MAX;
}

// Removes the added parameters that stand for a single value, replacing their
// uses with it, until none is left.
void removeTrivial(il::Function& fn, std::vector<il::ValueId>& created) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t c = 0; c < created.size(); ++c) {
            const il::ValueId param = created[c];
            il::BlockId owner = il::kNoBlock;
            size_t index = SIZE_MAX;
            for (size_t b = 0; b < fn.blocks.size() && owner == il::kNoBlock; ++b) {
                index = paramIndex(fn.blocks[b], param);
                if (index != SIZE_MAX) owner = static_cast<il::BlockId>(b);
            }
            if (owner == il::kNoBlock) continue;
            il::ValueId same = il::kNoValue;
            bool trivial = true;
            for (const il::Block& block : fn.blocks) {
                if (block.instructions.empty()) continue;
                const il::Instruction& term = block.instructions.back();
                for (const il::BlockTarget* t : {&term.target, &term.elseTarget}) {
                    if (t->block != owner || index >= t->args.size()) continue;
                    const il::ValueId arg = t->args[index];
                    if (arg == param || arg == same) continue;
                    if (same != il::kNoValue) trivial = false;
                    same = arg;
                }
            }
            if (!trivial || same == il::kNoValue) continue;
            for (il::Block& block : fn.blocks) {
                for (il::Instruction& inst : block.instructions) {
                    for (il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                        if (t->block == owner && index < t->args.size()) {
                            t->args.erase(t->args.begin() + static_cast<std::ptrdiff_t>(index));
                        }
                    }
                    forEachUse(inst, [&](il::ValueId& id) {
                        if (id == param) id = same;
                    });
                }
            }
            fn.blocks[owner].params.erase(fn.blocks[owner].params.begin() +
                                          static_cast<std::ptrdiff_t>(index));
            created.erase(created.begin() + static_cast<std::ptrdiff_t>(c));
            changed = true;
            break;
        }
    }
}

}  // namespace

bool repairSplitNames(il::Function& fn, uint32_t region,
                      const std::vector<std::pair<il::ValueId, il::ValueId>>& tailNames,
                      const std::vector<il::Type>& types) {
    // Each value with every name it has in the slow copy: the original first,
    // then the tails' parameters.
    std::vector<std::vector<il::ValueId>> classes;
    std::vector<size_t> classOf(fn.valueCount, SIZE_MAX);
    for (const auto& [original, tail] : tailNames) {
        if (original >= classOf.size()) return false;
        if (classOf[original] == SIZE_MAX) {
            classOf[original] = classes.size();
            classes.push_back({original});
        }
        classes[classOf[original]].push_back(tail);
    }

    const Cfg cfg = buildCfg(fn);
    std::vector<uint8_t> slow(fn.blocks.size(), 0);
    std::vector<uint8_t> handler(fn.blocks.size(), 0);
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const il::Block& block = fn.blocks[b];
        slow[b] = block.copyClass == il::CopyClass::Slow && block.copyRegion == region ? 1 : 0;
        if (block.handler != il::kNoBlock && block.handler < handler.size()) handler[block.handler] = 1;
    }

    il::ValueId nextValue = fn.valueCount;
    for (const std::vector<il::ValueId>& names : classes) {
        if (names.size() < 2) continue;
        Repair r(fn, cfg, slow, handler);
        r.nextValue = nextValue;
        r.inClass.assign(nextValue, 0);
        for (il::ValueId v : names) {
            if (v < r.inClass.size()) r.inClass[v] = 1;
        }
        r.entryValue.assign(fn.blocks.size(), il::kNoValue);
        r.type = names[0] < types.size() ? types[names[0]] : il::Type::Dynamic;

        for (size_t b = 0; b < fn.blocks.size() && !r.failed; ++b) {
            if (!slow[b]) continue;
            for (size_t i = 0; i < fn.blocks[b].instructions.size() && !r.failed; ++i) {
                bool reads = false;
                forEachUse(fn.blocks[b].instructions[i], [&](il::ValueId& id) {
                    reads = reads || (id < r.inClass.size() && r.inClass[id]);
                });
                if (!reads) continue;
                il::ValueId reaching = r.lastDefBefore(static_cast<il::BlockId>(b), i);
                if (reaching == il::kNoValue) reaching = r.atEntry(static_cast<il::BlockId>(b));
                if (r.failed) break;
                // Re-read after the lookup: `atEntry` may have appended
                // arguments to this very instruction, and they are already the
                // value at the block's end, which is `reaching`.
                forEachUse(fn.blocks[b].instructions[i], [&](il::ValueId& id) {
                    if (id < r.inClass.size() && r.inClass[id]) id = reaching;
                });
            }
        }
        if (r.failed) return false;
        nextValue = r.nextValue;
        removeTrivial(fn, r.created);
    }
    fn.valueCount = nextValue;
    return true;
}

}  // namespace bronze::lower
