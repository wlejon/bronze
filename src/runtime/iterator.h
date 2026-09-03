#pragma once

#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze {

class Shape;

// One live iteration. Every construct that walks a value — `for-of`, array
// spread, a rest element, array destructuring — opens one of these, steps it,
// and closes it if it stops early.
//
// The record is a heap object rather than a C++ struct on the stack because
// generated code holds it in a GC root slot across the loop body, and because
// `iter.close` on a `break` needs the same object the header block stepped.
// Every field is a Value, so the collector's generic payload scan forwards it
// without this file owning a root source.
struct IterRecordHeader {
    HeapObjectHeader header;  // flags == kFlags

    // The array / string / typed array / Map being walked, or, for a
    // protocol iteration, the ITERATOR object the @@iterator method returned.
    Value target;
    // The iterator's `next` method, read once at open time (7.4.2 GetIterator
    // step 3 reads it once, so an iterator that replaces its own `next`
    // mid-walk does not change what the loop calls).
    Value nextFn;
    Value current;  // what the last step produced
    Value cursor;   // double: the index the fast kinds walk
    Value kind;     // double, one of Kind below
    Value done;     // bool: the iteration is finished, so closing it is a no-op

    static constexpr uint16_t kFlags = HeapKind::Iterator;

    // Which walk this record is. The fast kinds exist because an array, a
    // string and a Map have a cursor the runtime can step directly — no
    // iterator object, no result object, no call into user code per element.
    // `Protocol` is the general answer and the only one a user-defined
    // iterable can take.
    enum Kind : uint32_t {
        Array = 0,
        String = 1,
        TypedArray = 2,
        MapEntries = 3,
        SetValues = 4,
        Protocol = 5,
        // A built-in iterator OBJECT — what `map.values()`, `set.keys()`,
        // `arr.entries()` return — whose `next` and whose `[Symbol.iterator]`
        // are still the intrinsics. `target` is the object itself, and a step
        // reads and writes its internal slots exactly as its `next` would,
        // minus the `{value, done}` object and the call: the object is left as
        // far along as the protocol would have left it, so `it.next()` after a
        // `break` continues where the loop stopped. Closing one still asks the
        // object for a `return` method, because a program may have given it
        // one after the open.
        MapIterator = 6,
        ArrayIterator = 7,
    };

    static IterRecordHeader* create(Heap& heap, uint32_t kind);

    uint32_t kindOf() const noexcept { return static_cast<uint32_t>(kind.asNumber()); }
    uint32_t cursorOf() const noexcept { return static_cast<uint32_t>(cursor.asNumber()); }
};

}  // namespace bronze

namespace bronze::runtime {

// GetIterator (ECMA-262 7.4.2), as a record. Raises the TypeError 7.4.2 step
// 4 defines for a value with no @@iterator method, so the caller must test
// the pending cell.
Value rtOpenIterator(Value source);

// 7.4.1 CreateIterResultObject: `{ value, done }`, in that order, as one
// object of ONE shape for every built-in iterator in the runtime. The shape is
// minted once per thread; a result is then an allocation and two slot stores,
// where building it by name paid a transition scan and an arena copy of both
// keys per object — per element of every `for-of` over a Map.
Value rtCreateIterResult(Rooted<Value>& value, bool done);

// The step of a built-in iterator object, minus the result object: what its
// `next` does to its internal slots, with the produced value handed back raw.
// False is exhaustion, with the iterator latched exactly as `next` latches it.
// Implemented beside each `next` (builtin_map.cpp, builtin_array_iterator.cpp)
// so the two cannot drift; the code pointer is what `rtOpenIterator` compares
// an object's own `next` against before it trusts the step.
bool rtMapIteratorStep(Rooted<Value>& it, Value& produced);
bool rtArrayIteratorStep(Rooted<Value>& it, Value& produced);
bronze_fn_code rtMapIteratorNextCode();
bronze_fn_code rtArrayIteratorNextCode();
Value rtOpenAsyncIterator(Value source);

// The kind of a value, for that TypeError. `rt_object.cpp` answers the same
// question for "is not a function"; the two spellings are deliberately not
// shared, because that one has to name an array and a typed array and this
// one never sees either.
std::string rtIterableKindName(Value v);

// The property key `Symbol.iterator` denotes: the well-known symbol itself
// (runtime/symbol.h), as a Value ready to hand to `getProp`. One identity for
// the whole process, matched by ADDRESS — which is the entire difference from
// the string `"@@iterator"` this used to be, and the reason
// `{ "@@iterator": f }` is no longer an iterable.
Value rtIteratorKey();
Value rtAsyncIteratorKey();

// The prototype chain an ITERATOR OBJECT hangs from, and the reason none of
// them carries an own `[Symbol.iterator]`.
//
// ECMA-262 gives each built-in iterator a prototype of its own —
// %MapIteratorPrototype% (24.1.5.2), %ArrayIteratorPrototype% (23.1.5.2, which
// a typed array's iterator shares by 23.2.5.2), %RegExpStringIteratorPrototype%
// (22.2.9.1), %StringIteratorPrototype% (22.1.5.1) and %GeneratorPrototype%
// (27.5.1) — and all five inherit %IteratorPrototype% (27.1.2), whose ONE
// member is `[Symbol.iterator]() { return this; }` (27.1.2.1). That is where
// the self-hook belongs, and putting it there rather than on each iterator is
// what keeps `Object.getOwnPropertySymbols(m.entries())` at zero: an inherited
// property is not an own one, and `getOwnPropertySymbols` reports own keys.
//
// The kinds are kept apart even though bronze puts no member on most of them,
// because the prototype is what an iterator's `next` recognises its own
// receiver by. A brand check needs the five to be five objects.
// `Helper` and `Wrap` joined for ES2025's iterator helpers: %IteratorHelperPrototype%
// (27.1.4.2) is the prototype of the object `it.map(f)` returns, and
// %WrapForValidIteratorPrototype% (27.1.3.2.1) is the one `Iterator.from` wraps a
// foreign iterator in. Both carry a `next` and a `return` of their own, which is
// why they are kinds here rather than plain objects built somewhere: the brand
// check is what makes those two methods safe to call on a receiver.
enum class IteratorProto : uint32_t {
    Map, Set, Array, RegExpString, Generator, String, AsyncGenerator, Helper, Wrap
};

// The INTERNAL SLOTS each kind carries, named after ECMA-262's. They are real
// fields on the object (`ObjectHeader::internalSlot`) and not properties under
// a reserved name, which is what makes them invisible to
// `Object.getOwnPropertyNames` as well as to `Object.keys`.
//
// The COUNT lives here rather than in the file that reads the slots because it
// is half of the BRAND: `rtIsIteratorObject` asks the question 24.1.5.1 step 3
// spells "does it have an [[IteratedMap]] internal slot", and the only way to
// ask it is the pair (this kind's prototype, this kind's slots).
namespace MapIteratorSlot {
enum : uint32_t { IteratedMap, NextIndex, Kind, kCount };
}
// [[Kind]] joined when `keys` and `entries` did (23.1.5.1 names all three
// slots). A typed array's iterator shares the layout and leaves the slot
// `undefined`: bronze exposes only its `values`, and its `next` never reads it.
namespace ArrayIteratorSlot {
enum : uint32_t { IteratedArrayLike, NextIndex, Kind, kCount };
}
namespace RegExpStringIteratorSlot {
enum : uint32_t { IteratingRegExp, IteratedString, Done, kCount };
}
// 22.1.5.1: [[IteratedString]] and [[StringNextIndex]]. The cursor counts CODE
// UNITS while the iterator steps code POINTS, which is why a step can advance
// it by two.
namespace StringIteratorSlot {
enum : uint32_t { IteratedString, NextIndex, kCount };
}
// A generator object's are GeneratorSlot, in runtime/generator.h — beside the
// state machine that is the only thing that reads them.

// An Iterator Helper object's (27.1.4.2, plus what a lazy helper needs to
// resume): the underlying iterator and its `next`, the closure or the count the
// helper was created with, the index the callbacks are passed, and — for
// `flatMap` alone — the INNER iterator currently being drained.
//
// `Kind` is which helper this is, and it is a slot rather than five prototypes
// because 27.1.4.2 gives all five ONE prototype: `Object.getPrototypeOf([].values().map(f))
// === Object.getPrototypeOf([].values().filter(f))` is true in the language, and
// five kinds here would make it false.
//
// %WrapForValidIteratorPrototype%'s objects are allocated with the same layout
// and use only the first three, so the two kinds are told apart by their
// PROTOTYPE — which is the half of `rtIsIteratorObject`'s brand that
// distinguishes objects with equal slot counts.
namespace IteratorHelperSlot {
enum : uint32_t { Kind, Iterated, NextMethod, Fn, Counter, Inner, InnerNext, State, kCount };
}

// A fresh iterator object of that kind: the kind's prototype, and the internal
// slots above, all `undefined`. The caller fills the slots in and adds `next`.
//
// A generator object goes through here too. It is the one kind whose `next` is
// NOT added afterwards: 27.5.1 puts all three of its methods on
// %GeneratorPrototype%, so the object comes back with its slots to fill and no
// own property at all.
Value rtNewIteratorObject(IteratorProto kind);

// The brand. Both halves are needed: a prototype alone can be forged with
// `Object.create(Object.getPrototypeOf(m.keys()))`, and that object was not
// allocated with the slots, so reading them off it would read past its end; a
// slot count alone does not say WHOSE slots they are, and three kinds have
// three.
bool rtIsIteratorObject(Value v, IteratorProto kind);

// That kind's prototype object. `rtNewIteratorObject` builds one on first use;
// this only reads it back.
Value rtIteratorPrototype(IteratorProto kind);

// %IteratorPrototype% (27.1.2) itself — the ONE object every kind above
// inherits, and the object `Iterator.prototype` denotes. Named separately from
// the per-kind prototypes because it is not one of them: it is their common
// parent, and the helpers of 27.1.4.1 are installed on it.
Value rtIteratorSharedPrototype();

// ---- the iterator helpers (iterator_helpers.cpp) ----------------------------

// ECMA-262 27.1.4.1's members, installed onto %IteratorPrototype% by the
// initializer that builds it. Installing them THERE rather than on each
// iterator kind is what makes every iterator in the program inherit them —
// a generator, an array's iterator, a Map's, a string's and a matchAll's all
// have %IteratorPrototype% on their chain already, so this is the one edit that
// gives all five the eleven methods.
void rtInstallIteratorHelpers(Rooted<Value>& proto);

// %IteratorHelperPrototype% (27.1.4.2) and %WrapForValidIteratorPrototype%
// (27.1.3.2.1): a `next` and a `return` each, installed onto the prototype
// object `iteratorObjectShape` allocates for their kinds — the arrangement
// %GeneratorPrototype%'s three methods already use.
void rtInstallIteratorHelperPrototype(Rooted<Value>& proto);
void rtInstallIteratorWrapPrototype(Rooted<Value>& proto);

// `Iterator` (27.1.3), by the name lowering resolved; `undefined` for anything
// else. The object carries `Iterator.from` as an own property and
// %IteratorPrototype% in its `prototype` slot, so `x instanceof Iterator` is the
// ordinary chain walk and needs nothing of its own.
Value rtIteratorConstructor(const std::string& name);

}  // namespace bronze::runtime
