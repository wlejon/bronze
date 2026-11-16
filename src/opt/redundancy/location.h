#pragma once

// WHAT AN INSTRUCTION INVALIDATES: the pass's one memory model.
//
// il::effectsOf says which abstract location classes (il::Loc) an
// instruction reads and writes. That is enough to be correct and too coarse
// to be useful for the speculation forms: every `slot.store` writes `Slots`,
// yet a store to slot 3 cannot change what a load of slot 5 reads. So an
// access here is the effect table's LocSet PLUS, for the handful of keyed
// accesses, the precise place it touches, read off the instruction's own
// operands and immediates (effects.h invites exactly this refinement). An
// access without a place touches "anywhere in its LocSet".
//
// THE INVALIDATION RULE (writeKillsRead), which every job uses:
//   1. A read of an initialized immutable binding (`env.get` with
//      `envImmutable`) is killed by nothing: the slot is never written again.
//   2. A write whose LocSet does not meet the read's LocSet kills nothing.
//   3. If either side has no place, it kills (the LocSets met).
//   4. Both have places of the same kind:
//        slot     same slot number AND the two objects may alias;
//        element  the arrays may alias AND the indices are not two
//                 different constants;
//        env      same slot index AND NOT (same record value at different
//                 depths, which are different records of one chain);
//        module   always (one location).
//   5. Places of different kinds: kills (cannot happen for one LocSet, but
//      the safe answer costs nothing).
// `callsUnknown` arrives as a write of every location (effects.h) and so kills
// everything but rule 1.
//
// ALIASING (AliasInfo): two different SSA values may name the same object,
// except when one of them is a FRESH, NON-ESCAPING allocation: the result of
// `create.object` / `create.array` whose every use is the object operand of a
// non-capturing access (the list in location.cpp). Nothing else can hold such
// an object, so no other value names it. This is flow-insensitive: an object
// that escapes anywhere (an edge argument, a call, a leave's state, a store of
// it as a value) may alias everywhere. Environment records are never treated
// as fresh: a record is reachable through its children's parent links, which
// the depth of an access walks.

#include <cstdint>
#include <optional>
#include <vector>

#include "il/effects.h"
#include "il/il.h"
#include "opt/core/cfg.h"

namespace bronze::opt::redundancy {

enum class Place : uint8_t {
    None,       // anywhere in the access's LocSet
    Slot,       // named-property slot `a` of object `base`
    Elem,       // element `index` of array `base`
    Env,        // slot `b` of the record `a` parent hops above `base`
    ModuleEnv,  // the module scope's record pointer
    Shape,      // the shape word of object `base`
};

struct Location {
    Place place = Place::None;
    ValueId base = kNoValue;
    ValueId index = kNoValue;
    int64_t a = 0;
    int64_t b = 0;

    bool operator==(const Location&) const = default;
};

struct Access {
    il::LocSet locs;
    Location where;
    // Rule 1 above; only ever set on a read.
    bool immutable = false;

    bool empty() const { return locs.empty(); }
};

// Which values are fresh non-escaping allocations, and which hold an i32
// constant. Built from the function as it stands; a snapshot like a Cfg.
class AliasInfo {
public:
    explicit AliasInfo(const il::Function& fn);

    // Whether `a` and `b` may name the same object. A value always may alias
    // itself.
    bool mayAlias(ValueId a, ValueId b) const;
    bool isFreshUnescaped(ValueId v) const { return v < fresh_.size() && fresh_[v] != 0; }
    std::optional<int32_t> constI32(ValueId v) const;

private:
    std::vector<uint8_t> fresh_;
    std::vector<uint8_t> isConst_;
    std::vector<int32_t> constValue_;
};

// The read and write an instruction makes, from its effects (pass
// `il::effectsOf(inst)`). A `callsUnknown` instruction reads and writes every
// location, without a place.
Access readOf(const il::Instruction& inst, const il::Effects& effects);
Access writeOf(const il::Instruction& inst, const il::Effects& effects);

// The rule above: whether `write` may change what `read` reads.
bool writeKillsRead(const Access& write, const Access& read, const AliasInfo& alias);
// Whether two accesses may touch the same memory at all, immutability ignored
// (a read of an immutable binding still OBSERVES the store that initialized
// it). Used by dead store elimination.
bool mayOverlap(const Access& x, const Access& y, const AliasInfo& alias);
// Whether two places are certainly the same memory: same kind, same base
// value, same slot / depth / index.
bool sameLocation(const Location& x, const Location& y, const AliasInfo& alias);

// The value a keyed store writes (`slot.store`, `elem.store`, `env.set`,
// `module.env.set`), or kNoValue for every other instruction —
// `env.init.tdz` included: it writes the uninitialized marker, which no
// read may be handed.
ValueId storedValue(const il::Instruction& inst);

// A union of writes: the coarse part (locations written with no known place)
// and up to kMaxPrecise keyed writes, past which they fold into the coarse
// part. Used for what a region of blocks writes: the paths between a
// dominator and a join, or a loop body.
class WriteSet {
public:
    static constexpr size_t kMaxPrecise = 64;

    void add(const Access& write);
    void addAll(const WriteSet& other);
    // Whether some write in the set kills `read` (writeKillsRead).
    bool kills(const Access& read, const AliasInfo& alias) const;
    // Every location class written, places dropped: what FactEnv kills by.
    il::LocSet allLocs() const;
    bool empty() const { return coarse_.empty() && precise_.empty(); }

private:
    il::LocSet coarse_;
    std::vector<Access> precise_;
};

// The writes of each block's instructions.
std::vector<WriteSet> blockWriteSets(const il::Function& fn);

// What every path from `from` (exclusive) to `to` (inclusive when a path
// loops back through it) writes, for `from` dominating `to`: the precise
// counterpart of opt::writesBetween, walked the same way.
WriteSet regionWrites(const Cfg& cfg, const std::vector<WriteSet>& writes, BlockId from, BlockId to);

}  // namespace bronze::opt::redundancy
