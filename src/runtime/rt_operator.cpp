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
#include "runtime/env.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/namespace.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
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

// The strings `typeof` can produce, made once and rooted for the life of the
// program. A fresh heap string per evaluation would put an allocation — and so
// a possible collection — inside an operator that cannot fail.
constexpr int kTypeOfCount = 7;
Value g_typeofStrings[kTypeOfCount] = {};
bool g_typeofReady = false;

enum TypeOfKind { kUndefined, kObject, kBoolean, kNumber, kString, kFunction, kSymbol };

Value typeofString(TypeOfKind kind) {
    if (!g_typeofReady) {
        static const char* const kNames[kTypeOfCount] = {
            "undefined", "object", "boolean", "number", "string", "function", "symbol"};
        for (int i = 0; i < kTypeOfCount; ++i) {
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
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// A key as the string the language would use to look it up. `in` takes an
// arbitrary expression on its left — `0 in arr` is as ordinary as
// `"length" in arr` — so this is ToPropertyKey's ToString branch. A SYMBOL
// never reaches it: `bronze_has_property` answers for one before converting,
// because ToString of a symbol is the TypeError that would make `sym in o`
// throw instead of answering.
std::string keyText(Value key) {
    Rooted<Value> str{rtValueToString(key)};
    return rtAsciiChars(str.get().asString<StringHeader>());
}

// Does `holder`, or anything up its prototype chain, define `name`? The walk
// is the property path's own (ObjectHeader::getProp), minus the part that
// reads the value — which is exactly the difference between `in` and a
// property read, and why a property whose value is undefined still answers
// true here.
bool plainObjectHas(ObjectHeader* holder, PropertyKey name) {
    for (uint32_t depth = 0; depth <= 1000; ++depth) {
        uint32_t slot = 0;
        if (holder->shape && holder->shape->lookupProperty(name, slot)) return true;
        ObjectHeader* next = holder->protoAncestor(1);
        if (!next) return false;
        holder = next;
    }
    fatal("prototype chain too deep (a cycle?)");
}

// ---- `in`, kind by kind ----------------------------------------------------
//
// The two dispatches below are SWITCHES over the whole of HeapKind, and the
// reason is a memory-safety bug rather than a matter of taste. `in` used to be
// an if-chain whose tail cast whatever was left to `ObjectHeader*` and read
// `->shape`. For a Map that word is the entries table, for a RegExp the source
// string, for a module namespace the export count — so `'size' in new Map()`
// did not answer wrongly, it dereferenced a `Value` as a `Shape*` and the
// process died with no diagnostic. Any kind added later would have inherited
// that by doing nothing at all.
//
// So every kind is named, and the fall-through is gone: a kind either has an
// arm that answers or is refused by name. `flags` is a plain `uint16_t` and
// HeapKind is an unnamed enum, so no compiler warning can check these switches
// for exhaustiveness — the static_assert below is the tripwire instead. It
// fails the BUILD when a kind is added, at the one place that has to have an
// opinion about it, which is the property a runtime `default:` alone cannot
// give.
static_assert(HeapKind::Count == 12,
              "a HeapKind was added or removed: give `in` an arm for it in the two switches "
              "below, or refuse it there by name. A kind with no arm used to fall through to "
              "a cast that read its payload's first word as a Shape*.");

// The kinds no program can be holding, so that reaching one is a lowering bug
// and not something a program did — the same answer the property read path
// gives them (rt_prop.cpp).
[[noreturn]] void refuseInternalKind(uint16_t kind) {
    fatal(kind == EnvHeader::kFlags ? "internal: 'in' on an environment record"
                                    : "internal: 'in' on an iteration record");
}

// `Symbol.iterator` and `Symbol.toStringTag` are the two well-known symbols
// bronze has (runtime/symbol.h); every other symbol a program can hold is one it
// made with `Symbol()`, and nothing puts one of those on a receiver that has no
// shape. So for the kinds below the whole symbol question is: does this
// prototype carry one of those two.
//
// `in` can say yes even where a READ of it is a named hard error — an array's
// @@iterator is, because 23.1.3.34 makes it the same function object as
// `Array.prototype.values` and neither is built. That split is the one
// `rtDataViewHasMember` already makes: the member exists, and its value is what
// bronze has not got. Answering `false` instead is what this used to do, and it
// contradicted `m[Symbol.iterator]`, which hands back `Map.prototype.entries`.
//
// Both halves answer from the same place their READ answers from — the
// @@iterator table in rt_prop.cpp's `wellKnownSymbolMember` and the tag switch
// beside it — which is the rule every other arm of these switches follows.
bool shapelessHasSymbol(uint16_t kind, Value key) {
    if (key.asSymbol<SymbolHeader>() == rtSymbolToStringTag()) {
        switch (kind) {
            // 24.1.3.13, 24.2.3.12, 23.2.3.35, 25.1.6.6, 25.3.4.25 put it on
            // the prototype; 10.4.6.1 puts it on the namespace itself, which is
            // the one of these that is an OWN property.
            case HeapKind::Map:
            case HeapKind::Set:
            case HeapKind::TypedArray:
            case HeapKind::ArrayBuffer:
            case HeapKind::DataView:
            case HeapKind::ModuleNamespace:
                return true;
            // An array and a RegExp: 23.1.3 and 22.2.6 define none, which is
            // why 20.1.3.6 keeps a builtin-tag list for them.
            default:
                return false;
        }
    }
    if (key.asSymbol<SymbolHeader>() != rtSymbolIterator()) return false;
    switch (kind) {
        // 23.1.3.34, 23.2.3.34, 24.1.3.12, 24.2.3.11.
        case HeapKind::Array:
        case HeapKind::TypedArray:
        case HeapKind::Map:
        case HeapKind::Set:
            return true;
        // An ArrayBuffer, a DataView and a RegExp are not iterable, and a
        // module namespace exports no name that could be one (10.4.6.4 is
        // "is this an export", and @@toStringTag above is its only other key).
        default:
            return false;
    }
}

// `sym in obj`, for a key that is a Symbol. Only a receiver with a SHAPE can
// carry a symbol-keyed own property, so the two arms that have one do the walk
// and every other kind is asked the ECMA-262 question instead.
bool hasSymbolProperty(Rooted<Value>& objRoot, Value key) {
    const uint16_t kind = objRoot.get().asObject<HeapObjectHeader>()->flags;
    ObjectHeader* holder = nullptr;
    switch (kind) {
        case HeapKind::Plain:
            holder = reinterpret_cast<ObjectHeader*>(objRoot.get().asObject<HeapObjectHeader>());
            break;
        case HeapKind::Function: {
            // A function keeps its own properties, symbol-keyed ones included,
            // in the side object its statics live in.
            Value props = objRoot.get().asObject<FunctionHeader>()->properties;
            if (!props.isObject()) return false;
            holder = props.asObject<ObjectHeader>();
            break;
        }
        case HeapKind::Array:
        case HeapKind::TypedArray:
        case HeapKind::ArrayBuffer:
        case HeapKind::DataView:
        case HeapKind::Map:
        case HeapKind::Set:
        case HeapKind::RegExp:
        case HeapKind::ModuleNamespace:
            return shapelessHasSymbol(kind, key);
        case HeapKind::Iterator:
        case HeapKind::Env:
            refuseInternalKind(kind);
        default:
            fatal((std::string("internal: 'in' with a symbol key on ") +
                   rtObjectKindName(objRoot.get()) + ", a heap kind the operator has no arm for")
                      .c_str());
    }
    return plainObjectHas(holder, PropertyKey::fromValue(key));
}

// `"k" in obj`, for a key that has already been through ToPropertyKey's
// ToString branch.
//
// Every arm answers from the same place the property READ answers from, which
// is what makes `in` and `o.k` one question. For the kinds whose members live
// in a C table rather than on a prototype object bronze has not built, that
// means the table's own predicate — and a name the table knows but bronze has
// not implemented gets the SAME named refusal a read of it gets, because
// answering `false` for a member ECMA-262 defines would be the silent wrong
// answer the refusal exists to prevent.
bool hasNamedProperty(Rooted<Value>& objRoot, const std::string& key) {
    HeapObjectHeader* hdr = objRoot.get().asObject<HeapObjectHeader>();
    uint32_t index = 0;
    switch (hdr->flags) {
        case HeapKind::Array: {
            auto* arr = reinterpret_cast<ArrayHeader*>(hdr);
            if (key == "length") return true;
            // An index within the length is a key; one past the end is not,
            // which is the whole reason `in` exists on an array. A HOLE is not
            // one either — `delete a[1]` takes index 1 out of the own keys
            // without moving `length`.
            return rtIsIntegerLikeKey(key, index) && arr->hasElem(index);
        }
        case HeapKind::TypedArray: {
            // The index first, and against the LENGTH: 10.4.5.2 makes a
            // canonical numeric string outside the range absent rather than
            // inherited, so there is no member table to fall through to.
            auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
            if (rtIsIntegerLikeKey(key, index)) return index < view->length;
            return rtTypedArrayHasMember(view->kindName(), key);
        }
        case HeapKind::ArrayBuffer:
            return rtArrayBufferHasMember(key);
        // A DataView's members all live on its prototype, which bronze answers
        // on the property path — so `in`, which walks the chain, must ask the
        // same table the reads come from rather than report the object empty.
        case HeapKind::DataView:
            return rtDataViewHasMember(key);
        case HeapKind::Map:
            return rtMapHasMember(/*isSetReceiver=*/false, key);
        case HeapKind::Set:
            return rtMapHasMember(/*isSetReceiver=*/true, key);
        case HeapKind::RegExp:
            return rtRegExpHasMember(key);
        case HeapKind::ModuleNamespace: {
            // 10.4.6.4 [[HasProperty]] is exactly "is this an export name":
            // [[Prototype]] is null (10.4.6.1), so nothing else can be true.
            // The last allocation, and the header is re-derived through the
            // root afterwards.
            Rooted<Value> keyStr{rtMakeString(key)};
            return rtModuleNamespaceHasExport(objRoot.get(),
                                              keyStr.get().asString<StringHeader>());
        }
        case HeapKind::Function: {
            // `prototype` lives in its own slot and is materialised lazily, so
            // the walk below cannot see it — but the PROPERTY is there either
            // way, which is what `in` asks.
            if (key == "prototype") return true;
            // `length` and `name` (10.2.10, 10.2.9) live in the header for the
            // same reason and answer the same way. Asked of the header rather
            // than of the statics table, which is where the READ asks — a
            // `static name() {}` is found by the walk below either way, and
            // both spellings then agree that the property is there.
            if ((key == "length" || key == "name") &&
                objRoot.get().asObject<FunctionHeader>()->name != nullptr) {
                return true;
            }
            Rooted<Value> keyStr{rtMakeString(key)};
            Value props = objRoot.get().asObject<FunctionHeader>()->properties;
            if (!props.isObject()) return false;
            return plainObjectHas(props.asObject<ObjectHeader>(),
                                  keyStr.get().asString<StringHeader>());
        }
        case HeapKind::Plain: {
            // A String exotic object's index properties are own properties that
            // live nowhere the walk below can see them — 10.4.3.4 synthesises
            // them from the wrapped characters and bronze answers them on the
            // property path alone — so `"0" in new String("ab")` would read
            // false. Refused by name instead (rt_object.cpp carries the
            // reasoning).
            rtCheckStringExoticOwnKeys(objRoot.get(), "testing");
            Rooted<Value> keyStr{rtMakeString(key)};
            auto* holder =
                reinterpret_cast<ObjectHeader*>(objRoot.get().asObject<HeapObjectHeader>());
            return plainObjectHas(holder, keyStr.get().asString<StringHeader>());
        }
        case HeapKind::Iterator:
        case HeapKind::Env:
            refuseInternalKind(hdr->flags);
        default:
            fatal((std::string("internal: 'in' on ") + rtObjectKindName(objRoot.get()) +
                   ", a heap kind the operator has no arm for")
                      .c_str());
    }
}

// ToPrimitive with the NUMBER hint, for the two operands ECMA-262 13.10.1
// step 1 asks for. A primitive is already one; a primitive WRAPPER answers
// with its internal slot, which is what OrdinaryToPrimitive's `valueOf` call
// would return, and is what makes `new String("a") < "b"` a string comparison
// rather than a numeric one. Every other object needs the real algorithm —
// a user `valueOf`, then `toString`, with the primitive test between them —
// and it is not built; naming it is the honest answer, and it is the same
// answer `rtToNumber` already gives such an object.
//
// Nothing here allocates, which is what lets the comparison below hold raw
// StringHeader pointers across it.
Value relationalToPrimitive(Value v) {
    if (!v.isObject()) return v;
    if (Value prim; rtWrapperPrimitive(v, prim)) return prim;
    fatal("a relational operator ('<', '>', '<=', '>=') on an object needs ToPrimitive, "
          "which is unsupported");
}

// ECMA-262 13.10.1 IsLessThan, whose result is a Boolean **or undefined**.
// The third answer is the whole reason the four operators are not one compare
// and its negation: 13.10 maps undefined to false for every one of them, while
// `!` maps it to true for two of them.
enum class LessThan { False, True, Undefined };

// `x < y` in the spec's own terms. The LeftFirst flag of 13.10.1 orders the two
// ToPrimitive calls and nothing else, and neither call above can run user code
// or have an effect, so the order is unobservable here and is not threaded
// through.
LessThan isLessThan(Value x, Value y) {
    const Value px = relationalToPrimitive(x);
    const Value py = relationalToPrimitive(y);
    // Step 3: both Strings, compared by code unit with NOTHING converted. It
    // comes before ToNumeric, which is why `"2" < "10"` is true where
    // `2 < 10` is false — the digits are never read as digits.
    if (px.isString() && py.isString()) {
        return px.asString<StringHeader>()->lessThan(*py.asString<StringHeader>())
                   ? LessThan::True
                   : LessThan::False;
    }
    // Step 4, the else-branch: ToNumeric on both, and step 4.c's undefined for
    // a NaN on either side — which includes the case where one operand is a
    // string that does not parse as a number.
    const double nx = rtToNumber(px);
    const double ny = rtToNumber(py);
    if (std::isnan(nx) || std::isnan(ny)) return LessThan::Undefined;
    return nx < ny ? LessThan::True : LessThan::False;
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
    if (v.isSymbol()) return typeofString(kSymbol).rawBits();
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
               objRoot.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array;
    }
    // A primitive left operand has no prototype chain, so the answer is
    // false — not an error, which is what makes `x instanceof C` a safe
    // guard on an unknown value.
    if (!objRoot.get().isObject()) return false;
    if (objRoot.get().asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
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
    // A SYMBOL key, answered before ToPropertyKey below can try to stringify
    // it — ToString of a symbol is the TypeError that would make `sym in o`
    // throw rather than answer. A symbol is arena-allocated and never moves
    // (runtime/symbol.h), so holding these bits across the calls below is safe.
    if (Value(keyBits).isSymbol()) return hasSymbolProperty(objRoot, Value(keyBits));
    // keyText allocates a string, so every header below is derived afterwards,
    // from the root the collector updates.
    return hasNamedProperty(objRoot, keyText(Value(keyBits)));
}

// The four relational operators of ECMA-262 13.10, each written as the
// standard writes it: one IsLessThan call, with the operands in the order that
// clause gives, and its undefined folded to false.
//
// The pairing is what matters. `a < b` and `a >= b` ask IsLessThan(a, b);
// `a > b` and `a <= b` ask IsLessThan(b, a). Within a pair the two operators
// differ only in which of the three answers they call true — and `<=` calls
// true exactly one of them, so the undefined a NaN produces lands on false
// where a negation of the boolean would have put it on true.
bool bronze_rel_lt(uint64_t aBits, uint64_t bBits) {
    return isLessThan(Value(aBits), Value(bBits)) == LessThan::True;
}

bool bronze_rel_gt(uint64_t aBits, uint64_t bBits) {
    return isLessThan(Value(bBits), Value(aBits)) == LessThan::True;
}

bool bronze_rel_le(uint64_t aBits, uint64_t bBits) {
    return isLessThan(Value(bBits), Value(aBits)) == LessThan::False;
}

bool bronze_rel_ge(uint64_t aBits, uint64_t bBits) {
    return isLessThan(Value(aBits), Value(bBits)) == LessThan::False;
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

    // A symbol is loosely equal to the same symbol and to nothing else. 7.2.14
    // reaches that by omission — no step converts a Symbol — so the answer for
    // every mixed pairing is false WITHOUT a conversion, which is what keeps
    // `sym == "Symbol(tag)"` false rather than a TypeError. An object on the
    // other side is the one exception and falls to the ToPrimitive error below.
    if ((a.isSymbol() || b.isSymbol()) && !a.isObject() && !b.isObject()) {
        return aBits == bBits;
    }

    // A boolean operand is ToNumber'd and the comparison restarts, so
    // `true == "1"` becomes `1 == "1"` becomes `1 == 1`.
    if (a.isBool()) return bronze_loose_eq(Value::fromDouble(a.asBool() ? 1.0 : 0.0).rawBits(), bBits);
    if (b.isBool()) return bronze_loose_eq(aBits, Value::fromDouble(b.asBool() ? 1.0 : 0.0).rawBits());

    // Number against string: the STRING is converted, never the number, so
    // `2 == "2.0"` is true and `1 == "1x"` is false.
    if (aNum && b.isString()) return rtToNumber(a) == rtToNumber(b);
    if (a.isString() && bNum) return rtToNumber(a) == rtToNumber(b);

    // What is left is an object against a primitive, which 7.2.14 steps 11-12
    // settle by ToPrimitive'ing the object and restarting the comparison. A
    // primitive WRAPPER is the one object bronze can take that step for — its
    // internal slot is what OrdinaryToPrimitive's `valueOf` call would answer —
    // and it is the step that makes `new String("ab") == "ab"` true where
    // `===` is false, which is the whole observable difference between a
    // wrapper and the primitive it wraps.
    if (Value prim; rtWrapperPrimitive(a, prim)) {
        return bronze_loose_eq(prim.rawBits(), bBits);
    }
    if (Value prim; rtWrapperPrimitive(b, prim)) {
        return bronze_loose_eq(aBits, prim.rawBits());
    }
    // Every other object still needs the algorithm — valueOf then toString,
    // with the primitive test between them — and it is not built. Named rather
    // than guessed at, on the same rule as ToString of an object.
    fatal("'==' between an object and a primitive needs ToPrimitive, which is unsupported");
}

}  // extern "C"

}  // namespace bronze::runtime
