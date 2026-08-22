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

#define _CRT_SECURE_NO_WARNINGS

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/builtin_object.h"
#include "runtime/exception.h"
#include "runtime/profile.h"
#include "runtime/namespace.h"
#include "runtime/proxy.h"
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

// Is this key already in the first `count` elements of `list`? The string
// comparison the arena-key walk below does with `alreadySeen`, over HEAP
// strings instead — which is what a proxy level forces: a trap's key list is
// whatever the handler built, so there is no interned form to compare
// pointers with.
bool listHasKey(Rooted<Value>& list, uint32_t count, Value key) {
    StringHeader* wanted = key.asString<StringHeader>();
    for (uint32_t i = 0; i < count; ++i) {
        Value seen = list.get().asObject<ArrayHeader>()->getElem(i);
        if (seen.isString() && seen.asString<StringHeader>()->equals(*wanted)) return true;
    }
    return false;
}

// 14.7.5.6 EnumerateObjectProperties spelled out over the INTERNAL METHODS,
// for a chain that has a Proxy somewhere in it.
//
// The ordinary walk above reads shape keys directly, which is both faster and
// impossible here: a proxy's own keys are the `ownKeys` trap's answer and its
// enumerability is the `getOwnPropertyDescriptor` trap's, so each level is two
// calls into user code that allocate and can throw. That is also why this walk
// keeps TWO lists. The specification's `visited` set records every own string
// key a level reports, enumerable or not, and only the enumerable ones are
// yielded — so a non-enumerable own property SHADOWS an inherited enumerable
// one of the same name rather than being skipped past. Collapsing the two into
// one list would let the inherited one through.
uint64_t proxyChainForInKeys(Value receiver) {
    Rooted<Value> level{receiver};
    Rooted<Value> visited{Value(bronze_create_array(0))};
    Rooted<Value> out{Value(bronze_create_array(0))};
    uint32_t seenCount = 0;
    uint32_t at = 0;
    for (uint32_t depth = 0; depth <= kMaxPrototypeDepth; ++depth) {
        if (!level.get().isObject()) break;
        const bool isProxy =
            level.get().asObject<HeapObjectHeader>()->flags == ProxyHeader::kFlags;
        if (!isProxy) {
            // The rest of the chain is ordinary. Its own keys are read
            // straight off the shapes, which is the walk `bronze_for_in_keys`
            // takes for a receiver with no proxy in it — asked here for both
            // enumerabilities so the shadowing rule above still holds across
            // the boundary.
            ObjectHeader* holder = namedPropertyHolder(level.get());
            if (!holder) break;
            // Both lists are built BEFORE a single allocation, which is what
            // lets `holder` stay a raw pointer across the walk: the keys are
            // arena-interned and immortal, and the flags are read off the same
            // shape the keys came from. The copying loop below allocates on
            // every key and touches neither.
            const std::vector<StringHeader*> levelKeys =
                rtOwnStringKeysOrdered(holder, /*enumerableOnly=*/false);
            std::vector<bool> levelEnumerable;
            levelEnumerable.reserve(levelKeys.size());
            for (StringHeader* key : levelKeys) {
                PropertyInfo info;
                levelEnumerable.push_back(holder->shape != nullptr &&
                                          holder->shape->lookupProperty(
                                              PropertyKey::forString(key), info) &&
                                          info.enumerable);
            }
            ObjectHeader* next = holder->protoAncestor(1);
            Rooted<Value> nextLevel{next ? Value::fromObject(next) : Value::fromNull()};
            for (size_t i = 0; i < levelKeys.size(); ++i) {
                Rooted<Value> copy{rtKeyAsValue(levelKeys[i])};
                if (listHasKey(visited, seenCount, copy.get())) continue;
                visited.get().asObject<ArrayHeader>()->setElem(rtHeap(), seenCount++, copy);
                if (!levelEnumerable[i]) continue;
                out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, copy);
            }
            if (!nextLevel.get().isObject()) break;
            level.set(nextLevel.get());
            continue;
        }

        Rooted<Value> keys{rtProxyOwnKeys(level.get())};
        if (rtExceptionPending()) return out.get().rawBits();
        const uint32_t keyCount = keys.get().asObject<ArrayHeader>()->length;
        for (uint32_t ki = 0; ki < keyCount; ++ki) {
            Rooted<Value> key{keys.get().asObject<ArrayHeader>()->getElem(ki)};
            // A symbol key is not a property NAME, and 14.7.5.6 yields names.
            if (!key.get().isString()) continue;
            if (listHasKey(visited, seenCount, key.get())) continue;
            visited.get().asObject<ArrayHeader>()->setElem(rtHeap(), seenCount++, key);
            OwnPropertyDetail found;
            const bool present = rtProxyGetOwnProperty(level.get(), key.get(), found);
            if (rtExceptionPending()) return out.get().rawBits();
            if (!present || !found.enumerable) continue;
            out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
        }
        Value proto = rtProxyGetPrototypeOf(level.get());
        if (rtExceptionPending()) return out.get().rawBits();
        if (!proto.isObject()) break;
        level.set(proto);
    }
    return out.get().rawBits();
}

// An empty array, which is what a nullish receiver enumerates: ECMA-262
// 14.7.5.5 returns an empty completion for null and undefined rather than
// throwing, and a program that writes `for (const k in maybe)` depends on it.
uint64_t emptyKeyArray() {
    ArrayHeader* arr = ArrayHeader::create(rtHeap(), 4);
    arr->length = 0;
    return Value::fromObject(arr).rawBits();
}

// ---- the shape-keyed enumeration cache -------------------------------------
//
// A `for-in` over a plain-object chain answers a question that is a pure
// function of (holder shape, prototype-mutation epoch) whenever the chain is
// one the IC machinery can already vouch for: the shape pins the holder's own
// enumerable keys AND its prototype (a Shape carries its chain root), the
// epoch moves on every way a key can appear on a marked chain — an add to any
// marked-prototype shape, a dictionary define, a prototype swap — and
// `chainIsCacheable` (the ABSENT entry's own witness, runtime/object.cpp)
// proves at probe time that no link has since been demoted to a dictionary,
// which is what a DELETE on a prototype does. So the guard is exactly the
// negative-entry validity condition, asked of the holder: same shape, same
// epoch, chain still provable.
//
// What the cache holds is the ordered key LIST — arena-interned StringHeader
// pointers, immortal and non-moving — so the entry contains no heap pointer at
// all and the collector never needs to know the cache exists. The per-call
// result array is still built fresh (the loop mutates a cursor over it), but a
// hit skips the chain walk, the per-level key-vector allocations, and the
// O(keys^2) cross-level dedup that made three.js's per-mesh-per-frame
// enumerations (1.8M a run on `many_meshes`) the third-largest helper cost.
//
// Seam: BRONZE_NO_ENUM_CACHE=1 disables both probe and fill, so one binary
// A/Bs the cache against the walk it replaces.
struct EnumCacheEntry {
    uint64_t epoch = 0;
    std::vector<StringHeader*> keys;
};

bool enumCacheEnabled() {
    static const bool disabled = [] {
        const char* env = std::getenv("BRONZE_NO_ENUM_CACHE");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return !disabled;
}

std::unordered_map<Shape*, EnumCacheEntry>& enumCache() {
    static thread_local std::unordered_map<Shape*, EnumCacheEntry> cache;
    return cache;
}

// The result array for a key list: the arena keys THEMSELVES, for the same
// identity-latch reason the walk's own foot states below.
uint64_t keyListToArray(const std::vector<StringHeader*>& keys) {
    const auto total = static_cast<uint32_t>(keys.size());
    Rooted<Value> out{Value::fromObject(ArrayHeader::create(rtHeap(), total ? total : 4))};
    uint32_t at = 0;
    for (StringHeader* key : keys) {
        Rooted<Value> keyVal{rtKeyAsValue(key)};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, keyVal);
    }
    return out.get().rawBits();
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
            // the allocations `setElem` makes — and the array is handed the
            // arena keys THEMSELVES (rtKeyAsValue), which is what keeps the
            // computed-read cache's identity latch hitting for keys a program
            // reads back through `a[k]`.
            for (StringHeader* named : rtArrayOwnNamedKeys(src.get())) {
                Rooted<Value> key{rtKeyAsValue(named)};
                out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, key);
            }
        }
        return out.get().rawBits();
    }

    // A module namespace has no prototype at all (10.4.6.1), so a `for-in` over
    // one visits exactly its exports — in 10.4.6.2's sorted order, which is the
    // same list and the same order `Object.keys` reports and is answered by the
    // same function so the two cannot drift.
    if (rtIsModuleNamespace(v)) return bronze_object_keys(objBits);

    // A PROXY has no storage of its own to read: 10.5.11 makes its own keys the
    // `ownKeys` trap's answer, so the shape walk below would find nothing and
    // report a proxy over `{a: 1}` as having no properties at all. The
    // trap-driven walk answers instead — and answers for the whole chain,
    // because a proxy's [[GetPrototypeOf]] is a trap too.
    if (v.isObject() && v.asObject<HeapObjectHeader>()->flags == ProxyHeader::kFlags) {
        return proxyChainForInKeys(v);
    }

    // A number, a boolean, an ArrayBuffer: no own enumerable properties, and
    // no prototype bronze models as an object, so nothing to visit.
    ObjectHeader* holder = namedPropertyHolder(v);
    if (!holder) return emptyKeyArray();

    // The enumeration cache, probed and (on the walk below) filled under one
    // decision: the holder's key list is a function of (shape, epoch) only for
    // a non-dictionary shape over a chain `chainIsCacheable` vouches for. Both
    // sides ask before anything allocates, so the raw holder pointer is live
    // for the whole question; shapes are immortal, so `cacheShape` outlives
    // everything below.
    Shape* cacheShape = nullptr;
    const uint64_t epochNow = protoMutationEpoch();
    if (enumCacheEnabled() && holder->shape && !holder->shape->isDictionary() &&
        holder->chainIsCacheable()) {
        cacheShape = holder->shape;
        auto& cache = enumCache();
        if (auto it = cache.find(cacheShape);
            it != cache.end() && it->second.epoch == epochNow) {
            return keyListToArray(it->second.keys);
        }
    }

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

    // The fill: exactly what the walk just proved, under the decision taken
    // before it ran. The epoch could not have moved in between — phase one
    // neither allocates nor runs user code — so the entry's epoch is the one
    // the keys were collected at.
    if (cacheShape) {
        EnumCacheEntry& entry = enumCache()[cacheShape];
        entry.epoch = epochNow;
        entry.keys = keys;
    }

    // The result array holds the arena keys THEMSELVES (rtKeyAsValue), not
    // per-call heap copies. That is what makes `for (name in attributes)
    // ... attributes[name]` present the SAME string object every frame, which
    // the computed-read cache's identity latch (elem_ic.h's `key_ident`)
    // turns into an inline hit — and it deletes the one-heap-string-per-key-
    // per-enumeration bill the copy used to run (three.js's many_meshes made
    // ~5M such copies a run).
    return keyListToArray(keys);
}

}  // extern "C"

}  // namespace bronze::runtime
