// `for-in`, as a snapshot of the keys it will visit (docs/0018 decision 1).
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
// where several levels define it. Per level the order is docs/0009's: integer
// -like keys ascending, then the rest in insertion order.

#include <charconv>
#include <string>
#include <vector>

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
    HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags == 1) {
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
// function — its own-property object, which is where a static member is
// stored and whose prototype `extends` linked to the base's (docs/0012
// decision 6). Null when the receiver keeps no named properties at all.
ObjectHeader* namedPropertyHolder(Value v) {
    if (!v.isObject()) return nullptr;
    HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        return reinterpret_cast<ObjectHeader*>(hdr);
    }
    if (hdr->flags == 2) {
        Value props = v.asObject<FunctionHeader>()->properties;
        return props.isObject() ? props.asObject<ObjectHeader>() : nullptr;
    }
    return nullptr;
}

// An empty array, which is what a nullish receiver enumerates: ECMA-262
// 14.7.5.5 returns an empty completion for null and undefined rather than
// throwing, and a program that writes `for (const k in maybe)` depends on it.
uint64_t emptyKeyArray() {
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), 4);
    arr->header.flags = 1;
    arr->length = 0;
    return Value::fromObject(arr).rawBits();
}

}  // namespace

extern "C" {

uint64_t bronze_for_in_keys(uint64_t objBits) {
    Value v(objBits);
    if (v.isNull() || v.isUndefined()) return emptyKeyArray();

    uint32_t indexCount = 0;
    if (indexedLength(v, indexCount)) {
        // Nothing in this branch reads a shape, so nothing allocated below can
        // invalidate it; the source is rooted anyway, because the array of
        // digit strings is built one allocation at a time.
        Rooted<Value> src{v};
        // An array index that a `delete` turned into a HOLE is no longer an
        // own property, so the enumeration skips it — 14.7.5.6 visits own
        // keys, and a hole is not one (docs/0019 decision 2). The result is
        // therefore not simply `0..indexCount-1`, which is why the length is
        // left to the writes below rather than set up front.
        //
        // Asked BEFORE the allocation below, and of the ROOT: a plain `v`
        // read afterwards is a pointer into dead from-space, which under
        // --gc-stress reported every string as an array and every array as
        // something else on the very first case that had a hole in it.
        const bool skipHoles =
            src.get().isObject() && src.get().asObject<HeapObjectHeader>()->flags == 1;
        Rooted<Value> out{Value::fromObject(
            ArrayHeader::create(rtHeap(), indexCount ? indexCount : 4))};
        out.get().asObject<ArrayHeader>()->header.flags = 1;
        uint32_t at = 0;
        for (uint32_t i = 0; i < indexCount; ++i) {
            if (skipHoles && !src.get().asObject<ArrayHeader>()->hasElem(i)) continue;
            char buf[16];
            auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), i);
            Rooted<Value> key{Value::fromString(
                StringHeader::createFromUTF8(rtHeap(), std::string_view(buf, end - buf)))};
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        return out.get().rawBits();
    }

    // A number, a boolean, an ArrayBuffer: no own enumerable properties, and
    // no prototype bronze models as an object, so nothing to visit.
    ObjectHeader* holder = namedPropertyHolder(v);
    if (!holder) return emptyKeyArray();

    // Phase one collects only arena-interned shape keys, which are immortal
    // and non-moving (docs/0004 decision 2). That is what lets the whole chain
    // be walked before a single allocation happens: no raw object pointer here
    // has to survive one.
    std::vector<StringHeader*> keys;
    for (uint32_t depth = 0; depth <= kMaxPrototypeDepth && holder != nullptr; ++depth) {
        for (StringHeader* key : rtOwnKeysOrdered(holder)) {
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
    out.get().asObject<ArrayHeader>()->header.flags = 1;
    uint32_t at = 0;
    for (StringHeader* key : keys) {
        Rooted<Value> copy{rtCopyKeyToHeap(key)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, copy);
    }
    return out.get().rawBits();
}

}  // extern "C"

}  // namespace bronze::runtime
