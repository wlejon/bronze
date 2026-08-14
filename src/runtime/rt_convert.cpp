// Type conversion: the boxing and unboxing entry points of the ABI, JS
// ToString / ToNumber, truthiness, strict equality, and the two `+` helpers
// that sit on top of them.
//
// ToPrimitive (7.1.1) lives here, and the two conversions below deliberately do
// NOT call it.
//
// `valueToString` and `rtToNumber` are ToString and ToNumber for a value that
// is ALREADY primitive, plus the objects whose answer is a pure function of
// what they hold — a RegExp's source and flags, a pristine wrapper's internal
// slot. Any other object is a hard error there, and stays one, because those
// two are reached from places that must not run user code: `console.log` is on
// `il::canThrow`'s cannot-throw list and `rtToNumber` is called with a raw
// typed-array pointer held across it. Widening either would put a call to a
// user `toString` inside both.
//
// So ToPrimitive is applied where ECMA-262 says it is applied — at `+`
// (13.15.3) and at ToString's own entry (`String(x)`, a template literal) — and
// those two sites hand a PRIMITIVE down to the conversions below, which is the
// only kind of value they ever claimed to take. What still refuses an object by
// name is every site whose clause also calls ToPrimitive and whose helper
// cannot yet run user code: `rel.lt` and friends, `==` against a primitive, a
// computed property key, and ToNumber. Each names itself.

#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

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
        // The one object bronze can convert without ToPrimitive: a RegExp's
        // `toString` is a pure function of its source and flags (22.2.6.13),
        // so `"" + /a/g` is "/a/g" rather than a named error. Every other
        // object still goes through ToPrimitive and is still refused.
        return rtMakeString(rtRegExpText(v));
    } else if (Value prim; rtWrapperPrimitive(v, prim)) {
        // A primitive WRAPPER, which is the other object whose ToPrimitive
        // answer bronze can give without the algorithm: OrdinaryToPrimitive
        // would call `valueOf`, and for a pristine wrapper that is the internal
        // slot. So `String(new String("ab"))` and `new String("ab") + "!"` are
        // the characters rather than a named error.
        return valueToString(prim);
    } else {
        // Every caller that is ALLOWED to run user code has already been
        // through `rtToPrimitive`, so an object arriving here came from one
        // that is not: `console.log` (whose `Op::Print` is on `il::canThrow`'s
        // cannot-throw list), `JSON.stringify` (25.5.2 has its own algorithm
        // and must not borrow this one), or a key conversion holding a raw
        // header. Naming that beats calling a user `toString` from a place that
        // cannot survive the collection or the throw.
        fatal("unsupported: ToString on an object from a site that cannot run ToPrimitive "
              "(7.1.17 step 1 calls a user `toString`, and this caller — console.log, "
              "JSON.stringify, or a property-key conversion — holds state across it); "
              "`+` and `String(x)` do run it");
    }
    return Value::fromString(StringHeader::createFromUTF8(rtHeap(), literal));
}

Value rtMakeString(std::string_view utf8) {
    return Value::fromString(StringHeader::createFromUTF8(rtHeap(), utf8));
}

Value rtValueToString(Value v) { return valueToString(v); }

// ECMA-262 7.1.1 ToPrimitive, with 7.1.1.1 OrdinaryToPrimitive under it.
//
// A primitive is already one and is returned untouched, which is the whole of
// step 1 for every value that is not an object.
//
// STEP 2 — `GetMethod(input, @@toPrimitive)` — is not performed, and that is a
// refusal rather than a silent skip. `Symbol.toPrimitive` is on
// builtin_symbol.cpp's unimplemented list, so a program asking for the
// well-known symbol gets a named hard error; and 20.4.2.1's registry hands back
// a symbol that is NOT the well-known one, so `Symbol.for("Symbol.toPrimitive")`
// cannot smuggle it in either. No bronze program can hold the key, therefore no
// bronze object can carry the property, therefore the lookup provably finds
// undefined. The day `Symbol.toPrimitive` lands, this is where its step goes.
//
// The HINT decides only the order of the two calls, and getting it backwards is
// the classic bug: `'' + {}` is hint DEFAULT (13.15.3 asks for no hint at all
// and decides on Strings afterwards) while `String({})` is hint STRING, and the
// two reach "[object Object]" by opposite routes. Default and number are the
// same order, which is why 7.1.1.1 takes them together.
//
// Step 3's TypeError is thrown rather than fataled: the clause names it, both
// callers are on `il::canThrow`'s list, and a program can catch it.
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

    double magnitude = 0.0;
    auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), magnitude,
                                     std::chars_format::general);
    if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return sign * magnitude;
}

double rtToNumber(Value v) {
    // ToNumber of a Symbol is a TypeError in 6.1.5.1, and a HARD error here for
    // the reason ToNumber of an object is one: `Op::Unbox` is on `canThrow`'s
    // cannot-throw list, so generated code emits no cell test after it and a
    // thrown TypeError would propagate past the `catch` that should have taken
    // it. Naming the construct is the honest answer until the numeric path
    // learns to unwind; guessing a number is not.
    if (v.isSymbol()) {
        fatal("unsupported: a Symbol in an arithmetic position (ECMA-262 6.1.5.1 makes "
              "ToNumber of a Symbol a TypeError, and bronze's numeric path cannot yet "
              "raise one; use sym.description or sym.toString())");
    }
    if (v.isNumber()) return v.asNumber();
    if (v.isInt32()) return static_cast<double>(static_cast<int32_t>(v.payload()));
    if (v.isBool()) return v.asBool() ? 1.0 : 0.0;
    if (v.isNull()) return 0.0;
    if (v.isUndefined()) return std::numeric_limits<double>::quiet_NaN();
    if (v.isString()) return stringToNumber(v.asString<StringHeader>());
    // A primitive wrapper, unwrapped from its internal slot rather than through
    // ToPrimitive. This function ALLOCATES NOTHING — two typed-array writes in
    // rt_prop.cpp hold a raw view pointer across a call to it — and the unwrap
    // keeps that promise: reading a slot allocates nothing, and neither does
    // the string or boolean conversion it hands back to.
    if (Value prim; rtWrapperPrimitive(v, prim)) return rtToNumber(prim);
    // Not a missing algorithm: 7.1.4 step 1 is ToPrimitive with hint number,
    // and `rtToPrimitive` takes that hint. What blocks it is the sentence four
    // lines up — this function allocates nothing, and two typed-array writes in
    // rt_prop.cpp hold a raw view pointer across it — together with
    // `bronze_unbox_f64`, which reaches here from `Op::Unbox` and is on
    // `il::canThrow`'s cannot-throw list, so a TypeError raised inside would
    // propagate past the `catch` that should have taken it. Both have to move
    // before `+{}` can be a number.
    fatal("unsupported: ToNumber on an object (7.1.4 step 1 runs ToPrimitive with hint "
          "number, which bronze applies at `+` and `String(x)`; the numeric path allocates "
          "nothing and cannot raise, so it cannot yet call it)");
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
    // Only ToPrimitive on an object is still unbuilt, and that stays a named
    // hard error rather than a silent 0.
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
    // It is raised HERE rather than left to `rtToNumber` below because this
    // helper is on `il::canThrow`'s list and the unbox is not: a throw from
    // inside the arithmetic would propagate past the `catch` that should have
    // taken it. `undefined` goes into the caller's root slot before it tests
    // the pending cell, which is the contract every raising helper keeps.
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
