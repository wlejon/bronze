// The operators whose meaning is an ALGORITHM rather than a machine
// instruction: ToInt32, exponentiation, abstract (loose) equality, `typeof`,
// `instanceof` and `in`.
//
// Each of these is a numbered sequence of steps in ECMA-262 that no single
// target instruction implements. ToInt32 is the clearest case — `fptosi` is
// poison for a double outside the int32 range, while the language demands a
// wraparound modulo 2^32 — so the conversion is a call and the arithmetic
// around it is not.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {
namespace {

// ECMA-262 7.1.6 ToInt32, spelled out because no cast does it: the double is
// truncated toward zero, reduced modulo 2^32, and the result reinterpreted as
// a signed 32-bit integer. NaN and both infinities have no integer part at
// all and convert to +0, which is what makes `NaN | 0` zero rather than
// undefined behaviour.
int32_t toInt32(double d) {
    if (!std::isfinite(d) || d == 0.0) return 0;
    const double truncated = std::trunc(d);
    // std::fmod keeps the sign of the dividend, so the residue is in
    // (-2^32, 2^32); the cast to uint32_t is the modulo-2^32 reduction and
    // the cast back is the signed reinterpretation. Going through uint32_t
    // matters: a direct (int32_t) cast of an out-of-range double is UB.
    const double residue = std::fmod(truncated, 4294967296.0);
    return static_cast<int32_t>(static_cast<uint32_t>(
        static_cast<int64_t>(residue < 0 ? residue + 4294967296.0 : residue)));
}

// The six strings `typeof` can produce, made once and rooted for the life of
// the program. A fresh heap string per evaluation would put an allocation —
// and so a possible collection — inside an operator that cannot fail.
Value g_typeofStrings[6] = {};
bool g_typeofReady = false;

enum TypeOfKind { kUndefined, kObject, kBoolean, kNumber, kString, kFunction };

Value typeofString(TypeOfKind kind) {
    if (!g_typeofReady) {
        static const char* const kNames[6] = {"undefined", "object",  "boolean",
                                              "number",    "string",  "function"};
        for (int i = 0; i < 6; ++i) {
            g_typeofStrings[i] = rtMakeString(kNames[i]);
            rtHeap().add_permanent_root(&g_typeofStrings[i]);
        }
        g_typeofReady = true;
    }
    return g_typeofStrings[kind];
}

// Whether an object-tagged value can be the right operand of `instanceof`.
// The spec asks for a callable, and the only callable bronze builds is a
// function object.
bool isCallable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == 2;
}

// A key as the string the language would use to look it up. `in` takes an
// arbitrary expression on its left — `0 in arr` is as ordinary as
// `"length" in arr` — and ToPropertyKey is ToString for everything bronze
// has, since there are no symbols.
std::string keyText(Value key) {
    Rooted<Value> str{rtValueToString(key)};
    return rtAsciiChars(str.get().asString<StringHeader>());
}

// Does `holder`, or anything up its prototype chain, define `name`? The walk
// is the property path's own (ObjectHeader::getProp), minus the part that
// reads the value — which is exactly the difference between `in` and a
// property read, and why a property whose value is undefined still answers
// true here.
bool plainObjectHas(ObjectHeader* holder, StringHeader* name) {
    for (uint32_t depth = 0; depth <= 1000; ++depth) {
        uint32_t slot = 0;
        if (holder->shape && holder->shape->lookupProperty(name, slot)) return true;
        ObjectHeader* next = holder->protoAncestor(1);
        if (!next) return false;
        holder = next;
    }
    fatal("prototype chain too deep (a cycle?)");
}

}  // namespace

double rtExponentiate(double base, double exponent) {
    // ECMA-262 Number::exponentiate is not C's pow. std::pow(1, NaN) is 1
    // and std::pow(-1, inf) is 1; the language says both are NaN, and a
    // program that branches on `x ** y` being NaN can tell the difference.
    if (std::isnan(exponent)) return std::numeric_limits<double>::quiet_NaN();
    if (std::abs(base) == 1.0 && std::isinf(exponent)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::pow(base, exponent);
}

extern "C" {

int32_t bronze_to_int32_f64(double d) { return toInt32(d); }

int32_t bronze_to_int32(uint64_t bits) {
    // ToNumber first, which is where a string operand is parsed: `"12" & 10`
    // is 8, not NaN-and-therefore-0. rtToNumber names the object case as a
    // hard error rather than guessing at ToPrimitive.
    return toInt32(rtToNumber(Value(bits)));
}

double bronze_pow(double base, double exponent) { return rtExponentiate(base, exponent); }

uint64_t bronze_typeof(uint64_t bits) {
    Value v(bits);
    // `null` first, and reported as "object": ECMA-262's oldest wart, kept
    // because every engine keeps it and programs test for it.
    if (v.isNull()) return typeofString(kObject).rawBits();
    if (v.isUndefined() || v.isHole()) return typeofString(kUndefined).rawBits();
    if (v.isBool()) return typeofString(kBoolean).rawBits();
    if (v.isNumber() || v.isInt32()) return typeofString(kNumber).rawBits();
    if (v.isString()) return typeofString(kString).rawBits();
    if (v.isSymbol()) fatal("typeof on a symbol is unsupported (bronze has no symbols)");
    if (isCallable(v)) return typeofString(kFunction).rawBits();
    return typeofString(kObject).rawBits();
}

bool bronze_instanceof(uint64_t objBits, uint64_t ctorBits) {
    // Both operands are rooted before anything below allocates, and every
    // header is derived from a root AFTER the last allocation: the collector
    // moves objects, so a raw HeapObjectHeader* held across a call that can
    // allocate points into dead from-space.
    Rooted<Value> objRoot{Value(objBits)};
    Rooted<Value> ctorRoot{Value(ctorBits)};
    if (!isCallable(ctorRoot.get())) {
        rtThrowTypeError("Right-hand side of 'instanceof' is not callable");
        return false;
    }
    // `x instanceof Array` is IsArray(x), answered here rather than by the walk
    // below. The walk cannot answer it at all: an array carries no shape and
    // therefore no prototype chain, so it would report false for every array,
    // on one of the most common guards written in JS. The shortcut is EXACT and
    // not an approximation, because bronze refuses `class X extends Array`
    // (bronze_class_extends) — so there is no array in a bronze program whose
    // chain would have made the two differ.
    if (rtIsArrayConstructor(ctorRoot.get())) {
        return objRoot.get().isObject() &&
               objRoot.get().asObject<HeapObjectHeader>()->flags == 1;
    }
    // A primitive left operand has no prototype chain, so the answer is
    // false — not an error, which is what makes `x instanceof C` a safe
    // guard on an unknown value.
    if (!objRoot.get().isObject()) return false;
    if (objRoot.get().asObject<HeapObjectHeader>()->flags != 0) {
        return false;  // arrays, functions, typed arrays
    }

    rtEnsureFunctionPrototype(ctorRoot);
    Value proto = ctorRoot.get().asObject<FunctionHeader>()->prototype;
    if (!proto.isObject()) return false;

    // The walk compares OBJECT IDENTITY at each link, which is why the
    // prototype has to be materialized above: a constructor whose
    // `.prototype` was never read has no object yet, and creating a
    // different one per test would answer false for its own instances.
    // Nothing in the walk allocates, so this pointer stays valid.
    auto* cur = reinterpret_cast<ObjectHeader*>(objRoot.get().asObject<HeapObjectHeader>());
    for (uint32_t depth = 0; depth <= 1000; ++depth) {
        ObjectHeader* next = cur->protoAncestor(1);
        if (!next) return false;
        if (Value::fromObject(next).rawBits() == proto.rawBits()) return true;
        cur = next;
    }
    fatal("prototype chain too deep (a cycle?)");
}

bool bronze_has_property(uint64_t keyBits, uint64_t objBits) {
    Rooted<Value> objRoot{Value(objBits)};
    if (!objRoot.get().isObject()) {
        rtThrowTypeError("Cannot use 'in' operator: the right-hand side is not an object");
        return false;
    }
    // keyText allocates a string, so the header is derived only afterwards,
    // from the root the collector updates.
    const std::string key = keyText(Value(keyBits));
    uint32_t index = 0;

    HeapObjectHeader* hdr = objRoot.get().asObject<HeapObjectHeader>();
    if (hdr->flags == 1) {  // Array
        auto* arr = reinterpret_cast<ArrayHeader*>(hdr);
        if (key == "length") return true;
        // An index within the length is a key; one past the end is not, which
        // is the whole reason `in` exists on an array. A HOLE is not one either
        // — `delete a[1]` takes index 1 out of the own keys without moving
        // `length`.
        return rtIsIntegerLikeKey(key, index) && arr->hasElem(index);
    }
    if (hdr->flags == TypedArrayHeader::kFlags) {
        auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
        if (key == "length" || key == "buffer" || key == "byteLength" ||
            key == "byteOffset" || key == "BYTES_PER_ELEMENT") {
            return true;
        }
        return rtIsIntegerLikeKey(key, index) && index < view->length;
    }
    if (hdr->flags == ArrayBufferHeader::kFlags) return key == "byteLength";

    const bool isFunction = hdr->flags == 2;
    if (isFunction && key == "prototype") return true;

    // The last allocation; everything the walk touches is re-derived below it.
    Rooted<Value> keyStr{rtMakeString(key)};
    ObjectHeader* holder = nullptr;
    if (isFunction) {
        Value props = objRoot.get().asObject<FunctionHeader>()->properties;
        if (!props.isObject()) return false;
        holder = props.asObject<ObjectHeader>();
    } else {
        holder = reinterpret_cast<ObjectHeader*>(objRoot.get().asObject<HeapObjectHeader>());
    }
    return plainObjectHas(holder, keyStr.get().asString<StringHeader>());
}

// ECMA-262 7.2.14, IsLooselyEqual, in the order the spec states it. The
// order is the specification: `null == 0` is false only because step 2
// answers before any ToNumber can run, and reordering the coercions would
// make it true.
bool bronze_loose_eq(uint64_t aBits, uint64_t bBits) {
    Value a(aBits);
    Value b(bBits);

    const bool aNullish = a.isNull() || a.isUndefined() || a.isHole();
    const bool bNullish = b.isNull() || b.isUndefined() || b.isHole();
    // null and undefined are loosely equal to each other and to NOTHING
    // else — not to 0, not to false, not to "". This is checked before any
    // conversion, which is exactly what makes that true.
    if (aNullish || bNullish) return aNullish && bNullish;

    const bool aNum = a.isNumber() || a.isInt32();
    const bool bNum = b.isNumber() || b.isInt32();

    // Same type: strict equality, NaN and signed zero included.
    if (aNum && bNum) return rtToNumber(a) == rtToNumber(b);
    if (a.isString() && b.isString()) {
        return a.asString<StringHeader>()->equals(*b.asString<StringHeader>());
    }
    if (a.isBool() && b.isBool()) return a.asBool() == b.asBool();
    if (a.isObject() && b.isObject()) return aBits == bBits;

    // A boolean operand is ToNumber'd and the comparison restarts, so
    // `true == "1"` becomes `1 == "1"` becomes `1 == 1`.
    if (a.isBool()) return bronze_loose_eq(Value::fromDouble(a.asBool() ? 1.0 : 0.0).rawBits(), bBits);
    if (b.isBool()) return bronze_loose_eq(aBits, Value::fromDouble(b.asBool() ? 1.0 : 0.0).rawBits());

    // Number against string: the STRING is converted, never the number, so
    // `2 == "2.0"` is true and `1 == "1x"` is false.
    if (aNum && b.isString()) return rtToNumber(a) == rtToNumber(b);
    if (a.isString() && bNum) return rtToNumber(a) == rtToNumber(b);

    // What is left is an object against a primitive, which the language settles
    // with ToPrimitive — valueOf then toString, neither of which bronze has an
    // Object.prototype to find. Named rather than guessed at, on the same rule
    // as ToString of an object.
    fatal("'==' between an object and a primitive needs ToPrimitive, which is unsupported");
}

}  // extern "C"

}  // namespace bronze::runtime
