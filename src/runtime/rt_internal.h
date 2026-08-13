#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "abi/bronze_abi.h"
#include "regex/regex.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
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

// console.log of a container, in the pinned inspect format. Returns the text;
// the caller writes it.
std::string rtInspect(Value v);

// A heap string from UTF-8 bytes, and JS ToString / ToNumber for a value that
// is ALREADY primitive — plus the objects whose answer is a pure function of
// what they hold. Any other object is a hard error in both, and deliberately:
// these two are reached from places that must not run user code, so ToPrimitive
// is applied by the CALLER (`rtToStringValue` below, `bronze_dynamic_add`)
// rather than folded in here. rt_convert.cpp's header says which sites those
// are and which still refuse.
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

// The heap kind of an object, in the spelling a program would use: "an array",
// "a function", "a Map", "a DataView". For a diagnostic that REFUSES a
// receiver, which has to say what the receiver is — and the reason this is one
// function rather than one per refusing file is that a message naming the wrong
// kind sends a reader to the wrong place. Defined in integrity.cpp, which had
// the switch first. A non-object is a caller error, not an answer here.
const char* rtObjectKindName(Value v);

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

// A function's `.prototype` and its own-property object, created on first
// demand: a function that is never used as a constructor and never given a
// static member pays for neither. Both allocate, so both take the function
// through a root and the caller must re-derive any raw pointer afterwards.
void rtEnsureFunctionPrototype(Rooted<Value>& fnVal);
void rtEnsureFunctionProperties(Rooted<Value>& fnVal);

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
    return Value(bronze_function_singleton(code, arity, /*length=*/0,
                                           BRONZE_ABI_FN_NAME_NONE));
}

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

    // The rooted slots themselves, for a caller that hands them onward as a
    // contiguous view: the collector updates these in place, so a span over
    // them stays CURRENT across anything the callee allocates — which a copy
    // taken with operator[] would not. The embed module's host callbacks read
    // their arguments through exactly this.
    const Value* data() const noexcept { return slots_.data(); }

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

// The members of `Object.prototype` (builtin_object_proto.cpp), installed onto
// the object builtin_object.cpp allocates. Two files for one pair of intrinsics
// because 20.1.2.1 and 20.1.3.1 make the namespace and the prototype each
// other's property, so one initializer has to hold both — the arrangement
// `String.prototype`'s members already reach their object through.
void rtInstallObjectProtoMethods(Rooted<Value>& proto);

// ToPropertyKey (7.1.19) into the immortal arena form a DictEntry can hold, and
// own-property existence over it. Both are shared by the `Object` statics and
// the `Object.prototype` methods, which are the same operations with the
// receiver in a different position (20.1.2.13 and 20.1.3.2) — writing either
// twice is how the two would come to disagree about a dictionary-mode object.
PropertyKey rtInternPropertyKey(Value keyVal);
bool rtHasOwnPropertyNamed(Rooted<Value>& self, Value key);

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

// ECMA-262 7.1.1 ToPrimitive's hint. Default and Number ask `valueOf` before
// `toString` and String asks the other way round, which is the ONLY thing the
// hint decides — and the classic bug, since `'' + {}` is Default (13.15.3 asks
// for no hint) where `String({})` is String.
enum class ToPrimitiveHint { Default, Number, String };

// ToPrimitive, and ToString with its step 1 attached. Both RUN USER CODE — a
// `toString` or a `valueOf` on the input's chain — so a caller must have
// everything it holds rooted, and must be reached from an IL op `il::canThrow`
// marks, or a TypeError raised here propagates past the `catch` that should
// have taken it. Today that means `+` (13.15.3) and `String(x)`; rt_convert.cpp
// names the sites that still refuse an object and why.
Value rtToPrimitive(Rooted<Value>& input, ToPrimitiveHint hint);
Value rtToStringValue(Rooted<Value>& v);

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

// A property read on a PRIMITIVE receiver (rt_prop_primitive.cpp): a string
// reaches `String.prototype` and a boolean `Boolean.prototype` by the ordinary
// prototype walk, while a number's and a symbol's members are still handed out
// beside the value. The one branch of the property path that is not about the
// receiver's own storage, which is why it is not in rt_prop.cpp.
Value rtPrimitiveMember(Value objVal, const std::string& keyStr, StringHeader* keyHeader,
                        struct InlineCache* ic);

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

// Does this key name an ELEMENT of a receiver that stores its elements by
// index? The canonical-array-index test and nothing else, in both spellings a
// key arrives in — already a string, or still a value.
bool rtKeyAsIndex(const std::string& key, uint32_t& out);
bool rtValueToElementIndex(Value idxVal, uint32_t& out);

// ToPropertyKey (7.1.19) as a heap string, for a computed key that named no
// element. ALLOCATES, so the caller must have the receiver rooted.
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
// that is allocation-free, which two typed-array writes in rt_prop.cpp depend
// on, and which `rtToNumber` needs for the same reason. `+` and `String(x)`
// run the real algorithm and never reach here.
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

// ---- the module namespace exotic object (ECMA-262 10.4.6) ------------------
//
// One receiver kind, four questions, and they are gathered here rather than
// spread over the files that ask them because every one of the four differs
// from the ordinary answer in a way no property attribute can express. The
// object itself is runtime/namespace.h.

bool rtIsModuleNamespace(Value v);

// The exported names in 10.4.6.2 [[OwnPropertyKeys]] order: SORTED by code
// unit, so `z` declared first still comes back after `a`. The strings are
// arena-interned and immortal, like every other own-key answer here.
std::vector<StringHeader*> rtModuleNamespaceKeys(Value nsVal);

// 10.4.6.7 [[Get]]. False means the receiver is not a namespace at all; true
// with `out` undefined is the answer for a name the module does not export,
// which is NOT an error — `import { missing }` is the early error, `ns.missing`
// is this. ALLOCATES and RUNS USER CODE: the value comes from the getter that
// closes over the exporting module's binding, which is what makes it live.
bool rtModuleNamespaceGet(Value nsVal, const StringHeader* key, Value& out);

// 10.4.6.4 [[HasProperty]]: is `key` one of the exports. False for a receiver
// that is not a namespace, and false for a name it does not export — there is
// no prototype chain to continue on (10.4.6.1 fixes [[Prototype]] at null), so
// this is the complete answer and not a first step. Unlike [[Get]] above it
// neither allocates nor runs the binding's getter, which is the difference
// between asking whether a property is there and reading it.
bool rtModuleNamespaceHasExport(Value nsVal, const StringHeader* key);

// 10.4.6.1's one own SYMBOL-keyed property: `@@toStringTag`, whose value is the
// string "Module". It is the only own key of a namespace that is not an export,
// and it is ANSWERED rather than stored — the object has no shape to keep a
// property in, and nothing about this one can differ between two namespaces.
// False for any other symbol, for a string key, and for a receiver that is not
// a namespace. ALLOCATES (the answer is a fresh string).
bool rtModuleNamespaceOwnSymbol(Value nsVal, Value keyVal, Value& out);

// 10.4.6.5 [[GetOwnProperty]] minus the descriptor OBJECT, which the caller
// builds: is `key` one of the exports, and what is its value NOW. False for a
// symbol — the symbol half is the question above — for a name the module does
// not export, and for a receiver that is not a namespace. The attributes it
// would have reported are constants and so are written at the one call site
// that needs them.
//
// ALLOCATES (ToString on the key, and the getter behind the value).
bool rtModuleNamespaceOwnProperty(Value nsVal, Value keyVal, Value& outValue);

// 10.4.6.9 [[Set]], which returns false for EVERY key. True means the write was
// refused, whether or not it threw; 13.15.2 PutValue step 6.d makes it a
// TypeError for a strict reference, and module code is always strict.
//
// The same shape as `rtStringDataWriteRefused` above, deliberately: both are a
// receiver kind saying "this write cannot happen" ahead of the ordinary
// property path, and a namespace's shapeless storage would otherwise have no
// way to say so.
bool rtModuleNamespaceWriteRefused(Value nsVal, const std::string& key, bool strict);

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
Value rtWeakCollectionConstructor(const std::string& name);
const char* rtWeakCollectionConstructorName(Value fn);
Value rtWeakCollectionMethod(bool isWeakSetReceiver, const std::string& key);
bool rtWeakCollectionHasMember(bool isWeakSetReceiver, const std::string& key);
void rtCheckWeakCollectionMember(bool isWeakSetReceiver, const std::string& key);

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
// The same two questions asked for EXISTENCE, which is `in`'s. Both walk the
// lists their readers walk and end at the same named refusal, and neither
// allocates — so a caller may hold a header across the call. The typed-array
// one takes the receiver's constructor name for the same reason
// `rtCheckTypedArrayMember` does.
bool rtTypedArrayHasMember(const char* kindName, const std::string& key);
bool rtArrayBufferHasMember(const std::string& key);
// `undefined` for a name that is not an implemented method, so the property
// path can fall through to the unimplemented-member table.
Value rtTypedArrayMethod(const std::string& key);
// The same table by name alone: no function object is built, so this is what
// an existence question asks.
bool rtTypedArrayHasMethod(const std::string& key);
// `v[Symbol.iterator]`, which 23.2.3.34 makes the same function object as
// `values`. By key and not by name, because the key is a symbol.
Value rtTypedArrayIteratorMethod();

// ---- DataView (ECMA-262 25.3) ----------------------------------------------
//
// A separate object from the nine views and so a separate set of entry points:
// its accessors choose a width and a byte order per CALL, which is the whole
// reason it is not a tenth element kind.

// `DataView` by the name lowering resolved; `undefined` for anything else.
Value rtDataViewConstructor(const std::string& name);
// `"DataView"` when this function object IS that constructor, else nullptr —
// the counterpart of `rtTypedArrayConstructorName`, for the same reason.
const char* rtDataViewConstructorName(Value fn);
// A member of a DataView INSTANCE by name: `buffer`, `byteLength`,
// `byteOffset`, `constructor`, or one of the sixteen accessors. A BigInt
// accessor is diagnosed by name here rather than read as `undefined`; anything
// else really is absent.
Value rtDataViewMember(Value view, const std::string& key);
// Whether 25.3.4 defines `key` on a DataView at all — what `in` asks, and
// which must agree with the function above about every name.
bool rtDataViewHasMember(const std::string& key);

// ---- regular expressions ---------------------------------------

bool rtIsRegExp(Value v);
// `RegExp`, for the provided-global path; `undefined` for any other name.
Value rtRegExpConstructor(const std::string& name);
// A member of a RegExp instance by name: the flag accessors, `source`,
// `flags`, `lastIndex`, and the three methods. A name ECMA-262 defines and
// bronze has not built is a named error here rather than `undefined`.
Value rtRegExpMember(Value re, const std::string& key);
Value rtRegExpMethod(const std::string& key);
// Whether 22.2.6 defines `key` on a RegExp at all — `in`'s question, off the
// same tables the reader above uses and ending at the same named refusal.
bool rtRegExpHasMember(const std::string& key);
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

// Rows of builtin_array.cpp's method table whose bodies live in a translation
// unit of their own — `sort` (builtin_array_sort.cpp) and the three iterator
// methods (builtin_array_iterator.cpp). Declared here so the table stays the
// ONE list of what Array.prototype implements; rt_prop.cpp also hands
// `rtArrayValuesBuiltin` out as `[Symbol.iterator]`, which 23.1.3.41 makes
// the SAME function object — an identity the code-pointer intern table gives
// for free.
uint64_t rtArraySortBuiltin(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtArrayValuesBuiltin(uint64_t env, uint64_t thisBits, uint32_t argc,
                              const uint64_t* argv);
uint64_t rtArrayKeysBuiltin(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtArrayEntriesBuiltin(uint64_t env, uint64_t thisBits, uint32_t argc,
                               const uint64_t* argv);

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
