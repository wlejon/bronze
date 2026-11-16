// TRANSITION CELLS (il.h Module::protoCells): what lets a speculated
// property add be a slot store and a shape store. A cell is one word in the
// module's per-thread data, the prototype epoch it was stamped at; the
// generated code compares it with the thread's epoch (`proto.cell`), and on a
// mismatch asks for a stamp here (`proto.publish`) and runs the add
// generically. Any bump of the epoch (an add to, a delete from, a redefine
// on, or a prototype change of any object some shape names as its
// prototype) retires every stamp, so a stamp says the chain above `from` is
// the chain that was checked.

#include "abi/bronze_abi.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// Whether an ordinary [[Set]] of `key` on an object whose prototype is
// `proto` adds an own data property, for as long as the epoch stands: no
// link that could answer otherwise (an accessor, a read-only property, a
// proxy or another exotic link) and every link a prototype whose changes
// bump the epoch.
bool chainAdmitsAdd(Value proto, PropertyKey key) {
    for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        if (!proto.isObject()) return true;
        const auto* hdr = proto.asObject<HeapObjectHeader>();
        if (!HeapKind::shapeNamesPrototype(hdr->flags)) return false;
        const auto* link = reinterpret_cast<const ObjectHeader*>(hdr);
        const Shape* shape = link->shape;
        if (shape == nullptr || !shape->readCacheable()) return false;
        if (!shape->used_as_prototype && !shape->isDictionary()) return false;
        PropertyInfo info;
        if (shape->lookupProperty(key, info)) return !info.accessor && info.writable;
        proto = shape->prototypeValue();
    }
    return false;
}

}  // namespace

extern "C" {

void bronze_transition_publish(uint64_t* cell, uint64_t fromBits, uint32_t keyIndex, uint64_t toBits,
                               uint32_t slot) {
    recordHelperCall("bronze_transition_publish");
    const auto* from = reinterpret_cast<const Shape*>(static_cast<uintptr_t>(fromBits));
    const auto* to = reinterpret_cast<const Shape*>(static_cast<uintptr_t>(toBits));
    if (cell == nullptr || from == nullptr || to == nullptr) return;
    // `from` is an extensible plain layout (a non-extensible object is a
    // dictionary) that no object is a prototype through: an add to a
    // prototype must bump the epoch, which the fast path does not.
    if (from->isDictionary() || to->isDictionary() || from->used_as_prototype || to->used_as_prototype) return;
    StringHeader* name = rtKeyHeader(keyIndex);
    if (name == nullptr) return;
    const PropertyKey key = PropertyKey::forString(name);
    if (to->parent != from || !to->key.matches(key) || to->slot_index != slot) return;
    if (!to->enumerable || !to->writable || !to->configurable || to->accessor) return;
    if (slot >= to->inline_slots || slot >= ObjectHeader::kOverflowSlot) return;
    if (to->slotIsDouble(slot) || to->slotIsObject(slot)) return;
    if (!chainAdmitsAdd(from->prototypeValue(), key)) return;
    *cell = protoMutationEpoch();
}

}  // extern "C"

}  // namespace bronze::runtime
