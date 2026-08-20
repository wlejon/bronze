#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/property_key.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

// The property and member protocol: how a key is decoded, what order own keys
// come back in, and what a receiver answers for a name ECMA-262 defines and
// bronze has not built.
//
// A key means the same thing whichever direction it is used in, so the READ
// dispatch (rt_prop.cpp), the WRITE dispatch (rt_prop_write.cpp), `delete`,
// `in` and the enumeration walks all ask these through one implementation
// rather than each carrying its own. Two copies of the canonical-index test
// would be two answers to `a["01"]`.

namespace bronze::runtime {

// A plain object's own keys in ECMA-262 6.1.7.1 OwnPropertyKeys order:
// integer-like keys ascending, then the remaining STRING keys in insertion
// order, then the SYMBOL keys in insertion order. The keys are arena-interned
// or arena-allocated, so they are immortal and non-moving and stay valid across
// the allocations a caller makes while walking them.
std::vector<PropertyKey> rtOwnKeysOrdered(const struct ObjectHeader* obj,
                                          bool enumerableOnly = true);

// The same walk with the symbol keys dropped, which is what every ENUMERATION
// caller wants: `Object.keys`, `Object.entries`, `for-in`, `JSON.stringify` and
// `Object.getOwnPropertyNames` are all defined over string keys alone (7.3.23
// EnumerableOwnProperties takes `key-of-type-String` as a filter). Its own
// function rather than a flag, so that a caller reading the result as strings
// cannot have got a symbol.
std::vector<StringHeader*> rtOwnStringKeysOrdered(const struct ObjectHeader* obj,
                                                  bool enumerableOnly = true);

// A heap copy of an arena-interned key. Every consumer that hands a shape key
// back to a program needs one — the arena string is immortal and the heap
// string is an ordinary JS value — and it has to preserve the ENCODING:
// re-reading a UTF-16 key's bytes as UTF-8 produced mojibake for any property
// name outside Latin-1.
Value rtCopyKeyToHeap(const StringHeader* key);

// The own keys of a STRING as a fresh array of heap strings, in 10.4.3.3
// OwnPropertyKeys order: the indices ascending, then `length` — which
// `enumerableOnly` drops, because 10.4.3.4 defines it non-enumerable where
// 10.4.3.5 makes every index enumerable. That one flag is the whole difference
// between `Object.keys("ab")` and `Object.getOwnPropertyNames("ab")`.
Value rtStringOwnKeyNames(Value strVal, bool enumerableOnly);

// Is this key an ARRAY INDEX spelled as a string? Enumeration order asks it and
// so does console.log of an object, which reports the same order — one test, so
// the two answers cannot drift.
bool rtIsIntegerLikeKey(std::string_view key, uint32_t& out);

// Diagnose `key` if it is a real member of `receiver` that bronze has not
// implemented; return quietly otherwise, so the caller reads `undefined`,
// which is what the language says for a property that does not exist.
// The tables are the ECMA-262 question "does this member exist?", never
// "have we got round to it?" — see rt_members.cpp.
void rtCheckUnimplementedMember(const char* receiver, const char* const* names, size_t count,
                                const std::string& key);

// The same check against each prototype rt_members.cpp carries a table for.
// A member that lands leaves its table, so these only ever fire for names
// ECMA-262 defines and bronze has not built.
void rtCheckArrayMember(const std::string& key);
void rtCheckStringMember(const std::string& key);
void rtCheckFunctionMember(const std::string& key);

// The Array table asked without the refusal: does 23.1.3 define this member,
// whether or not bronze has built it. `in` needs the question in this form —
// the property EXISTS and only its value is missing — where a READ of the same
// name is the hard error above.
bool rtArrayMemberUnimplemented(const std::string& key);
// The typed-array table takes the RECEIVER's constructor name, so the message
// says `Uint8Array.prototype.sort` and not `%TypedArray%.prototype.sort`: nine
// views share one implementation, and a diagnostic that forgot which one the
// program was holding would be a worse message for the sake of the
// implementation's convenience.
void rtCheckTypedArrayMember(const char* kindName, const std::string& key);

// ToPropertyKey (7.1.19) into the immortal arena form a DictEntry can hold, and
// own-property existence over it. Both are shared by the `Object` statics and
// the `Object.prototype` methods, which are the same operations with the
// receiver in a different position (20.1.2.13 and 20.1.3.2) — writing either
// twice is how the two would come to disagree about a dictionary-mode object.
PropertyKey rtInternPropertyKey(Value keyVal);
bool rtHasOwnPropertyNamed(Rooted<Value>& self, Value key);

// A property read on a PRIMITIVE receiver (rt_prop_primitive.cpp): a string
// reaches `String.prototype` and a boolean `Boolean.prototype` by the ordinary
// prototype walk, while a number's and a symbol's members are still handed out
// beside the value. The one branch of the property path that is not about the
// receiver's own storage, which is why it is not in rt_prop.cpp.
Value rtPrimitiveMember(Value objVal, const std::string& keyStr, StringHeader* keyHeader,
                        struct InlineCacheSite* ic);

// ---- the property path's shared key decoding (rt_prop.cpp) ------------------
//
// A key means the same thing whichever direction it is used in, so the READ
// dispatch (rt_prop.cpp) and the WRITE dispatch (rt_prop_write.cpp) ask these
// through one implementation rather than each carrying its own. Two copies of
// the canonical-index test would be two answers to `a["01"]`, and the pair of
// them would drift the way `o.k` and `o[k]` once did.

// The inline-cache entry a property site owns, as the runtime's type. Inline
// because it is a cast on the hot path and nothing else; the entry is null only
// for a caller with no site to cache against.
inline struct InlineCache* rtAsCache(uint64_t* entry) noexcept {
    return reinterpret_cast<struct InlineCache*>(entry);
}

// The same pointer read as the whole SITE. Way 0 is at offset 0, so the two
// casts of one pointer agree by construction — which is what lets a write
// path keep speaking of a single entry while a read path scans four.
inline struct InlineCacheSite* rtAsCacheSite(uint64_t* entry) noexcept {
    return reinterpret_cast<struct InlineCacheSite*>(entry);
}

// Record, in this site's cache, that `keyStr` is on NEITHER the receiver nor
// any link of its prototype chain — after which generated code answers
// `undefined` inline instead of walking. Refuses for every receiver and key a
// shape-keyed entry cannot speak for; rt_prop_absent.cpp lists them.
//
// Called from the read path's plain-object tail only, and only once the
// diagnostics for a name ECMA-262 defines and bronze has not built have run.
void rtInstallAbsentEntry(struct InlineCacheSite* site, Value objVal, const std::string& keyStr);

// Does this key name an ELEMENT of a receiver that stores its elements by
// index? The canonical-array-index test and nothing else, in both spellings a
// key arrives in — already a string, or still a value.
bool rtKeyAsIndex(const std::string& key, uint32_t& out);
bool rtValueToElementIndex(Value idxVal, uint32_t& out);

// The REST of ToPropertyKey (7.1.19), for a computed key that named no element:
// the ToString of a primitive, as a heap string. Step 1 — ToPrimitive, for an
// object key — is not here and must already have run at the ABI entry point,
// because it calls user code where this only allocates. rt_key.cpp says why the
// split falls there.
Value rtElemKeyAsString(Value idxVal);

// Is this array the one an `arguments` binding holds (ECMA-262 10.2.11)? bronze
// stands an ordinary array in for the object, so this is the brand: the `callee`
// accessor is the one own named property it is ever given, and nothing a program
// can write puts a named property on an array. `Object.prototype.toString`'s
// step 5 asks it; nothing else can tell the two receivers apart.
bool rtIsArgumentsObject(Value v);

// The plain object a receiver keeps SYMBOL-keyed properties on: itself, or —
// for a function — the side object its statics live in. Null for every receiver
// with no shape, which the two directions report differently and so is not
// decided here.
struct ObjectHeader* rtSymbolKeyHolder(Value objVal);

// ---- what a symbol key names, for the receivers with no prototype object
//      to name it on (rt_prop_symbol.cpp) ----

// The answer a WELL-KNOWN symbol has on a receiver whose intrinsic prototype
// bronze does not build. `handled` separates "no answer here" from "the answer
// is undefined" — the same bits and different facts, since only the first may
// fall through to the ordinary walk. ALLOCATES: several answers are heap
// strings, one materializes an intrinsic, and even the well-known-symbol
// COMPARISONS intern on first use — which is why the receiver and key arrive
// as roots rather than values: every internal read stays current across
// those allocations.
Value rtWellKnownSymbolMember(Rooted<Value>& obj, Rooted<Value>& key, bool& handled);

// Where a symbol-keyed READ starts its prototype walk. Deliberately not
// `rtSymbolKeyHolder`: a primitive has no storage to write a symbol-keyed
// property into, but it does have a chain to read one off. ALLOCATES, because
// the first read of any intrinsic builds it.
Value rtSymbolReadStart(Value v);

// ---- an array's own properties that are not elements (rt_prop_array.cpp) ----
//
// `length` and the named ones. Six paths ask — a read, a write, `in`, `delete`,
// `for-in` and the own-key walks — and each reaches an array from its own file,
// so the question is answered in one place rather than restated in six.

// Is `name` an own NAMED property of this array, and with what attributes? No
// allocation, so the answer is good until the next one. The receiver must be an
// array; every caller has already dispatched on the kind.
bool rtArrayOwnNamed(Value arrVal, PropertyKey name, PropertyInfo& out);

// This array's own named keys in insertion order. They come AFTER the indices
// in every own-key answer, which is 6.1.7.1's order and needs no sort here: an
// integer-like key names an element and can never reach the named storage.
std::vector<StringHeader*> rtArrayOwnNamedKeys(Value arrVal, bool enumerableOnly = true);

// `a.foo = v`, creating the side object on first use. Allocates and may run an
// inherited setter, so the array arrives through a root.
SetRefusal rtArrayNamedSet(Rooted<Value>& arr, Rooted<Value>& key, Rooted<Value>& val);

// `delete a.foo`, whose false is 13.5.1's — a non-configurable property, which
// only `Object.seal` and `Object.freeze` create here.
bool rtArrayNamedDelete(Value arrVal, PropertyKey name);

// `a.length = v` — ECMA-262 10.4.2.4 ArraySetLength, which truncates or leaves
// a run of holes. Throws the RangeError 10.4.2.4 names for a value that is not
// an array length; the refusal is a frozen `length` or a sealed array's
// elements refusing to be deleted.
SetRefusal rtArraySetLength(Rooted<Value>& arr, Value newLenVal);

// ---- a Map's, Set's, WeakMap's or WeakSet's own named properties
//      (rt_prop_map.cpp) ----
//
// The same seam as the array block above, for the four collections that share
// MapHeader. A Map is an ordinary object with internal slots (24.1.4): its
// ENTRIES are reached by `get`/`set` and are not properties, and its
// PROPERTIES are ordinary and have nothing to do with the entry table. Both
// stores exist, they never see each other, and the same six paths ask this
// one question about the second.

// Is `name` an own named property of this collection? No allocation, so the
// answer is good until the next one. The receiver must be one of the four
// kinds; every caller has already dispatched.
bool rtMapOwnNamed(Value mapVal, PropertyKey name, PropertyInfo& out);

// Its own named keys in insertion order — 6.1.7.1's, with no integer-like key
// to order ahead of them, because an index on a Map is an ordinary name.
std::vector<StringHeader*> rtMapOwnNamedKeys(Value mapVal, bool enumerableOnly = true);

// `m.foo = v`, creating the side object on first use. Allocates and may run an
// inherited setter, so the collection arrives through a root.
SetRefusal rtMapNamedSet(Rooted<Value>& map, Rooted<Value>& key, Rooted<Value>& val);

// `delete m.foo`. Absent is already the state `delete` wants, so a collection
// that never took a named write answers true (13.5.1).
bool rtMapNamedDelete(Value mapVal, PropertyKey name);

// Whether this receiver is one of the four MapHeader kinds — the one test the
// paths above share, so that "a Map or a Set or a weak one" is spelled once.
bool rtIsMapLike(Value v);

// `undefined` for a name that is not an implemented method, so the property
// path can fall through to the unimplemented-member table and then to the
// language's own answer for a property that does not exist. An array's members
// are still answered BESIDE the value this way; a string's moved onto
// `String.prototype` and are found by the ordinary prototype walk.
Value rtArrayMethod(const std::string& key);

// The same table asked for EXISTENCE, which is `in`'s question and allocates
// nothing — a method's value is a function object this would otherwise have to
// build to throw away. Its own function so that `'push' in a` and `a.push` are
// answered from one list: they were two, and `in` reported false for every
// method an array has.
bool rtArrayHasMember(const std::string& key);

}  // namespace bronze::runtime
