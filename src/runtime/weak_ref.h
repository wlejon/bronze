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
// bronze's collector is a MOVING SEMISPACE (runtime/heap.cpp), which decides
// what these can be. Its payload scan forwards every `Value` in an object's
// payload, so a weak slot cannot BE a payload Value — it would be traced, and a
// traced weak reference is a strong one under another name. So:
//
//  - a WeakRef's target is `uint64_t` raw bits under a `Tag::RawBytes` header,
//    which is the tag `payload_holds_values` refuses to scan; and
//  - a FinalizationRegistry's cells live in a C++ table in weak_ref.cpp, whose
//    STRONG halves (the held value, the callback) are visited by a registered
//    root source and whose WEAK halves (the target, the unregister token) are
//    raw bits nothing traces.
//
// What makes them weak rather than merely untraced is the post-collection
// sweep. `Heap::add_post_collection_hook` runs it inside `collect()`, after the
// copy phase and before the semispace swap — the one moment liveness of an
// arbitrary heap pointer is decidable, because a survivor's old header reads
// `Tag::Forwarded` and a dead object's still reads the tag it was allocated
// with. The sweep FORWARDS every weak slot whose target survived and CLEARS
// every one whose target did not, which is the whole of the design.
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
//  - A FinalizationRegistry, once constructed, is retained for the rest of the
//    run: the registry table holds it STRONGLY. The specification lets a host
//    collect a registry (and then never call its callbacks); bronze keeps it,
//    which over-retains one object per registry and buys a cleanup queue that
//    cannot lose a callback to the death of its own registry. Over-retention
//    is the safe direction and the one deliberately taken.
//  - Cleanup callbacks run from the JOB QUEUE (microtask.cpp's drain), never
//    from inside `collect()`. The sweep only moves a dead cell's held value
//    onto a pending list; user code runs later, with the heap consistent.

// A WeakRef. `Tag::RawBytes`, so the collector copies these bytes and never
// reads them as Values — see the header comment.
struct WeakRefHeader {
    HeapObjectHeader header;
    // The target Value's raw bits, or `undefined`'s once the target has died.
    // Deliberately NOT a `Value` field: the type is what a future reader would
    // "fix", and a `Value` here under any tag but RawBytes is a WeakRef that
    // keeps its target alive forever.
    uint64_t targetBits;
    // Zero. Present so the payload is a whole number of 8-byte words and the
    // struct's size is stated rather than inferred from padding.
    uint64_t reserved;

    static constexpr uint16_t kFlags = HeapKind::WeakRef;

    Value target() const noexcept { return Value(targetBits); }

    // The target arrives ROOTED because this allocates, and it is read back
    // through the root afterwards for the usual reason.
    static WeakRefHeader* create(Heap& heap, Rooted<Value>& target);
};

static_assert(sizeof(WeakRefHeader) == 24,
              "a WeakRef is a header plus two payload words; a third would change the size the "
              "collector copies");

// A FinalizationRegistry. An ordinary `Tag::Object`, because both of its
// payload words are strong: the cleanup callback, and the index of its cell
// block in weak_ref.cpp's table. The WEAK halves are in that table and not
// here, which is exactly why this one can be a scanned object.
struct FinalizationRegistryHeader {
    HeapObjectHeader header;
    Value cleanupCallback;
    // A double. The cells are C++ memory, so what lives here is an index and
    // not a pointer — a pointer in a scanned payload word would be read as a
    // Value, and a malloc address's top sixteen bits are zero, which reads as a
    // small double and would be silently "forwarded" as nothing.
    Value cellBlockId;

    static constexpr uint16_t kFlags = HeapKind::FinalizationRegistry;

    uint32_t blockId() const noexcept { return static_cast<uint32_t>(cellBlockId.asNumber()); }

    static FinalizationRegistryHeader* create(Heap& heap, Rooted<Value>& callback);
};

static_assert(sizeof(FinalizationRegistryHeader) == 24,
              "a FinalizationRegistry is a header, its callback and its block id");

// 4.2.1 CanBeHeldWeakly: an object, or a symbol that is not in the `Symbol.for`
// registry. Shared with builtin_weak_map.cpp's copy of the question by living
// here, so WeakMap and WeakRef cannot come to disagree about a symbol.
bool rtCanBeHeldWeakly(Value v);

// ---- WeakRef ----------------------------------------------------------------

// 26.1.1.1. Registers the new cell with the sweep and adds the target to the
// kept-objects list (step 4). ALLOCATES.
Value rtMakeWeakRef(Rooted<Value>& target);

// 26.1.3.2 WeakRef.prototype.deref: the target, or `undefined` once a
// collection has found it dead. Adds a live target to the kept-objects list, so
// the answer cannot change again within this job.
Value rtWeakRefDeref(Value weakRef);

// ---- FinalizationRegistry ---------------------------------------------------

// 26.2.1.1. The callback is not checked here; the caller has. ALLOCATES.
Value rtMakeFinalizationRegistry(Rooted<Value>& callback);

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
size_t rtWeakRefCellCount();
size_t rtFinalizationCellCount();
size_t rtFinalizationPendingCount();

// The JS surface (builtin_weak_ref.cpp): the global ladder's two constructors,
// the member tables the property path answers from, and the identity check
// `extends` needs.
Value rtWeakRefConstructor(const std::string& name);
const char* rtWeakRefConstructorName(Value fn);
Value rtWeakRefMember(Value self, const std::string& key);
bool rtWeakRefHasMember(Value self, const std::string& key);
void rtCheckWeakRefMember(bool isRegistry, const std::string& key);

}  // namespace bronze::runtime
