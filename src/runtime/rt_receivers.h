#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "regex/regex.h"
#include "runtime/gc.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

// The receiver kinds with INTERNAL SLOTS the property paths must know about:
// the module namespace exotic object, the twelve typed-array views with
// ArrayBuffer and DataView, and RegExp.
//
// The namespace object carries no shape, so its members are handed out
// BESIDE the value and every question about one is asked of a table here —
// which is why it needs a `Has` form beside its reader: `in` must not
// disagree with a read. The byte-store family and RegExp are ordinary objects
// with a real prototype each (typed_array.h, regexp.h); what this file gives
// them is their intrinsics by name, the allocation helpers, the element and
// `lastIndex` protocols, and the pristine-chain witnesses the fast paths use.

namespace bronze {
struct RegExpHeader;
}

namespace bronze::runtime {

struct NativeMethod;

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

// ---- typed arrays (ECMA-262 23.2), ArrayBuffer (25.1), SharedArrayBuffer
// ---- (25.2) and DataView (25.3)
//
// Every one of these is an ORDINARY OBJECT with a real prototype object now
// (typed_array.h): `%TypedArray%.prototype` carries the shared methods and the
// four accessors, each view's `prototype` carries `constructor` and
// `BYTES_PER_ELEMENT` and nothing else (23.2.7), and `ArrayBuffer.prototype`,
// `SharedArrayBuffer.prototype` and `DataView.prototype` are what their
// clauses list. A NAMED read of any of them is the ordinary shape walk; what
// this section exposes is the constructors by name, the instance shapes the
// runtime's own allocations use, the element funnel, and the two questions
// the property path still answers from the KIND — the integer index and the
// four accessors on a pristine chain.

// The twelve view constructors and `ArrayBuffer`, by the name lowering
// resolved; `undefined` for anything else. Building the first one builds the
// whole family — `%TypedArray%`, every view and every prototype — because the
// views share one parent and one prototype chain.
Value rtTypedArrayConstructor(const std::string& name);
Value rtTypedArrayConstructorFor(ElementKind kind);
// The name of the view this function object constructs (never `ArrayBuffer`:
// `rtArrayBufferConstructorName` answers that), else nullptr. By CODE POINTER,
// so nothing is built to answer.
const char* rtTypedArrayConstructorName(Value fn);
// The view kind this function object constructs, or false. What
// `rtNativeBaseOf` asks (runtime/native_base.h).
bool rtTypedArrayConstructorKind(Value fn, ElementKind& out);
// Is `fn` the abstract `%TypedArray%` (23.2.1)? It is a constructor in name
// only — 23.2.1.1 throws from every call — and it carries no NativeBase.
bool rtIsTypedArrayIntrinsic(Value fn);

// %SharedArrayBuffer% by name for the global ladder, and its identity by CODE
// POINTER — never by building one to compare against.
Value rtSharedArrayBufferConstructor(const std::string& name);
const char* rtSharedArrayBufferConstructorName(Value fn);
// `DataView` by the name lowering resolved; `undefined` for anything else.
Value rtDataViewConstructor(const std::string& name);
const char* rtDataViewConstructorName(Value fn);

// The memoized root shape of each intrinsic's OWN prototype — what
// 10.1.13 OrdinaryCreateFromConstructor derives when NewTarget is the
// intrinsic itself, and what every allocation the runtime makes on its own
// behalf (a `slice`, a `subarray`, an embedder's view) passes to the header's
// `create`. Each builds its family on first use.
Shape* rtTypedArrayInstanceShape(ElementKind kind);
Shape* rtArrayBufferInstanceShape();
Shape* rtSharedArrayBufferInstanceShape();
Shape* rtDataViewInstanceShape();

// The allocations, with the intrinsic prototypes above: a view over a fresh
// zero-filled buffer, a view over an existing (rooted) buffer, the three
// buffer flavours, and a DataView. The offset and length are the caller's to
// validate — 23.2.5.1 and 25.3.2.1 carry the ladders — and every one ALLOCATES,
// so the result is rooted by the caller before anything else runs.
Value rtNewTypedArray(ElementKind kind, uint32_t length);
Value rtNewTypedArrayOverBuffer(ElementKind kind, Rooted<Value>& buffer, uint32_t byteOffset,
                                uint32_t length, bool tracking);
Value rtNewArrayBuffer(uint32_t byteLength);
Value rtNewResizableArrayBuffer(uint32_t byteLength, uint32_t maxByteLength);
Value rtNewSharedArrayBuffer(uint32_t byteLength, uint32_t maxByteLength);
Value rtNewDataView(Rooted<Value>& buffer, uint32_t byteOffset, uint32_t byteLength);

// 23.2.4.1 TypedArraySpeciesCreate for the `length` argument list: the view
// `exemplar`'s species constructor builds, validated (23.2.4.4) and checked
// for the same content type (step 3). RUNS USER CODE — a subclass's
// constructor — and can throw, answering `undefined` with the exception
// pending. When the exemplar's chain is pristine (its own prototype's
// `constructor` and `%TypedArray%[@@species]` untouched) the answer is a
// fresh intrinsic view of the exemplar's kind with no `Get` performed: that
// is the path every `map`, `filter` and `slice` in a renderer's frame takes.
Value rtTypedArraySpeciesCreate(Rooted<Value>& exemplar, uint32_t length);
// The buffer-argument form (23.2.3.30 `subarray` step 15).
Value rtTypedArraySpeciesCreateOverBuffer(Rooted<Value>& exemplar, Rooted<Value>& buffer,
                                          uint32_t byteOffset, uint32_t length, bool tracking);

// Is `fn` the intrinsic `%TypedArray%.prototype.values` (which 23.2.3.34 makes
// `[Symbol.iterator]` too)? What for-of asks before stepping a view's elements
// from a cursor instead of running the protocol. Reads two words.
bool rtIsIntrinsicTypedArrayIterator(Value fn);

// 23.2.3's members whose bodies live beside their tables
// (builtin_typed_array_methods.cpp, builtin_typed_array_iteration.cpp), from
// which `%TypedArray%.prototype` is populated.
const NativeMethod* rtTypedArrayMethodTable(size_t& count);

// May the four accessors of 23.2.3 — `buffer`, `byteLength`, `byteOffset`,
// `length` — be answered for this view from its header WITHOUT a walk? True
// when the chain is pristine: the view's shape is the intrinsic's own
// instance shape (so no own property shadows a name and its prototype is the
// view's own `prototype`, untouched), and nothing on any of the thirteen
// prototype objects has been added, deleted or redefined since the family was
// built. False means "walk": a subclass instance, a view with an expando, a
// program that redefined `length`. One shape compare and one epoch compare on
// the hit; a changed epoch re-verifies the thirteen shapes and the four getter
// slots once and latches the answer.
bool rtTypedArrayChainPristine(const TypedArrayHeader* view) noexcept;

// The above, plus: does `%TypedArray%.prototype[Symbol.iterator]` still hold
// the intrinsic `values`? What for-of asks before stepping a view's elements
// from a cursor with no property read at all (7.4.2 GetIterator would find
// exactly that function). A data property is overwritten in place, so this is
// a value compare each time rather than a latch.
bool rtTypedArrayIteratorPristine(const TypedArrayHeader* view) noexcept;

// 7.1.21 CanonicalNumericIndexString, the half of it a typed array's
// [[Get]]/[[Set]]/[[HasProperty]]/[[Delete]] need: is this string key a
// NUMERIC index that is not a valid integer one — "-0", "1.5", "NaN",
// "Infinity" — which 10.4.5 makes absent (read `undefined`, discard the
// write, `in` false, `delete` true) rather than a name the chain answers?
// An integer index has already been tried by every caller.
bool rtIsCanonicalNumericString(const std::string& key);

// ---- regular expressions ---------------------------------------

bool rtIsRegExp(Value v);
// `RegExp`, for the provided-global path; `undefined` for any other name.
Value rtRegExpConstructor(const std::string& name);
// `RegExp.prototype` (22.2.6) and the shape a `new RegExp` takes its
// [[Prototype]] from. Both build the intrinsics on first use.
Value rtRegExpPrototypeObject();
Shape* rtRegExpInstanceShape();
// 22.2.3.2 RegExpAlloc: a RegExp header with `shape` and no pattern yet —
// what `new` allocates from NewTarget before the constructor body fills it.
Value rtAllocateRegExp(Shape* shape);
// Is this RegExp's chain exactly as built — its own shape the intrinsic
// instance shape (no own key, the intrinsic prototype) and `RegExp.prototype`
// still holding its `exec` and its five symbol-keyed algorithms? When it is,
// the string members and the flag getters answer from the header with no
// property read, which is the whole of the `str.replace(/x/g, ...)` fast
// path. Allocates nothing.
bool rtRegExpChainPristine(const RegExpHeader* re) noexcept;
// `/source/flags`, which is what console.log prints.
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
// `lastIndex` (22.2.4.1), the own data property the header carries. The
// WRITE is Set(R, "lastIndex", v, true) — a TypeError thrown on a frozen
// RegExp, true otherwise. The READ for a match is ToLength of whatever was
// assigned, which can run user code and throw (`ok` is set true when it
// returns); the raw value is for `[@@search]`'s save-and-restore, which must
// put back exactly what it found.
bool rtRegExpSetLastIndex(Rooted<Value>& re, double value);
double rtRegExpLastIndexLength(Rooted<Value>& re, bool& ok);
Value rtRegExpLastIndexValue(Value re);
void rtRegExpRestoreLastIndex(Value re, Value saved);

// ---- the string/regexp protocol (22.1.3 dispatches, 22.2.6 implements) ------
//
// `RegExp.prototype`'s five SYMBOL-keyed members, as ALGORITHMS
// (builtin_regexp_symbols.cpp). `String.prototype.match` and its five siblings
// call these directly for a RegExp argument rather than reading the symbol off
// it, which is what keeps `"s".replace(/re/, "x")` free of a property read; the
// function objects a program reaches through `/re/[Symbol.replace]` run exactly
// these bodies.
Value rtRegExpMatch(Rooted<Value>& re, Rooted<Value>& str);
Value rtRegExpMatchAll(Rooted<Value>& re, Rooted<Value>& str);
Value rtRegExpReplace(Rooted<Value>& re, Rooted<Value>& str, Rooted<Value>& replaceValue);
Value rtRegExpSearch(Rooted<Value>& re, Rooted<Value>& str);
Value rtRegExpSplit(Rooted<Value>& re, Rooted<Value>& str, Value limit);

// The well-known key a pattern-taking `String.prototype` member dispatches on.
enum class PatternSymbol : uint8_t { Match, MatchAll, Replace, Search, Split };

// 22.1.3's step 2, shared by all six members: GetMethod(argument, @@which).
// `true` with `out` set to a CALLABLE method when the argument carries one.
// `false` — with NO property read at all — for the two argument shapes that
// need none: a non-object, and a RegExp whose chain is as built
// (`rtRegExpChainPristine`), where the read would find exactly the algorithm
// the caller runs directly. A TypeError is thrown when the property was
// present and not callable, and a getter's throw propagates.
bool rtPatternMethod(Rooted<Value>& arg, PatternSymbol which, Rooted<Value>& out);

// The call that dispatch makes: `Call(method, argument, «first[, second]»)`.
// The receiver is the PATTERN ARGUMENT, which is where the method was found.
Value rtCallPatternMethod(Rooted<Value>& method, Rooted<Value>& receiver, Rooted<Value>& first,
                          Rooted<Value>& second, uint32_t argCount);
// A RegExp from a source string and a flags string, which is what
// `String.prototype.matchAll` needs to make its own `g` copy of a pattern.
Value rtRegExpFromParts(Rooted<Value>& sourceStr, const std::string& flagsText);

// `String.prototype.split` with a RegExp separator, which is 22.2.6.14's
// SplitMatcher and not the string search `split` otherwise does. It stays a
// call from builtin_string.cpp rather than a second `split` in the method
// table, because a program that reads `"".split` must get ONE function object
// whichever kind of separator it later passes.
uint64_t rtStringSplitWithRegExp(uint64_t thisBits, uint32_t argc, const uint64_t* argv);

}  // namespace bronze::runtime
