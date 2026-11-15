#pragma once

// The AVAILABLE-EXPRESSION TABLE of the dominator walk: for each computation
// the walk has seen, the value that already holds its answer.
//
// One table serves value numbering and load elimination. An entry whose read
// is empty is a pure computation and lives until its scope is popped; an
// entry with a read is a memory read (or a store's forwarded value) and dies
// at the first write that kills its read (location.h, writeKillsRead).
//
// Scoped like opt::FactEnv: entering a dominator-tree node pushes a scope,
// leaving it pops, and a pop undoes the scope's additions, kills and hides —
// a kill in one subtree says nothing about a sibling, whose paths never ran
// the killing instruction.
//
// HIDING: `hideAll` makes every entry made so far invisible until the scope
// that hid them is popped. The walk hides at a resume point (code after it is
// entered from S with only the point's state in hand, so no value computed
// before it may be reused after it) and on entering a handler from the middle
// of its own dominator (whatever the dominator computed after the throwing
// instruction never existed on that path).

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "il/il.h"
#include "opt/redundancy/location.h"

namespace bronze::opt::redundancy {

// What defines a computation's value: the op, the result type, the
// immediates that select the operation, and the operands. NOT the span, the
// IC site or the slow-site counter, which say where a computation is and not
// what it computes.
struct ExprKey {
    il::Op op = il::Op::ConstUndefined;
    il::Type type = il::Type::Void;
    il::Type boxType = il::Type::Void;
    int32_t immI32 = 0;
    uint64_t immBits = 0;  // immF64's bit pattern: 0.0 and -0.0 differ, as do NaNs
    uint32_t calleeIndex = 0;
    uint32_t keyIndex = 0;
    uint32_t envDepth = 0;
    uint32_t envIndex = 0;
    uint32_t callEnvHops = 0;
    uint32_t directTarget = 0;
    std::vector<ValueId> operands;

    bool operator==(const ExprKey&) const = default;
};

struct ExprKeyHash {
    size_t operator()(const ExprKey& key) const;
};

// The key of `inst` as it stands (operands already resolved by the caller).
// Normalised: the operands of a commutative op are sorted, and the env reads
// drop the binding's name and immutability, which do not change what the slot
// holds.
ExprKey exprKeyOf(const il::Instruction& inst);

// The key a read of `where` by `loadOp` at result type `type` would have, for
// registering a value under a load it was not made by (a store's forwarded
// value; the checked env read standing for the plain one). Built through
// exprKeyOf on a synthetic instruction so the two cannot disagree.
ExprKey loadKey(il::Op loadOp, il::Type type, const Location& where);

struct Available {
    ValueId value = kNoValue;
    bool forwarded = false;  // the entry came from a store, not a read
};

class ExprTable {
public:
    void push();
    void pop();

    // The newest visible, live entry for `key`; value kNoValue when none.
    Available lookup(const ExprKey& key) const;
    // Records that `value` holds `key`'s answer. `read` is what the answer
    // depends on (empty for a pure computation).
    void add(const ExprKey& key, ValueId value, const Access& read, bool forwarded = false);

    // Kills every live entry whose read `write` kills.
    void kill(const Access& write, const AliasInfo& alias);
    void killAll(const WriteSet& writes, const AliasInfo& alias);
    // See the header comment.
    void hideAll();

private:
    struct Entry {
        ExprKey key;
        ValueId value;
        Access read;
        bool forwarded;
        bool alive;
    };
    enum class LogKind : uint8_t { Added, Killed, Hid };
    struct LogEntry {
        LogKind kind;
        uint32_t index;  // Added / Killed: the entry; Hid: the floor before
    };
    void killEntry(uint32_t i);

    std::vector<Entry> entries_;
    // Entries with a non-empty read, oldest first: the ones a write can kill.
    std::vector<uint32_t> memEntries_;
    // Lookups only; never iterated.
    std::unordered_map<ExprKey, std::vector<uint32_t>, ExprKeyHash> index_;
    std::vector<LogEntry> log_;
    std::vector<size_t> scopes_;
    // Entries below this index are hidden.
    uint32_t floor_ = 0;
};

}  // namespace bronze::opt::redundancy
