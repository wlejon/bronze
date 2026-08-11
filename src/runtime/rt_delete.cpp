// `delete o.k` and `delete o[i]` (ECMA-262 13.5.1, 10.5.6, 10.4.2.1).
//
// The operator's answer is a BOOLEAN, and it is `true` far more often than
// people expect: for a property that was removed, for one that was never
// there, and for one only a prototype defines — all three are already the
// state delete wants. `false` is reserved for a non-configurable property,
// which bronze cannot yet create, so nothing here returns it. That is a
// gap in what bronze can *express*, not a shortcut: `Object.defineProperty`
// is where non-configurable properties come from, and it is not built.
//
// Deleting is not writing `undefined`. `"k" in o` goes false, the key leaves
// every enumeration, and an inherited property it was shadowing becomes
// visible again — none of which a write can do.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
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

// The plain object a receiver keeps its NAMED properties in: itself, or a
// function's side object of statics (docs/0012 decision 6). Null when the
// receiver has nowhere for one to be, which makes the delete a no-op that
// still answers true.
ObjectHeader* namedPropertyOwner(Value v) {
    if (!v.isObject()) return nullptr;
    HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) return reinterpret_cast<ObjectHeader*>(hdr);
    if (hdr->flags == 2) {
        Value props = v.asObject<FunctionHeader>()->properties;
        return props.isObject() ? props.asObject<ObjectHeader>() : nullptr;
    }
    return nullptr;
}

// `delete a[i]` on an array leaves a HOLE: `length` is a separate own
// property and 10.4.2.1 only intercepts writes to it, so it does not move.
bool deleteElementByIndex(Value objVal, Value idxVal) {
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags != 1) return true;

    uint32_t idx = 0;
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
    reinterpret_cast<ArrayHeader*>(hdr)->deleteElem(idx);
    return true;
}

}  // namespace

extern "C" {

bool bronze_prop_delete(uint64_t objBits, uint32_t keyIndex) {
    Value objVal(objBits);
    // ToObject first, exactly as a read does: `delete null.x` is a TypeError
    // in 13.5.1 step 5, and bronze has no `throw` to raise it with.
    if (objVal.isNull() || objVal.isUndefined()) {
        fatal((std::string("deleting property '") + rtKeyString(keyIndex) + "' of " +
               (objVal.isNull() ? "null" : "undefined"))
                  .c_str());
    }
    if (!objVal.isObject()) return true;

    StringHeader* keyHeader = rtKeyHeader(keyIndex);
    if (!keyHeader) fatal("property delete with an unregistered key index");

    // A key that names an ARRAY ELEMENT reaches the elements, not a shape:
    // `delete a["1"]` and `delete a[1]` are the same property (7.1.19).
    if (objVal.asObject<HeapObjectHeader>()->flags == 1) {
        return deleteElementByIndex(objVal, Value::fromString(keyHeader));
    }

    ObjectHeader* owner = namedPropertyOwner(objVal);
    if (!owner) return true;
    return owner->deleteProperty(rtArena(), keyHeader);
}

bool bronze_elem_delete(uint64_t objBits, uint64_t idxBits) {
    Value objVal(objBits);
    Value idxVal(idxBits);
    if (objVal.isNull() || objVal.isUndefined()) {
        fatal("deleting a computed property of null or undefined");
    }
    if (!objVal.isObject()) return true;

    if (objVal.asObject<HeapObjectHeader>()->flags == 1) {
        return deleteElementByIndex(objVal, idxVal);
    }

    ObjectHeader* owner = namedPropertyOwner(objVal);
    if (!owner) return true;
    // ToPropertyKey allocates the key string, so the owner is reached through
    // a root afterwards rather than through the pointer taken above.
    Rooted<Value> ownerRoot{Value::fromObject(owner)};
    Rooted<Value> key{rtValueToString(idxVal)};
    return ownerRoot.get().asObject<ObjectHeader>()->deleteProperty(
        rtArena(), key.get().asString<StringHeader>());
}

}  // extern "C"

}  // namespace bronze::runtime
