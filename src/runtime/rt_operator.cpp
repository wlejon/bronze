// The operators whose meaning is an ALGORITHM rather than a machine
// instruction: ToInt32, exponentiation, the four relational comparisons,
// abstract (loose) equality, `typeof`, `instanceof` and `in`.
//
// Each of these is a numbered sequence of steps in ECMA-262 that no single
// target instruction implements. ToInt32 is the clearest case — `fptosi` is
// poison for a double outside the int32 range, while the language demands a
// wraparound modulo 2^32 — so the conversion is a call and the arithmetic
// around it is not.
//
// The comparisons are the ones that run USER CODE. 13.10.1 step 1 and 7.2.14
// steps 11-12 both call ToPrimitive, so `a < b` and `a == b` can collect and can
// throw — which is why both are written over rooted operands and why the `==`
// restart the spec spells as recursion is a loop over two root slots here.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/env.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/namespace.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/proxy.h"
#include "runtime/regexp.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"
#include "runtime/weak_ref.h"

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
constexpr int kTypeOfCount = 8;
thread_local Value g_typeofStrings[kTypeOfCount] = {};
thread_local bool g_typeofReady = false;

enum TypeOfKind { kUndefined, kObject, kBoolean, kNumber, kString, kFunction, kSymbol, kBigInt };

Value typeofString(TypeOfKind kind) {
    if (!g_typeofReady) {
        static const char* const kNames[kTypeOfCount] = {
            "undefined", "object", "boolean", "number", "string", "function", "symbol", "bigint"};
        for (int i = 0; i < kTypeOfCount; ++i) {
            g_typeofStrings[i] = rtMakeString(kNames[i]);
            rtHeap().add_permanent_root(&g_typeofStrings[i]);
        }
        g_typeofReady = true;
    }
    return g_typeofStrings[kind];
}

// ECMA-262 7.2.3 IsCallable, which `typeof`, `instanceof` and @@hasInstance
// all ask. A function object — or a Proxy whose target was callable when it
// was created, which 10.5.14 makes callable in exactly the same sense, and
// which is why the question lives beside the proxy (runtime/proxy.h).
bool isCallable(Value v) {
    return rtIsCallableValue(v);
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
static_assert(HeapKind::Count == 18,
              "a HeapKind was added or removed: give `in` an arm for it in the two switches "
              "below, or refuse it there by name. A kind with no arm used to fall through to "
              "a cast that read its payload's first word as a Shape*.");

// The kinds no program can be holding, so that reaching one is a lowering bug
// and not something a program did — the same answer the property read path
// gives them (rt_prop.cpp).
[[noreturn]] void refuseInternalKind(uint16_t kind) {
    if (kind == EnvHeader::kFlags) fatal("internal: 'in' on an environment record");
    if (kind == MapHeader::kPrivateFlags) {
        fatal("internal: 'in' on a private-element table");
    }
    fatal("internal: 'in' on an iteration record");
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
            // the prototype — 24.3.3.6 and 24.4.3.5 for the weak pair;
            // 10.4.6.1 puts it on the namespace itself, which is the one of
            // these that is an OWN property.
            case HeapKind::Map:
            case HeapKind::Set:
            case HeapKind::WeakMap:
            case HeapKind::WeakSet:
            case HeapKind::TypedArray:
            case HeapKind::ArrayBuffer:
            case HeapKind::DataView:
            case HeapKind::ModuleNamespace:
                return true;
            // 26.1.3.3 and 26.2.3.3 put one on each prototype, which is the
            // only reason `Object.prototype.toString.call(wr)` reads "[object
            // WeakRef]" — 20.1.3.6's builtin-tag list has no entry for either,
            // so step 14 would have said "Object" without it.
            case HeapKind::WeakRef:
            case HeapKind::FinalizationRegistry:
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
        // An ArrayBuffer, a DataView, a RegExp, a WeakMap and a WeakSet are
        // not iterable — non-iterability is half of what makes the weak pair
        // weak — and a module namespace exports no name that could be one
        // (10.4.6.4 is "is this an export", and @@toStringTag above is its
        // only other key).
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
        case HeapKind::WeakMap:
        case HeapKind::WeakSet:
        case HeapKind::RegExp:
        case HeapKind::ModuleNamespace:
        case HeapKind::WeakRef:
        case HeapKind::FinalizationRegistry:
            return shapelessHasSymbol(kind, key);
        case HeapKind::Proxy:
            // 10.5.7: the handler's question, or the target's — either way the
            // whole question moves, so this arm returns rather than falling
            // out to a walk of the proxy's own (nonexistent) shape.
            return rtProxyHas(objRoot.get(), key);
        case HeapKind::Iterator:
        case HeapKind::Env:
        case HeapKind::PrivateTable:
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
//
// An arm that finds nothing FALLS OUT of the switch rather than returning
// false, because the chain does not end at a member table: every one of these
// receivers inherits from `Object.prototype`, and `'hasOwnProperty' in f` read
// false while `f.toString` — a member of the prototype one link nearer — was
// already a named error. The two returning arms are the two whose chain really
// does end where they say: a module namespace has a null [[Prototype]]
// (10.4.6.1), and a plain object was walking the whole chain already.
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
            if (rtIsIntegerLikeKey(key, index) && arr->hasElem(index)) return true;
            // A named own property — one a program wrote, or a match array's
            // `index`. Asked of the same storage the READ answers from, so `in`
            // and `a.k` cannot disagree; the string allocates, so the header
            // above is dead from here and the root is what is asked.
            {
                Rooted<Value> keyStr{rtMakeString(key)};
                PropertyInfo info;
                if (rtArrayOwnNamed(objRoot.get(), keyStr.get().asString<StringHeader>(), info)) {
                    return true;
                }
            }
            // `Array.prototype`'s members, which an array answers BESIDE the
            // value rather than off a prototype object on its chain — so this
            // table is what stands in for that object, exactly as the typed
            // array's and the Map's do below. Without it `'push' in a` was
            // false while `a.push` was a function: one question with two
            // answers, which is what every arm of this switch exists to
            // prevent. A name 23.1.3 defines and bronze has NOT built answers
            // true here where a read of it is a named hard error, which is the
            // split every other arm makes: the member exists, and its value is
            // what bronze has not got.
            if (rtArrayHasMember(key)) return true;
            break;
        }
        case HeapKind::TypedArray: {
            // The index first, and against the LENGTH: 10.4.5.2 makes a
            // canonical numeric string outside the range absent rather than
            // INHERITED — so this arm returns for an index either way and is
            // the one place the fall-through below must not be reached from.
            auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
            if (rtIsIntegerLikeKey(key, index)) return index < view->length;
            if (rtTypedArrayHasMember(view->kindName(), key)) return true;
            break;
        }
        case HeapKind::ArrayBuffer:
            if (rtArrayBufferHasMember(reinterpret_cast<ArrayBufferHeader*>(hdr)->isShared(),
                                       key)) {
                return true;
            }
            break;
        // A DataView's members all live on its prototype, which bronze answers
        // on the property path — so `in`, which walks the chain, must ask the
        // same table the reads come from rather than report the object empty.
        case HeapKind::DataView:
            if (rtDataViewHasMember(key)) return true;
            break;
        // The collections answer from two places, and `in` has to ask both:
        // an ORDINARY own property a program assigned (24.1.4 leaves a Map an
        // ordinary object), then the members 24.1.3 and its siblings put on the
        // prototype. An entry is neither — `m.set("k", 1)` does not make
        // `"k" in m` true, and that is the language and not a gap.
        case HeapKind::Map:
        case HeapKind::Set:
        case HeapKind::WeakMap:
        case HeapKind::WeakSet: {
            Rooted<Value> keyStr{rtMakeString(key)};
            if (PropertyInfo info;
                rtMapOwnNamed(objRoot.get(), keyStr.get().asString<StringHeader>(), info)) {
                return true;
            }
            const uint16_t k = objRoot.get().asObject<HeapObjectHeader>()->flags;
            if (k == HeapKind::Map || k == HeapKind::Set) {
                if (rtMapHasMember(k == HeapKind::Set, key)) return true;
            } else if (rtWeakCollectionHasMember(k == HeapKind::WeakSet, key)) {
                return true;
            }
            break;
        }
        case HeapKind::RegExp:
            if (rtRegExpHasMember(key)) return true;
            break;
        // Both answer from the one member table their READ answers from, so
        // `'deref' in wr` and `wr.deref` cannot disagree.
        case HeapKind::WeakRef:
        case HeapKind::FinalizationRegistry:
            if (rtWeakRefHasMember(objRoot.get(), key)) return true;
            break;
        case HeapKind::ModuleNamespace: {
            // 10.4.6.4 [[HasProperty]] is exactly "is this an export name":
            // [[Prototype]] is null (10.4.6.1), so nothing else can be true.
            // The last allocation, and the header is re-derived through the
            // root afterwards.
            Rooted<Value> keyStr{rtMakeString(key)};
            return rtModuleNamespaceHasExport(objRoot.get(),
                                              keyStr.get().asString<StringHeader>());
        }
        case HeapKind::Proxy: {
            // 10.5.7: the handler's question, or the target's. Returns rather
            // than falling out, because forwarding re-asks the WHOLE question
            // of the target — Object.prototype tail included.
            Rooted<Value> keyStr{rtMakeString(key)};
            return rtProxyHas(objRoot.get(), keyStr.get());
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
            if (props.isObject() && plainObjectHas(props.asObject<ObjectHeader>(),
                                                   keyStr.get().asString<StringHeader>())) {
                return true;
            }
            break;
        }
        case HeapKind::Plain: {
            if (Value data; rtStringWrapperData(objRoot.get(), data)) {
                if (rtStringDataHasOwnKey(data, key)) return true;
            }
            Rooted<Value> keyStr{rtMakeString(key)};
            auto* holder =
                reinterpret_cast<ObjectHeader*>(objRoot.get().asObject<HeapObjectHeader>());
            return plainObjectHas(holder, keyStr.get().asString<StringHeader>());
        }
        case HeapKind::Iterator:
        case HeapKind::Env:
        case HeapKind::PrivateTable:
            refuseInternalKind(hdr->flags);
        default:
            fatal((std::string("internal: 'in' on ") + rtObjectKindName(objRoot.get()) +
                   ", a heap kind the operator has no arm for")
                      .c_str());
    }
    // The rest of the chain, for every arm that fell out of the switch. Reached
    // only after that receiver's own table has both answered and refused, so a
    // member the prototype it stands in for defines still shadows this step —
    // the same order the READ path takes (builtin_object_proto.cpp says why
    // skipping the unbuilt intermediate is exact rather than approximate).
    return rtObjectProtoHasMember(key);
}

// ECMA-262 13.10.1 IsLessThan, whose result is a Boolean **or undefined**.
// The third answer is the whole reason the four operators are not one compare
// and its negation: 13.10 maps undefined to false for every one of them, while
// `!` maps it to true for two of them.
enum class LessThan { False, True, Undefined };

// `x < y` in the spec's own terms.
//
// `leftFirst` is 13.10.1's own flag, and it is observable now that step 1 runs
// user code: `a > b` converts `a` before `b` even though it asks
// IsLessThan(b, a), so the operand order of the two `valueOf` calls follows the
// SOURCE and not the argument list. Both operands are rooted before either
// conversion runs, because the second call can collect and move what the first
// returned — the two-object comparison is where that shows up.
//
// Step 4 is ToNumeric rather than ToNumber, which is the seam a BigInt lands on:
// its else-branch compares a mathematical value, not a double, so the two
// conversions below become one numeric-kind dispatch rather than growing a case.
LessThan isLessThan(Rooted<Value>& x, Rooted<Value>& y, bool leftFirst) {
    // Two numbers convert to themselves, so the whole algorithm collapses to
    // the compare — and this is the shape inference-typed code that stayed
    // boxed arrives in.
    if (x.get().isNumber() && y.get().isNumber()) {
        const double nx = x.get().asNumber();
        const double ny = y.get().asNumber();
        if (std::isnan(nx) || std::isnan(ny)) return LessThan::Undefined;
        return nx < ny ? LessThan::True : LessThan::False;
    }
    Rooted<Value> px{Value::fromUndefined()};
    Rooted<Value> py{Value::fromUndefined()};
    if (leftFirst) {
        px.set(rtToPrimitive(x, ToPrimitiveHint::Number));
        if (rtExceptionPending()) return LessThan::Undefined;
        py.set(rtToPrimitive(y, ToPrimitiveHint::Number));
    } else {
        py.set(rtToPrimitive(y, ToPrimitiveHint::Number));
        if (rtExceptionPending()) return LessThan::Undefined;
        px.set(rtToPrimitive(x, ToPrimitiveHint::Number));
    }
    if (rtExceptionPending()) return LessThan::Undefined;
    // Step 3: both Strings, compared by code unit with NOTHING converted. It
    // comes before ToNumeric, which is why `"2" < "10"` is true where
    // `2 < 10` is false — the digits are never read as digits.
    if (px.get().isString() && py.get().isString()) {
        return px.get().asString<StringHeader>()->lessThan(*py.get().asString<StringHeader>())
                   ? LessThan::True
                   : LessThan::False;
    }
    // Steps 3.c and 3.d: a BigInt against a STRING parses the string with
    // StringToBigInt rather than with ToNumber, and a string that is not a
    // StringIntegerLiteral makes the whole comparison *undefined* — so
    // `1n < "2"` is true and `1n < "2x"` is false for every one of the four
    // operators. These come before step 4 for the same reason step 3 does.
    if (px.get().isBigInt() && py.get().isString()) {
        BigNum rhs;
        if (!rtStringToBigInt(rtAsciiChars(py.get().asString<StringHeader>()), rhs)) {
            return LessThan::Undefined;
        }
        return BigNum::compare(rtBigIntValue(px.get()), rhs) < 0 ? LessThan::True
                                                                 : LessThan::False;
    }
    if (px.get().isString() && py.get().isBigInt()) {
        BigNum lhs;
        if (!rtStringToBigInt(rtAsciiChars(px.get().asString<StringHeader>()), lhs)) {
            return LessThan::Undefined;
        }
        return BigNum::compare(lhs, rtBigIntValue(py.get())) < 0 ? LessThan::True
                                                                 : LessThan::False;
    }
    if (px.get().isBigInt() && py.get().isBigInt()) {
        return BigNum::compare(rtBigIntValue(px.get()), rtBigIntValue(py.get())) < 0
                   ? LessThan::True
                   : LessThan::False;
    }
    // Step 4's mixed BigInt/Number arm, which compares MATHEMATICAL values —
    // the one place the two numeric types meet without an error. It is exact by
    // construction: `9007199254740993n < 9007199254740992` is false and
    // `9007199254740993n > 9007199254740992` is true, which converting either
    // side to the other's type could not both give.
    if (px.get().isBigInt() || py.get().isBigInt()) {
        const bool leftIsBig = px.get().isBigInt();
        const double other = rtToNumber(leftIsBig ? py.get() : px.get());
        if (rtExceptionPending()) return LessThan::Undefined;
        const int order = leftIsBig ? rtCompareBigIntWithNumber(px.get(), other)
                                    : -rtCompareBigIntWithNumber(py.get(), other);
        if (order == BigNum::kUnordered || order == -BigNum::kUnordered) {
            return LessThan::Undefined;
        }
        return order < 0 ? LessThan::True : LessThan::False;
    }
    // Step 4, the else-branch: ToNumeric on both, and step 4.c's undefined for
    // a NaN on either side — which includes the case where one operand is a
    // string that does not parse as a number. Both operands are primitive by
    // now, so neither conversion can allocate; a Symbol is the one that raises.
    const double nx = rtToNumber(px.get());
    if (rtExceptionPending()) return LessThan::Undefined;
    const double ny = rtToNumber(py.get());
    if (rtExceptionPending()) return LessThan::Undefined;
    if (std::isnan(nx) || std::isnan(ny)) return LessThan::Undefined;
    return nx < ny ? LessThan::True : LessThan::False;
}

// ECMA-262 7.2.14, IsLooselyEqual, in the order the spec states it. The
// order is the specification: `null == 0` is false only because step 2
// answers before any ToNumber can run, and reordering the coercions would
// make it true.
//
// The RESTART the spec writes as recursion is a loop over two roots here, and
// that is not a stylistic choice. Steps 9-12 each convert one operand and ask
// the question again; a conversion can be a user `valueOf` and so can collect,
// which would leave the operand it did NOT convert stale in any recursion that
// passed raw bits. Two rooted slots, rewritten in place, is the shape that
// survives. Each pass strictly reduces what is left to convert — an object
// becomes a primitive, a boolean becomes a number — so the bound below is a
// tripwire for a step that failed to make progress, not a real limit.
bool looseEq(Rooted<Value>& aRoot, Rooted<Value>& bRoot) {
    for (int pass = 0; pass < 8; ++pass) {
        const Value a = aRoot.get();
        const Value b = bRoot.get();

        const bool aNullish = a.isNull() || a.isUndefined() || a.isHole();
        const bool bNullish = b.isNull() || b.isUndefined() || b.isHole();
        // null and undefined are loosely equal to each other and to NOTHING
        // else — not to 0, not to false, not to "". This is checked before any
        // conversion, which is exactly what makes that true.
        if (aNullish || bNullish) return aNullish && bNullish;

        const bool aNum = a.isNumber() || a.isInt32();
        const bool bNum = b.isNumber() || b.isInt32();

        // Same type: strict equality, NaN and signed zero included. The two
        // numeric tags are one type here, which is the seam a BigInt does NOT
        // join: 7.2.14 keeps Number and BigInt distinct and compares their
        // mathematical values in steps 6-8, so that case needs its own arm
        // rather than a wider `aNum`.
        if (aNum && bNum) return rtToNumber(a) == rtToNumber(b);
        if (a.isString() && b.isString()) {
            return a.asString<StringHeader>()->equals(*b.asString<StringHeader>());
        }
        if (a.isBool() && b.isBool()) return a.asBool() == b.asBool();
        if (a.isObject() && b.isObject()) return a.rawBits() == b.rawBits();
        // Step 1 for two BigInts, which is BigInt::equal — a value compare, not
        // the pointer compare `rawBits` would be.
        if (a.isBigInt() && b.isBigInt()) {
            return BigNum::compare(rtBigIntValue(a), rtBigIntValue(b)) == 0;
        }
        // Steps 7 and 8: a BigInt against a String parses the string with
        // StringToBigInt, and a string that is not one makes the answer FALSE
        // rather than an error. `0n == ""` is true, because StringToBigInt of
        // the empty string is 0n.
        if ((a.isBigInt() && b.isString()) || (a.isString() && b.isBigInt())) {
            const Value bigVal = a.isBigInt() ? a : b;
            const Value strVal = a.isBigInt() ? b : a;
            BigNum parsed;
            if (!rtStringToBigInt(rtAsciiChars(strVal.asString<StringHeader>()), parsed)) {
                return false;
            }
            return BigNum::compare(rtBigIntValue(bigVal), parsed) == 0;
        }

        // A symbol is loosely equal to the same symbol and to nothing else.
        // 7.2.14 reaches that by omission — no step converts a Symbol — so the
        // answer for every mixed pairing is false WITHOUT a conversion, which
        // keeps `sym == "Symbol(tag)"` false rather than a TypeError. An object
        // on the other side is the one exception: steps 11 and 12 name Symbol
        // among the types whose object counterpart is ToPrimitive'd, so
        // `sym == { [Symbol.toPrimitive]: () => sym }` is true.
        if ((a.isSymbol() || b.isSymbol()) && !a.isObject() && !b.isObject()) {
            return a.rawBits() == b.rawBits();
        }

        // A boolean operand is ToNumber'd and the comparison restarts, so
        // `true == "1"` becomes `1 == "1"` becomes `1 == 1`.
        if (a.isBool()) {
            aRoot.set(Value::fromDouble(a.asBool() ? 1.0 : 0.0));
            continue;
        }
        if (b.isBool()) {
            bRoot.set(Value::fromDouble(b.asBool() ? 1.0 : 0.0));
            continue;
        }

        // Number against string: the STRING is converted, never the number, so
        // `2 == "2.0"` is true and `1 == "1x"` is false.
        if (aNum && b.isString()) return rtToNumber(a) == rtToNumber(b);
        if (a.isString() && bNum) return rtToNumber(a) == rtToNumber(b);

        // Step 13: a BigInt against a Number, compared as MATHEMATICAL values
        // with no conversion in either direction. A non-finite Number is never
        // equal to any BigInt (step 13.a), which the exact comparison already
        // answers — an infinity is ordered against every magnitude and a NaN is
        // unordered against all of them.
        if ((a.isBigInt() && bNum) || (aNum && b.isBigInt())) {
            const Value bigVal = a.isBigInt() ? a : b;
            const Value numVal = a.isBigInt() ? b : a;
            return rtCompareBigIntWithNumber(bigVal, rtToNumber(numVal)) == 0;
        }

        // Steps 11 and 12: an object against a primitive is ToPrimitive'd with
        // NO hint and the comparison restarts. Hint default is what makes
        // `[1] == 1` true — `valueOf` is asked first, answers the array itself,
        // and `toString` then gives "1" — and it is also what makes
        // `new String("ab") == "ab"` true where `===` is false, which is the
        // whole observable difference between a wrapper and what it wraps.
        if (b.isObject()) {
            bRoot.set(rtToPrimitive(bRoot, ToPrimitiveHint::Default));
            if (rtExceptionPending()) return false;
            continue;
        }
        if (a.isObject()) {
            aRoot.set(rtToPrimitive(aRoot, ToPrimitiveHint::Default));
            if (rtExceptionPending()) return false;
            continue;
        }
        // Step 13: two primitives of types no step above paired up.
        return false;
    }
    fatal("internal: '==' failed to reach an answer (7.2.14's restarts must strictly reduce "
          "what is left to convert, and one of them did not)");
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

bool rtOrdinaryHasInstance(Value ctor, Value obj) {
    if (!obj.isObject()) return false;

    Rooted<Value> objRoot{obj};
    Rooted<Value> ctorRoot{ctor};
    Rooted<Value> protoRoot{Value::fromUndefined()};

    if (ctorRoot.get().rawBits() == rtObjectNamespace().rawBits()) {
        protoRoot.set(rtObjectPrototype());
    } else {
        if (!isCallable(ctorRoot.get())) return false;
        if (rtIsArrayConstructor(ctorRoot.get())) {
            return objRoot.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array;
        }
        // A Map and a Set are the same case as an array and were missing from
        // it only because nothing could produce one whose chain a walk would
        // find: `new (class extends Map)() instanceof Map` has to be true, and
        // the kind IS the answer, since the intrinsic has no prototype OBJECT
        // for the walk below to compare against.
        if (const char* name = rtMapConstructorName(ctorRoot.get())) {
            const uint16_t flags = objRoot.get().asObject<HeapObjectHeader>()->flags;
            return flags == (std::strcmp(name, "Set") == 0 ? MapHeader::kSetFlags
                                                           : MapHeader::kMapFlags);
        }
        // 25.2 gives a SharedArrayBuffer its own prototype, so it is NOT an
        // `instanceof ArrayBuffer` -- the two brands share a header in bronze and
        // nothing else.
        if (rtSharedArrayBufferConstructorName(ctorRoot.get())) {
            auto* sabHdr = objRoot.get().asObject<HeapObjectHeader>();
            return sabHdr->flags == ArrayBufferHeader::kFlags &&
                   reinterpret_cast<ArrayBufferHeader*>(sabHdr)->isShared();
        }
        if (const char* name = rtTypedArrayConstructorName(ctorRoot.get())) {
            const uint16_t flags = objRoot.get().asObject<HeapObjectHeader>()->flags;
            if (std::strcmp(name, "ArrayBuffer") == 0) {
                return flags == ArrayBufferHeader::kFlags &&
                       !objRoot.get().asObject<ArrayBufferHeader>()->isShared();
            }
            if (flags == TypedArrayHeader::kFlags) {
                auto* view = objRoot.get().asObject<TypedArrayHeader>();
                return std::strcmp(view->kindName(), name) == 0;
            }
            return false;
        }
        if (rtDataViewConstructorName(ctorRoot.get())) {
            return objRoot.get().asObject<HeapObjectHeader>()->flags == DataViewHeader::kFlags;
        }
        if (rtIsFunctionConstructor(ctorRoot.get())) {
            return objRoot.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function;
        }

        rtEnsureFunctionPrototype(ctorRoot);
        protoRoot.set(ctorRoot.get().asObject<FunctionHeader>()->prototype);
        if (!protoRoot.get().isObject()) return false;
    }

    if (objRoot.get().asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
        if (protoRoot.get().rawBits() == rtObjectPrototype().rawBits()) {
            return true;
        }
        if (objRoot.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function &&
            protoRoot.get().rawBits() == rtFunctionPrototypeObject().rawBits()) {
            return true;
        }
        // A SUBCLASS instance of an exotic kind does have a chain, on the box
        // holding its ordinary half (runtime/native_base.h) — so `sub
        // instanceof MySubArray` walks it here rather than answering the false
        // that every shapeless receiver used to get. An instance that is not a
        // subclass has no box and falls straight through, unchanged.
        Rooted<Value> chain{rtExoticSubclassPrototype(objRoot.get())};
        for (uint32_t depth = 0;
             chain.get().isObject() && depth < ObjectHeader::kMaxPrototypeDepth; ++depth) {
            if (protoRoot.get().rawBits() == chain.get().rawBits()) return true;
            ObjectHeader* next = chain.get().asObject<ObjectHeader>()->protoAncestor(1);
            if (!next) break;
            chain.set(Value::fromObject(next));
        }
        return false;  // arrays, functions, typed arrays
    }

    // The walk compares OBJECT IDENTITY at each link, which is why the
    // prototype has to be materialized above: a constructor whose
    // `.prototype` was never read has no object yet, and creating a
    // different one per test would answer false for its own instances.
    // Nothing in the walk allocates, so this pointer stays valid.
    auto* cur = reinterpret_cast<ObjectHeader*>(objRoot.get().asObject<HeapObjectHeader>());
    for (uint32_t depth = 0; depth <= 1000; ++depth) {
        ObjectHeader* next = cur->protoAncestor(1);
        if (!next) return false;
        if (protoRoot.get().rawBits() == Value::fromObject(next).rawBits()) return true;
        cur = next;
    }
    fatal("prototype chain too deep (a cycle?)");
}

extern "C" {

int32_t bronze_to_int32_f64(double d) { return toInt32(d); }

int32_t bronze_to_int32(uint64_t bits) {
    // ToNumber first (7.1.6 step 1), which is where a string operand is parsed —
    // `"12" & 10` is 8, not NaN-and-therefore-0 — and where an object runs
    // ToPrimitive, so `({valueOf: () => 5}) | 0` is 5. Only the boxed form of
    // `to.int32` reaches here, and `il::canThrow` marks exactly that form.
    return toInt32(rtToNumber(Value(bits)));
}

double bronze_pow(double base, double exponent) {
    recordHelperCall("bronze_pow");
    return rtExponentiate(base, exponent);
}

uint64_t bronze_typeof(uint64_t bits) {
    recordHelperCall("bronze_typeof");
    Value v(bits);
    // `null` first, and reported as "object": ECMA-262's oldest wart, kept
    // because every engine keeps it and programs test for it.
    if (v.isNull()) return typeofString(kObject).rawBits();
    if (v.isUndefined() || v.isHole()) return typeofString(kUndefined).rawBits();
    if (v.isBool()) return typeofString(kBoolean).rawBits();
    if (v.isNumber() || v.isInt32()) return typeofString(kNumber).rawBits();
    if (v.isString()) return typeofString(kString).rawBits();
    if (v.isSymbol()) return typeofString(kSymbol).rawBits();
    // 13.5.3's table gained a row with the type: "bigint", and it is the whole
    // reason a BigInt is its own TAG rather than a heap kind under Tag::Object
    // — this question has to be answerable from the bits.
    if (v.isBigInt()) return typeofString(kBigInt).rawBits();
    if (isCallable(v)) return typeofString(kFunction).rawBits();
    return typeofString(kObject).rawBits();
}

bool bronze_instanceof(uint64_t objBits, uint64_t ctorBits) {
    recordHelperCall("bronze_instanceof");
    Rooted<Value> objRoot{Value(objBits)};
    Rooted<Value> ctorRoot{Value(ctorBits)};

    if (!ctorRoot.get().isObject()) {
        rtThrowTypeError("Right-hand side of 'instanceof' is not an object");
        return false;
    }

    Rooted<Value> hasInstKey{Value::fromSymbol(rtSymbolHasInstance())};
    Value handler(bronze_elem_get(ctorRoot.get().rawBits(), hasInstKey.get().rawBits()));
    if (rtExceptionPending()) return false;

    if (!handler.isNull() && !handler.isUndefined()) {
        if (!isCallable(handler)) {
            rtThrowTypeError("Symbol.hasInstance is not a function");
            return false;
        }
        uint64_t arg = objRoot.get().rawBits();
        uint64_t res = bronze_dynamic_call(handler.rawBits(), ctorRoot.get().rawBits(), 1, &arg);
        if (rtExceptionPending()) return false;
        return bronze_truthy(res);
    }

    if (ctorRoot.get().rawBits() == rtObjectNamespace().rawBits()) {
        return rtOrdinaryHasInstance(ctorRoot.get(), objRoot.get());
    }

    if (!isCallable(ctorRoot.get())) {
        rtThrowTypeError("Right-hand side of 'instanceof' is not callable");
        return false;
    }
    return rtOrdinaryHasInstance(ctorRoot.get(), objRoot.get());
}

bool bronze_has_property(uint64_t keyBits, uint64_t objBits) {
    recordHelperCall("bronze_has_property");
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
//
// The LeftFirst argument is what keeps the SWAPPED pair honest: `a > b` hands
// IsLessThan (b, a) and false, so the conversions still run on `a` first, which
// is the order a program with a `valueOf` on both sides can watch.
bool bronze_rel_lt(uint64_t aBits, uint64_t bBits) {
    recordHelperCall("bronze_rel_lt");
    Rooted<Value> a{Value(aBits)};
    Rooted<Value> b{Value(bBits)};
    return isLessThan(a, b, /*leftFirst=*/true) == LessThan::True;
}

bool bronze_rel_gt(uint64_t aBits, uint64_t bBits) {
    recordHelperCall("bronze_rel_gt");
    Rooted<Value> a{Value(aBits)};
    Rooted<Value> b{Value(bBits)};
    return isLessThan(b, a, /*leftFirst=*/false) == LessThan::True;
}

bool bronze_rel_le(uint64_t aBits, uint64_t bBits) {
    recordHelperCall("bronze_rel_le");
    Rooted<Value> a{Value(aBits)};
    Rooted<Value> b{Value(bBits)};
    return isLessThan(b, a, /*leftFirst=*/false) == LessThan::False;
}

bool bronze_rel_ge(uint64_t aBits, uint64_t bBits) {
    recordHelperCall("bronze_rel_ge");
    Rooted<Value> a{Value(aBits)};
    Rooted<Value> b{Value(bBits)};
    return isLessThan(a, b, /*leftFirst=*/true) == LessThan::False;
}

bool bronze_loose_eq(uint64_t aBits, uint64_t bBits) {
    recordHelperCall("bronze_loose_eq");
    Rooted<Value> a{Value(aBits)};
    Rooted<Value> b{Value(bBits)};
    return looseEq(a, b);
}

}  // extern "C"

}  // namespace bronze::runtime
