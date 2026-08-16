#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "runtime/gc.h"
#include "runtime/string.h"
#include "runtime/value.h"

// The conversions of ECMA-262 clause 7.1, and the two text encodings a string
// is read through.
//
// They are one header because they are one hazard: every conversion below either
// RUNS USER CODE (7.1.1 ToPrimitive, and the ToString and ToPropertyKey that
// begin with it) or reads bytes out of a heap string, so a caller must know
// which it is holding across the call. Gathering them makes that question
// answerable in one place instead of per builtin.

namespace bronze::runtime {

// console.log of a container, in the pinned inspect format. Returns the text;
// the caller writes it.
std::string rtInspect(Value v);

// A heap string from UTF-8 bytes, and JS ToString (7.1.17) / ToNumber (7.1.4)
// entire — step 1 of each, ToPrimitive, included.
//
// So both RUN USER CODE for an OBJECT argument, and both can leave a TypeError
// pending: a caller must root what it holds across them and be reached from an
// IL op `il::canThrow` marks. For a PRIMITIVE neither allocates and only a
// Symbol raises, which is what keeps them usable from the builtins that hold a
// raw element pointer across a numeric argument conversion.
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

// ECMA-262 7.1.1 ToPrimitive's hint. Default and Number ask `valueOf` before
// `toString` and String asks the other way round, which is the ONLY thing the
// hint decides — and the classic bug, since `'' + {}` is Default (13.15.3 asks
// for no hint) where `String({})` is String.
enum class ToPrimitiveHint { Default, Number, String };

// ToPrimitive, ToString with its step 1 attached, and ToPropertyKey (7.1.19),
// which is that same step 1 with the Symbol case carved out — a symbol IS a
// key, so stringifying one would throw where `o[sym]` must simply read.
//
// All three RUN USER CODE — a `Symbol.toPrimitive`, a `valueOf` or a `toString`
// on the input's chain — so a caller must have everything it holds rooted, and
// must be reached from an IL op `il::canThrow` marks, or a TypeError raised
// here propagates past the `catch` that should have taken it.
Value rtToPrimitive(Rooted<Value>& input, ToPrimitiveHint hint);
// 7.1.1.1 OrdinaryToPrimitive alone — the `valueOf`/`toString` pair WITHOUT the
// `@@toPrimitive` lookup in front of it. One caller, and it could not use the
// whole algorithm: `Date.prototype[Symbol.toPrimitive]` IS the @@toPrimitive
// hook, so asking for step 2 would find itself.
Value rtOrdinaryToPrimitive(Rooted<Value>& input, ToPrimitiveHint hint);
Value rtToStringValue(Rooted<Value>& v);
Value rtToPropertyKey(Rooted<Value>& key);

}  // namespace bronze::runtime
