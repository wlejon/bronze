#include "runtime/rt_helpers.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "runtime/array.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/number_format.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

// Reservation is virtual (commit is on demand), so reserve generously:
// generated code cannot yet survive a moving collection (its SSA values
// are unrooted — see docs/0004), so headroom delays that day of reckoning.
static Heap g_heap(512 * 1024 * 1024);
static NonMovingArena g_arena;
static std::vector<std::string> g_keyStrings;
// The same keys as immortal arena strings: property paths use these
// directly so a property access allocates nothing.
static std::vector<StringHeader*> g_keyHeaders;
static std::vector<InlineCache> g_inlineCaches;
static const std::string g_emptyKey;

static_assert(Value::fromUndefined().rawBits() == BRONZE_ABI_UNDEFINED_BITS,
              "BRONZE_ABI_UNDEFINED_BITS in bronze_abi.h has drifted from the value model");
static_assert(Value::fromNull().rawBits() == BRONZE_ABI_NULL_BITS,
              "BRONZE_ABI_NULL_BITS in bronze_abi.h has drifted from the value model");

static uint64_t bronze_builtin_string_char_code_at(uint64_t thisBits, uint32_t argc, const uint64_t* argvBits) {
    Value thisArg(thisBits);
    if (!thisArg.isString()) return Value::fromDouble(0.0).rawBits();
    StringHeader* str = thisArg.asString<StringHeader>();
    uint32_t idx = 0;
    if (argc > 0) {
        Value arg0(argvBits[0]);
        if (arg0.isNumber()) {
            idx = static_cast<uint32_t>(arg0.asNumber());
        } else if (arg0.isInt32()) {
            idx = static_cast<uint32_t>(arg0.payload());
        }
    }
    return Value::fromDouble(str->charCodeAt(idx)).rawBits();
}

static FunctionHeader* g_charCodeAtFn = nullptr;

static Value valueToString(Value v) {
    if (v.isString()) return v;
    if (v.isNumber()) {
        char buf[64];
        size_t len = formatJsNumber(v.asNumber(), buf);
        StringHeader* sh = StringHeader::createFromUTF8(g_heap, std::string_view(buf, len));
        return Value::fromString(sh);
    } else if (v.isBool()) {
        StringHeader* sh = StringHeader::createFromUTF8(g_heap, v.asBool() ? "true" : "false");
        return Value::fromString(sh);
    } else if (v.isNull()) {
        return Value::fromString(StringHeader::createFromUTF8(g_heap, "null"));
    } else if (v.isUndefined()) {
        return Value::fromString(StringHeader::createFromUTF8(g_heap, "undefined"));
    }
    fatal("ToString on an object is unsupported");
}

extern "C" {

uint64_t bronze_box_f64(double v) {
    return Value::fromDouble(v).rawBits();
}

uint64_t bronze_box_i32(int32_t v) {
    return Value::fromTagAndPayload(static_cast<uint16_t>(Tag::Int32), static_cast<uint32_t>(v)).rawBits();
}

uint64_t bronze_box_bool(bool v) {
    return Value::fromBool(v).rawBits();
}

uint64_t bronze_box_str(const char* s) {
    if (!s) return Value::fromUndefined().rawBits();
    StringHeader* sh = StringHeader::createFromUTF8(g_heap, std::string_view(s));
    return Value::fromString(sh).rawBits();
}

uint64_t bronze_box_str_key(uint32_t keyIndex) {
    std::string keyStr = (keyIndex < g_keyStrings.size()) ? g_keyStrings[keyIndex] : "";
    return bronze_box_str(keyStr.c_str());
}

double bronze_unbox_f64(uint64_t bits) {
    // ToNumber for the primitives it is defined on; anything needing string
    // parsing or ToPrimitive is a named hard error, never a silent 0.
    Value v(bits);
    if (v.isNumber()) return v.asNumber();
    if (v.isBool()) return v.asBool() ? 1.0 : 0.0;
    if (v.isNull()) return 0.0;
    if (v.isUndefined()) return std::numeric_limits<double>::quiet_NaN();
    fatal("ToNumber on a string or object is unsupported");
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
    if (v.isInt32()) {
        return static_cast<int32_t>(v.payload()) != 0;
    }
    if (v.isString()) {
        StringHeader* str = v.asString<StringHeader>();
        return str && (str->getLength() > 0);
    }
    return true;
}

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

bool bronze_unbox_bool(uint64_t bits) {
    return bronze_truthy(bits);
}

uint64_t bronze_create_object() {
    ObjectHeader* obj = ObjectHeader::create(g_heap, g_arena, nullptr);
    obj->header.flags = 0;
    return Value::fromObject(obj).rawBits();
}

uint64_t bronze_create_array(uint32_t length) {
    uint32_t cap = (length < 4) ? 4 : length;
    ArrayHeader* arr = ArrayHeader::create(g_heap, cap);
    arr->header.flags = 1;
    arr->length = length;
    return Value::fromObject(arr).rawBits();
}

uint64_t bronze_create_function(bronze_fn_code code, uint32_t arity) {
    FunctionHeader* fn = FunctionHeader::create(g_heap, code, nullptr, arity);
    fn->header.flags = 2;
    return Value::fromObject(fn).rawBits();
}

uint64_t bronze_prop_get(uint64_t objBits, uint32_t keyIndex, uint32_t icIndex) {
    Value objVal(objBits);

    // IC-hit fast path first: a shape match needs no key at all.
    if (objVal.isObject()) {
        HeapObjectHeader* fastHdr = objVal.asObject<HeapObjectHeader>();
        if (fastHdr->flags == 0 && icIndex < g_inlineCaches.size()) {
            const InlineCache& fastIc = g_inlineCaches[icIndex];
            auto* fastObj = reinterpret_cast<ObjectHeader*>(fastHdr);
            if (fastIc.cached_shape && fastIc.cached_shape == fastObj->shape) {
                return fastObj->getSlot(fastIc.cached_slot).rawBits();
            }
        }
    }

    const std::string& keyStr = (keyIndex < g_keyStrings.size()) ? g_keyStrings[keyIndex] : g_emptyKey;

    if (objVal.isString()) {
        StringHeader* str = objVal.asString<StringHeader>();
        if (keyStr == "length") {
            return Value::fromDouble(str->getLength()).rawBits();
        }
        if (keyStr == "charCodeAt") {
            if (!g_charCodeAtFn) {
                g_charCodeAtFn = FunctionHeader::create(g_heap, bronze_builtin_string_char_code_at, nullptr, 1);
                g_charCodeAtFn->header.flags = 2;
            }
            return Value::fromObject(g_charCodeAtFn).rawBits();
        }
        return Value::fromUndefined().rawBits();
    }

    if (!objVal.isObject()) {
        return Value::fromUndefined().rawBits();
    }

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags == 1) {
        // Array
        ArrayHeader* arr = reinterpret_cast<ArrayHeader*>(hdr);
        if (keyStr == "length") {
            return Value::fromDouble(arr->length).rawBits();
        }
        int idx = -1;
        auto [ptr, ec] = std::from_chars(keyStr.data(), keyStr.data() + keyStr.size(), idx);
        if (ec == std::errc{} && idx >= 0) {
            return arr->getElem(static_cast<uint32_t>(idx)).rawBits();
        }
        return Value::fromUndefined().rawBits();
    }

    if (icIndex >= g_inlineCaches.size()) {
        g_inlineCaches.resize(icIndex + 1);
    }
    InlineCache* ic = &g_inlineCaches[icIndex];
    if (keyIndex >= g_keyHeaders.size() || !g_keyHeaders[keyIndex]) {
        fatal("property access with an unregistered key index");
    }
    // Interned arena key: no allocation on the property path.
    Rooted<Value> key(Value::fromString(g_keyHeaders[keyIndex]));
    ObjectHeader* obj = objVal.asObject<ObjectHeader>();
    Value result = obj->getProp(g_heap, key, ic);
    return result.rawBits();
}

void bronze_prop_set(uint64_t objBits, uint32_t keyIndex, uint64_t valBits, uint32_t icIndex) {
    Value objVal(objBits);
    Value valVal(valBits);
    if (!objVal.isObject()) return;

    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();

    // IC-hit fast path: a shape match writes the slot with no key and no
    // rooting (nothing below can allocate).
    if (hdr->flags == 0 && icIndex < g_inlineCaches.size()) {
        const InlineCache& fastIc = g_inlineCaches[icIndex];
        auto* fastObj = reinterpret_cast<ObjectHeader*>(hdr);
        if (fastIc.cached_shape && fastIc.cached_shape == fastObj->shape) {
            fastObj->setSlot(fastIc.cached_slot, valVal);
            return;
        }
    }

    const std::string& keyStr = (keyIndex < g_keyStrings.size()) ? g_keyStrings[keyIndex] : g_emptyKey;

    if (hdr->flags == 1) {
        // Array
        ArrayHeader* arr = reinterpret_cast<ArrayHeader*>(hdr);
        int idx = -1;
        auto [ptr, ec] = std::from_chars(keyStr.data(), keyStr.data() + keyStr.size(), idx);
        if (ec == std::errc{} && idx >= 0) {
            Rooted<Value> val(valVal);
            arr->setElem(g_heap, static_cast<uint32_t>(idx), val);
        }
        return;
    }

    if (icIndex >= g_inlineCaches.size()) {
        g_inlineCaches.resize(icIndex + 1);
    }
    InlineCache* ic = &g_inlineCaches[icIndex];
    if (keyIndex >= g_keyHeaders.size() || !g_keyHeaders[keyIndex]) {
        fatal("property write with an unregistered key index");
    }
    // Interned arena key: no allocation before the object is dereferenced.
    // setProp itself may still allocate (overflow growth); it re-derives
    // the object through its own root, but this caller's objBits raw value
    // is dead after the call, so that is safe.
    Rooted<Value> key(Value::fromString(g_keyHeaders[keyIndex]));
    Rooted<Value> val(valVal);
    ObjectHeader* obj = objVal.asObject<ObjectHeader>();
    obj->setProp(g_heap, g_arena, key, val, ic);
}

uint64_t bronze_dynamic_call(uint64_t calleeBits, uint64_t thisBits, uint32_t argc, const uint64_t* argvBits) {
    Value calleeVal(calleeBits);
    Value thisVal(thisBits);
    if (!calleeVal.isObject()) {
        std::cerr << "Hard runtime error: Attempted to call non-object dynamic value (" << std::hex << calleeBits << ")" << std::endl;
        std::abort();
    }
    HeapObjectHeader* hdr = calleeVal.asObject<HeapObjectHeader>();
    if (hdr->flags != 2) {
        std::cerr << "Hard runtime error: Attempted to call non-function object (flags=" << hdr->flags << ")" << std::endl;
        std::abort();
    }
    FunctionHeader* fn = reinterpret_cast<FunctionHeader*>(hdr);
    std::vector<Value> args(argc);
    for (uint32_t i = 0; i < argc; ++i) {
        args[i] = Value(argvBits[i]);
    }
    Value res = fn->call(thisVal, argc, args.data());
    return res.rawBits();
}

uint64_t bronze_string_concat(uint64_t aBits, uint64_t bBits) {
    Value aVal(aBits);
    Value bVal(bBits);
    Rooted<Value> aRoot(valueToString(aVal));
    Rooted<Value> bRoot(valueToString(bVal));
    Value res = StringHeader::concat(g_heap, aRoot, bRoot);
    return res.rawBits();
}

uint64_t bronze_dynamic_add(uint64_t aBits, uint64_t bBits) {
    Value aVal(aBits);
    Value bVal(bBits);
    if (aVal.isString() || bVal.isString()) {
        Rooted<Value> aRoot(valueToString(aVal));
        Rooted<Value> bRoot(valueToString(bVal));
        Value res = StringHeader::concat(g_heap, aRoot, bRoot);
        return res.rawBits();
    }
    double aNum = bronze_unbox_f64(aBits);
    double bNum = bronze_unbox_f64(bBits);
    return Value::fromDouble(aNum + bNum).rawBits();
}

void bronze_print_value(uint64_t valBits) {
    Value v(valBits);
    if (v.isNumber()) {
        double num = v.asNumber();
        char buf[64];
        size_t len = 0;
        // console.log distinguishes -0 (inspect formatting), unlike
        // ToString(Number) which yields "0" — node prints "-0" here.
        if (num == 0.0 && std::signbit(num)) {
            buf[len++] = '-';
        }
        len += formatJsNumber(num, buf + len);
        buf[len++] = '\n';
        std::fwrite(buf, 1, len, stdout);
    } else if (v.isString()) {
        StringHeader* str = v.asString<StringHeader>();
        if (str->isLatin1()) {
            const char* data = str->latin1Data();
            uint32_t len = str->getLength();
            for (uint32_t i = 0; i < len; ++i) {
                unsigned char c = static_cast<unsigned char>(data[i]);
                if (c <= 0x7F) {
                    std::fputc(c, stdout);
                } else {
                    std::fputc(static_cast<char>(0xC0 | (c >> 6)), stdout);
                    std::fputc(static_cast<char>(0x80 | (c & 0x3F)), stdout);
                }
            }
        } else {
            const uint16_t* u16 = str->utf16Data();
            uint32_t len = str->getLength();
            for (uint32_t i = 0; i < len; ++i) {
                uint32_t cp = u16[i];
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
                    uint32_t low = u16[i + 1];
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i++;
                    }
                }
                if (cp <= 0x7F) {
                    std::fputc(static_cast<char>(cp), stdout);
                } else if (cp <= 0x7FF) {
                    std::fputc(static_cast<char>(0xC0 | (cp >> 6)), stdout);
                    std::fputc(static_cast<char>(0x80 | (cp & 0x3F)), stdout);
                } else if (cp <= 0xFFFF) {
                    std::fputc(static_cast<char>(0xE0 | (cp >> 12)), stdout);
                    std::fputc(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), stdout);
                    std::fputc(static_cast<char>(0x80 | (cp & 0x3F)), stdout);
                } else {
                    std::fputc(static_cast<char>(0xF0 | (cp >> 18)), stdout);
                    std::fputc(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)), stdout);
                    std::fputc(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), stdout);
                    std::fputc(static_cast<char>(0x80 | (cp & 0x3F)), stdout);
                }
            }
        }
        std::fputc('\n', stdout);
    } else if (v.isBool()) {
        const char* s = v.asBool() ? "true\n" : "false\n";
        std::fputs(s, stdout);
    } else if (v.isUndefined()) {
        std::fputs("undefined\n", stdout);
    } else {
        std::fputs("[object]\n", stdout);
    }
    std::fflush(stdout);
}

void bronze_print_string(const char* s) {
    if (s) {
        std::fputs(s, stdout);
    }
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void bronze_register_key_string(uint32_t index, const char* str) {
    if (index >= g_keyStrings.size()) {
        g_keyStrings.resize(index + 1);
        g_keyHeaders.resize(index + 1, nullptr);
    }
    g_keyStrings[index] = str ? str : "";
    StringHeader* tmp = StringHeader::createFromUTF8(g_heap, std::string_view(g_keyStrings[index]));
    g_keyHeaders[index] = StringHeader::internToArena(g_arena, tmp);
}

}  // extern "C"
}  // namespace bronze::runtime
