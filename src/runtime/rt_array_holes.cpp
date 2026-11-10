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
#include <cstdlib>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_object.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/proxy.h"
#include "runtime/typed_array.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/profile.h"
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
namespace {

bool chainHoldsIndexLatched(Value arr);

}  // namespace

// BRONZE_CHECK_INDEX_HAZARD=1: while the hazard is clear, the latch's scan is
// run anyway and a chain it finds holding an index is a fatal error — the
// test that every way an index reaches a prototype trips the hazard
// (object.h), which generated code relies on without asking. Read once at
// load: the inline test (rt_property.h) asks it beside the hazard word.
const bool g_checkIndexHazard = [] {
    const char* env = std::getenv("BRONZE_CHECK_INDEX_HAZARD");
    return env && env[0] && env[0] != '0';
}();

bool rtArrayChainHoldsIndexSlow(Value arr) {
    if (rtTls()->array_index_hazard == 0) {
        if (BRONZE_UNLIKELY(g_checkIndexHazard) && chainHoldsIndexLatched(arr)) {
            fatal("internal: an array's chain holds an index but the array index hazard is clear");
        }
        return false;
    }
    return chainHoldsIndexLatched(arr);
}

namespace {

bool chainHoldsIndexLatched(Value arr) {
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

}  // namespace

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

Value rtArrayIndexGet(Value arr, uint32_t index) {
    const ArrayHeader* a = arr.asObject<ArrayHeader>();
    if (index < a->length) {
        const Value v = a->elementsData()[index];
        if (!v.isHole()) return v;
    }
    if (!rtArrayChainHoldsIndex(arr)) return Value::fromUndefined();
    Rooted<Value> root{arr};
    return rtArrayInheritedGet(root, index);
}

bool rtArrayInheritedSet(Rooted<Value>& arr, uint32_t index, Rooted<Value>& val, bool strict, bool& ok) {
    ok = true;
    Rooted<Value> key{rtMakeString(std::to_string(index))};
    Rooted<Value> holder{prototypeOf(arr.get())};
    for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        // Step 2.a's end of the chain: absent everywhere, so the write is the
        // array's own.
        if (!holder.get().isObject()) return false;
        const uint16_t kind = holder.get().asObject<HeapObjectHeader>()->flags;
        if (kind == HeapKind::Proxy) {
            // 10.5.9: the trap (or the target's [[Set]]) decides, with the array
            // as the receiver; a forward that reaches the end lands on the
            // array through CreateDataProperty, never back here
            // (rtOrdinarySetWithReceiver).
            ok = rtProxySet(holder.get(), key.get(), val.get(), strict, arr.get());
            return true;
        }
        if (kind == HeapKind::TypedArray) {
            // 10.4.5.5 with a receiver that is not the view: an index the view
            // does not have makes the write a silent success, and one it has is
            // OrdinarySet over a writable element, which lands on the receiver.
            return index >= holder.get().asObject<TypedArrayHeader>()->length;
        }
        OwnPropertyDetail found;
        if (rtOwnPropertyOf(holder, key.get(), found)) {
            if (found.accessor) {
                // Step 3: the setter, with the array as `this` (or the refusal
                // of an accessor without one).
                const SetRefusal refusal = rtOrdinarySetWithReceiver(holder, key, val, arr);
                ok = refusal == SetRefusal::None;
                if (!ok) rtReportSetRefusal(refusal, strict, std::to_string(index));
                return true;
            }
            // Step 2.a: a writable data property is shadowed by the array's own
            // element; a non-writable one refuses the write (unless its holder
            // is hardened, which shadows instead: lockdown.h).
            if (found.writable || rtIsHardened(holder.get())) return false;
            ok = false;
            rtReportSetRefusal(SetRefusal::NotWritable, strict, std::to_string(index));
            return true;
        }
        const uint64_t call[1] = {holder.get().rawBits()};
        holder.set(Value(objectGetPrototypeOf(0, 0, 1, call)));
    }
    return false;
}

bool rtArrayInheritedNamedSet(Rooted<Value>& arr, Rooted<Value>& key, Rooted<Value>& val, SetRefusal& refusal) {
    refusal = SetRefusal::None;
    Rooted<Value> holder{prototypeOf(arr.get())};
    for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        if (!holder.get().isObject()) return false;
        if (holder.get().asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
            // 10.5.9 with the array as the receiver; a forward that reaches
            // the end defines onto the array (rtOrdinarySetWithReceiver), never
            // back here. Non-strict: the caller reports a false per its mode.
            const bool ok = rtProxySet(holder.get(), key.get(), val.get(), /*strict=*/false, arr.get());
            if (!ok) refusal = SetRefusal::NotWritable;
            return true;
        }
        OwnPropertyDetail found;
        if (rtOwnPropertyOf(holder, key.get(), found)) {
            if (found.accessor) {
                refusal = rtOrdinarySetWithReceiver(holder, key, val, arr);
                return true;
            }
            if (found.writable || rtIsHardened(holder.get())) return false;
            refusal = SetRefusal::NotWritable;
            return true;
        }
        const uint64_t call[1] = {holder.get().rawBits()};
        holder.set(Value(objectGetPrototypeOf(0, 0, 1, call)));
    }
    return false;
}

}  // namespace bronze::runtime
