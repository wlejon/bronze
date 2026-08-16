// Type conversion: the boxing and unboxing entry points of the ABI, JS
// ToString / ToNumber / ToPropertyKey, truthiness, strict equality, and the two
// `+` helpers that sit on top of them.
//
// ToPrimitive (7.1.1) lives here, and every clause that calls it reaches it: `+`
// (13.15.3), ToString's own entry (`String(x)`, a template literal), ToNumber
// (7.1.4 step 1), the four relational operators (13.10.1), `==` against a
// primitive (7.2.14 steps 11-12) and a computed property key (7.1.19). It runs
// a user `valueOf`, a user `toString` or a `Symbol.toPrimitive`, so a caller
// must root everything it holds across the call and must sit under an IL op
// `il::canThrow` marks.
//
// `valueToString` — the static below — is the REMAINDER: ToString for a value
// that is already primitive, plus the two objects whose answer is a pure
// function of what they hold (a RegExp's source and flags, a pristine wrapper's
// internal slot). It is what `console.log` and `JSON.stringify` use, and an
// object reaching it is an internal error rather than a program error: those
// two have their own algorithms and must not run a user `toString` at all —
// `Op::Print` is on `il::canThrow`'s cannot-throw list and inspect holds raw
// heap pointers across every element it formats.
//
// The HINT is the thing to get right rather than the call: `'' + {}` is hint
// DEFAULT, `String({})` is hint STRING, `{} * 2` and `{} < 1` are hint NUMBER,
// and `o[{}]` is hint STRING again. Default and Number share an order (valueOf
// first) and String reverses it, so a site that passes the wrong one produces a
// plausible answer from the wrong method.

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <clocale>
#include <locale.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <xlocale.h>
#endif

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/number_format.h"
#include "runtime/profile.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

static Value valueToString(Value v) {
    if (v.isString()) return v;
    // A symbol does not coerce (ECMA-262 6.1.5.1: ToString of a Symbol throws a
    // TypeError). That is the whole reason the type is worth having — an
    // accidental stringification is loud instead of silently producing a name
    // no other symbol would have produced. `sym.toString()` and `String(sym)`
    // are the two ways to ask for the text, and both spell it out.
    //
    // Thrown rather than fatal, and CATCHABLE: `"" + sym` lowers to a dynamic
    // add, which `il::canThrow` marks, so generated code tests the pending cell
    // right after it. The empty string returned here is what the caller stores
    // into its root slot before that test, never a value a program reads.
    if (v.isSymbol()) {
        rtThrowTypeError("Cannot convert a Symbol value to a string");
        return rtMakeString("");
    }
    char buf[64];
    if (v.isNumber()) {
        size_t len = formatJsNumber(v.asNumber(), buf);
        return Value::fromString(
            StringHeader::createFromUTF8(rtHeap(), std::string_view(buf, len)));
    }
    if (v.isInt32()) {
        size_t len = formatJsNumber(static_cast<double>(static_cast<int32_t>(v.payload())), buf);
        return Value::fromString(
            StringHeader::createFromUTF8(rtHeap(), std::string_view(buf, len)));
    }
    const char* literal = nullptr;
    if (v.isBool()) {
        literal = v.asBool() ? "true" : "false";
    } else if (v.isNull()) {
        literal = "null";
    } else if (v.isUndefined()) {
        literal = "undefined";
    } else if (rtIsRegExp(v)) {
        // A RegExp, whose `toString` is a pure function of its source and flags
        // (22.2.6.13) — so `console.log` and `JSON.stringify`, the two callers
        // that must not run user code, can still name one. A program's `'' +
        // /a/g` does not come here: it runs the real algorithm and finds
        // 22.2.6.13 on the prototype.
        return rtMakeString(rtRegExpText(v));
    } else if (Value prim; rtWrapperPrimitive(v, prim)) {
        // A primitive WRAPPER, for the same two callers: OrdinaryToPrimitive
        // would call `valueOf`, and for a PRISTINE wrapper that call answers
        // exactly the internal slot — so the answer is available without
        // running anything. A wrapper whose `valueOf` the program replaced is
        // named by `rtWrapperPrimitive` rather than answered wrongly.
        return valueToString(prim);
    } else {
        // ToString's step 1 is ToPrimitive, and every entry point that performs
        // it — `rtValueToString`, `rtToStringValue`, `bronze_dynamic_add` — has
        // already run it before calling here. So an object at this point came
        // from a caller that is NOT allowed to: `console.log` (whose `Op::Print`
        // is on `il::canThrow`'s cannot-throw list, and whose inspect walk holds
        // raw heap pointers) or `JSON.stringify`, which has its own algorithm in
        // 25.5.2 and must not borrow this one. Reaching it is a bug in this
        // file's own layering rather than something a program did.
        fatal("internal: ToString of an object below ToPrimitive (7.1.17 step 1 runs it, and "
              "every entry point here does; console.log and JSON.stringify have their own "
              "algorithms and must not reach this one)");
    }
    return Value::fromString(StringHeader::createFromUTF8(rtHeap(), literal));
}

Value rtMakeString(std::string_view utf8) {
    return Value::fromString(StringHeader::createFromUTF8(rtHeap(), utf8));
}

// ECMA-262 7.1.17 ToString for a value that arrives as raw bits rather than
// through a root the caller already holds. Step 1 is ToPrimitive with hint
// STRING, so a user `toString` runs — which is what makes `new Error(obj)`,
// `"ab".replace(obj, r)` and `Symbol(obj)` say what the object says.
//
// The primitive case does not root at all: nothing before the conversion can
// collect, so `String(5)` costs exactly what it did.
Value rtValueToString(Value v) {
    if (!v.isObject()) return valueToString(v);
    Rooted<Value> input{v};
    return rtToStringValue(input);
}

// ECMA-262 7.1.1 ToPrimitive, with 7.1.1.1 OrdinaryToPrimitive under it.
//
// A primitive is already one and is returned untouched, which is the whole of
// step 1 for every value that is not an object.
//
// STEP 2 is `GetMethod(input, @@toPrimitive)`, and it comes FIRST: an object
// that defines `Symbol.toPrimitive` never has `valueOf` or `toString` called at
// all, and the hint it receives is the hint this call was made with, spelled as
// one of the three strings 7.1.1 names. The lookup is an ordinary property read
// through the prototype chain, so an inherited one is found.
//
// The HINT decides only the order of the two calls, and getting it backwards is
// the classic bug: `'' + {}` is hint DEFAULT (13.15.3 asks for no hint at all
// and decides on Strings afterwards) while `String({})` is hint STRING, and the
// two reach "[object Object]" by opposite routes. Default and number are the
// same order, which is why 7.1.1.1 takes them together.
//
// Step 3's TypeError is thrown rather than fataled: the clause names it, every
// caller sits under an IL op `il::canThrow` marks, and a program can catch it.
Value rtToPrimitive(Rooted<Value>& input, ToPrimitiveHint hint) {
    if (!input.get().isObject()) return input.get();

    // 7.1.1 step 2: check for @@toPrimitive method
    {
        Rooted<Value> toPrimKey{Value::fromSymbol(rtSymbolToPrimitive())};
        Rooted<Value> exoticToPrim{
            Value(bronze_elem_get(input.get().rawBits(), toPrimKey.get().rawBits()))};
        if (rtExceptionPending()) return Value::fromUndefined();
        if (!exoticToPrim.get().isUndefined() && !exoticToPrim.get().isNull()) {
            if (!exoticToPrim.get().isObject() ||
                exoticToPrim.get().asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
                rtThrowTypeError("Symbol.toPrimitive is not a function");
                return Value::fromUndefined();
            }
            const char* hintStr = (hint == ToPrimitiveHint::String) ? "string"
                                : (hint == ToPrimitiveHint::Number) ? "number"
                                : "default";
            Rooted<Value> hintVal{rtMakeString(hintStr)};
            const uint64_t args[1] = {hintVal.get().rawBits()};
            Rooted<Value> result{Value(bronze_dynamic_call(
                exoticToPrim.get().rawBits(), input.get().rawBits(), 1, args))};
            if (rtExceptionPending()) return Value::fromUndefined();
            if (result.get().isObject()) {
                rtThrowTypeError("Cannot convert object to primitive value");
                return Value::fromUndefined();
            }
            return result.get();
        }
    }

    return rtOrdinaryToPrimitive(input, hint);
}

// 7.1.1.1 OrdinaryToPrimitive, on its own so that the ONE caller which must
// skip step 2 above can reach it: `Date.prototype[Symbol.toPrimitive]`
// (21.4.4.45) is defined as a call to this, and going through `rtToPrimitive`
// would find itself and recurse forever. Every other caller wants the whole
// algorithm and asks for it above.
Value rtOrdinaryToPrimitive(Rooted<Value>& input, ToPrimitiveHint hint) {
    // 7.1.1.1 step 1/2: "string" tries toString then valueOf, and both other
    // hints try valueOf then toString.
    const char* order[2] = {"valueOf", "toString"};
    if (hint == ToPrimitiveHint::String) {
        order[0] = "toString";
        order[1] = "valueOf";
    }
    for (const char* name : order) {
        Rooted<Value> key{rtMakeString(name)};
        // Through the ordinary property path, so the method is whatever the
        // receiver's chain really answers — a program's own `toString`, an
        // inherited one, or `Object.prototype`'s. It is also where an
        // unimplemented member of a nearer prototype is refused BY NAME, which
        // is how `'' + [1, 2]` reports that `Array.prototype.toString` is not
        // built instead of answering "[object Array]" — a wrong answer that
        // would have looked right.
        Rooted<Value> method{Value(bronze_elem_get(input.get().rawBits(), key.get().rawBits()))};
        if (rtExceptionPending()) return Value::fromUndefined();
        // Step 3.a is IsCallable, and a non-callable member is SKIPPED rather
        // than an error: `{ toString: 1 }` falls through to `valueOf`.
        if (!method.get().isObject() ||
            method.get().asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
            continue;
        }
        Rooted<Value> result{
            Value(bronze_dynamic_call(method.get().rawBits(), input.get().rawBits(), 0, nullptr))};
        if (rtExceptionPending()) return Value::fromUndefined();
        if (!result.get().isObject()) return result.get();
    }
    rtThrowTypeError("Cannot convert object to primitive value");
    return Value::fromUndefined();
}

// ToString (7.1.17) at its own entry: the conversion a program spells, rather
// than the one the runtime performs on a value it already knows is primitive.
// Step 1 is ToPrimitive with hint STRING, and everything after it is
// `valueToString`.
Value rtToStringValue(Rooted<Value>& v) {
    Rooted<Value> prim{rtToPrimitive(v, ToPrimitiveHint::String)};
    if (rtExceptionPending()) return rtMakeString("");
    return valueToString(prim.get());
}

// ECMA-262 7.1.19 ToPropertyKey: ToPrimitive with hint STRING, and then ToString
// UNLESS the result is a Symbol — a symbol already is a property key, and
// running ToString on one is the TypeError that would turn `o[sym]` into a
// throw. That exception is why this is not simply `rtToStringValue`.
//
// A value that is not an object is handed back UNTOUCHED rather than
// stringified here, so `a[0]` still reaches the element fast paths with a
// number in hand. The conversion those paths need is `rtElemKeyAsString`
// (rt_key.cpp), which is the rest of step 2 for the primitives; this function's
// whole job is to make sure an object never gets that far.
Value rtToPropertyKey(Rooted<Value>& key) {
    if (!key.get().isObject()) return key.get();
    Rooted<Value> prim{rtToPrimitive(key, ToPrimitiveHint::String)};
    if (rtExceptionPending()) return rtMakeString("");
    if (prim.get().isSymbol()) return prim.get();
    return valueToString(prim.get());
}

namespace {

void appendCodePoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

std::string rtUtf8Chars(const StringHeader* s) {
    std::string out;
    const uint32_t len = s->getLength();
    if (s->isLatin1()) {
        const char* data = s->latin1Data();
        for (uint32_t i = 0; i < len; ++i) {
            appendCodePoint(out, static_cast<unsigned char>(data[i]));
        }
        return out;
    }
    const uint16_t* u16 = s->utf16Data();
    for (uint32_t i = 0; i < len; ++i) {
        uint32_t cp = u16[i];
        // A well-formed surrogate pair is one code point; a lone surrogate is
        // encoded as itself, which is what a JS string is allowed to hold.
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
            const uint32_t low = u16[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        appendCodePoint(out, cp);
    }
    return out;
}

std::string rtAsciiChars(const StringHeader* s) {
    std::string out;
    const uint32_t len = s->getLength();
    out.reserve(len);
    if (s->isLatin1()) {
        const char* data = s->latin1Data();
        for (uint32_t i = 0; i < len; ++i) {
            unsigned char c = static_cast<unsigned char>(data[i]);
            out.push_back(c < 0x80 ? static_cast<char>(c) : '\xFF');
        }
        return out;
    }
    const uint16_t* data = s->utf16Data();
    for (uint32_t i = 0; i < len; ++i) {
        out.push_back(data[i] < 0x80 ? static_cast<char>(data[i]) : '\xFF');
    }
    return out;
}

// ECMA-262 StringNumericLiteral: leading/trailing whitespace is stripped, the
// empty string is 0, `Infinity` is spelled out, the three radix prefixes are
// unsigned, and anything the whole of which is not consumed is NaN.
// Deliberately NOT std::strtod: that accepts `0x` forms with a sign, `nan`,
// and locale decimal points, none of which JS does.
static double stringToNumber(const StringHeader* s) {
    static constexpr std::string_view kSpace = " \t\n\r\f\v";
    std::string text = rtAsciiChars(s);
    size_t begin = text.find_first_not_of(kSpace);
    if (begin == std::string::npos) return 0.0;
    size_t end = text.find_last_not_of(kSpace) + 1;
    std::string_view body(text.data() + begin, end - begin);
    if (body.empty()) return 0.0;

    if (body.size() > 2 && body[0] == '0') {
        int base = 0;
        switch (body[1]) {
            case 'x': case 'X': base = 16; break;
            case 'o': case 'O': base = 8; break;
            case 'b': case 'B': base = 2; break;
            default: break;
        }
        if (base != 0) {
            uint64_t magnitude = 0;
            auto [ptr, ec] = std::from_chars(body.data() + 2, body.data() + body.size(),
                                             magnitude, base);
            if (ec != std::errc{} || ptr != body.data() + body.size()) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            return static_cast<double>(magnitude);
        }
    }

    std::string_view digits = body;
    double sign = 1.0;
    if (digits.front() == '+' || digits.front() == '-') {
        sign = digits.front() == '-' ? -1.0 : 1.0;
        digits.remove_prefix(1);
    }
    if (digits == "Infinity") return sign * std::numeric_limits<double>::infinity();
    if (digits.empty()) return std::numeric_limits<double>::quiet_NaN();

    char first = digits[0];
    if (first == '.') {
        if (digits.size() < 2 || digits[1] < '0' || digits[1] > '9') {
            return std::numeric_limits<double>::quiet_NaN();
        }
    } else if (first < '0' || first > '9') {
        return std::numeric_limits<double>::quiet_NaN();
    }
    for (char c : digits) {
        if (c == 'p' || c == 'P' || c == 'x' || c == 'X') {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }

    std::string str(digits);
    char* endptr = nullptr;
    errno = 0;
#if defined(_WIN32)
    static _locale_t c_loc = _create_locale(LC_ALL, "C");
    double magnitude = _strtod_l(str.c_str(), &endptr, c_loc);
#elif defined(__APPLE__)
    double magnitude = strtod_l(str.c_str(), &endptr, NULL);
#else
    static locale_t c_loc = newlocale(LC_ALL_MASK, "C", nullptr);
    double magnitude = strtod_l(str.c_str(), &endptr, c_loc);
#endif
    if (endptr != str.c_str() + str.size()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return sign * magnitude;
}

// ECMA-262 7.1.4 ToNumber, entire.
//
// A PRIMITIVE costs what it always did: no root, no allocation, no call. That
// half of the contract is load-bearing — `rtToNumber` is the conversion every
// builtin runs on its numeric arguments, and several of them hold a raw view or
// element pointer across it — so the allocating half is confined to the one tag
// that can reach it, and every caller that can be handed an OBJECT has been
// re-rooted (rt_prop_write.cpp's typed-array writes are the ones that had to
// move).
//
// A Symbol THROWS rather than fatals: 6.1.5.1 makes ToNumber of a Symbol a
// TypeError, and a program is entitled to catch it. That is what forced
// `Op::Unbox` and `Op::ToInt32` off `il::canThrow`'s cannot-throw list — the
// unbox is generated code's only numeric coercion, so without a cell test after
// it the throw would have propagated past the `catch` that should have taken
// it. NaN is what a raising path returns, and it is never a value a program
// reads: the caller stores it and then tests the cell.
double rtToNumber(Value v) {
    if (v.isNumber()) return v.asNumber();
    if (v.isInt32()) return static_cast<double>(static_cast<int32_t>(v.payload()));
    if (v.isBool()) return v.asBool() ? 1.0 : 0.0;
    if (v.isNull()) return 0.0;
    if (v.isUndefined()) return std::numeric_limits<double>::quiet_NaN();
    if (v.isString()) return stringToNumber(v.asString<StringHeader>());
    if (v.isSymbol()) {
        rtThrowTypeError("Cannot convert a Symbol value to a number");
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (!v.isObject()) return std::numeric_limits<double>::quiet_NaN();
    // Step 1: ToPrimitive with hint NUMBER, so `valueOf` is asked before
    // `toString` and a `Symbol.toPrimitive` before either. The result is a
    // primitive or the algorithm threw, so the recursion is one level deep.
    //
    // A primitive WRAPPER comes through here too rather than through
    // `rtWrapperPrimitive`'s internal-slot shortcut, which is the point: a
    // `new Number(1)` whose `valueOf` the program replaced answers with the
    // replacement, where reading the slot would have quietly ignored it.
    Rooted<Value> input{v};
    Rooted<Value> prim{rtToPrimitive(input, ToPrimitiveHint::Number)};
    if (rtExceptionPending()) return std::numeric_limits<double>::quiet_NaN();
    return rtToNumber(prim.get());
}

// A canonical array index: the decimal form must round-trip, so "0" and "42"
// qualify while "01", "1.0", "-1" and " 1" are ordinary string keys.
bool rtIsIntegerLikeKey(std::string_view key, uint32_t& out) {
    if (key.empty() || key.size() > 10) return false;
    if (key.size() > 1 && key[0] == '0') return false;
    uint64_t v = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if (v > 4294967294ull) return false;  // 2^32-2, the last array index
    out = static_cast<uint32_t>(v);
    return true;
}

extern "C" {

// 7.1.17 ToString, the one a template substitution lowers to (13.2.8.6). It is
// the ONLY generated-code entry point that runs ToPrimitive on its own — every
// other conversion helper here takes a value it may assume is already primitive
// — which is why it is a helper of its own rather than an `Add` with an empty
// string on the left.
uint64_t bronze_to_string(uint64_t bits) {
    recordHelperCall("bronze_to_string");
    Rooted<Value> v{Value(bits)};
    return rtToStringValue(v).rawBits();
}

uint64_t bronze_box_f64(double v) { return Value::fromDouble(v).rawBits(); }

uint64_t bronze_box_i32(int32_t v) {
    return Value::fromTagAndPayload(static_cast<uint16_t>(Tag::Int32),
                                    static_cast<uint32_t>(v))
        .rawBits();
}

uint64_t bronze_box_bool(bool v) { return Value::fromBool(v).rawBits(); }

uint64_t bronze_box_str(const char* s) {
    if (!s) return Value::fromUndefined().rawBits();
    return Value::fromString(StringHeader::createFromUTF8(rtHeap(), std::string_view(s)))
        .rawBits();
}

uint64_t bronze_box_str_key(uint32_t keyIndex) {
    return bronze_box_str(rtKeyString(keyIndex).c_str());
}

double bronze_unbox_f64(uint64_t bits) {
    // Generated code's only numeric coercion, and it is exactly ToNumber
    // (7.1.4) — the same function the builtins already call, rather than a
    // second, smaller one that agreed with it on four tags and diverged on the
    // rest. It had its own copy that fataled on a string, so `+"5"` and
    // `o.k = "5", o.k--` were hard errors while `Number("5")` next to them was
    // 5, and an int32-tagged value fell all the way through to the fatal.
    //
    // An OBJECT operand runs ToPrimitive here, which is a call into user code
    // from an unbox — the reason `il::canThrow` had to admit `Op::Unbox`. The
    // operand needs no root of its own: it is the only live value this helper
    // has, and the caller's is a frame slot the collector updates.
    return rtToNumber(Value(bits));
}

int32_t bronze_unbox_i32(uint64_t bits) {
    Value v(bits);
    if (v.isInt32()) return static_cast<int32_t>(v.payload());
    if (v.isNumber()) return static_cast<int32_t>(v.asNumber());
    if (v.isBool()) return v.asBool() ? 1 : 0;
    return 0;
}

bool bronze_truthy(uint64_t bits) {
    Value v(bits);
    if (v.isUndefined() || v.isNull() || v.isHole()) return false;
    if (v.isBool()) return v.asBool();
    if (v.isNumber()) {
        double d = v.asNumber();
        return (d != 0.0) && !std::isnan(d);
    }
    if (v.isInt32()) return static_cast<int32_t>(v.payload()) != 0;
    if (v.isString()) {
        StringHeader* str = v.asString<StringHeader>();
        return str && (str->getLength() > 0);
    }
    return true;
}

bool bronze_unbox_bool(uint64_t bits) { return bronze_truthy(bits); }

bool bronze_is_nullish(uint64_t bits) {
    Value v(bits);
    return v.isNull() || v.isUndefined() || v.isHole();
}

bool bronze_strict_eq(uint64_t aBits, uint64_t bBits) {
    Value a(aBits);
    Value b(bBits);
    if (a.isNumber() && b.isNumber()) {
        return a.asNumber() == b.asNumber();  // NaN !== NaN, +0 === -0
    }
    if (a.isString() && b.isString()) {
        return a.asString<StringHeader>()->equals(*b.asString<StringHeader>());
    }
    // Same tag + same payload: bools, null, undefined, object identity.
    // Different tags can never be strictly equal.
    return aBits == bBits;
}

uint64_t bronze_string_concat(uint64_t aBits, uint64_t bBits) {
    recordHelperCall("bronze_string_concat");
    // BOTH operands are rooted before EITHER conversion runs. `valueToString`
    // of a number allocates, and an allocation moves every other live object
    // — including the second operand, whose raw bits are just a pointer until
    // something roots them. Converting a then b left b's pointer stale across
    // a's allocation, so `a.length + ":"` concatenated an empty string under
    // BRONZE_GC_STRESS.
    Rooted<Value> aRoot{Value(aBits)};
    Rooted<Value> bRoot{Value(bBits)};
    aRoot.set(valueToString(aRoot.get()));
    bRoot.set(valueToString(bRoot.get()));
    return StringHeader::concat(rtHeap(), aRoot, bRoot).rawBits();
}

// 13.15.3 ApplyStringOrNumericBinaryOperator for `+`, in the order that clause
// states it — which is the whole subject of the function.
//
// ToPrimitive runs on BOTH operands FIRST, with no hint, and only then is the
// String test made. Testing the raw operands would put the decision before the
// conversion, and for an object that is the difference between `'' + {}` being
// "[object Object]" and being a number: `{}` is not a String, so the wrong
// order sends it to ToNumber and its own named refusal.
//
// No hint is not the same as hint string. `{ toString: () => 'T', valueOf: ()
// => 7 }` is 7 here and "T" under `String(...)`, because default order asks
// `valueOf` first — so `'' + o` and `String(o)` really do disagree, and they
// are meant to.
uint64_t bronze_dynamic_add(uint64_t aBits, uint64_t bBits) {
    recordHelperCall("bronze_dynamic_add");
    Value aVal(aBits);
    Value bVal(bBits);
    if (aVal.isNumber() && bVal.isNumber()) {
        return Value::fromDouble(aVal.asNumber() + bVal.asNumber()).rawBits();
    }
    Rooted<Value> aRoot{aVal};
    Rooted<Value> bRoot{bVal};
    // Both are rooted before either conversion runs: ToPrimitive can call user
    // code, and a collection there moves the other operand out from under any
    // raw bits still being held.
    aRoot.set(rtToPrimitive(aRoot, ToPrimitiveHint::Default));
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    bRoot.set(rtToPrimitive(bRoot, ToPrimitiveHint::Default));
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    if (aRoot.get().isString() || bRoot.get().isString()) {
        return bronze_string_concat(aRoot.get().rawBits(), bRoot.get().rawBits());
    }
    // A symbol has no `+` at all: step 3 runs either ToString or ToNumeric, and
    // 6.1.5.1 makes both a TypeError for a Symbol — so the string branch above
    // covers `"" + sym` and this covers the rest.
    //
    // It is raised HERE rather than left to `rtToNumber` below so that the
    // message names the operator's own step: 13.15.3 step 3 is where a Symbol
    // operand of `+` fails, before either half of the addition is attempted.
    // `undefined` goes into the caller's root slot before it tests the pending
    // cell, which is the contract every raising helper keeps.
    if (aRoot.get().isSymbol() || bRoot.get().isSymbol()) {
        rtThrowTypeError("Cannot convert a Symbol value to a number");
        return Value::fromUndefined().rawBits();
    }
    return Value::fromDouble(bronze_unbox_f64(aRoot.get().rawBits()) +
                             bronze_unbox_f64(bRoot.get().rawBits()))
        .rawBits();
}

}  // extern "C"

}  // namespace bronze::runtime
