#pragma once

#include <string>
#include <utility>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"

// The intrinsics: the namespace objects, the constructor objects, and the
// prototype method tables a program reaches through them.
//
// Each family owns its own translation unit and exposes exactly two things here:
// the object itself, and the miss check that keeps an unimplemented member loud
// instead of `undefined`. The tables stay beside the members they name, so a
// member that lands or leaves changes the file it is written in and not a
// registry somewhere else — what is declared here is only how the property path
// reaches them.

namespace bronze::runtime {

// A function's `.prototype` and its own-property object, created on first
// demand: a function that is never used as a constructor and never given a
// static member pays for neither. Both allocate, so both take the function
// through a root and the caller must re-derive any raw pointer afterwards.
void rtEnsureFunctionPrototype(Rooted<Value>& fnVal);
void rtEnsureFunctionProperties(Rooted<Value>& fnVal);
Value rtGlobalThisObject();
Value rtReflectNamespace();
// `Date` (builtin_date.cpp). The constructor object for the global ladder, its
// identity for the `extends` refusal, and the two questions the printing paths
// ask: is this a Date, and what does one print as. The BRAND itself and the
// instance's internal slots are builtin_date_internal.h's, because only the two
// Date translation units may read them.
Value rtDateConstructor();
bool rtIsDateConstructor(Value fn);
// True — with `out` filled — for a Date. Neither allocates a JS value nor runs
// user code, which is what lets `console.log`'s inspect walk (whose whole
// contract is that it cannot move the heap) ask.
bool rtDateInspectText(Value v, std::string& out);
bool rtIsDateObject(Value v);
// The builtin half of global resolution, shared by `bronze_global_get` and
// the global object's population (rt_object.cpp) so the two cannot drift.
bool rtResolveBuiltinGlobal(const std::string& keyStr, Value& out);
// The host-global registry's entries, for the same population. host_globals.h
// is the EXTERNAL surface (registration and lookup); enumeration is internal.
const std::vector<std::pair<std::string, Value>>& rtHostGlobalEntries();
// Own-property lookup on the global object, for `bronze_global_get`'s
// user-defined-global fallback. Distinguishes an absent name from one
// assigned `undefined`, which the fallback needs and a plain read cannot give.
bool rtGlobalThisOwnLookup(const std::string& name, Value& out);

// 10.2.9 SetFunctionName and 10.2.10 SetFunctionLength, filled in from the key
// index the creator was given. `BRONZE_ABI_FN_NAME_NONE` leaves both absent.
void rtSetFunctionNameAndLength(struct FunctionHeader* fn, uint32_t nameKey, uint32_t length);

// A NATIVE builtin as a function object, interned by code pointer.
//
// It records NO `name` and NO `length`, and that is a decision rather than an
// omission: `arity` in the tables these are built from is the count a short
// call is padded to, which is not 10.2.10's `length` (`Object.assign` pads to 0
// and has length 2), and a `length` copied from it would be a wrong answer
// where none is a diagnosed missing one. So `Object.keys.name` stays the named
// hard error it has always been while a function bronze COMPILED answers, which
// is the split rt_members.cpp's tables already draw everywhere else.
inline Value rtNativeFunction(bronze_fn_code code, uint32_t arity) {
    // No slot cell: a native builtin belongs to no compiled module, so there
    // is no module-local table to cache it in. The by-code-pointer map is the
    // authority regardless, and it is what answers here.
    // BRONZE_ABI_FN_FLAGS_ORDINARY, because a native builtin has no syntax
    // behind it to say otherwise: `Array` and `Map` really are constructors,
    // and the tables these are built from do not record which of the rest are
    // not. So the constructibility of a NATIVE stays what it has always been,
    // and only functions bronze COMPILED carry the syntax's answer.
    return Value(bronze_function_singleton(code, arity, /*length=*/0,
                                           BRONZE_ABI_FN_NAME_NONE,
                                           BRONZE_ABI_FN_FLAGS_ORDINARY |
                                               BRONZE_ABI_FN_FLAG_NATIVE,
                                           /*slotCell=*/nullptr));
}

// ---- builtin namespaces ---------------------------------------------------
// Each family owns its own translation unit and exposes exactly two things:
// the namespace object, and the miss check that keeps an unimplemented
// member loud instead of `undefined`.

Value rtMathObject();

// The `Atomics` namespace object (ECMA-262 25.4), built once on first use like
// `Math`, and the named refusal for the three operations on it that need an
// agent cluster bronze does not have.
Value rtAtomicsObject();
void rtAtomicsCheckMissingMember(Value obj, const std::string& key);
void rtMathCheckMissingMember(Value obj, const std::string& key);

Value rtObjectNamespace();
void rtObjectCheckMissingMember(Value obj, const std::string& key);

// `Object.prototype`: the intrinsic every plain object's chain ends at, and a
// real object rather than a table consulted beside the chain — so a program can
// hold it, compare it, and add to it. `rtPlainObjectShape` names it as the
// prototype of every `{}`, which is the one edge that makes it the value model
// rather than a builtin.
//
// Reentrancy: this and `rtObjectNamespace` above reference each other
// (`Object.prototype.constructor` and `Object.prototype`), so both are built by
// one initializer and either accessor triggers it.
Value rtObjectPrototype();

// The members of `Object.prototype` (builtin_object_proto.cpp), installed onto
// the object builtin_object.cpp allocates. Two files for one pair of intrinsics
// because 20.1.2.1 and 20.1.3.1 make the namespace and the prototype each
// other's property, so one initializer has to hold both — the arrangement
// `String.prototype`'s members already reach their object through.
void rtInstallObjectProtoMethods(Rooted<Value>& proto);

// `Function.prototype.call` / `.apply`, answered beside a function rather than
// found on a prototype object — a FunctionHeader has no shape for a walk to
// follow. `undefined` for every other name, which leaves `bind`, `name` and
// `length` to the unimplemented table in rt_members.cpp.
Value rtFunctionMethod(const std::string& key);
// The miss check for a plain object's prototype chain: a name 20.1.3 defines
// and bronze has not built. Reached only after the whole chain misses, which is
// why it is safe to apply to every plain object — a program's own property of
// the same name is found first and never reaches here.
void rtObjectProtoCheckMissingMember(const std::string& key);

// The rest of the chain for a receiver with NO SHAPE — a function, an array, a
// Map, a Set, a RegExp, a typed array, an ArrayBuffer, a DataView, a number, a
// symbol. Each answers its members from a C table standing in for a prototype
// object bronze has not built, and before this step the search ended at that
// table: `f.hasOwnProperty` read `undefined` while `f.toString`, one link
// nearer, was diagnosed by name.
//
// Called at the TAIL of each receiver's own lookup, after its table has both
// answered and refused, so a member the intermediate prototype defines still
// shadows this object's. builtin_object_proto.cpp says why skipping the
// intermediate is exact. `undefined` means the whole chain missed, and the
// named refusal for an unimplemented 20.1.3 member has already fired.
//
// A module namespace is NOT a caller: 10.4.6.1 fixes its [[Prototype]] at null,
// so its own exports really are the end of its chain.
Value rtObjectProtoMember(Rooted<Value>& receiver, const std::string& key);
bool rtObjectProtoHasMember(const std::string& key);

// `[Symbol.toStringTag]: tag`, as the non-enumerable data property every clause
// that defines one asks for (21.3.1.9 for `Math`, 25.5.3 for `JSON`, 27.5.1.5
// for %GeneratorPrototype%, and the iterator prototypes' own clauses). It lives
// beside `Object.prototype.toString`, which is the only thing that reads it,
// so an object that carries a tag and the code that consults one cannot drift
// about what the property is called or how it is defined.
//
// Non-enumerable is not tidiness: these are prototype and namespace objects,
// and `for-in` walks a chain — an enumerable tag on %IteratorPrototype% would
// appear in every for-in over every iterator in the program.
void rtDefineToStringTag(Rooted<Value>& obj, const char* tag);

// The intrinsics a function of a NON-ORDINARY form reports (27.3, 27.4, 27.7):
// its `constructor` — %GeneratorFunction%, %AsyncFunction% or
// %AsyncGeneratorFunction% — and its [[Prototype]], which is that
// constructor's `prototype` object. Both answer `undefined` for an ordinary
// function, whose answers are `Function` and %Function.prototype% and are
// given elsewhere.
Value rtFunctionKindConstructor(Value fnVal);
Value rtFunctionKindPrototype(Value fnVal);

// A member of %Function.prototype% read off one of those three prototype
// objects. Their chains end at a link the plain-object walk will not cross, so
// an inherited `call` would read `undefined`; this makes it a diagnostic
// instead. Fires only for those exact objects, and only once one has been
// built.
void rtFunctionKindCheckMissingMember(Value obj, const std::string& key);

// ---- the primitive wrappers (builtin_wrappers.cpp) --------------------------

// A native method installed on an intrinsic PROTOTYPE object. The tables stay
// beside the members they name — `String.prototype`'s plain members in
// builtin_string.cpp, its pattern-taking ones in builtin_string_regexp.cpp — so
// a member that lands or leaves changes the file it is written in and not a
// registry somewhere else.
struct NativeMethod {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

// Define each as a NON-ENUMERABLE own property. That attribute is not tidiness:
// `for-in` walks the prototype chain, so one enumerable member on
// `String.prototype` would appear in every for-in over every string in the
// program (22.1.3, 20.3.3).
void rtDefineMethods(Rooted<Value>& proto, const NativeMethod* methods, size_t count);
void rtInstallStringMethods(Rooted<Value>& proto);
void rtInstallStringPatternMethods(Rooted<Value>& proto);
void rtInstallNumberMethods(Rooted<Value>& proto);

// `String.prototype`, `Boolean.prototype` and `Number.prototype`: real objects
// on the real chain, which a primitive reaches by the ordinary prototype walk
// rather than through a table consulted beside it. A program can hold any of
// them, compare it, add to it and pass its methods to `.call` — where an
// array's and a function's members are still handed out BESIDE the value.
//
// `Symbol.prototype` is the fourth and lives in symbol.h, because 20.4.3 makes
// it an ordinary object rather than a wrapper.
Value rtStringPrototype();
Value rtBooleanPrototype();
Value rtNumberPrototype();

// The String (10.4.3), Boolean (20.3) and Number (21.1) exotic objects: a plain
// object with one internal slot holding the wrapped primitive, and the matching
// intrinsic on its chain.
Value rtMakeStringWrapper(Rooted<Value>& str);
Value rtMakeBooleanWrapper(bool value);
Value rtMakeNumberWrapper(double value);

// The [[StringData]] / [[BooleanData]] / [[NumberData]] of a wrapper; false for
// every other value. The brand is the internal-slot count paired with the
// slot's type — builtin_wrappers.cpp says why it is not the (prototype, count)
// pair an iterator object uses.
bool rtStringWrapperData(Value v, Value& out);
bool rtBooleanWrapperData(Value v, Value& out);
bool rtNumberWrapperData(Value v, Value& out);

// 22.1.3.35 thisStringValue / 20.3.3.3 thisBooleanValue / 21.1.3's
// thisNumberValue: the primitive itself, or a wrapper's slot. False for
// anything else, which the caller reports as the TypeError those clauses name.
bool rtThisStringValue(Value self, Value& out);
bool rtThisBooleanValue(Value self, Value& out);
bool rtThisNumberValue(Value self, Value& out);

// One code unit of a string, as a String of length 1 — 10.4.3.5's answer, and
// the value `s[i]` is. `undefined` past the end. ALLOCATES.
Value rtStringCharAsString(Value str, uint32_t index);

// 7.1.1's answer for a primitive WRAPPER, without running the algorithm:
// OrdinaryToPrimitive would call `valueOf`, and for a pristine wrapper that
// call answers exactly the internal slot. False for every other object, so the
// caller's named error for the general algorithm stands.
//
// It is what a site that CANNOT call `rtToPrimitive` uses instead — the one
// that is allocation-free and cannot raise. `valueToString` (rt_convert.cpp) is
// the last such site: console.log and JSON.stringify reach it, and a wrapper
// they print must not become a call into user code. Every conversion a program
// spells runs the real algorithm and never comes here.
bool rtWrapperPrimitive(Value v, Value& out);

// An own property 10.4.3 gives a String, computed from the CHARACTERS rather
// than from an object. `writable` and `configurable` are false for every one of
// them, so `enumerable` is the only attribute that varies: an index is
// (10.4.3.5), `length` is not (10.4.3.4).
struct StringOwnProperty {
    Value value;
    bool enumerable;
};

// 10.4.3.5 StringGetOwnProperty plus 10.4.3.4's `length`, asked of the string
// ITSELF. Both a primitive string and the String exotic object ToObject would
// build answer from exactly this, which is why it is one function — and it is
// what lets `Object.hasOwn("ab", 0)` and `Object.getOwnPropertyNames("ab")`
// answer without allocating a box to read a constant off it. ALLOCATES (an
// index property's value is a fresh one-code-unit string).
bool rtStringDataOwnProperty(Value str, const std::string& key, StringOwnProperty& out);
// The same question with no value produced and so no allocation, for the
// callers that only need to know whether the key names one.
bool rtStringDataHasOwnKey(Value str, const std::string& key);

// The same, for a receiver that is the exotic OBJECT: unwraps [[StringData]]
// and asks the above. False means "not an own property of this exotic object",
// which is the fall-through to the ordinary lookup that 10.4.3 requires.
// ALLOCATES.
bool rtStringExoticOwnProperty(Value obj, const std::string& key, Value& out);

// Is `key` an own property of this [[StringData]], and therefore one a write
// cannot change? 10.4.3 makes every one of them — each index (10.4.3.5) and
// `length` (10.4.3.4) — non-writable and non-configurable, so the write is a
// no-op in sloppy code and a TypeError in strict (13.15.2 PutValue step 6.d,
// through 10.1.9.2's false). CopyDataProperties passes `strict` true whatever
// mode its caller is in, because 7.3.25 step 5.c.ii spells `Set(to, key, v,
// true)`.
//
// True means the write is REFUSED, whether or not it threw. It has to be asked
// ahead of the ordinary property path: a wrapper is a plain object with a
// shape, so `setProp` would store a slot under "0" happily — and every read
// consults 10.4.3.5 first, so nothing could ever observe it. A property that
// exists and cannot be read is the worse half of a wrong answer.
bool rtStringDataWriteRefused(Value stringData, const std::string& key, bool strict);

// Refuse `operation` by name when `v` is a String object with characters in it.
// 10.4.3.3 OwnPropertyKeys puts index properties ahead of the ordinary own
// keys, and bronze answers those on the property path only — so every
// own-key operation would report an object with no indices, which is a wrong
// answer rather than a missing one (cases/blocked/string_object_own_keys.js).
void rtCheckStringExoticOwnKeys(Value v, const char* operation);

// `new String(x)` / `new Boolean(x)`. True — with `out` set — when `fn` is one
// of the two; `bronze_construct` has nothing else to do for them, since the
// wrapper IS the instance rather than something a body fills in.
bool rtConstructPrimitiveWrapper(Value fn, uint32_t argc, const uint64_t* argv, Value& out);

// 21.1.1.1's body as a conversion, and its step 1 pulled out so the `new` form
// can run the same one. `rtNumberValueOfArgument` is ToNumeric, exception
// included: a SYMBOL is the TypeError 6.1.5.1 names, THROWN here where
// `rtToNumber` can only be fatal.
uint64_t rtNumberConstructorBody(uint64_t env, uint64_t thisBits, uint32_t argc,
                                 const uint64_t* argv);
Value rtNumberValueOfArgument(Value v);
// 21.1.2's fourteen own properties of `Number`, as non-enumerable own
// properties of the function object. Idempotent; `rtNativeFunction` interns on
// the code pointer, so every route reaches the one object.
void rtInstallNumberStatics(Rooted<Value>& fn);

// The four function properties of the global object that ECMA-262 19.2
// defines over numbers: `isNaN`, `isFinite`, `parseInt`, `parseFloat`.
// `undefined` for any other name. They live beside the `Number` statics
// because two of them ARE the `Number` statics, interned by code pointer.
Value rtGlobalNumericFunction(const std::string& name);

// A name 21.1.3 defines on `Number.prototype` and bronze has not built,
// diagnosed rather than read as `undefined`. Asked only after the intrinsic
// itself has missed.
void rtCheckNumberProtoMember(const std::string& key);

Value rtJsonNamespace();
void rtJsonCheckMissingMember(Value obj, const std::string& key);
Value rtJsonParse(std::string_view utf8);

// 25.5.2 SerializeJSONProperty over the root, which is what `JSON.stringify`
// is. Separate from the namespace object because it is a pinned BYTE FORMAT
// with its own file, and deliberately not the one `console.log` uses.
// `undefined` for a root the algorithm omits.
Value rtJsonStringify(Value value, Value replacer, Value space);

// `Map` / `Set`, by the name lowering resolved. `undefined` for anything else.
Value rtMapConstructor(const std::string& name);
// A method of a Map (or, with `isSetReceiver`, of a Set), by name. The two
// tables differ — `add` is a Set's and `get`/`set` are a Map's — which is why
// the receiver kind is a parameter rather than something the caller applies
// afterwards.
Value rtMapMethod(bool isSetReceiver, const std::string& key);
// `"Map"` / `"Set"` when this function object IS one of the two interned
// constructors, else nullptr. The property path needs the NAME, not just the
// fact, so that the intrinsic bronze has not built can be refused by it.
const char* rtMapConstructorName(Value fn);

// ECMA-262 24.2.4's set operations (builtin_set_ops.cpp), as the table beside
// their bodies. `rtMapMethod` and `rtMapHasMember` both read it, so a Set's
// members are one list however they are asked for — and the seven live in their
// own file because they are the only members of a Set that read a SECOND
// collection, through the set-like protocol rather than another Set's table.
const NativeMethod* rtSetOperationMethods(size_t& count);

// The intrinsic constructors bronze builds no prototype OBJECT for — the Map
// family, the nine views, DataView — by name, else nullptr. One list, because
// two facts depend on it: the property path answers `X.prototype` with a named
// refusal, and `rtEnsureFunctionPrototype` must leave the FunctionHeader slot
// empty so that generated code's inline read of it misses and reaches that
// refusal instead of a fresh empty object.
const char* rtNoPrototypeObjectIntrinsic(Value fn);
void rtCheckMapMember(bool isSetReceiver, const std::string& key);
// Whether 24.1.3 / 24.2.3 give the receiver `key` at all — what `in` asks. It
// reads the same tables `rtMapMethod` does and ends at the same named refusal,
// so the operator and a property read never disagree about a Map's members.
bool rtMapHasMember(bool isSetReceiver, const std::string& key);
// What `m[Symbol.iterator]` answers: a Map's default iterator is `entries`
// and a Set's is `values` (24.1.3.12, 24.2.3.11).
Value rtMapDefaultIterator(bool isSetReceiver);

// `WeakMap` / `WeakSet` (builtin_weak_map.cpp), in exactly the Map/Set
// arrangement: a constructor by name for the global ladder, the constructor's
// name back from the function object, a method by name for the property path,
// `in`'s predicate off the same tables, and the named refusal for a member
// ECMA-262 defines and bronze has not built. No default iterator, because
// 24.3 and 24.4 define none — a WeakMap is not iterable.
// One typed-array element as a JS value, and one store into one
// (builtin_typed_array.cpp). They exist as a pair because the two BigInt views
// answer BigInts where the other ten answer Numbers, and every read and write
// path in the runtime — the keyed fast path, the computed path, `from`, `of`,
// construction from an array-like — has to make the same distinction. One
// funnel, so none of them can come to believe every element is a double.
//
// The read ALLOCATES for a BigInt kind; the write runs ToNumber or ToBigInt and
// so can run user code and can throw, which is why it takes the view rooted.
Value rtTypedArrayElement(Value viewVal, uint32_t index);
void rtTypedArraySetElement(Rooted<Value>& view, uint32_t index, Value value);

Value rtWeakCollectionConstructor(const std::string& name);
const char* rtWeakCollectionConstructorName(Value fn);
Value rtWeakCollectionMethod(bool isWeakSetReceiver, const std::string& key);
bool rtWeakCollectionHasMember(bool isWeakSetReceiver, const std::string& key);
void rtCheckWeakCollectionMember(bool isWeakSetReceiver, const std::string& key);

// Rows of builtin_array.cpp's method table whose bodies live in a translation
// unit of their own — `sort` (builtin_array_sort.cpp) and the three iterator
// methods (builtin_array_iterator.cpp). Declared here so the table stays the
// ONE list of what Array.prototype implements; rt_prop.cpp also hands
// `rtArrayValuesBuiltin` out as `[Symbol.iterator]`, which 23.1.3.41 makes
// the SAME function object — an identity the code-pointer intern table gives
// for free.
uint64_t rtArraySortBuiltin(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtArrayToStringBuiltin(uint64_t env, uint64_t thisBits, uint32_t argc,
                                const uint64_t* argv);
uint64_t rtArrayValuesBuiltin(uint64_t env, uint64_t thisBits, uint32_t argc,
                              const uint64_t* argv);
uint64_t rtArrayKeysBuiltin(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtArrayEntriesBuiltin(uint64_t env, uint64_t thisBits, uint32_t argc,
                               const uint64_t* argv);
uint64_t rtObjectProtoToString(uint64_t env, uint64_t thisBits, uint32_t argc,
                               const uint64_t* argv);

// ECMA-262 7.3.35 GroupBy's two members, both in builtin_group_by.cpp because
// they are one operation with keyCoercion flipped. Declared here so the tables
// that publish them — `Object`'s static list in builtin_object.cpp and the
// `Map` constructor's own-member answer in builtin_map.cpp — each stay the one
// list of what their intrinsic implements.
uint64_t rtObjectGroupBy(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtMapGroupBy(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

// The array-like protocol (builtin_constructors.cpp): does this value have an
// @@iterator (23.1.2.1 step 3's GetMethod), how long is it (7.3.18
// LengthOfArrayLike) and what is at index i. `Array.from` is where the two
// protocols first had to be told apart; `Function.prototype.apply`,
// `Reflect.apply` and the typed-array constructors ask the same question, and
// ask it here so four callers cannot disagree about `{length: 2}`.
bool rtHasIteratorMethod(Rooted<Value>& src);
uint32_t rtArrayLikeLength(Rooted<Value>& src);
Value rtArrayLikeElement(Rooted<Value>& src, uint32_t index);

// The bound on an argument list built from an array-like. Answers false having
// thrown a RangeError that names the count and the limit.
constexpr uint32_t kMaxAppliedArguments = 65535;
bool rtCheckAppliedArgumentCount(uint32_t count, const char* member);

// The own members of the `Map` / `Set` constructor FUNCTION objects — today
// just `Map.groupBy`. Answered from a table here for the reason
// `rtTypedArrayStatic` is: a constructor is an interned function singleton
// with no property object to install statics into, so the property path asks
// this instead.
bool rtMapStatic(Value fn, const std::string& key, Value& out);

// The same table, written into the constructor's `properties` box so that a
// SUBCLASS reaches it by the ordinary chain walk rather than by a code-pointer
// match it can never make (runtime/native_base.h says why the two spellings of
// one table are not a second list: both loops read `kMapStatics`). False when
// this function is neither `Map` nor `Set`.
bool rtInstallMapStatics(Rooted<Value>& ctor);

// The `Array.prototype` OBJECT — the value the expression denotes, built from
// the same method table an array answers beside itself, never a link on any
// array's chain (builtin_array.cpp's comment above it says why both halves
// hold). The identity test and the miss check exist for the two paths a plain
// object takes: a write to it is refused by name (rt_prop_write.cpp), and a
// full-chain read miss on it is diagnosed against the Array unimplemented
// table rather than read as `undefined` (rt_prop.cpp).
Value rtArrayPrototypeObject();
bool rtIsArrayPrototypeObject(Value v);
void rtArrayPrototypeCheckMissingMember(Value obj, const std::string& key);

// `String.prototype[Symbol.iterator]` (22.1.3.36), installed by
// builtin_wrappers.cpp's intrinsic initializer beside the string methods.
void rtInstallStringIterator(Rooted<Value>& proto);

// `Function.prototype.bind` (20.2.3.2) and the bound function's brand
// (builtin_function_bind.cpp). The state accessor is what `bronze_construct`
// unwraps a bound layer with: 10.4.1.2 constructs the TARGET — bound args
// prepended, [[BoundThis]] ignored — so the instance's prototype is the
// ultimate target's.
uint64_t rtFunctionBindBuiltin(uint64_t env, uint64_t thisBits, uint32_t argc,
                               const uint64_t* argv);
bool rtBoundFunctionState(Value fn, Value& target, Value& boundThis, Value& boundArgs);

// `Function` and `Function.prototype` (builtin_function.cpp).
Value rtFunctionConstructorObject();
Value rtFunctionPrototypeObject();
bool rtIsFunctionConstructor(Value fn);
bool rtIsFunctionPrototype(Value fn);

// ---- the global constructor objects -----------------------------

// `Array`, `String` and `Boolean`, by the name lowering resolved; `undefined`
// for anything else. Interned by code pointer, exactly as the typed-array
// constructors are, so the bare name and the `constructor` back-pointer below
// are one object.
Value rtGlobalConstructor(const std::string& name);
Value rtArrayConstructorObject();
Value rtStringConstructorObject();
Value rtBooleanConstructorObject();
Value rtNumberConstructorObject();

// A member read on one of those constructor objects. True — with `out` set —
// only when it was answered; a name ECMA-262 defines and bronze has not built
// is diagnosed here and never returns, and everything else falls through to the
// ordinary function-member path so `Array.call` keeps its own diagnosis.
bool rtGlobalConstructorMember(Value fn, const std::string& key, Value& out);

// The statics half of that table, written into the constructor's `properties`
// box for the reason `rtInstallMapStatics` above is — `class MyArr extends
// Array` must reach `Array.of` — and off the SAME `kCtors` entry the read path
// answers from. False for a function that is not one of them.
bool rtInstallGlobalConstructorStatics(Rooted<Value>& ctor);

// The name of the intrinsic this function object IS, or null for any other
// function. Two operations need to recognise one: `instanceof`, which cannot
// walk a prototype chain an array does not have, and `extends`, which must
// refuse a base whose instances it cannot actually produce.
const char* rtIntrinsicConstructorName(Value fn);
bool rtIsArrayConstructor(Value fn);
bool rtIsArrayBufferConstructor(Value fn);
bool rtIsTypedArrayConstructor(Value fn);
bool rtIsRegExpConstructor(Value fn);
bool rtOrdinaryHasInstance(Value ctor, Value obj);
uint64_t rtFunctionHasInstanceBuiltin(uint64_t env, uint64_t thisBits, uint32_t argc,
                                      const uint64_t* argv);

// The name of the intrinsic whose `new` form builds a primitive WRAPPER
// object, or null. `bronze_construct` dispatches on it rather than running the
// function body: a native constructor cannot see NewTarget through the uniform
// calling convention, so its body returns the primitive — which 13.3.5.1 then
// discards in favour of the plain instance. The wrapper has to be built instead
// of the instance, not after it.
const char* rtPrimitiveWrapperConstructorName(Value fn);

struct NewTargetScope {
    Rooted<Value> targetRoot_;
    NewTargetScope* prev_{nullptr};
    explicit NewTargetScope(Value target);
    ~NewTargetScope();
    static Value current();
};

}  // namespace bronze::runtime
