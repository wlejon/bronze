#pragma once

#include <cstdint>
#include <string>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze {

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

    static constexpr uint16_t kFlags = 7;

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

// The kind of a value, for that TypeError. `rt_object.cpp` answers the same
// question for "is not a function"; the two spellings are deliberately not
// shared, because that one has to name an array and a typed array and this
// one never sees either.
std::string rtIterableKindName(Value v);

// The property key `Symbol.iterator` evaluates to: bronze has no symbol
// primitive, so the well-known symbol is the string `"@@iterator"`,
// arena-interned so the property path allocates nothing.
StringHeader* rtIteratorKey();

// Is this an own key that stands for a well-known symbol? Such a key is
// created NON-ENUMERABLE, which is what keeps `Object.keys`, `for-in`, object
// spread and console.log agreeing with node about an object that defines its
// own iterator.
bool rtIsWellKnownSymbolKey(const StringHeader* name) noexcept;

// `Symbol` itself — a function object, so that `Symbol("x")` is a named error
// rather than "an object is not a function", carrying `iterator` as its one
// implemented member.
Value rtSymbolFunction();
void rtSymbolCheckMissingMember(Value fn, const std::string& key);

}  // namespace bronze::runtime
