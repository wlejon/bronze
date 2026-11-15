#include "lower/fresh_literals.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

struct Literal {
    il::BlockId block = il::kNoBlock;
    bool held = false;
    // Its initializing stores: key constant index -> the value stored.
    std::unordered_map<uint32_t, il::ValueId> fields;
    // The literals it is a field of.
    std::vector<il::ValueId> owners;
};

bool initStore(const il::Instruction& inst) {
    if (inst.op != il::Op::PropSet || inst.operands.size() != 2) return false;
    const int32_t mode = il::writeMode(inst);
    return mode == il::kWriteDefine || mode == il::kWriteLiteralElement;
}

// The value a read of a literal's field answers, through reads of literals
// that are themselves fields: `alias[r]` for every `r = prop.get L, k` where L
// (itself possibly such a read) is a literal `usable` admits and k is one of
// its fields. A destructuring pattern's `pattern.check` of a literal is the
// literal: it is an object.
template <typename Usable>
std::vector<il::ValueId> fieldReads(const il::Function& fn, const std::unordered_map<il::ValueId, Literal>& lits,
                                    Usable usable) {
    std::vector<il::ValueId> alias(fn.valueCount, il::kNoValue);
    for (bool changed = true; changed;) {
        changed = false;
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                const bool read = inst.op == il::Op::PropGet && inst.type == il::Type::Dynamic;
                if ((!read && inst.op != il::Op::PatternCheck) || inst.operands.size() != 1 ||
                    inst.result == il::kNoValue || alias[inst.result] != il::kNoValue) {
                    continue;
                }
                il::ValueId recv = inst.operands[0];
                while (recv < alias.size() && alias[recv] != il::kNoValue) recv = alias[recv];
                const auto it = lits.find(recv);
                if (it == lits.end() || !usable(it->second)) continue;
                if (read) {
                    const auto field = it->second.fields.find(inst.keyIndex);
                    if (field == it->second.fields.end()) continue;
                    alias[inst.result] = field->second;
                } else {
                    alias[inst.result] = recv;
                }
                changed = true;
            }
        }
    }
    return alias;
}

// An `is.dense_array` of an array literal that spells every element the test
// covers. The test guards a destructuring's indexed reads, with the iterator
// protocol on its other side; over an array nothing has touched the two
// answer the same (the array iterator is an intrinsic, which is frozen), so
// while the literal is unheld the test is true and its other side is not
// reached.
struct DenseTest {
    il::ValueId cond = il::kNoValue;
    il::ValueId literal = il::kNoValue;
    bool on = true;
};

// The blocks reached from the entry when each test in `tests` that is on
// goes its true way.
std::vector<uint8_t> reachable(const il::Function& fn, const std::vector<DenseTest>& tests) {
    const auto folded = [&](il::ValueId cond) {
        for (const DenseTest& t : tests) {
            if (t.on && t.cond == cond) return true;
        }
        return false;
    };
    std::vector<uint8_t> seen(fn.blocks.size(), 0);
    std::vector<il::BlockId> work{0};
    seen[0] = 1;
    const auto add = [&](il::BlockId b) {
        if (b < seen.size() && !seen[b]) {
            seen[b] = 1;
            work.push_back(b);
        }
    };
    while (!work.empty()) {
        const il::Block& block = fn.blocks[work.back()];
        work.pop_back();
        if (block.handler != il::kNoBlock) add(block.handler);
        for (const il::Instruction& inst : block.instructions) {
            if (inst.op == il::Op::Jump) add(inst.target.block);
            if (inst.op == il::Op::Branch) {
                add(inst.target.block);
                if (inst.operands.empty() || !folded(inst.operands[0])) add(inst.elseTarget.block);
            }
        }
    }
    return seen;
}

bool removeIn(il::Function& fn, const std::vector<uint8_t>& plainKey, const std::vector<uint32_t>& indexKey) {
    std::unordered_map<il::ValueId, Literal> lits;
    std::unordered_map<il::ValueId, int32_t> arrayLength;
    for (il::BlockId b = 0; b < fn.blocks.size(); ++b) {
        for (const il::Instruction& inst : fn.blocks[b].instructions) {
            if ((inst.op == il::Op::CreateObject || inst.op == il::Op::CreateArray) && inst.operands.empty() &&
                inst.result != il::kNoValue) {
                lits[inst.result].block = b;
                if (inst.op == il::Op::CreateArray) arrayLength[inst.result] = inst.immI32;
            }
        }
    }
    if (lits.empty()) return false;
    // The fields: the initializing stores in the literal's own block, each
    // key once. Any other store into it holds it (the use walk below).
    for (il::BlockId b = 0; b < fn.blocks.size(); ++b) {
        for (const il::Instruction& inst : fn.blocks[b].instructions) {
            if (!initStore(inst)) continue;
            const auto it = lits.find(inst.operands[0]);
            if (it == lits.end()) continue;
            Literal& lit = it->second;
            const bool plain = inst.keyIndex < plainKey.size() && plainKey[inst.keyIndex];
            if (lit.block != b || !plain || !lit.fields.emplace(inst.keyIndex, inst.operands[1]).second) {
                lit.held = true;
            }
        }
    }
    // A literal is made, then filled, then read: its stores follow it
    // directly in its block, so every read of a field comes after the store.
    std::vector<il::ValueId> alias = fieldReads(fn, lits, [](const Literal&) { return true; });
    const auto resolve = [&](il::ValueId v) {
        while (v < alias.size() && alias[v] != il::kNoValue) v = alias[v];
        return v;
    };

    std::vector<DenseTest> tests;
    for (const il::Block& block : fn.blocks) {
        for (const il::Instruction& inst : block.instructions) {
            if (inst.op != il::Op::IsDenseArray || inst.operands.size() != 1 || inst.immI32 < 0) continue;
            const il::ValueId lit = resolve(inst.operands[0]);
            const auto len = arrayLength.find(lit);
            if (len == arrayLength.end() || inst.immI32 >= len->second) continue;
            bool spelled = static_cast<size_t>(inst.immI32) < indexKey.size();
            for (int32_t i = 0; spelled && i <= inst.immI32; ++i) {
                spelled = indexKey[i] != UINT32_MAX && lits.at(lit).fields.count(indexKey[i]) != 0;
            }
            if (spelled) tests.push_back({inst.result, lit, true});
        }
    }

    // What holds a literal, over the blocks reached. A test's literal that
    // turns out held gives the test back its other side, which may hold more.
    const auto hold = [&](il::ValueId v) {
        const auto it = lits.find(resolve(v));
        if (it != lits.end()) it->second.held = true;
    };
    for (bool again = true; again;) {
        again = false;
        const std::vector<uint8_t> live = reachable(fn, tests);
        for (il::BlockId b = 0; b < fn.blocks.size(); ++b) {
            if (!live[b]) continue;
            for (const il::Instruction& inst : fn.blocks[b].instructions) {
                for (il::ValueId v : inst.target.args) hold(v);
                for (il::ValueId v : inst.elseTarget.args) hold(v);
                if (initStore(inst) && lits.count(inst.operands[0])) {
                    const auto val = lits.find(resolve(inst.operands[1]));
                    if (val != lits.end()) val->second.owners.push_back(inst.operands[0]);
                    continue;
                }
                if (inst.result != il::kNoValue && inst.result < alias.size() && alias[inst.result] != il::kNoValue) {
                    continue;  // a read of a field, or a pattern's check
                }
                if (inst.op == il::Op::IsDenseArray) {
                    const bool folded = std::any_of(tests.begin(), tests.end(), [&](const DenseTest& t) {
                        return t.on && t.cond == inst.result;
                    });
                    if (folded) continue;
                }
                for (il::ValueId v : inst.operands) hold(v);
            }
        }
        // A field of a held literal is held.
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& [id, lit] : lits) {
                if (lit.held) continue;
                for (il::ValueId owner : lit.owners) {
                    if (lits.at(owner).held) {
                        lit.held = true;
                        changed = true;
                        break;
                    }
                }
            }
        }
        for (DenseTest& t : tests) {
            if (t.on && lits.at(t.literal).held) {
                t.on = false;
                again = true;
            }
        }
    }
    bool any = false;
    for (const auto& [id, lit] : lits) any = any || !lit.held;
    if (!any) return false;

    const auto unheld = [&](il::ValueId v) {
        const auto it = lits.find(v);
        return it != lits.end() && !it->second.held;
    };
    const auto folded = [&](il::ValueId cond) {
        return std::any_of(tests.begin(), tests.end(), [&](const DenseTest& t) { return t.on && t.cond == cond; });
    };
    // The reads that go: those of the literals that go. A held literal's
    // reads stay reads, and what they answer is held with it.
    alias = fieldReads(fn, lits, [](const Literal& lit) { return !lit.held; });
    bool pruned = false;
    for (il::Block& block : fn.blocks) {
        std::vector<il::Instruction> kept;
        kept.reserve(block.instructions.size());
        for (il::Instruction& inst : block.instructions) {
            if (inst.op == il::Op::IsDenseArray && folded(inst.result)) {
                il::Instruction yes;
                yes.op = il::Op::ConstBool;
                yes.type = il::Type::Bool;
                yes.result = inst.result;
                yes.immI32 = 1;
                kept.push_back(std::move(yes));
                continue;
            }
            if (inst.op == il::Op::Branch && !inst.operands.empty() && folded(inst.operands[0])) {
                il::Instruction jump;
                jump.op = il::Op::Jump;
                jump.type = il::Type::Void;
                jump.target = std::move(inst.target);
                for (il::ValueId& v : jump.target.args) v = resolve(v);
                kept.push_back(std::move(jump));
                pruned = true;
                continue;
            }
            if (inst.result != il::kNoValue && (unheld(inst.result) || alias[inst.result] != il::kNoValue)) continue;
            if (inst.op == il::Op::PropSet && !inst.operands.empty() && unheld(inst.operands[0])) continue;
            for (il::ValueId& v : inst.operands) v = resolve(v);
            for (il::ValueId& v : inst.target.args) v = resolve(v);
            for (il::ValueId& v : inst.elseTarget.args) v = resolve(v);
            kept.push_back(std::move(inst));
        }
        block.instructions = std::move(kept);
    }
    if (pruned) {
        std::vector<uint8_t> built(fn.blocks.size(), 1);
        pruneUnreachable(fn, built);
    }
    return true;
}

}  // namespace

std::vector<uint32_t> removeUnheldLiterals(il::Module& module) {
    // `__proto__: v` in a literal sets the prototype; it is not a field.
    std::vector<uint8_t> plainKey(module.keyConstants.size(), 1);
    // The key constant spelling each small index, or UINT32_MAX.
    std::vector<uint32_t> indexKey(16, UINT32_MAX);
    for (size_t k = 0; k < module.keyConstants.size(); ++k) {
        const std::string& key = module.keyConstants[k];
        if (key == "__proto__") plainKey[k] = 0;
        for (size_t i = 0; i < indexKey.size(); ++i) {
            if (key == std::to_string(i)) indexKey[i] = static_cast<uint32_t>(k);
        }
    }
    std::vector<uint32_t> changed;
    for (uint32_t f = 0; f < module.functions.size(); ++f) {
        il::Function& fn = module.functions[f];
        if (fn.isExternal || fn.blocks.empty()) continue;
        if (removeIn(fn, plainKey, indexKey)) changed.push_back(f);
    }
    return changed;
}

}  // namespace bronze::lower
