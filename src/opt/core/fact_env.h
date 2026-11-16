#pragma once

// The facts that hold at a point, kept over a walk of the dominator tree.
//
// Entering a block pushes a scope and adds what its entry edge proves;
// walking its instructions kills, at each one, the memory facts its effects
// write; leaving its subtree pops the scope, which undoes both the additions
// AND the kills — a kill in one subtree says nothing about a sibling, whose
// paths never passed the killing instruction.
//
// `walkWithFacts` does all of that for a pass, including the part a plain
// dominator walk gets wrong: at a JOIN (a block with more than one incoming
// edge, a loop header among them) the facts of its immediate dominator only
// hold if nothing on any path from that dominator to the join writes what
// they depend on, so the walker kills by the writes of every block on those
// paths, loop bodies included, before the pass sees the block.

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "il/effects.h"
#include "opt/core/cfg.h"
#include "opt/core/edge_facts.h"
#include "opt/core/uses.h"

namespace bronze::opt {

// The most specific tag a value is known to have.
enum class ValueTag : uint8_t {
    Unknown,
    I32,      // a Number holding an int32
    Number,   // in the double box
    Object,   // a heap object
    Nullish,  // null or undefined
};

class FactEnv {
public:
    // Scopes. Every push is matched by a pop, innermost first.
    void push();
    void pop();
    size_t depth() const { return scopes_.size(); }

    void add(const Fact& fact);
    void addAll(const std::vector<Fact>& facts);
    // Drops every live fact that depends on a location `effects` writes.
    void killBy(const il::Effects& effects);
    void killLocs(il::LocSet writes);
    // Shorthand for killBy(effectsOf(inst)), for walking a block.
    void stepOver(const il::Instruction& inst);

    // Whether a fact of `kind` about `v` (with `imm`, and for InBounds/PtrEq
    // `other`) is known to hold (true), known not to (false), or unknown.
    // The newest matching fact answers.
    std::optional<bool> known(FactKind kind, ValueId v, int64_t imm = 0,
                              ValueId other = kNoValue) const;

    std::optional<bool> boolValue(ValueId v) const;
    ValueTag tagOf(ValueId v) const;
    bool isI32(ValueId v) const { return known(FactKind::I32, v) == true; }
    bool isNumber(ValueId v) const { return known(FactKind::Number, v) == true; }
    // An object test, or any fact only an object can satisfy (a shape, an
    // array kind, a callee).
    bool isObject(ValueId v) const;
    bool isNotHole(ValueId v) const { return known(FactKind::Hole, v) == false; }
    // The layout cell `v`'s shape is known to be, if any.
    std::optional<uint32_t> shapeOf(ValueId v) const;
    // Whether `v` is known NOT to have the shape of cell `cell`.
    bool lacksShape(ValueId v, uint32_t cell) const { return known(FactKind::Shape, v, cell) == false; }
    // The module function `v` is known to be the code of, if any.
    std::optional<uint32_t> calleeOf(ValueId v) const;
    // A dense-array fact covering index `index`: `is.dense_array v, k` held
    // for some k >= index.
    bool isDenseArray(ValueId v, int32_t index = 0) const;
    // An Array: `is.array`, dense or plain.
    bool isArray(ValueId v) const;
    // Whether a holding `is.typed_array` fact (of any kind) is known of `v`.
    bool isTypedArray(ValueId v) const;
    bool inBounds(ValueId index, ValueId length) const {
        return known(FactKind::InBounds, index, 0, length) == true;
    }
    // Whether a holding InRange fact puts base + c in [0, length) for every
    // c in lo..hi (any length when `length` is kNoValue).
    bool inRange(ValueId base, ValueId length, int32_t lo, int32_t hi) const;

    // Every live fact, oldest first (for dumps and tests).
    std::vector<Fact> live() const;

private:
    struct LogEntry {
        uint32_t fact;
        bool added;  // false: a kill
    };
    void indexFact(uint32_t i);
    void unindexFact(uint32_t i);

    std::vector<Fact> facts_;
    std::vector<uint8_t> alive_;
    std::vector<uint32_t> dependent_;  // indices of facts with a non-empty dependsOn
    std::unordered_map<ValueId, std::vector<uint32_t>> byValue_;
    std::vector<LogEntry> log_;
    std::vector<size_t> scopes_;
};

// The union of the writes of every instruction in each block.
std::vector<il::LocSet> blockWrites(const il::Function& fn);

// The writes on every path from `from` (exclusive: its own instructions are
// the caller's to have accounted for) to `to` (inclusive, when a path loops
// back through it), where `from` dominates `to`. Paths through a throw edge
// count the whole throwing block.
il::LocSet writesBetween(const Cfg& cfg, const std::vector<il::LocSet>& writes, BlockId from,
                         BlockId to);

// Walks every reachable block in dominator-tree preorder (children
// ascending), calling `visit(b, env)` with `env` holding exactly the facts
// known on entry to `b`. `visit` walks the block's instructions itself,
// querying `env` and calling `env.stepOver(inst)` after each one it keeps, so
// its children see the facts as the block leaves them. `visit` may add facts
// of its own; they are scoped to the block's subtree. `visit` must not edit
// the CFG.
void walkWithFacts(const il::Function& fn, const Cfg& cfg, const DomTree& dom, const DefUse& defs,
                   FactEnv& env, const std::function<void(BlockId, FactEnv&)>& visit);

}  // namespace bronze::opt
