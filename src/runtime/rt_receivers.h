#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "regex/regex.h"
#include "runtime/gc.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

// The receiver kinds that answer their own members: the module namespace exotic
// object, the nine typed-array views with ArrayBuffer and DataView, and RegExp.
//
// None of them carries a shape, so none has a prototype object for a lookup to
// walk — their members are handed out BESIDE the value by the property path,
// and every question about one has to be asked of a table here. They are
// together because that is the one property they share and the reason each
// needs a `Has` form beside its reader: `in` must not disagree with a read
// about a member whose value bronze refuses to produce.

namespace bronze::runtime {

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

// The same five as FUNCTION OBJECTS, by key — what a symbol-keyed read of a
// RegExp answers (rt_prop_symbol.cpp). `undefined` for any other symbol.
// Interned on the code pointer, so `/a/[Symbol.split] === /b/[Symbol.split]`.
Value rtRegExpSymbolMethod(Value symbolKey);

// The well-known key a pattern-taking `String.prototype` member dispatches on.
enum class PatternSymbol : uint8_t { Match, MatchAll, Replace, Search, Split };

// 22.1.3's step 2, shared by all six members: GetMethod(argument, @@which).
// `true` with `out` set to a CALLABLE method when the argument carries one.
// `false` — with NO property read at all — for the two argument shapes that
// cannot: a non-object, and a RegExp (whose five are answered beside the value
// and cannot be shadowed, because it has no shape). `false` with an exception
// pending when the property was present and not callable, or a getter threw.
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
