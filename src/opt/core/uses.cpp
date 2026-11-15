#include "opt/core/uses.h"

namespace bronze::opt {

DefUse::DefUse(const il::Function& fn) {
    // Size to the largest id actually present as well as valueCount, so a
    // function whose count lags its ids is still answered rather than read
    // out of bounds.
    size_t n = fn.valueCount;
    auto grow = [&](ValueId v) {
        if (v != kNoValue && v >= n) n = static_cast<size_t>(v) + 1;
    };
    grow(fn.params.empty() ? 0 : static_cast<ValueId>(fn.params.size() - 1));
    for (const il::Block& block : fn.blocks) {
        for (const il::BlockParam& p : block.params) grow(p.id);
        for (const il::Instruction& inst : block.instructions) {
            grow(inst.result);
            forEachUse(inst, grow);
        }
    }
    defs_.resize(n);
    types_.assign(n, il::Type::Void);
    uses_.resize(n);

    for (size_t i = 0; i < fn.params.size(); ++i) {
        defs_[i] = DefSite{DefSite::Kind::Param, 0, static_cast<uint32_t>(i)};
        types_[i] = fn.params[i].type;
    }
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const il::Block& block = fn.blocks[b];
        const BlockId id = static_cast<BlockId>(b);
        for (size_t p = 0; p < block.params.size(); ++p) {
            const ValueId v = block.params[p].id;
            if (v == kNoValue) continue;
            defs_[v] = DefSite{DefSite::Kind::BlockParam, id, static_cast<uint32_t>(p)};
            types_[v] = block.params[p].type;
        }
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const il::Instruction& inst = block.instructions[i];
            const uint32_t at = static_cast<uint32_t>(i);
            if (inst.result != kNoValue) {
                defs_[inst.result] = DefSite{DefSite::Kind::Instruction, id, at};
                types_[inst.result] = inst.type;
            }
            for (size_t s = 0; s < inst.operands.size(); ++s) {
                if (inst.operands[s] == kNoValue) continue;
                uses_[inst.operands[s]].push_back(
                    UseSite{id, at, UseSite::Kind::Operand, static_cast<uint32_t>(s)});
            }
            for (size_t s = 0; s < inst.target.args.size(); ++s) {
                if (inst.target.args[s] == kNoValue) continue;
                uses_[inst.target.args[s]].push_back(
                    UseSite{id, at, UseSite::Kind::ThenArg, static_cast<uint32_t>(s)});
            }
            for (size_t s = 0; s < inst.elseTarget.args.size(); ++s) {
                if (inst.elseTarget.args[s] == kNoValue) continue;
                uses_[inst.elseTarget.args[s]].push_back(
                    UseSite{id, at, UseSite::Kind::ElseArg, static_cast<uint32_t>(s)});
            }
        }
    }
}

const DefSite& DefUse::def(ValueId v) const {
    static const DefSite kNone;
    return v < defs_.size() ? defs_[v] : kNone;
}

il::Type DefUse::type(ValueId v) const { return v < types_.size() ? types_[v] : il::Type::Void; }

const std::vector<UseSite>& DefUse::uses(ValueId v) const {
    static const std::vector<UseSite> kNone;
    return v < uses_.size() ? uses_[v] : kNone;
}

const il::Instruction* DefUse::defInst(const il::Function& fn, ValueId v) const {
    const DefSite& d = def(v);
    if (d.kind != DefSite::Kind::Instruction) return nullptr;
    return &fn.blocks[d.block].instructions[d.index];
}

const il::Instruction* DefUse::defInstIs(const il::Function& fn, ValueId v, il::Op op) const {
    const il::Instruction* inst = defInst(fn, v);
    return inst != nullptr && inst->op == op ? inst : nullptr;
}

ValueId& useRef(il::Function& fn, const UseSite& use) {
    il::Instruction& inst = fn.blocks[use.block].instructions[use.inst];
    switch (use.kind) {
        case UseSite::Kind::Operand: return inst.operands[use.slot];
        case UseSite::Kind::ThenArg: return inst.target.args[use.slot];
        case UseSite::Kind::ElseArg: return inst.elseTarget.args[use.slot];
    }
    return inst.operands[use.slot];
}

namespace {

template <typename F>
size_t rewriteUses(il::Function& fn, F&& replace) {
    size_t count = 0;
    auto visit = [&](ValueId& v) {
        const ValueId to = replace(v);
        if (to != v) {
            v = to;
            ++count;
        }
    };
    for (il::Block& block : fn.blocks) {
        for (il::Instruction& inst : block.instructions) {
            for (ValueId& v : inst.operands) visit(v);
            for (ValueId& v : inst.target.args) visit(v);
            for (ValueId& v : inst.elseTarget.args) visit(v);
        }
    }
    return count;
}

}  // namespace

size_t replaceAllUses(il::Function& fn, ValueId from, ValueId to) {
    if (from == to) return 0;
    return rewriteUses(fn, [&](ValueId v) { return v == from ? to : v; });
}

size_t replaceAllUses(il::Function& fn, const std::vector<ValueId>& map) {
    // Resolve chains once, with a step bound so a cycle in a malformed map
    // ends rather than spins.
    std::vector<ValueId> resolved(map.size(), kNoValue);
    for (size_t v = 0; v < map.size(); ++v) {
        ValueId cur = static_cast<ValueId>(v);
        for (size_t steps = 0; steps <= map.size(); ++steps) {
            if (cur >= map.size() || map[cur] == kNoValue || map[cur] == cur) break;
            cur = map[cur];
        }
        if (cur != v) resolved[v] = cur;
    }
    return rewriteUses(fn, [&](ValueId v) {
        return v < resolved.size() && resolved[v] != kNoValue ? resolved[v] : v;
    });
}

ValueId newValue(il::Function& fn) { return fn.valueCount++; }

il::Block& newBlock(il::Function& fn) {
    il::Block& block = fn.blocks.emplace_back();
    block.id = static_cast<BlockId>(fn.blocks.size() - 1);
    return block;
}

}  // namespace bronze::opt
