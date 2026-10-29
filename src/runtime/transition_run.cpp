// Publishing a TRANSITION RUN (lower/transition_runs.h): the cells that let a
// constructor's stretch of adds run as one guard, one shape store and a store
// per key (codegen-brass il_transition_run.cpp).
//
// The guard asks four things of the receiver, and each stands for a check made
// here, against the shape the run really built, before anything is written:
//
//   shape == start   a plain object with no internal slots at the start shape,
//                    which is not a dictionary (frozen, sealed, non-extensible
//                    and deleted-from objects all are) and not a prototype's;
//   epoch            no setter, read-only property or proxy on the chain for any
//                    of the run's keys — verified by walking it here, the walk
//                    ObjectHeader::setProp makes, and kept true by every way one
//                    could appear (an add to a prototype, a dictionary define, a
//                    prototype swap) bumping the epoch;
//   end              the start shape with exactly the run's keys added, in
//                    order, as enumerable, writable, configurable data at the
//                    slots the compiler named — so a receiver given `end` is the
//                    object the writes one by one would have made;
//   doubles          `end`'s double slots, which the guard tests each value
//                    against, as a lone static-slot store does.
//
// Nothing upstream has to be right: a run whose writes do something else (keys
// already there, a setter, a subclass's different start) fails a check, keeps
// its cells, and costs its guard. Re-publishing replaces the cells — the run is
// monomorphic, last writer wins — and the count bounds how often: a run whose
// receivers alternate between start shapes stops trying after
// BRONZE_ABI_TRANSITION_RUN_PUBLISH_LIMIT publishes and keeps its slow copy.

#include "abi/bronze_abi.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/proxy.h"
#include "runtime/realm.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/shape_census.h"
#include "runtime/string.h"
#include "runtime/value.h"

#include <cstdlib>

namespace bronze::runtime {

namespace {

// The A/B seam: BRONZE_NO_TRANSITION_RUNS=1 never publishes, so every run
// keeps its slow copy, which is the code a build without runs emits.
bool runsDisabled() {
    static const bool off = [] {
        const char* v = std::getenv("BRONZE_NO_TRANSITION_RUNS");
        return v && v[0] == '1';
    }();
    return off;
}

bool plainData(const Shape* node) {
    return node->enumerable && node->writable && node->configurable && !node->accessor;
}

// 10.1.9.2's walk for a key the receiver does not have, as setProp walks it:
// an inherited accessor runs instead of the add, an inherited read-only
// property refuses it, and a proxy link takes the write. Any of them, and the
// run cannot be one store.
bool chainAdmitsAdd(ObjectHeader* receiver, PropertyKey key) {
    ObjectHeader* holder = receiver;
    for (uint32_t depth = 1; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        if (!holder->shape) return false;
        const Value parent = holder->shape->prototypeValue();
        if (parent.isObject() && parent.asObject<HeapObjectHeader>()->flags == ProxyHeader::kFlags) return false;
        holder = holder->protoAncestor(1);
        if (!holder) return true;
        PropertyInfo info;
        if (!holder->shape || !holder->shape->lookupProperty(key, info)) continue;
        return !info.accessor && info.writable;
    }
    return false;
}

uint32_t overflowNeed(const Shape* end) {
    const uint32_t slots = end->nextSlotIndex();
    return slots > ObjectHeader::kInlineSlots ? slots - ObjectHeader::kInlineSlots : 0;
}

}  // namespace

extern "C" {

void bronze_transition_run_publish(uint64_t objBits, uint64_t startBits, uint64_t* cells, const uint64_t* keys,
                                   uint32_t count, uint32_t firstSlot) {
    recordHelperCall("bronze_transition_run_publish");
    if (cells == nullptr || censusFillsSuppressed() || runsDisabled()) return;
    uint64_t& counter = cells[BRONZE_ABI_TRANSITION_RUN_COUNT];
    const uint64_t publishes = counter >> BRONZE_ABI_TRANSITION_RUN_COUNT_SHIFT;
    if (publishes >= BRONZE_ABI_TRANSITION_RUN_PUBLISH_LIMIT) return;

    // A refusal counts toward the limit, so a run that never fits stops
    // asking; a re-publish of the same two shapes at a newer epoch does not,
    // since the epoch moves for reasons that have nothing to do with the run.
    auto refuse = [&counter] { counter += uint64_t{1} << BRONZE_ABI_TRANSITION_RUN_COUNT_SHIFT; };
    Value objVal(objBits);
    if (!objVal.isObject() || startBits == 0 || count == 0) return refuse();
    auto* hdr = objVal.asObject<HeapObjectHeader>();
    // A plain object with no internal slots: the header the guard compares.
    if (hdr->flags != HeapKind::Plain || hdr->size != BRONZE_ABI_PLAIN_OBJECT_BYTES) return refuse();
    // A global object's writes go through the global cache's invalidation.
    // Asked of every live realm without making one: rtCurrentRealm would
    // create the default realm here, moving the thread's realm slot under
    // functions already stamped with the old one.
    if (rtRealmOfGlobal(objVal) != nullptr) return refuse();
    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    Shape* start = reinterpret_cast<Shape*>(static_cast<uintptr_t>(startBits));
    Shape* end = obj->shape;
    if (end == nullptr || end->isDictionary() || end->used_as_prototype) return refuse();
    if (start->isDictionary() || start->used_as_prototype) return refuse();

    const Shape* node = end;
    for (uint32_t i = count; i-- > 0;) {
        if (node == nullptr || !node->key.valid() || node->isDictionary() || !plainData(node)) return refuse();
        if (node->slot_index != firstSlot + i) return refuse();
        StringHeader* key = rtKeyHeader(static_cast<uint32_t>(keys[i]));
        if (key == nullptr || !node->key.matches(PropertyKey::forString(key))) return refuse();
        node = node->parent;
    }
    if (node != start) return refuse();
    for (uint32_t i = 0; i < count; ++i) {
        if (!chainAdmitsAdd(obj, PropertyKey::forString(rtKeyHeader(static_cast<uint32_t>(keys[i]))))) return refuse();
    }

    // Where the fast copy reads how big a block to give a receiver that has
    // none: a class's root learns the most any of its runs needed, so the
    // first run of a constructor allocates for the later ones; the plain
    // root is every literal's, so a literal's run asks only for its own.
    Shape* root = end->root;
    const uint32_t need = overflowNeed(end);
    uint32_t* capacity = nullptr;
    if (root != nullptr && root != rtPlainObjectShape()) {
        if (root->run_overflow_hint < need) root->run_overflow_hint = need;
        capacity = &root->run_overflow_hint;
    } else {
        // The low half of the counter word: little-endian, as every target
        // bronze emits for is.
        auto* own = reinterpret_cast<uint32_t*>(&counter);
        if (*own < need) *own = need;
        capacity = own;
    }

    const bool same = cells[BRONZE_ABI_TRANSITION_RUN_START] == startBits &&
                      cells[BRONZE_ABI_TRANSITION_RUN_END] == static_cast<uint64_t>(reinterpret_cast<uintptr_t>(end));
    cells[BRONZE_ABI_TRANSITION_RUN_START] = startBits;
    cells[BRONZE_ABI_TRANSITION_RUN_END] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(end));
    cells[BRONZE_ABI_TRANSITION_RUN_EPOCH] = protoMutationEpoch();
    cells[BRONZE_ABI_TRANSITION_RUN_CAPACITY] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(capacity));
    if (!same) counter += uint64_t{1} << BRONZE_ABI_TRANSITION_RUN_COUNT_SHIFT;
}

}  // extern "C"

}  // namespace bronze::runtime
