#pragma once

// THE EFFECT MODEL: one table that says, for every instruction, what it reads,
// what it writes and how control or the heap can move under it. Every pass
// asks this and nothing else; a pass that needs a sharper answer refines it
// from the instruction's own operands and immediates (two `slot.store`s with
// different slot numbers do not alias, whatever their receivers), never from
// a private list of ops.
//
// The contract passes rely on:
//   - Two instructions may swap when neither writes a location the other
//     reads or writes, at most one of them throws, collects or has
//     `observableOrder`, neither is a `barrier`, and a `guarded` one is not
//     moved above the branch that licensed it.
//   - `pure` instructions can be moved anywhere their operands are available,
//     merged with an identical one, and deleted when unused.
//   - `removableIfUnused` instructions can be deleted when their result is
//     unused (an allocation is: nothing can have seen it).
//   - A fact about memory (opt/core edge_facts.h) that depends on a LocSet
//     dies at any instruction whose `writes` meets it.
//   - `callsUnknown` means arbitrary user code can run (getters, valueOf,
//     proxies, a callee): such an instruction reads and writes everything.
//
// Written the SAFE WAY ROUND: an op the table does not list does everything,
// so an op added later is a missed optimisation and never a miscompile.

#include <cstdint>

#include "il/il.h"

namespace bronze::il {

// Abstract heap locations. Coarse on purpose: precision by key (slot index,
// env depth/index, element index) is layered on by the consumer.
enum class Loc : uint16_t {
    Slots = 1u << 0,          // named-property storage of any object, inline and overflow
    Shapes = 1u << 1,         // the shape word of any object; a transition writes it
    Elements = 1u << 2,       // the contents of array element blocks
    // Array length words, and with them the element block's pointer and
    // capacity, which only an append that grows (or code that writes
    // everything) moves.
    ArrayLen = 1u << 3,
    EnvSlots = 1u << 4,       // closure environment record slots
    ModuleEnv = 1u << 5,      // the module scope's singleton record pointer
    Globals = 1u << 6,        // the global object's bindings
    FnFields = 1u << 7,       // function object fields (prototype, home object, ...)
    PrivateFields = 1u << 8,  // private-name tables
    Heap = 1u << 9,           // everything else, and anything not known to be above
    Views = 1u << 10,         // a typed array's length and data address (its buffer can detach)
    TypedElements = 1u << 11, // the bytes of every ArrayBuffer
    // The prototype-mutation epoch and the module cells published under it
    // (`proto.cell`): what every change to a prototype chain moves. Any
    // instruction that can add to, delete from or swap a prototype writes
    // everything anyway.
    Protos = 1u << 12,
    // An Array's NUMBERS bit (runtime/array.h): what a store of a value that
    // may be no Number clears. A store of a known Number (`elem.store` /
    // `array.push` with kStoreNumber) writes `Elements` but not this, so a
    // NUMBERS dense test stays answered across the loop that fills the array.
    ElemKinds = 1u << 13,
    // An operation that may shrink an Array, detach its elements block, or trip
    // array_index_hazard: everything that writes everything writes this, whereas
    // monotonic `array.push` and growing `elem.store` only write `ArrayLen`.
    ArrayShrink = 1u << 14,
};

class LocSet {
public:
    constexpr LocSet() = default;
    constexpr LocSet(Loc l) : bits_(static_cast<uint16_t>(l)) {}
    static constexpr LocSet all() { return LocSet(kAllBits); }
    static constexpr LocSet none() { return LocSet(); }

    constexpr bool empty() const { return bits_ == 0; }
    constexpr bool isAll() const { return bits_ == kAllBits; }
    constexpr bool has(Loc l) const { return (bits_ & static_cast<uint16_t>(l)) != 0; }
    constexpr bool intersects(LocSet o) const { return (bits_ & o.bits_) != 0; }
    constexpr uint16_t bits() const { return bits_; }

    constexpr LocSet operator|(LocSet o) const { return LocSet(static_cast<uint16_t>(bits_ | o.bits_)); }
    constexpr LocSet operator&(LocSet o) const { return LocSet(static_cast<uint16_t>(bits_ & o.bits_)); }
    constexpr LocSet& operator|=(LocSet o) {
        bits_ = static_cast<uint16_t>(bits_ | o.bits_);
        return *this;
    }
    constexpr bool operator==(const LocSet&) const = default;

private:
    static constexpr uint16_t kAllBits = (1u << 15) - 1;
    explicit constexpr LocSet(uint16_t bits) : bits_(bits) {}
    uint16_t bits_ = 0;
};

constexpr LocSet operator|(Loc a, Loc b) { return LocSet(a) | LocSet(b); }

struct Effects {
    LocSet reads;
    LocSet writes;
    // Control may leave for the block's handler (or the caller) here. True for
    // `throw` itself, unlike `canThrow`, which answers the backend's narrower
    // question of whether an exception test follows a non-terminator.
    bool throws = false;
    // A heap object may move: an allocation, or user code that might make one.
    bool collects = false;
    // Arbitrary user code may run. Implies reads and writes everything.
    bool callsUnknown = false;
    // Makes a new heap object.
    bool allocates = false;
    // Order against other `observableOrder` instructions is visible outside
    // the heap model: I/O, the feedback instrument, keep.alive, leaving.
    bool observableOrder = false;
    // Unchecked: correct only under the dominating branch edge that
    // established its precondition (il_op.h, the speculation forms). Never
    // hoisted above that branch, even when otherwise free to move.
    bool guarded = false;
    // Nothing moves across it at all, pure instructions included: a
    // `resume.point`, which is an entry into the function.
    bool barrier = false;
    // Derived: no reads, no writes, no throw, no collection, no allocation,
    // no order, not guarded, not a barrier. Freely movable, CSE-able,
    // removable if unused.
    bool pure = false;
    // Derived: deleting it when its result is unused changes nothing
    // observable (no writes, no throw, no user code, no order, no barrier).
    bool removableIfUnused = false;
};

// The effects of one instruction. Never fails: an op without an entry gets
// `everything()`.
Effects effectsOf(const Instruction& inst);

// The answer for an op that can run anything.
Effects everythingEffects();

}  // namespace bronze::il
