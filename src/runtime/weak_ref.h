#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze::runtime {

// `WeakRef` (ECMA-262 26.1) and `FinalizationRegistry` (26.2): the two objects
// whose whole content is a reference the collector must NOT keep alive.
//
// Both are ORDINARY OBJECTS with internal slots, exactly as 26.1.1.1 and
// 26.2.1.1 create them (OrdinaryCreateFromConstructor over NewTarget), so each
// has a shape, a real prototype on its chain and a brand in its first slot —
// the arrangement builtin_map.cpp explains for a Map.
//
// The weakness is the collector's (brass's weak references, runtime/heap.h):
//
//  - a WeakRef's target is its LAST internal slot, and the object is
//    allocated with GcLayout::WeakLast, so the collector visits that slot
//    weakly: it follows the target when the target survives and writes
//    `undefined` there when it does not; and
//  - a FinalizationRegistry's cells live in a C++ table in weak_ref.cpp that a
//    tracer source visits at every collection: the registry reference, the
//    target and the unregister token WEAKLY, the held value strongly.
//
// A post-collection hook then sweeps the table: a cell whose target now reads
// `undefined` parks its held value for a cleanup job, and a registry that
// died takes its cells with it. A young target dies in the minor collection
// that finds it unreachable; an old one only in a full collection.
//
// Retention semantics shipped, stated plainly because a weak reference that
// over-retains is safe and one that under-retains is a use-after-free:
//
//  - KeepDuringJob (9.13) is REAL. `new WeakRef(t)` and `wr.deref()` both add
//    the target to the kept-objects list, a strongly rooted C++ vector, so a
//    target observed once in a job cannot vanish later in the same job —
//    `wr.deref() === wr.deref()` is true however many collections land between
//    them. The list is cleared at the microtask checkpoint (ClearKeptObjects),
//    which is the "no ECMAScript code is running" point 9.13 names.
//  - A FinalizationRegistry is collected when no user code or other roots reach
//    it. Its registration in the registry table is weak; when the registry
//    object is collected, its cell block and any uncalled pending cleanups are
//    cleared, and its block ID is recycled for future registries.
//  - Cleanup callbacks run from the JOB QUEUE (microtask.cpp's drain), never
//    from inside `collect()`. The sweep only moves a dead cell's held value
//    onto a pending list; user code runs later, with the heap consistent.

// A WeakRef's internal slots. [[WeakRefTarget]] must stay the last: it is the
// word GcLayout::WeakLast holds weakly.
namespace WeakRefSlot {
enum : uint32_t {
    Brand = 0,
    Target,
    kCount,
};
}  // namespace WeakRefSlot

// A FinalizationRegistry's. [[CleanupCallback]] is a strong Value; [[Cells]]
// is the index of its cell block in weak_ref.cpp's table — a double, because
// the cells are C++ memory and a malloc address in a scanned slot would be
// read as a Value.
namespace RegistrySlot {
enum : uint32_t {
    Brand = 0,
    CleanupCallback,
    CellBlockId,
    kCount,
};
}  // namespace RegistrySlot

// 4.2.1 CanBeHeldWeakly: an object, or a symbol that is not in the `Symbol.for`
// registry. Shared with builtin_weak_map.cpp's question by living here, so
// WeakMap and WeakRef cannot come to disagree about a symbol.
bool rtCanBeHeldWeakly(Value v);

// The slots past the brand in their EMPTY state — a cleared target, no
// callback, no cell block — written at allocation so that an object whose
// constructor body never ran (a derived constructor that returned without
// `super()`) derefs to `undefined` and refuses `register` by name rather than
// reading `undefined` bits as a number. The encoding is this file's, which is
// why the allocator in builtin_weak_ref.cpp asks rather than writes.
void rtWeakSlotsReset(Value obj, bool isRegistry);

// ---- WeakRef ----------------------------------------------------------------

// 26.1.1.1 steps 3-4 on an object `new` already allocated with the WeakRef
// slots: stores the target, registers the cell with the sweep, and adds the
// target to the kept-objects list. `self` arrives BRANDED — the caller
// (builtin_weak_ref.cpp) has checked — and rooted, because the kept-objects
// push is C++ memory but the caller's own argument reads were not.
void rtWeakRefInit(Rooted<Value>& self, Rooted<Value>& target);

// 26.1.3.2 WeakRef.prototype.deref: the target, or `undefined` once a
// collection has found it dead. Adds a live target to the kept-objects list, so
// the answer cannot change again within this job.
Value rtWeakRefDeref(Value weakRef);

// The slot pair decoded and nothing else — no KeepDuringJob — so a test can
// observe the sweep's clearing without the read itself retaining the target.
Value rtWeakRefTarget(Value weakRef);

// ---- FinalizationRegistry ---------------------------------------------------

// 26.2.1.1 steps 3-5 on an object `new` already allocated with the registry
// slots: stores the callback (checked by the caller) and claims a cell block.
void rtFinalizationRegistryInit(Rooted<Value>& self, Rooted<Value>& callback);

// 26.2.3.1 register(target, heldValue, unregisterToken). Every operand is
// rooted because the cell table's push can allocate nothing, but the callers'
// argument reads can.
void rtFinalizationRegister(Rooted<Value>& registry, Rooted<Value>& target,
                            Rooted<Value>& heldValue, Rooted<Value>& token);

// 26.2.3.2 unregister(unregisterToken): true when at least one cell was
// removed.
bool rtFinalizationUnregister(Rooted<Value>& registry, Rooted<Value>& token);

// ---- 9.13 AddToKeptObjects / ClearKeptObjects -------------------------------

void rtAddToKeptObjects(Value target);
void rtClearKeptObjects();

// ---- the cleanup-job queue -------------------------------------------------

// Is there a dead cell whose callback has not been called? The drain asks
// between microtask batches.
bool rtFinalizationCleanupPending();

// Run ONE cleanup job: 26.2.1.2 CleanupFinalizationRegistry over every cell the
// sweep has parked so far, in the order they died. A callback that throws is
// reported on stderr and the drain continues — a host job has no caller to
// propagate to, and one bad callback must not silence the rest.
void rtRunFinalizationCleanupJob();

// Test accessors, so a doctest can pin the sweep's arithmetic without going
// through the JS surface.
size_t rtKeptObjectCount();
size_t rtFinalizationCellCount();
size_t rtFinalizationPendingCount();

// The JS surface (builtin_weak_ref.cpp): the global ladder's two constructors,
// the identity check `extends` needs, the brand tests, and the allocators a
// construction and a test use. The `New*` forms build a complete object —
// allocated from the intrinsic's own instance shape and initialized — for a
// runtime path that produces one without a program's `new`.
Value rtWeakRefConstructor(const std::string& name);
const char* rtWeakRefConstructorName(Value fn);
bool rtIsWeakRefObject(Value v);
bool rtIsFinalizationRegistryObject(Value v);
Value rtNewWeakRef(Rooted<Value>& target);
Value rtNewFinalizationRegistry(Rooted<Value>& callback);
// Allocation only — the slots past the brand still `undefined` — for
// `rtAllocateNativeBaseInstance`, whose constructor body fills them.
Value rtNewWeakRefWithShape(class Shape* shape, bool isRegistry);

}  // namespace bronze::runtime
