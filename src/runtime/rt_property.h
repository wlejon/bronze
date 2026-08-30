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

// An arena-interned key as an ordinary JS value — the arena object ITSELF,
// not a copy. A JS string has no observable identity (`===` is content
// equality) and every string is immutable, so a program cannot tell the
// immortal arena key from the heap copy this used to mint; the collector
// skips out-of-reservation payloads by design. What handing out the one
// object buys is stated at the definition (rt_object.cpp): no allocation per
// key per enumeration, and frame-stable identity for the computed-read
// cache's string-key latch.
Value rtKeyAsValue(const StringHeader* key);

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

// And the WRITE half of the same receiver kind, in the same file, because the
// two are one question about one kind: where does the answer come from when it
// does not come from the receiver? `key` is the name as a value, `keyStr` the
// same name as text for the message, and `strict` decides whether 13.15.2 step
// 8's false becomes a TypeError or nothing at all. Both spellings of the write
// call it, so a refusal cannot be built into one of them and not the other.
void rtPrimitiveWrite(Rooted<Value>& recv, Rooted<Value>& key, const std::string& keyStr,
                      Rooted<Value>& val, bool strict);

// A property read on a FUNCTION receiver (rt_prop_function.cpp): a class
// constructor, an ordinary function, one of the interned intrinsic singletons.
// Its own file for the same reason the primitive one is: it is one receiver
// kind, and it is the kind whose answer comes from four places in a fixed
// order rather than from a shape and a slot. `site` is the read's inline-cache
// site, which this may warm when the answer turns out to be an own data
// property of the function's statics object — the one step of that order that
// IS a shape and a slot.
uint64_t rtFunctionMember(Value objVal, const std::string& keyStr, StringHeader* keyHeader,
                          struct InlineCacheSite* site);

// THE SEAM. `BRONZE_NO_FN_STATICS_IC=1` stops the statics fills, so that one
// binary A/Bs them together with codegen's half of the same variable. Latched
// at heap init beside the other seams that are not in the ABI's TLS block.
void fnStaticsIcReadSeam() noexcept;

// May a shape-keyed entry describe this (function receiver, statics box) pair?
// The refusals — intrinsic constructor, dictionary box, the seam — and why each
// is a correctness condition are stated once, at the definition in
// rt_prop_function.cpp. Asked by the read fill there and by the method-call
// latch in rt_method_call.cpp; an ACCESSOR property is each caller's own gate,
// since only the caller knows whether what it found is one.
bool rtStaticsBoxCacheable(Value fnVal, const ObjectHeader* box) noexcept;

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

// The one place a refused Set becomes a TypeError: ECMA-262 10.1.9.2 answers
// false three ways, and 13.15.2 PutValue step 6.d raises for a STRICT
// reference and discards for a sloppy one. Declared here rather than kept
// private to rt_prop_write.cpp because `throw` is not always the CODE's
// strictness: 20.1.2.1 Object.assign spells its copy `Set(to, k, v, true)`, so
// it raises out of sloppy code too, and it must raise the same three errors a
// strict `to.k = v` would rather than a fourth opinion about the same refusal.
void rtReportSetRefusal(SetRefusal refusal, bool strict, const std::string& key);

// 10.1.9.2 OrdinarySetWithOwnDescriptor with a RECEIVER that is not the object
// the chain walk starts at. `Reflect.set` and `super.k = v` are the two
// spellings, and step 2 sends a data write to the RECEIVER rather than to the
// holder — which is the whole content of the operation, so it is answered once
// (rt_reflect.cpp) rather than restated per spelling. The key must already
// have been through ToPropertyKey, and `target` must not be a proxy; a
// pending exception comes back as `None` and stays pending.
SetRefusal rtOrdinarySetWithReceiver(Rooted<Value>& target, Rooted<Value>& key,
                                     Rooted<Value>& val, Rooted<Value>& receiver);

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
