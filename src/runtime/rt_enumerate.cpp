// `for-in`, as a snapshot of the keys it will visit.
//
// The list is built ONCE, before the first iteration, and the loop then walks
// it like any other array. That is what makes the loop itself nothing more
// than for-of's index walk, and it is also the answer to the one question
// ECMA-262 deliberately leaves open: a property added during the enumeration
// may or may not be visited (14.7.5.6 note), so bronze visits the keys that
// existed when the loop began and says so.
//
// What the walk collects is own AND INHERITED enumerable string keys, level by
// level from the receiver up its prototype chain, each key visited once even
// where several levels define it. Per level the order is own-enumerable order:
// integer -like keys ascending, then the rest in insertion order.

#include <charconv>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/namespace.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// A cycle in a prototype chain would hang the enumeration rather than crash
// it, exactly as the property path's own walk would; bounded, and named.
constexpr uint32_t kMaxPrototypeDepth = 1000;

bool alreadySeen(const std::vector<StringHeader*>& seen, const StringHeader* key) {
    for (const StringHeader* s : seen) {
        if (s->equals(*key)) return true;
    }
    return false;
}

// The receivers whose own enumerable properties are their INDICES and nothing
// else: an array, a typed array and a string. `length` is not among them —
// it is a non-enumerable own property in the language, and bronze stores it
// outside the shape system entirely, so it could not be enumerated by
// accident. Returns false for anything that is not one of the three.
bool indexedLength(Value v, uint32_t& outLength) {
    if (v.isString()) {
        outLength = v.asString<StringHeader>()->getLength();
        return true;
    }
    if (!v.isObject()) return false;
    if (Value data; rtStringWrapperData(v, data)) {
        outLength = data.asString<StringHeader>()->getLength();
        return true;
    }
    HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags == HeapKind::Array) {
        outLength = reinterpret_cast<ArrayHeader*>(hdr)->length;
        return true;
    }
    if (hdr->flags == TypedArrayHeader::kFlags) {
        outLength = reinterpret_cast<TypedArrayHeader*>(hdr)->length;
        return true;
    }
    return false;
}

// The plain object a receiver's named properties live on: itself, or — for a
// function — its own-property object, which is where a static member is stored
// and whose prototype `extends` linked to the base's. Null when the receiver
// keeps no named properties at all.
ObjectHeader* namedPropertyHolder(Value v) {
    if (!v.isObject()) return nullptr;
    HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        return reinterpret_cast<ObjectHeader*>(hdr);
    }
    if (hdr->flags == HeapKind::Function) {
        Value props = v.asObject<FunctionHeader>()->properties;
        return props.isObject() ? props.asObject<ObjectHeader>() : nullptr;
    }
    // A Map or a Set, whose ordinary properties live in a side object of the
    // same shape (rt_prop_map.cpp). Its ENTRIES are not properties, so a
    // `for-in` over a Map visits what a program assigned to it and nothing
    // else — which is exactly what 24.1 says it should.
    if (rtIsMapLike(v)) {
        Value props = v.asObject<MapHeader>()->properties;
        return props.isObject() ? props.asObject<ObjectHeader>() : nullptr;
    }
    return nullptr;
}

// An empty array, which is what a nullish receiver enumerates: ECMA-262
// 14.7.5.5 returns an empty completion for null and undefined rather than
// throwing, and a program that writes `for (const k in maybe)` depends on it.
uint64_t emptyKeyArray() {
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), 4);
    arr->length = 0;
    return Value::fromObject(arr).rawBits();
}

}  // namespace

extern "C" {

uint64_t bronze_for_in_keys(uint64_t objBits) {
    recordHelperCall("bronze_for_in_keys");
    Value v(objBits);
    if (v.isNull() || v.isUndefined()) return emptyKeyArray();

    uint32_t indexCount = 0;
    if (indexedLength(v, indexCount)) {
        // Nothing in this branch reads a shape, so nothing allocated below can
        // invalidate it; the source is rooted anyway, because the array of
        // digit strings is built one allocation at a time.
        Rooted<Value> src{v};
        // Which of the three this receiver is, asked once. An array is the only
        // one that can have a HOLE — an index a `delete` turned into one is no
        // longer an own property, so the enumeration skips it (14.7.5.6 visits
        // own keys, and a hole is not one), and the result is therefore not
        // simply `0..indexCount-1`, which is why the length is left to the
        // writes below rather than set up front. It is also the only one that
        // can have a NAMED own property, which the tail below appends.
        //
        // Asked BEFORE the allocation below, and of the ROOT: a plain `v`
        // read afterwards is a pointer into dead from-space, which under
        // --gc-stress reported every string as an array and every array as
        // something else on the very first case that had a hole in it.
        const bool isArray =
            src.get().isObject() &&
            src.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array;
        Rooted<Value> out{Value::fromObject(
            ArrayHeader::create(rtHeap(), indexCount ? indexCount : 4))};
        uint32_t at = 0;
        for (uint32_t i = 0; i < indexCount; ++i) {
            if (isArray && !src.get().asObject<ArrayHeader>()->hasElem(i)) continue;
            char buf[16];
            auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), i);
            Rooted<Value> key{Value::fromString(
                StringHeader::createFromUTF8(rtHeap(), std::string_view(buf, end - buf)))};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        // An array's own named properties, AFTER its indices — 6.1.7.1's order,
        // which needs no sort here because an integer-like key names an element
        // and can never have reached the named storage. A string and a typed
        // array have none, and the prototype step below is skipped for all
        // three: `Array.prototype`'s members are answered beside the value
        // rather than by an object with enumerable properties, and 23.2.3 and
        // 22.1.3 put nothing enumerable on the other two either.
        if (isArray) {
            // The keys are arena-interned and immortal, so the vector survives
            // the allocations the copy below makes.
            for (StringHeader* named : rtArrayOwnNamedKeys(src.get())) {
                Rooted<Value> copy{rtCopyKeyToHeap(named)};
                out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, copy);
            }
        }
        return out.get().rawBits();
    }

    // A module namespace has no prototype at all (10.4.6.1), so a `for-in` over
    // one visits exactly its exports — in 10.4.6.2's sorted order, which is the
    // same list and the same order `Object.keys` reports and is answered by the
    // same function so the two cannot drift.
    if (rtIsModuleNamespace(v)) return bronze_object_keys(objBits);

    // A number, a boolean, an ArrayBuffer: no own enumerable properties, and
    // no prototype bronze models as an object, so nothing to visit.
    ObjectHeader* holder = namedPropertyHolder(v);
    if (!holder) return emptyKeyArray();

    // Phase one collects only arena-interned shape keys, which are immortal and
    // non-moving. That is what lets the whole chain be walked before a single
    // allocation happens: no raw object pointer here has to survive one.
    std::vector<StringHeader*> keys;
    for (uint32_t depth = 0; depth <= kMaxPrototypeDepth && holder != nullptr; ++depth) {
        // String keys only: 14.7.5.6 EnumerateObjectProperties yields
        // property names, and a symbol key is not one. It is the same filter
        // `Object.keys` applies, asked in the same place, so the two cannot
        // disagree about what a `for-in` visits.
        for (StringHeader* key : rtOwnStringKeysOrdered(holder)) {
            // A key redefined further up the chain is visited once, at the
            // level nearest the receiver — the level whose value a read would
            // find (ECMA-262 14.7.5.6).
            if (!alreadySeen(keys, key)) keys.push_back(key);
        }
        if (depth == kMaxPrototypeDepth) fatal("prototype chain too deep (a cycle?)");
        holder = holder->protoAncestor(1);
    }

    const auto total = static_cast<uint32_t>(keys.size());
    Rooted<Value> out{Value::fromObject(ArrayHeader::create(rtHeap(), total ? total : 4))};
    uint32_t at = 0;
    for (StringHeader* key : keys) {
        Rooted<Value> copy{rtCopyKeyToHeap(key)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, copy);
    }
    return out.get().rawBits();
}

}  // extern "C"

}  // namespace bronze::runtime
