// An array's HOLES, and the one question every fast path over an array's
// elements has to be able to answer without a walk: could the prototype chain
// supply what a hole lacks?
//
// A hole is not an own property (10.4.2 stores only what was written), so
// [[HasProperty]] and [[Get]] of its index continue to the prototype
// (10.1.7.1, 10.1.8.1), and every array method is written in those two
// operations — `shift` moves an inherited element down into an own one,
// `includes` finds it, `forEach` visits it. bronze's element block answers a
// hole as `undefined` and as absent, which is the right answer exactly when no
// link of the chain holds an array index, and that is what nearly every
// program's chains are. So the test is the fast paths' premise and the walk is
// what they fall to when it fails.
//
// THE TEST IS CACHED, never re-derived per element. A chain's keys change only
// by an add, delete or redefinition on an object serving as a prototype, or by
// a prototype swap, and each of those moves the prototype-mutation epoch
// (runtime/object.h). The ordinary chain — `Array.prototype` and above, which
// every array without a custom prototype has — is latched in the TLS block, so
// generated code can read the same answer (bronze_abi_tls.h,
// array_hole_epoch); a custom chain is latched here by the SHAPE of its first
// link, which fixes both that link's keys and the prototype behind it, and
// which lives in the arena and so cannot move under the latch.

#include <cstdint>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/gc.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// Does this ordinary object have an own array-index key? Asked of a shape's
// keys rather than of index 0..n, because what matters is whether ANY index can
// be answered, and a chain's links have few keys where an array has many holes.
//
// A shape that is not a dictionary is a transition-tree node whose keys can
// never change, so an index-free one is remembered for good: the epoch moves
// on every prototype add in the program, and re-reading `Array.prototype`'s
// forty-odd keys each time would make the latch cost what it exists to save.
constexpr size_t kIndexFreeMemo = 16;
thread_local const Shape* g_indexFree[kIndexFreeMemo] = {};

bool ownIndexKey(const ObjectHeader* obj) {
    const Shape* shape = obj->shape;
    const size_t at = (reinterpret_cast<uintptr_t>(shape) >> 4) % kIndexFreeMemo;
    if (!shape->isDictionary() && g_indexFree[at] == shape) return false;
    for (const StringHeader* key : rtOwnStringKeysOrdered(obj, /*enumerableOnly=*/false)) {
        uint32_t index = 0;
        if (rtIsIntegerLikeKey(rtUtf8Chars(key), index)) return true;
    }
    if (!shape->isDictionary()) g_indexFree[at] = shape;
    return false;
}

// Walk the chain from `link` up. A link that is not an ordinary object is
// answered true without looking: a proxy's `has` is user code, and the walk
// that follows asks it properly. Only a plain object may be a prototype
// otherwise (`Object.create` and [[SetPrototypeOf]] refuse the other kinds).
bool chainHoldsIndex(Value link) {
    for (uint32_t depth = 0; depth < ObjectHeader::kMaxPrototypeDepth; ++depth) {
        if (!link.isObject()) return false;
        if (link.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) return true;
        const auto* obj = link.asObject<ObjectHeader>();
        if (!obj->shape) return false;
        if (ownIndexKey(obj)) return true;
        link = obj->shape->prototypeValue();
    }
    return true;
}

// The ordinary chain's answer for the other half of the pair: the epoch it was
// last found to HOLD an index at, so a program that put one on
// `Object.prototype` pays the scan once per epoch rather than once per hole.
thread_local uint64_t g_ordinaryHeldEpoch = 0;

bool ordinaryChainHoldsIndex() {
    bronze_tls_block* tls = bronze_tls_block_addr();
    const uint64_t epoch = tls->proto_epoch;
    if (tls->array_hole_epoch == epoch) return false;
    if (g_ordinaryHeldEpoch == epoch) return true;
    // Without building either intrinsic: the fast paths ask this holding raw
    // headers, and one that is not built yet has been given nothing.
    Value start = rtArrayPrototypeObjectIfBuilt();
    if (!start.isObject()) start = rtObjectPrototypeIfBuilt();
    const bool holds = chainHoldsIndex(start);
    tls->array_hole_epoch = holds ? 0 : epoch;
    g_ordinaryHeldEpoch = holds ? epoch : 0;
    return holds;
}

// One latch for custom chains: a subclass's instances, or arrays given a
// prototype by `Object.setPrototypeOf`, mostly share one.
struct CustomChainLatch {
    const Shape* first = nullptr;
    uint64_t epoch = 0;
    bool holds = false;
};
thread_local CustomChainLatch g_customLatch;

bool hasCustomPrototype(Value arr) {
    return (arr.asObject<ArrayHeader>()->reserved & ArrayHeader::kHasCustomPrototype) != 0;
}

// The array's [[Prototype]]: the box's, when the array was given one, and
// `Array.prototype` otherwise (rtExoticSubclassPrototype says where it lives).
Value prototypeOf(Value arr) {
    if (!hasCustomPrototype(arr)) return rtArrayPrototypeObject();
    return rtExoticSubclassPrototype(arr);
}

}  // namespace

// Allocates nothing on the GC heap — a fast path with a raw header in hand may
// ask it.
bool rtArrayChainHoldsIndex(Value arr) {
    if (!hasCustomPrototype(arr)) return ordinaryChainHoldsIndex();
    const Value proto = rtExoticSubclassPrototype(arr);
    if (!proto.isObject()) return false;
    if (proto.asObject<HeapObjectHeader>()->flags != HeapKind::Plain ||
        proto.asObject<ObjectHeader>()->shape == nullptr) {
        return chainHoldsIndex(proto);
    }
    const Shape* first = proto.asObject<ObjectHeader>()->shape;
    const uint64_t epoch = protoMutationEpoch();
    if (g_customLatch.first == first && g_customLatch.epoch == epoch) return g_customLatch.holds;
    const bool holds = chainHoldsIndex(proto);
    g_customLatch = {first, epoch, holds};
    return holds;
}

bool rtArrayHolesSeeChain(Value arr) {
    if (!rtArrayChainHoldsIndex(arr)) return false;
    const ArrayHeader* a = arr.asObject<ArrayHeader>();
    for (uint32_t i = 0; i < a->length; ++i) {
        if (!a->hasElem(i)) return true;
    }
    return false;
}

bool rtArrayInheritedHas(Rooted<Value>& arr, uint32_t index) {
    Rooted<Value> proto{prototypeOf(arr.get())};
    if (!proto.get().isObject()) return false;
    Rooted<Value> key{Value::fromDouble(static_cast<double>(index))};
    return bronze_has_property(key.get().rawBits(), proto.get().rawBits());
}

Value rtArrayInheritedGet(Rooted<Value>& arr, uint32_t index) {
    Rooted<Value> proto{prototypeOf(arr.get())};
    if (!proto.get().isObject()) return Value::fromUndefined();
    // The canonical spelling of the index: ToPropertyKey's answer, and the
    // form every holder of a named key stores.
    Rooted<Value> key{rtMakeString(std::to_string(index))};
    return rtGetWithReceiver(proto, key, arr);
}

}  // namespace bronze::runtime
