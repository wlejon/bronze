// Reconciling a compile-time accessor guess with a run-time prototype chain.
//
// A property site whose receiver's class declares an accessor for the key
// (lower/direct_method_table.h, `resolveAccessors`) compiles to a guard on a
// two-word module CELL and then a direct call of — or the inlined body of —
// that accessor's getter or setter. Lowering can only GUESS: the receiver's
// static class is not its run-time shape, a subclass may override the
// accessor, an instance may carry an own property of the same name, and any
// of the prototypes may be patched. This is the one function that decides,
// for one shape, that the guess is what [[Get]] or [[Set]] would actually do.
//
// The cell is (shape, epoch), and the guard is
//
//     receiver is plain  &&  receiver.shape == cell[0]  &&  cell[1] == epoch
//
// The SHAPE stands for everything about the receiver itself: a non-dictionary
// shape fixes the object's own keys (so the key is still not own) and its
// [[Prototype]] (which lives on the shape's root). The EPOCH stands for
// everything above it: every change that could make the chain answer
// differently — an add to a prototype (which may shadow), a delete or a
// dictionary demotion on one, an accessor redefined in place, a prototype
// swap — bumps the thread's prototype-mutation epoch (object.cpp,
// accessor.cpp, dictionary.cpp), which is the same contract the inline
// caches' depth > 0 entries already lean on. `chainIsCacheable` is the check
// that makes it hold for the chain in front of us: every link is marked
// `used_as_prototype`, so a later add to any of them does bump.
//
// A data write to a prototype that leaves no shape behind does not bump, and
// does not need to: overwriting a slot that held a FUNCTION does bump
// (`notePrototypeSlotOverwrite`), and an accessor's pair is never written by
// an ordinary [[Set]] (that calls the setter). `Matrix4.prototype.isMatrix4 =
// true` in every construction therefore leaves `Euler`'s cells alone.
//
// The cell's states:
//
//   [0, 0]               never tried; the guard misses (no live shape is 0).
//   [kRefused, epoch]    tried at this epoch and the chain did not answer the
//                        guess; the guard misses and the slow path stops
//                        calling here until the epoch moves.
//   [shape, epoch]       this shape resolves the key to the guessed accessor.
//
// Unlike a static-slot cell, an accessor cell is re-published when the epoch
// moves: setup bumps it often (every class body adds methods to a prototype),
// and a cell pinned once would go stale for good. It is still one call per
// site per epoch, not one per miss.

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/realm.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/shape_census.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// Not a legal shape address (shapes come from the arena 8-aligned), so the
// guard needs no extra test to treat a refusal as a miss.
constexpr uint64_t kRefused = 1;

// The accessor half the key resolves to on `obj`'s prototype chain, or null
// when the chain answers anything else: the key own on the receiver, a data
// property above it, a link the epoch does not watch, or no such key at all.
const FunctionHeader* resolvedAccessorHalf(ObjectHeader* obj, PropertyKey key, bool forSet) {
    PropertyInfo info;
    // An own property — data or accessor — is what [[Get]] finds first.
    if (obj->shape->lookupProperty(key, info)) return nullptr;
    if (!obj->chainIsCacheable()) return nullptr;
    ObjectHeader* cur = obj;
    for (uint32_t depth = 1; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        const Value proto = cur->shape->prototypeValue();
        if (!proto.isObject()) return nullptr;
        auto* hdr = proto.asObject<HeapObjectHeader>();
        // A link that is not a plain shape-chain object answers through
        // machinery of its own, and a dictionary (even a frozen one, which
        // `chainIsCacheable` admits) is a holder whose slots no shape names.
        if (hdr->flags != HeapKind::Plain) return nullptr;
        cur = reinterpret_cast<ObjectHeader*>(hdr);
        if (cur->shape == nullptr || cur->shape->isDictionary()) return nullptr;
        if (!cur->shape->lookupProperty(key, info)) continue;
        if (!info.accessor) return nullptr;
        // The setter is the slot after the getter (accessor.cpp).
        const Value half = cur->getSlot(info.slot + (forSet ? 1u : 0u));
        if (!half.isObject() || half.asObject<HeapObjectHeader>()->flags != HeapKind::Function) return nullptr;
        return half.asObject<FunctionHeader>();
    }
    return nullptr;
}

}  // namespace

extern "C" {

// `code` is the guessed accessor's call wrapper: the code word its function
// object carries. The fast path calls the accessor's body with no function
// object in hand, so each condition below is one it will not re-check:
//
//   - a plain, non-dictionary receiver (the shape word stands for its keys
//     and its prototype only then);
//   - the chain resolves the key, at this epoch, to an accessor whose half
//     for this site is a function with exactly that code;
//   - a function compiled code may enter without the runtime, made in the
//     realm running now — what the runtime's own dispatch would enter.
void bronze_accessor_cell_publish(uint64_t objBits, uint32_t keyIndex, uint64_t* cell, bronze_fn_code code,
                                  bool forSet) {
    recordHelperCall("bronze_accessor_cell_publish");
    // Census mode: never publish, so every access reaches the helpers the
    // census counts in (static_shape.cpp says the same of its cells).
    if (censusFillsSuppressed()) return;
    if (cell == nullptr) return;
    const uint64_t epoch = protoMutationEpoch();
    if (cell[1] == epoch) return;

    uint64_t published = kRefused;
    Value objVal(objBits);
    StringHeader* key = rtKeyHeader(keyIndex);
    if (key != nullptr && objVal.isObject() && objVal.asObject<HeapObjectHeader>()->flags == HeapKind::Plain) {
        auto* obj = objVal.asObject<ObjectHeader>();
        if (obj->shape != nullptr && !obj->shape->isDictionary()) {
            const FunctionHeader* fn = resolvedAccessorHalf(obj, PropertyKey::forString(key), forSet);
            if (fn != nullptr && fn->code == code && rtEntersWithoutScope(fn) &&
                (fn->realmSlot() == 0 || fn->realmSlot() == rtCurrentRealmSlot())) {
                published = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(obj->shape));
            }
        }
    }
    cell[0] = published;
    cell[1] = epoch;
}

}  // extern "C"

}  // namespace bronze::runtime
