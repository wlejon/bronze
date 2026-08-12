#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "abi/bronze_abi.h"
#include "regex/regex.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

// The runtime's process-wide state — the heap, the non-moving arena, the
// root shapes, the key registry and the collector's root sources — is owned
// by ONE translation unit (rt_state.cpp), so its construction order is that
// unit's business alone. Every other runtime translation unit reaches it
// through the accessors below rather than declaring statics of its own,
// which would put the collector's roots at the mercy of cross-TU
// initialization order.
//
// Nothing here is part of the generated-code ABI: that is bronze_abi.h, and
// it stays pure C. This header is C++ and internal to src/runtime.

namespace bronze::runtime {

Heap& rtHeap();
NonMovingArena& rtArena();

// A root shape registered with the collector, for a builtin that needs its
// own hidden class rather than the one every `{}` literal shares.
Shape* rtNewRootShape(Value proto);

// The root shape for objects whose prototype is `proto`, memoized on the
// prototype's identity. `Object.create(p)` in a loop must not mint a hidden
// class per object — every one of them would be a shape no inline cache had
// ever seen, and each would leak an immortal arena shape.
Shape* rtRootShapeForPrototype(Value proto);

// The one root shape every plain `{}` literal starts from. Per-literal root
// shapes would give two identical literals unrelated hidden classes, so a site
// seeing both would miss its inline cache every time.
Shape* rtPlainObjectShape();

// A property key by the index lowering assigned it. The string form is for
// comparisons; the header form is the arena-interned key the property path
// uses, so a property access allocates nothing. `rtKeyHeader` is null for an
// index no `bronze_register_key_string` call ever covered.
const std::string& rtKeyString(uint32_t index);
StringHeader* rtKeyHeader(uint32_t index);

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

// Is this key an ARRAY INDEX spelled as a string? Enumeration order asks it and
// so does console.log of an object, which reports the same order — one test, so
// the two answers cannot drift.
bool rtIsIntegerLikeKey(std::string_view key, uint32_t& out);

// console.log of a container, in the pinned inspect format. Returns the text;
// the caller writes it.
std::string rtInspect(Value v);

// A heap string from UTF-8 bytes, and JS ToString / ToNumber. ToString on
// an object and ToNumber on an object are hard errors: both need
// ToPrimitive, which bronze has not built.
Value rtMakeString(std::string_view utf8);
Value rtValueToString(Value v);
double rtToNumber(Value v);

// ECMA-262 Number::exponentiate, which `**` and `Math.pow` are both defined
// as. One implementation, because the two must not drift: C's pow disagrees
// with it on a NaN exponent and on a base of magnitude 1 with an infinite
// one (rt_operator.cpp).
double rtExponentiate(double base, double exponent);

// The characters of a string as bytes, with any code unit past U+007F
// replaced by 0xFF — enough for the numeric and structural parsing the
// builtins do, and never enough to be mistaken for a general conversion.
std::string rtAsciiChars(const StringHeader* s);

// A string's UTF-16 code units, and the string a sequence of them makes.
// This is the currency for anything defined PER CODE UNIT rather than per
// code point — every `String.prototype` method, and 25.5.2.2's JSON quoting,
// which escapes an unpaired surrogate and passes a paired one through. One
// pair of conversions, so two callers cannot disagree about a lone surrogate.
std::vector<uint16_t> rtStringUnits(const StringHeader* s);
Value rtStringFromUnits(const std::vector<uint16_t>& units);

// The same string as UTF-8, losing nothing. This is the conversion for text
// that will be PRINTED; rtAsciiChars is the one for text that will be
// PARSED, and they are deliberately two functions so a caller has to say
// which it meant.
std::string rtUtf8Chars(const StringHeader* s);

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
// The typed-array table takes the RECEIVER's constructor name, so the message
// says `Uint8Array.prototype.sort` and not `%TypedArray%.prototype.sort`: nine
// views share one implementation, and a diagnostic that forgot which one the
// program was holding would be a worse message for the sake of the
// implementation's convenience.
void rtCheckTypedArrayMember(const char* kindName, const std::string& key);

// A function's `.prototype` and its own-property object, created on first
// demand: a function that is never used as a constructor and never given a
// static member pays for neither. Both allocate, so both take the function
// through a root and the caller must re-derive any raw pointer afterwards.
void rtEnsureFunctionPrototype(Rooted<Value>& fnVal);
void rtEnsureFunctionProperties(Rooted<Value>& fnVal);

// A native builtin's prologue. bronze_dynamic_call hands a builtin an argument
// block that is rooted only as long as the CALLER's frame is — generated code's
// block lives in its GC root frame, but FunctionHeader::call's arity-adaptation
// vector and the blocks builtins build for callbacks are plain stack memory.
// The contract that makes both safe is that the callee copies its parameters
// into roots of its own before it allocates, and this is that copy, made
// explicit: after constructing one, read arguments from HERE and never from
// `argv` again.
//
// The std::vector allocation is C++'s, not the bronze heap's, so no
// collection can happen between the copy and the rooting.
class RootedArgs {
public:
    RootedArgs(uint32_t argc, const uint64_t* argv) : slots_(argc) {
        for (uint32_t i = 0; i < argc; ++i) slots_[i] = Value(argv[i]);
        frame_ = ShadowStackFrame::current();
        if (frame_) {
            for (Value& slot : slots_) frame_->push(&slot);
        }
    }

    ~RootedArgs() {
        if (frame_) {
            for (Value& slot : slots_) frame_->pop(&slot);
        }
    }

    RootedArgs(const RootedArgs&) = delete;
    RootedArgs& operator=(const RootedArgs&) = delete;

    uint32_t count() const noexcept { return static_cast<uint32_t>(slots_.size()); }

    // Out of range is `undefined`, which is what a JS call site that omitted
    // the argument means — so a builtin never has to bounds-check first.
    Value operator[](uint32_t i) const noexcept {
        return i < slots_.size() ? slots_[i] : Value::fromUndefined();
    }
    Value at(uint32_t i, Value fallback) const noexcept {
        return i < slots_.size() ? slots_[i] : fallback;
    }

private:
    std::vector<Value> slots_;
    ShadowStackFrame* frame_{nullptr};
};

// RootedArgs in the other direction: an argument block the RUNTIME builds and
// hands to a callee, rather than one a callee copies out of.
//
// Most blocks the runtime builds need nothing like this — `builtin_array`'s
// `Value block[3]`, the JSON replacer's and the regexp replacer's are all
// filled from roots on the statement before the call, and `bronze_dynamic_call`
// reaches the callee without allocating, so nothing can move in between.
//
// `bronze_construct` is the exception and the reason this class exists: it
// allocates the INSTANCE before it reads the block, so a block that is not
// rooted holds pre-collection addresses by the time the constructor is
// entered. Every slot here is pushed onto the shadow stack for the block's
// lifetime, which makes it safe to pass to a helper that allocates first.
// The vector is sized once and never resized, so the pushed pointers stay
// valid — the same reason RootedArgs above may push into its own storage.
class RootedBlock {
public:
    explicit RootedBlock(uint32_t count) : slots_(count, Value::fromUndefined()) {
        frame_ = ShadowStackFrame::current();
        if (frame_) {
            for (Value& slot : slots_) frame_->push(&slot);
        }
    }

    ~RootedBlock() {
        if (frame_) {
            for (Value& slot : slots_) frame_->pop(&slot);
        }
    }

    RootedBlock(const RootedBlock&) = delete;
    RootedBlock& operator=(const RootedBlock&) = delete;

    void set(uint32_t i, Value v) { slots_[i] = v; }
    uint32_t count() const noexcept { return static_cast<uint32_t>(slots_.size()); }
    const uint64_t* data() const noexcept {
        return reinterpret_cast<const uint64_t*>(slots_.data());
    }

private:
    std::vector<Value> slots_;
    ShadowStackFrame* frame_{nullptr};
};

// ---- builtin namespaces ---------------------------------------------------
// Each family owns its own translation unit and exposes exactly two things:
// the namespace object, and the miss check that keeps an unimplemented
// member loud instead of `undefined`.

Value rtMathObject();
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

// A property read on a PRIMITIVE receiver (rt_prop_primitive.cpp): a string
// reaches `String.prototype` and a boolean `Boolean.prototype` by the ordinary
// prototype walk, while a number's and a symbol's members are still handed out
// beside the value. The one branch of the property path that is not about the
// receiver's own storage, which is why it is not in rt_prop.cpp.
Value rtPrimitiveMember(Value objVal, const std::string& keyStr, StringHeader* keyHeader,
                        struct InlineCache* ic);

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

// `String.prototype` and `Boolean.prototype`: real objects on the real chain,
// which a primitive reaches by the ordinary prototype walk rather than through
// a table consulted beside it. A program can hold either, compare it, add to it
// and pass its methods to `.call` — where an array's and a function's members
// are still handed out BESIDE the value.
Value rtStringPrototype();
Value rtBooleanPrototype();

// The String (10.4.3) and Boolean (20.3) exotic objects: a plain object with
// one internal slot holding the wrapped primitive, and the matching intrinsic
// on its chain.
Value rtMakeStringWrapper(Rooted<Value>& str);
Value rtMakeBooleanWrapper(bool value);

// The [[StringData]] / [[BooleanData]] of a wrapper; false for every other
// value. The brand is the internal-slot count paired with the slot's type —
// builtin_wrappers.cpp says why it is not the (prototype, count) pair an
// iterator object uses.
bool rtStringWrapperData(Value v, Value& out);
bool rtBooleanWrapperData(Value v, Value& out);

// 22.1.3.35 thisStringValue / 20.3.3.3 thisBooleanValue: the primitive itself,
// or a wrapper's slot. False for anything else, which the caller reports as the
// TypeError those clauses name.
bool rtThisStringValue(Value self, Value& out);
bool rtThisBooleanValue(Value self, Value& out);

// One code unit of a string, as a String of length 1 — 10.4.3.5's answer, and
// the value `s[i]` is. `undefined` past the end. ALLOCATES.
Value rtStringCharAsString(Value str, uint32_t index);

// ToPrimitive of a primitive WRAPPER, which is the one object kind bronze can
// take 7.1.1's step for: OrdinaryToPrimitive would call `valueOf`, and for a
// pristine wrapper that call answers exactly the internal slot. False for every
// other object, so the caller's named error for the general algorithm stands.
// Allocation-free, which two typed-array writes in rt_prop.cpp depend on.
bool rtWrapperPrimitive(Value v, Value& out);

// 10.4.3.5 StringGetOwnProperty, plus the `length` 10.4.3.4 defines: true —
// with `out` set — for a canonical index below the length and for `length`
// itself. False means "not an own property of this exotic object", which is the
// fall-through to the ordinary lookup that 10.4.3 requires. ALLOCATES.
bool rtStringExoticOwnProperty(Value obj, const std::string& key, Value& out);

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

Value rtNumberNamespace();
void rtNumberCheckMissingMember(Value obj, const std::string& key);

// The four function properties of the global object that ECMA-262 19.2
// defines over numbers: `isNaN`, `isFinite`, `parseInt`, `parseFloat`.
// `undefined` for any other name. They live beside the `Number` statics
// because two of them ARE the `Number` statics, interned by code pointer.
Value rtGlobalNumericFunction(const std::string& name);

// `Number.prototype`, reached by a property read on a PRIMITIVE number the
// way `String.prototype` already is. `undefined` for a name that is not an
// implemented method, so the caller falls through to the unimplemented-member
// table and then to the language's own answer for a property that is not
// there.
Value rtNumberMethod(const std::string& key);
void rtCheckNumberProtoMember(const std::string& key);

Value rtJsonNamespace();
void rtJsonCheckMissingMember(Value obj, const std::string& key);

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
void rtCheckMapMember(bool isSetReceiver, const std::string& key);
// What `m[Symbol.iterator]` answers: a Map's default iterator is `entries`
// and a Set's is `values` (24.1.3.12, 24.2.3.11).
Value rtMapDefaultIterator(bool isSetReceiver);

// ---- typed arrays ----------------------------------------------

// `ArrayBuffer` and the nine views, by the name lowering resolved; `undefined`
// for anything else. One function object per name, interned by code pointer, so
// the bare name and `v.constructor` are the SAME object — which is what `switch
// (array.constructor)` needs.
Value rtTypedArrayConstructor(const std::string& name);
Value rtTypedArrayConstructorFor(ElementKind kind);
// The name of the view (or `"ArrayBuffer"`) this function object constructs,
// else nullptr — the counterpart of `rtMapConstructorName`, for the same
// reason.
const char* rtTypedArrayConstructorName(Value fn);

// `BYTES_PER_ELEMENT` off a constructor (23.2.6.2). Answers false — leaving
// `out` alone — when the function is not one of the nine, so the caller falls
// through to the ordinary function-member path.
bool rtTypedArrayStatic(Value fn, const std::string& key, Value& out);

// A member of a typed-array or ArrayBuffer INSTANCE by name. The caller has
// already tried the key as an index; what reaches here is `length`, `buffer`,
// `constructor`, a method, or a name that is diagnosed and then read as
// `undefined`.
Value rtTypedArrayMember(Value view, const std::string& key);
Value rtArrayBufferMember(Value buffer, const std::string& key);
// `undefined` for a name that is not an implemented method, so the property
// path can fall through to the unimplemented-member table.
Value rtTypedArrayMethod(const std::string& key);
// `v[Symbol.iterator]`, which 23.2.3.34 makes the same function object as
// `values`. By key and not by name, because the key is a symbol.
Value rtTypedArrayIteratorMethod();

// ---- regular expressions ---------------------------------------

bool rtIsRegExp(Value v);
// `RegExp`, for the provided-global path; `undefined` for any other name.
Value rtRegExpConstructor(const std::string& name);
// A member of a RegExp instance by name: the flag accessors, `source`,
// `flags`, `lastIndex`, and the three methods. A name ECMA-262 defines and
// bronze has not built is a named error here rather than `undefined`.
Value rtRegExpMember(Value re, const std::string& key);
Value rtRegExpMethod(const std::string& key);
// True when the write was a RegExp's business. `lastIndex` is the only
// writable property one has, and a write to anything else is the caller's to
// diagnose.
bool rtRegExpSetMember(Value re, const std::string& key, Value value);
// `/source/flags`, which is both `toString` and what console.log prints.
std::string rtRegExpText(Value re);
// 22.2.7.2 RegExpBuiltinExec: the match array, or `null`. Honours `lastIndex`
// for a `g` or `y` pattern and updates it. EVERY regular-expression operation
// in bronze goes through this one function, so none of them can disagree about
// the cursor.
Value rtRegExpExec(Rooted<Value>& re, Rooted<Value>& inputStr);
// The compiled pattern behind a RegExp, and the pieces `String.prototype`'s
// pattern methods need to drive it directly rather than through `exec` (a
// `replace` builds one result string from many matches, and going through the
// match array per match would allocate an array per match).
const regex::Pattern& rtRegExpPattern(Value re);
Value rtRegExpBuildMatchArray(const regex::Pattern& pattern, Rooted<Value>& inputStr,
                              const regex::MatchResult& match);
void rtRegExpSetLastIndex(Value re, double value);
double rtRegExpLastIndex(Value re);
// A RegExp from a source string and a flags string, which is what
// `String.prototype.matchAll` needs to make its own `g` copy of a pattern.
Value rtRegExpFromParts(Rooted<Value>& sourceStr, const std::string& flagsText);

// `String.prototype.split` with a RegExp separator, which is 22.2.6.14's
// SplitMatcher and not the string search `split` otherwise does. It stays a
// call from builtin_string.cpp rather than a second `split` in the method
// table, because a program that reads `"".split` must get ONE function object
// whichever kind of separator it later passes.
uint64_t rtStringSplitWithRegExp(uint64_t thisBits, uint32_t argc, const uint64_t* argv);

// `undefined` for a name that is not an implemented method, so the property
// path can fall through to the unimplemented-member table and then to the
// language's own answer for a property that does not exist. An array's members
// are still answered BESIDE the value this way; a string's moved onto
// `String.prototype` and are found by the ordinary prototype walk.
Value rtArrayMethod(const std::string& key);

// ---- the global constructor objects -----------------------------

// `Array`, `String` and `Boolean`, by the name lowering resolved; `undefined`
// for anything else. Interned by code pointer, exactly as the typed-array
// constructors are, so the bare name and the `constructor` back-pointer below
// are one object.
Value rtGlobalConstructor(const std::string& name);
Value rtArrayConstructorObject();
Value rtStringConstructorObject();
Value rtBooleanConstructorObject();

// A member read on one of those constructor objects. True — with `out` set —
// only when it was answered; a name ECMA-262 defines and bronze has not built
// is diagnosed here and never returns, and everything else falls through to the
// ordinary function-member path so `Array.call` keeps its own diagnosis.
bool rtGlobalConstructorMember(Value fn, const std::string& key, Value& out);

// The name of the intrinsic this function object IS, or null for any other
// function. Two operations need to recognise one: `instanceof`, which cannot
// walk a prototype chain an array does not have, and `extends`, which must
// refuse a base whose instances it cannot actually produce.
const char* rtIntrinsicConstructorName(Value fn);
bool rtIsArrayConstructor(Value fn);

// The name of the intrinsic whose `new` form builds a primitive WRAPPER
// object, or null. `bronze_construct` dispatches on it rather than running the
// function body: a native constructor cannot see NewTarget through the uniform
// calling convention, so its body returns the primitive — which 13.3.5.1 then
// discards in favour of the plain instance. The wrapper has to be built instead
// of the instance, not after it.
const char* rtPrimitiveWrapperConstructorName(Value fn);

}  // namespace bronze::runtime
