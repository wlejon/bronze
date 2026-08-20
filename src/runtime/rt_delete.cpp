// `delete o.k` and `delete o[i]` (ECMA-262 13.5.1, 10.5.6, 10.4.2.1).
//
// The operator's answer is a BOOLEAN, and it is `true` far more often than
// people expect: for a property that was removed, for one that was never
// there, and for one only a prototype defines — all three are already the
// state delete wants. `false` is reserved for a non-configurable property,
// which `Object.defineProperty`, `Object.seal` and `Object.freeze` create.
//
// In STRICT code that false is not an answer at all: 13.5.1.2 step 5.b makes
// it a TypeError, on the same rule that turns a refused assignment into one.
// `strict` is therefore a parameter here rather than a fact the runtime could
// look up — strictness is a property of the code the operator was WRITTEN in,
// and only the compiler still knows that.
//
// Deleting is not writing `undefined`. `"k" in o` goes false, the key leaves
// every enumeration, and an inherited property it was shadowing becomes
// visible again — none of which a write can do.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/proxy.h"
#include "runtime/namespace.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// The plain object a receiver keeps its NAMED properties in: itself, or a
// function's side object of statics. Null when the receiver has nowhere for one
// to be, which makes the delete a no-op that still answers true.
ObjectHeader* namedPropertyOwner(Value v) {
    if (!v.isObject()) return nullptr;
    HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) return reinterpret_cast<ObjectHeader*>(hdr);
    if (hdr->flags == HeapKind::Function) {
        Value props = v.asObject<FunctionHeader>()->properties;
        return props.isObject() ? props.asObject<ObjectHeader>() : nullptr;
    }
    // A Map or a Set keeps its ordinary properties in a side object too
    // (rt_prop_map.cpp), and `delete m.foo` is the ordinary delete over it.
    // Its ENTRIES are not properties and `delete` never reaches them, which is
    // why there is nothing here about the entry table.
    if (rtIsMapLike(v)) {
        Value props = v.asObject<MapHeader>()->properties;
        return props.isObject() ? props.asObject<ObjectHeader>() : nullptr;
    }
    return nullptr;
}

// `delete a[i]` on an array leaves a HOLE: `length` is a separate own
// property and 10.4.2.1 only intercepts writes to it, so it does not move.
//
// It answers false on exactly one condition — a SEALED array, whose elements
// 7.3.14 made non-configurable — which is the same false a plain object's
// non-configurable property gives, and travels to the same TypeError through
// the same `reportRefusedDelete`. `outIndex` is the index it resolved, which is
// what that message quotes: ToString of a canonical array index is its digits,
// so the number and the string spellings of `delete a[1]` name it the same way.
bool deleteElementByIndex(Value objVal, Value idxVal, uint32_t& outIndex) {
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Array) return true;

    uint32_t& idx = outIndex;
    idx = 0;
    if (idxVal.isNumber()) {
        double d = idxVal.asNumber();
        if (!(d >= 0.0) || d != static_cast<double>(static_cast<uint32_t>(d)) ||
            d > 4294967294.0) {
            return true;  // not a canonical index: no such own property
        }
        idx = static_cast<uint32_t>(d);
    } else if (idxVal.isString()) {
        const StringHeader* s = idxVal.asString<StringHeader>();
        if (!s->isLatin1() ||
            !rtIsIntegerLikeKey(std::string_view(s->latin1Data(), s->getLength()), idx)) {
            return true;
        }
    } else {
        return true;
    }
    // An index that is not an own property — past the end, or already a hole —
    // is in the state delete wants whatever the integrity level says, so a
    // sealed array still answers true for one (10.5.6 step 3).
    auto* arr = reinterpret_cast<ArrayHeader*>(hdr);
    if (!arr->hasElem(idx)) return true;
    if (!rtArrayElementsConfigurable(objVal)) return false;
    arr->deleteElem(idx);
    return true;
}

// `delete a.k` for ANY key, which is the whole of an array's [[Delete]]: an
// element, `length` — non-configurable from birth (10.4.2.2), so the answer is
// FALSE — or a named property in the side object.
//
// One function rather than three arms in two callers, because it is one
// question: which of an array's three kinds of own property does this key name.
// Splitting it is how `delete a.length` came to answer true.
//
// `outKeyText` is what a refusal quotes, and it is the key's ToString spelling
// in every arm — the digits of a canonical index, `length`, or the name.
bool deleteArrayProperty(Rooted<Value>& objRoot, Value keyVal, std::string& outKeyText) {
    // A SYMBOL is already a property key, and an array carries no symbol-keyed
    // property: a symbol-keyed write to one is refused by name
    // (rt_prop_write.cpp), so the key is absent and that is the state delete
    // wants.
    if (keyVal.isSymbol()) {
        outKeyText = "<symbol>";
        return true;
    }
    uint32_t idx = 0;
    if (keyVal.isNumber() || keyVal.isString()) {
        const bool isIndex =
            keyVal.isNumber()
                ? rtValueToElementIndex(keyVal, idx)
                : (keyVal.asString<StringHeader>()->isLatin1() &&
                   rtIsIntegerLikeKey(
                       std::string_view(keyVal.asString<StringHeader>()->latin1Data(),
                                        keyVal.asString<StringHeader>()->getLength()),
                       idx));
        if (isIndex) {
            outKeyText = std::to_string(idx);
            return deleteElementByIndex(objRoot.get(), keyVal, idx);
        }
    }
    // Not an index, so it names a property — through ToString, which allocates,
    // so the array is reached through the root from here on. `a[1.5]` arrives
    // here as the name "1.5", which is what the WRITE stored it under.
    Rooted<Value> key{rtValueToString(keyVal)};
    StringHeader* keyHeader = key.get().asString<StringHeader>();
    outKeyText = rtUtf8Chars(keyHeader);
    if (outKeyText == "length") return false;
    return rtArrayNamedDelete(objRoot.get(), keyHeader);
}

}  // namespace

extern "C" {

// The strict half of 13.5.1.2 step 5.b, in one place because `delete o.k` and
// `delete o[i]` differ only in how they spell the key.
static bool reportRefusedDelete(bool removed, bool strict, const std::string& key) {
    if (removed || !strict) return removed;
    rtThrowTypeError("Cannot delete property '" + key + "': it is not configurable");
    return false;
}

bool bronze_prop_delete(uint64_t objBits, uint32_t keyIndex, bool strict) {
    recordPropCall("bronze_prop_delete", keyIndex, nullptr);
    Value objVal(objBits);
    // ToObject first, exactly as a read does: `delete null.x` is the
    // TypeError of 13.5.1 step 5.
    if (objVal.isNull() || objVal.isUndefined()) {
        rtThrowTypeError("Cannot convert " +
                         std::string(objVal.isNull() ? "null" : "undefined") +
                         " to object (deleting '" + rtKeyString(keyIndex) + "')");
        return true;
    }
    if (!objVal.isObject()) return true;

    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("property delete with an unregistered key index");

    // A key that names an ARRAY ELEMENT reaches the elements, not a shape:
    // `delete a["1"]` and `delete a[1]` are the same property (7.1.19).
    // Everything else is one of the two properties an array has beside them.
    if (objVal.asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
        Rooted<Value> arrRoot{objVal};
        std::string keyText;
        return reportRefusedDelete(
            deleteArrayProperty(arrRoot, Value::fromString(keyHeader), keyText), strict, keyText);
    }

    // 10.4.6.10: deleting an EXPORTED name answers false — a namespace property
    // is non-configurable — and deleting anything else answers true, because it
    // was never there. The `!owner` fall-through below would answer true for
    // both, which is the wrong half of the pair.
    if (rtIsModuleNamespace(objVal)) {
        const bool exported =
            reinterpret_cast<ModuleNamespaceHeader*>(objVal.asObject<HeapObjectHeader>())
                ->indexOf(keyHeader) >= 0;
        return reportRefusedDelete(!exported, strict, rtKeyString(keyIndex));
    }

    // 10.5.10 [[Delete]]: the `deleteProperty` trap, or the target's delete.
    // Returns rather than falling through, because the whole question moves —
    // the proxy has no property table of its own for the tail below to walk.
    if (objVal.asObject<HeapObjectHeader>()->flags == ProxyHeader::kFlags) {
        return rtProxyDelete(objVal, Value::fromString(keyHeader), strict);
    }

    ObjectHeader* owner = namedPropertyOwner(objVal);
    if (!owner) return true;
    return reportRefusedDelete(owner->deleteProperty(rtArena(), keyHeader), strict,
                               rtKeyString(keyIndex));
}

bool bronze_elem_delete(uint64_t objBits, uint64_t idxBits, bool strict) {
    recordElemCall("bronze_elem_delete", objBits, idxBits);
    Value objVal(objBits);
    Value idxVal(idxBits);
    if (objVal.isNull() || objVal.isUndefined()) {
        rtThrowTypeError("Cannot convert " +
                         std::string(objVal.isNull() ? "null" : "undefined") + " to object");
        return true;
    }
    if (!objVal.isObject()) return true;

    // 7.1.19 ToPropertyKey for an OBJECT key, before the receiver's header is
    // read: the conversion is a user `toString` and can collect, so the
    // receiver goes through a root and the helper is re-entered with the
    // primitive key every branch below was written for.
    if (idxVal.isObject()) {
        Rooted<Value> objRoot{objVal};
        Rooted<Value> keyRoot{idxVal};
        keyRoot.set(rtToPropertyKey(keyRoot));
        if (rtExceptionPending()) return true;
        return bronze_elem_delete(objRoot.get().rawBits(), keyRoot.get().rawBits(), strict);
    }

    if (objVal.asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
        Rooted<Value> arrRoot{objVal};
        std::string keyText;
        return reportRefusedDelete(deleteArrayProperty(arrRoot, idxVal, keyText), strict, keyText);
    }

    if (objVal.asObject<HeapObjectHeader>()->flags == ProxyHeader::kFlags) {
        return rtProxyDelete(objVal, idxVal, strict);
    }

    ObjectHeader* owner = namedPropertyOwner(objVal);
    if (!owner) return true;
    // A SYMBOL key is already a property key, so ToPropertyKey is the identity
    // for it — and running ToString on one would throw where `delete o[sym]`
    // must simply remove the property. It also cannot allocate, so the owner
    // pointer taken above is still live.
    if (idxVal.isSymbol()) {
        return reportRefusedDelete(owner->deleteProperty(rtArena(), PropertyKey::fromValue(idxVal)),
                                   strict, "<symbol>");
    }
    // ToPropertyKey allocates the key string, so the owner is reached through
    // a root afterwards rather than through the pointer taken above.
    Rooted<Value> ownerRoot{Value::fromObject(owner)};
    Rooted<Value> key{rtValueToString(idxVal)};
    StringHeader* keyHeader = key.get().asString<StringHeader>();
    return reportRefusedDelete(
        ownerRoot.get().asObject<ObjectHeader>()->deleteProperty(rtArena(), keyHeader), strict,
        rtUtf8Chars(keyHeader));
}

}  // extern "C"

}  // namespace bronze::runtime
