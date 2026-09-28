#pragma once

// The primitives a host's structured-clone serializer is built from (HTML
// StructuredSerializeInternal / StructuredDeserialize): tell a value's kind
// apart, walk an object's own data properties, a Map's or a Set's entries and
// an array's elements, and build the same shapes back — each one a read of the
// runtime's own layout rather than a trip through the program-level API
// (`Object.entries`, `Array.from`, `instanceof`, a call per `map.set`).
//
// A companion of embed.h, carrying BRONZE_EMBED_API for the same reason it
// does. The same moving-heap contract holds: a Value answered here is current
// until the next call that ALLOCATES, and every such call says so.
//
// The format the host writes is its own; nothing here fixes one. What these
// calls add is speed and one guarantee each: a Kind is a brand check (never a
// prototype walk, so a program cannot make a Map read as a plain object), a
// layout is the object's OwnPropertyKeys order over enumerable string keys,
// and a define is CreateDataProperty — never an inherited setter.

#include <cstdint>
#include <vector>

#include "embed/embed.h"

namespace bronze::embed::clone {

enum class Kind : uint8_t {
    Primitive,    // not an object
    Plain,        // an ordinary object with no internal slots (a class instance included)
    Array,
    Map,
    Set,
    Date,
    Error,
    RegExp,
    ArrayBuffer,
    TypedArray,
    DataView,
    Function,
    Promise,
    Weak,         // WeakMap, WeakSet, WeakRef
    Proxy,
    Handle,       // a host handle (embed_handle.h), whatever its prototype
    Other,        // any other exotic or internal-slot object: the host's generic path
};

// By brand and heap kind, never by prototype. Allocates nothing.
BRONZE_EMBED_API Kind classify(Value v) noexcept;

// A flat string's code units: Latin-1 bytes, or UTF-16 units when `utf16`.
struct Chars {
    const void* data{nullptr};
    uint32_t length{0};  // in code units
    bool utf16{false};
};

// `str` must be a string. The pointer is into the moving heap: copy it out
// before the next allocating call. Allocates nothing.
BRONZE_EMBED_API Chars stringChars(Value str) noexcept;
// A fresh string of those code units, no transcoding. ALLOCATES.
BRONZE_EMBED_API Value makeString(const void* data, uint32_t length, bool utf16);

// An interned property key: immortal and non-moving, so it may be held
// across any allocation and compared by pointer.
using Key = const void*;
BRONZE_EMBED_API Chars keyChars(Key key) noexcept;
// Interns the code units as a property key. ALLOCATES (a transient string);
// the key itself is immortal.
BRONZE_EMBED_API Key internKey(const void* data, uint32_t length, bool utf16);
// The key as a string value (the key object itself; no allocation).
BRONZE_EMBED_API Value keyValue(Key key) noexcept;

// ---- plain objects ----------------------------------------------------------

struct PropSlot {
    Key key;
    uint32_t slot;
};

// A token for the object's layout that is stable for as long as the object
// keeps it: its shape, when that shape is shared (a non-dictionary shape),
// else nullptr. Two objects with one token have one ownDataLayout answer,
// so a host may cache the layout by it. `plain` must be Kind::Plain.
// Allocates nothing.
BRONZE_EMBED_API const void* layoutToken(Value plain) noexcept;

// The object's own ENUMERABLE STRING-keyed properties in OwnPropertyKeys
// order (integer-like keys ascending first, then insertion order), each with
// the slot that holds its value. False — `out` then unspecified — when one of
// them is an accessor: the host takes its generic path, which runs the getter.
// Allocates nothing.
BRONZE_EMBED_API bool ownDataLayout(Value plain, std::vector<PropSlot>& out);

// A slot's value (a Number is boxed as any read boxes it). Allocates nothing.
BRONZE_EMBED_API Value slotValue(Value plain, uint32_t slot) noexcept;

// The prototype of `obj` (null when it has none). Allocates nothing.
BRONZE_EMBED_API Value prototypeOf(Value obj) noexcept;

// CreateDataProperty(obj, key, v) on a plain object: a definition, never an
// inherited setter. `cache` is kDefineCacheWords zero-initialised words the
// host keeps per (layout, position): objects built in the same key order take
// the recorded shape transition, which is what makes a reader that builds ten
// thousand objects of one layout build each at a constructor's speed.
// Answers the object's current value. ALLOCATES.
inline constexpr size_t kDefineCacheWords = 3;
BRONZE_EMBED_API Value defineOwn(Value obj, Key key, Value v, uint64_t* cache);

// ---- arrays -----------------------------------------------------------------

// `arr` must be Kind::Array. A hole reads as undefined. Allocates nothing.
BRONZE_EMBED_API uint32_t arrayLength(Value arr) noexcept;
BRONZE_EMBED_API Value arrayElement(Value arr, uint32_t index) noexcept;
// A fresh array of `length` (its storage sized for all of them). ALLOCATES.
BRONZE_EMBED_API Value newArray(uint32_t length);
// Stores element `index` (< length) of an array from newArray. Answers the
// array's current value. May ALLOCATE (only if the storage must grow).
BRONZE_EMBED_API Value arrayPut(Value arr, uint32_t index, Value v);

// ---- Map / Set --------------------------------------------------------------

// The number of entry positions (deleted ones included), and entry `index`:
// false for a deleted position. A Set answers its element as `key` and
// `value` alike. `coll` must be Kind::Map or Kind::Set. Allocates nothing.
BRONZE_EMBED_API uint32_t collectionPositions(Value coll) noexcept;
BRONZE_EMBED_API bool collectionEntry(Value coll, uint32_t index, Value& key, Value& value) noexcept;
// Fresh empty collections with the intrinsic prototypes. ALLOCATE.
BRONZE_EMBED_API Value newMap();
BRONZE_EMBED_API Value newSet();
// Map.prototype.set / Set.prototype.add (a Set ignores `value`) without the
// method lookup. Answers the collection's current value. ALLOCATES.
BRONZE_EMBED_API Value collectionPut(Value coll, Value key, Value value);

// ---- Date -------------------------------------------------------------------

// [[DateValue]] of a Kind::Date. Allocates nothing.
BRONZE_EMBED_API double dateValue(Value date) noexcept;
// A fresh Date holding `t`. ALLOCATES.
BRONZE_EMBED_API Value newDate(double t);

}  // namespace bronze::embed::clone
