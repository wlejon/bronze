// Type conversion: the boxing and unboxing entry points of the ABI, JS
// ToString / ToNumber, truthiness, strict equality, and the two `+` helpers
// that sit on top of them.
//
// ToString and ToNumber of an OBJECT are hard errors: both need ToPrimitive
// (7.1.1 OrdinaryToPrimitive — valueOf, then toString), which is not built.
// `Object.prototype` now exists and carries `valueOf`, so the LOOKUP would
// succeed; what is missing is the algorithm around it — the ordered pair of
// calls, the "is the result a primitive" test between them, and the TypeError
// when neither answers one. Naming that beats guessing a number.

#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/number_format.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

static Value valueToString(Value v) {
    if (v.isString()) return v;
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
    } else {
        fatal("ToString on an object is unsupported");
    }
    return Value::fromString(StringHeader::createFromUTF8(rtHeap(), literal));
}

Value rtMakeString(std::string_view utf8) {
    return Value::fromString(StringHeader::createFromUTF8(rtHeap(), utf8));
}

Value rtValueToString(Value v) { return valueToString(v); }

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
    if (v.isNumber()) return v.asNumber();
    if (v.isInt32()) return static_cast<double>(static_cast<int32_t>(v.payload()));
    if (v.isBool()) return v.asBool() ? 1.0 : 0.0;
    if (v.isNull()) return 0.0;
    if (v.isUndefined()) return std::numeric_limits<double>::quiet_NaN();
    if (v.isString()) return stringToNumber(v.asString<StringHeader>());
    fatal("ToNumber on an object is unsupported");
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

uint64_t bronze_dynamic_add(uint64_t aBits, uint64_t bBits) {
    Value aVal(aBits);
    Value bVal(bBits);
    if (aVal.isString() || bVal.isString()) return bronze_string_concat(aBits, bBits);
    return Value::fromDouble(bronze_unbox_f64(aBits) + bronze_unbox_f64(bBits)).rawBits();
}

}  // extern "C"

}  // namespace bronze::runtime
