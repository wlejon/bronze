// The weak side of the collector: the tables that make a `WeakRef` weak and a
// `FinalizationRegistry` fire, and the post-collection sweep that is the only
// place either fact is decidable. weak_ref.h carries the design and the exact
// retention semantics; what is here is the machinery.
//
// The JS surface — the two constructors, `deref`, `register`, `unregister` — is
// builtin_weak_ref.cpp. This file has no opinion about property lookup.

#include "runtime/weak_ref.h"

#include <cstdio>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/heap.h"
#include "runtime/rt_state.h"
#include "runtime/symbol.h"

namespace bronze::runtime {

namespace {

// One FinalizationRegistry cell (26.2.1.1's [[Cells]] record). Two of its three
// references are WEAK and are therefore raw bits nothing traces; the held value
// is STRONG and is a Value the root source below visits.
struct Cell {
    uint64_t targetBits;
    uint64_t tokenBits;
    Value heldValue;
    bool hasToken;
};

// A dead cell whose callback has not run yet. The held value here is already a
// forwarded, post-collection Value: the root source visits this list too, and
// the sweep that fills it runs after the root sources have.
struct PendingCleanup {
    uint32_t blockId;
    Value heldValue;
};

// Every live WeakRef cell, by its CURRENT header address. Raw headers rather
// than Values, because the collector must not forward these: whether the
// WeakRef object itself survived is precisely what the sweep asks.
thread_local std::vector<HeapObjectHeader*> g_weakRefs;

// The registries, held STRONGLY (weak_ref.h says why), and their cells. Two
// parallel vectors indexed by the block id stored in the registry header: a
// registry is never removed, so the index is stable for the run.
thread_local std::vector<Value> g_registries;
thread_local std::vector<std::vector<Cell>> g_cells;

// A deque, because the two operations are push-back (from the sweep) and
// pop-front (from the job), and an entry must stay IN it until its callback has
// been called: the root source below is the only thing keeping a parked held
// value alive, and a callback allocates.
thread_local std::deque<PendingCleanup> g_pending;

// 9.13's [[KeptAlive]] list. A vector for the order (the root source walks it,
// and nothing about output may depend on a hash table's), plus a membership set
// so that a `deref` in a loop costs one hash rather than a scan of everything
// the job has touched. The set holds RAW BITS, which a collection invalidates —
// so the sweep rebuilds it, and a stale miss would only ever append a duplicate
// (more retention, never less).
thread_local std::vector<Value> g_kept;
thread_local std::unordered_set<uint64_t> g_keptSeen;

// Forward one weak slot, or clear it. False when the target died, which is the
// answer both callers branch on.
bool updateWeakSlot(Heap& heap, uint64_t& bits) {
    const Value v(bits);
    if (!v.isPointer()) return true;  // already cleared, or never a heap reference
    const auto address = static_cast<uintptr_t>(v.payload());
    if (address == 0) return true;
    HeapObjectHeader* live = heap.survivor_of(reinterpret_cast<HeapObjectHeader*>(address));
    if (!live) {
        bits = Value::fromUndefined().rawBits();
        return false;
    }
    bits = Value::fromTagAndPayload(v.tag(), reinterpret_cast<uintptr_t>(live)).rawBits();
    return true;
}

// The sweep. Runs inside `collect()`, after the copy phase and before the
// semispace swap, and allocates nothing on the bronze heap — both are
// `add_post_collection_hook`'s contract (heap.h).
void sweepWeakReferences() {
    Heap& heap = rtHeap();

    // WeakRef cells. A WeakRef whose OWN object died leaves the table; one that
    // survived has its target forwarded or cleared in the surviving copy, which
    // is the to-space object every later `deref` will read.
    size_t keep = 0;
    for (HeapObjectHeader* cell : g_weakRefs) {
        HeapObjectHeader* live = heap.survivor_of(cell);
        if (!live) continue;
        updateWeakSlot(heap, reinterpret_cast<WeakRefHeader*>(live)->targetBits);
        g_weakRefs[keep++] = live;
    }
    g_weakRefs.resize(keep);

    // Registry cells. A cell whose TARGET died is removed and its held value
    // parked for a cleanup job; a cell whose TOKEN died keeps working, with the
    // token cleared — 26.2.3.2 can no longer name it, which is what a dead
    // token means and not a reason to drop the registration.
    for (uint32_t block = 0; block < g_cells.size(); ++block) {
        std::vector<Cell>& cells = g_cells[block];
        size_t live_cells = 0;
        for (size_t i = 0; i < cells.size(); ++i) {
            Cell cell = cells[i];
            if (cell.hasToken && !updateWeakSlot(heap, cell.tokenBits)) {
                cell.hasToken = false;
            }
            if (!updateWeakSlot(heap, cell.targetBits)) {
                g_pending.push_back(PendingCleanup{block, cell.heldValue});
                continue;
            }
            cells[live_cells++] = cell;
        }
        cells.resize(live_cells);
    }

    // The kept-objects membership index is keyed on addresses every relocation
    // moves. The vector's Values were forwarded by the root source above, so
    // rebuilding from it is exact.
    g_keptSeen.clear();
    for (const Value& v : g_kept) g_keptSeen.insert(v.rawBits());
}

// Registered on FIRST USE and not at static initialization, for the reason
// microtask.cpp records at its own: `rtHeap()` is a static of another
// translation unit, and reaching it from this one's initializers is the
// cross-TU order fiasco.
void ensureWeakRegistries() {
    static thread_local const bool registered = [] {
        rtHeap().add_root_source([](const Heap::RootVisitor& visit) {
            for (Value& registry : g_registries) visit(registry);
            for (std::vector<Cell>& cells : g_cells) {
                for (Cell& cell : cells) visit(cell.heldValue);
            }
            for (PendingCleanup& p : g_pending) visit(p.heldValue);
            for (Value& kept : g_kept) visit(kept);
        });
        rtHeap().add_post_collection_hook(sweepWeakReferences);
        return true;
    }();
    (void)registered;
}

}  // namespace

// ---- construction -----------------------------------------------------------

WeakRefHeader* WeakRefHeader::create(Heap& heap, Rooted<Value>& target) {
    // `Tag::RawBytes`: the tag `payload_holds_values` refuses to scan, which is
    // the entire mechanism keeping this reference weak.
    HeapObjectHeader* raw = heap.allocate(sizeof(WeakRefHeader) - sizeof(HeapObjectHeader),
                                          Tag::RawBytes);
    auto* wr = reinterpret_cast<WeakRefHeader*>(raw);
    wr->header.flags = kFlags;
    // Read through the ROOT, after the allocation: it may have moved the target.
    wr->targetBits = target.get().rawBits();
    wr->reserved = 0;
    return wr;
}

FinalizationRegistryHeader* FinalizationRegistryHeader::create(Heap& heap,
                                                               Rooted<Value>& callback) {
    HeapObjectHeader* raw =
        heap.allocate(sizeof(FinalizationRegistryHeader) - sizeof(HeapObjectHeader), Tag::Object);
    auto* reg = reinterpret_cast<FinalizationRegistryHeader*>(raw);
    reg->header.flags = kFlags;
    reg->cleanupCallback = callback.get();
    reg->cellBlockId = Value::fromDouble(0.0);
    return reg;
}

// ---- CanBeHeldWeakly --------------------------------------------------------

bool rtCanBeHeldWeakly(Value v) {
    if (v.isObject()) return true;
    // A REGISTERED symbol can always be re-minted from its string, so nothing
    // can ever observe it become unreachable; 4.2.1 excludes exactly those.
    if (v.isSymbol()) return rtSymbolKeyFor(v).isUndefined();
    return false;
}

// ---- WeakRef ----------------------------------------------------------------

Value rtMakeWeakRef(Rooted<Value>& target) {
    ensureWeakRegistries();
    WeakRefHeader* wr = WeakRefHeader::create(rtHeap(), target);
    // No allocation between `create` and this push, so the address recorded is
    // the one the collector will next see — the rule embed's handle registry
    // states for its own table.
    g_weakRefs.push_back(&wr->header);
    // 26.1.1.1 step 4: constructing a WeakRef keeps its target alive for the
    // rest of the job, exactly as a `deref` does.
    rtAddToKeptObjects(target.get());
    return Value::fromObject(wr);
}

Value rtWeakRefDeref(Value weakRef) {
    const Value target = weakRef.asObject<WeakRefHeader>()->target();
    if (target.isUndefined()) return target;
    // 26.1.3.2 step 3 is WeakRefDeref, whose step 2 is AddToKeptObjects: once a
    // job has seen the target it may not stop seeing it, so a second `deref`
    // after any number of collections answers the same object.
    rtAddToKeptObjects(target);
    return target;
}

// ---- FinalizationRegistry ---------------------------------------------------

Value rtMakeFinalizationRegistry(Rooted<Value>& callback) {
    ensureWeakRegistries();
    Rooted<Value> reg{
        Value::fromObject(FinalizationRegistryHeader::create(rtHeap(), callback))};
    // The block is claimed AFTER the allocation and the registry is read back
    // through its root: `g_cells.emplace_back` is C++ memory and cannot move
    // the heap, but the allocation above already could have.
    const auto blockId = static_cast<uint32_t>(g_cells.size());
    g_cells.emplace_back();
    g_registries.push_back(reg.get());
    reg.get().asObject<FinalizationRegistryHeader>()->cellBlockId =
        Value::fromDouble(static_cast<double>(blockId));
    return reg.get();
}

void rtFinalizationRegister(Rooted<Value>& registry, Rooted<Value>& target,
                            Rooted<Value>& heldValue, Rooted<Value>& token) {
    ensureWeakRegistries();
    const uint32_t block = registry.get().asObject<FinalizationRegistryHeader>()->blockId();
    if (block >= g_cells.size()) {
        fatal("internal: a FinalizationRegistry whose cell block was never claimed");
    }
    Cell cell{};
    cell.targetBits = target.get().rawBits();
    cell.heldValue = heldValue.get();
    cell.hasToken = !token.get().isUndefined();
    cell.tokenBits = cell.hasToken ? token.get().rawBits() : 0;
    g_cells[block].push_back(cell);
}

bool rtFinalizationUnregister(Rooted<Value>& registry, Rooted<Value>& token) {
    ensureWeakRegistries();
    const uint32_t block = registry.get().asObject<FinalizationRegistryHeader>()->blockId();
    if (block >= g_cells.size()) return false;
    std::vector<Cell>& cells = g_cells[block];
    const uint64_t wanted = token.get().rawBits();
    bool removed = false;
    size_t keep = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        // SameValue on the token, which for an object or a symbol is identity —
        // and identity is address equality, which holds because every weak slot
        // in this table was forwarded by the sweep after the last collection.
        if (cells[i].hasToken && cells[i].tokenBits == wanted) {
            removed = true;
            continue;
        }
        cells[keep++] = cells[i];
    }
    cells.resize(keep);
    return removed;
}

// ---- 9.13 -------------------------------------------------------------------

void rtAddToKeptObjects(Value target) {
    ensureWeakRegistries();
    if (!target.isPointer()) return;
    if (!g_keptSeen.insert(target.rawBits()).second) return;
    g_kept.push_back(target);
}

void rtClearKeptObjects() {
    g_kept.clear();
    g_keptSeen.clear();
}

// ---- the cleanup-job queue -------------------------------------------------

bool rtFinalizationCleanupPending() { return !g_pending.empty(); }

void rtRunFinalizationCleanupJob() {
    // Exactly the cells that were parked when the job STARTED, and no more: a
    // callback allocates, so a collection inside one can park further cells, and
    // 26.2.1.2 makes those the next job's — otherwise a program whose callbacks
    // keep dropping objects would never leave this loop.
    //
    // Each entry leaves the deque only as its own callback is about to run, so
    // every held value not yet passed to a callback is still where the root
    // source can see it. That is the whole reason the batch is not swapped out.
    size_t remaining = g_pending.size();
    while (remaining-- > 0 && !g_pending.empty()) {
        const PendingCleanup entry = g_pending.front();
        g_pending.pop_front();
        if (entry.blockId >= g_registries.size()) continue;
        Rooted<Value> registry{g_registries[entry.blockId]};
        Rooted<Value> held{entry.heldValue};
        Rooted<Value> callback{
            registry.get().asObject<FinalizationRegistryHeader>()->cleanupCallback};
        if (!callback.get().isObject() ||
            callback.get().asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
            continue;
        }
        Value args[1] = {held.get()};
        bronze_dynamic_call(callback.get().rawBits(), Value::fromUndefined().rawBits(), 1,
                            reinterpret_cast<const uint64_t*>(args));
        if (rtExceptionPending()) {
            // 26.2.1.2 lets the throw propagate out of the job, and a host job
            // has no caller to propagate to. Reported on stderr — so an oracle
            // case's stdout stays byte-pinned around it — and cleared, because
            // the remaining callbacks are not this one's business.
            const std::string text = rtUncaughtText(Value(bronze_exception_cell));
            rtClearException();
            std::fflush(stdout);
            std::fprintf(stderr, "%s in a FinalizationRegistry cleanup callback\n", text.c_str());
            std::fflush(stderr);
        }
    }
}

size_t rtKeptObjectCount() { return g_kept.size(); }
size_t rtWeakRefCellCount() { return g_weakRefs.size(); }

size_t rtFinalizationCellCount() {
    size_t total = 0;
    for (const std::vector<Cell>& cells : g_cells) total += cells.size();
    return total;
}

size_t rtFinalizationPendingCount() { return g_pending.size(); }

}  // namespace bronze::runtime
