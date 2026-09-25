// The weak side of the collector: the tables that make a `WeakRef` weak and a
// `FinalizationRegistry` fire, and the post-collection sweep that is the only
// place either fact is decidable. weak_ref.h carries the design and the exact
// retention semantics; what is here is the machinery.
//
// The JS surface — the two constructors, the prototypes, `deref`, `register`,
// `unregister` — is builtin_weak_ref.cpp. This file has no opinion about
// property lookup and never checks a brand: every object it is handed was
// vetted by that file, and every cell in its own table was put there by an
// init below.

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
#include "runtime/object.h"
#include "runtime/rt_state.h"
#include "runtime/symbol.h"

namespace bronze::runtime {

namespace {

// One FinalizationRegistry cell (26.2.1.1's [[Cells]] record). The target and
// the unregister token are WEAK references the tracer source below visits as
// such; the held value is STRONG.
struct Cell {
    uint64_t targetBits;
    uint64_t tokenBits;
    Value heldValue;
    bool hasToken;
};

// A dead cell whose callback has not run yet. The tracer source keeps its held
// value alive until the callback has been handed it.
struct PendingCleanup {
    uint32_t blockId;
    Value heldValue;
};

// The registries, held WEAKLY (weak_ref.h says why), and their cells. Two
// parallel vectors indexed by the block id stored in the registry's slot:
// slots whose registry was collected are recycled by subsequent inits.
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

// The target: the WeakRef's last internal slot, which the collector visits
// weakly and sets to `undefined` when the target dies (GcLayout::WeakLast).
Value readTarget(const ObjectHeader* obj) { return obj->internalSlot(WeakRefSlot::Target); }

void writeTarget(ObjectHeader* obj, Value target) { obj->setInternalSlot(WeakRefSlot::Target, target); }

// The block id, or UINT32_MAX for a registry whose init never ran — one a
// derived constructor returned without calling `super()`, whose slot still
// holds the negative sentinel `rtWeakSlotsReset` wrote. Tested as a double
// BEFORE the narrowing, which is undefined for a negative value.
uint32_t blockIdOf(Value registry) {
    const double id =
        registry.asObject<ObjectHeader>()->internalSlot(RegistrySlot::CellBlockId).asNumber();
    if (!(id >= 0.0)) return UINT32_MAX;
    return static_cast<uint32_t>(id);
}

// Every reference the tables hold, visited at every collection: the
// registries, targets and tokens WEAKLY (a dead one reads `undefined`
// afterwards), the held values and the kept-objects list strongly.
void traceWeakTables(brass::gc::Tracer& t) {
    const uint64_t cleared = Value::fromUndefined().rawBits();
    for (size_t b = 0; b < g_cells.size(); ++b) {
        if (b < g_registries.size()) {
            if (g_registries[b].isUndefined()) continue;
            t.visit_weak(reinterpret_cast<uint64_t*>(&g_registries[b]), cleared);
        }
        for (Cell& cell : g_cells[b]) {
            t.visit_weak(&cell.targetBits, cleared);
            if (cell.hasToken) t.visit_weak(&cell.tokenBits, cleared);
            t.visit(reinterpret_cast<uint64_t*>(&cell.heldValue));
        }
    }
    for (PendingCleanup& p : g_pending) t.visit(reinterpret_cast<uint64_t*>(&p.heldValue));
    for (Value& kept : g_kept) t.visit(reinterpret_cast<uint64_t*>(&kept));
}

// The sweep, after the collector has settled every weak reference above. It
// allocates nothing on the bronze heap (`add_post_collection_hook`'s
// contract, heap.h).
void sweepWeakReferences() {
    // Registry cells. A registry that died takes its cells and its pending
    // cleanups with it; a cell whose target died parks its held value.
    for (uint32_t block = 0; block < g_cells.size(); ++block) {
        if (block >= g_registries.size() || g_registries[block].isUndefined()) {
            g_cells[block].clear();
            g_cells[block].shrink_to_fit();
            continue;
        }
        std::vector<Cell>& cells = g_cells[block];
        size_t live_cells = 0;
        for (size_t i = 0; i < cells.size(); ++i) {
            Cell cell = cells[i];
            if (cell.hasToken && Value(cell.tokenBits).isUndefined()) cell.hasToken = false;
            if (Value(cell.targetBits).isUndefined()) {
                g_pending.push_back(PendingCleanup{block, cell.heldValue});
                continue;
            }
            cells[live_cells++] = cell;
        }
        cells.resize(live_cells);
    }

    size_t keep_pending = 0;
    for (size_t i = 0; i < g_pending.size(); ++i) {
        if (g_pending[i].blockId < g_registries.size() &&
            !g_registries[g_pending[i].blockId].isUndefined()) {
            g_pending[keep_pending++] = g_pending[i];
        }
    }
    g_pending.resize(keep_pending);

    // The kept-objects membership index is keyed on addresses a collection
    // moves. The vector's Values were updated by the tracer source above, so
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
        rtHeap().add_tracer_source(traceWeakTables);
        rtHeap().add_post_collection_hook(sweepWeakReferences);
        return true;
    }();
    (void)registered;
}

}  // namespace

// ---- CanBeHeldWeakly --------------------------------------------------------

bool rtCanBeHeldWeakly(Value v) {
    if (v.isObject()) return true;
    // A REGISTERED symbol can always be re-minted from its string, so nothing
    // can ever observe it become unreachable; 4.2.1 excludes exactly those.
    if (v.isSymbol()) return rtSymbolKeyFor(v).isUndefined();
    return false;
}

void rtWeakSlotsReset(Value obj, bool isRegistry) {
    auto* o = obj.asObject<ObjectHeader>();
    if (isRegistry) {
        o->setInternalSlot(RegistrySlot::CleanupCallback, Value::fromUndefined());
        o->setInternalSlot(RegistrySlot::CellBlockId, Value::fromDouble(-1.0));
        return;
    }
    writeTarget(o, Value::fromUndefined());
}

// ---- WeakRef ----------------------------------------------------------------

void rtWeakRefInit(Rooted<Value>& self, Rooted<Value>& target) {
    ensureWeakRegistries();
    writeTarget(self.get().asObject<ObjectHeader>(), target.get());
    // 26.1.1.1 step 4: constructing a WeakRef keeps its target alive for the
    // rest of the job, exactly as a `deref` does.
    rtAddToKeptObjects(target.get());
}

Value rtWeakRefTarget(Value weakRef) { return readTarget(weakRef.asObject<ObjectHeader>()); }

Value rtWeakRefDeref(Value weakRef) {
    const Value target = readTarget(weakRef.asObject<ObjectHeader>());
    if (target.isUndefined()) return target;
    // 26.1.3.2 step 3 is WeakRefDeref, whose step 2 is AddToKeptObjects: once a
    // job has seen the target it may not stop seeing it, so a second `deref`
    // after any number of collections answers the same object.
    rtAddToKeptObjects(target);
    return target;
}

// ---- FinalizationRegistry ---------------------------------------------------

void rtFinalizationRegistryInit(Rooted<Value>& self, Rooted<Value>& callback) {
    ensureWeakRegistries();
    uint32_t blockId = UINT32_MAX;
    for (size_t i = 0; i < g_registries.size(); ++i) {
        if (g_registries[i].isUndefined()) {
            blockId = static_cast<uint32_t>(i);
            g_registries[i] = self.get();
            g_cells[i].clear();
            break;
        }
    }
    if (blockId == UINT32_MAX) {
        blockId = static_cast<uint32_t>(g_cells.size());
        g_cells.emplace_back();
        g_registries.push_back(self.get());
    }
    auto* obj = self.get().asObject<ObjectHeader>();
    obj->setInternalSlot(RegistrySlot::CleanupCallback, callback.get());
    obj->setInternalSlot(RegistrySlot::CellBlockId,
                         Value::fromDouble(static_cast<double>(blockId)));
}

void rtFinalizationRegister(Rooted<Value>& registry, Rooted<Value>& target,
                            Rooted<Value>& heldValue, Rooted<Value>& token) {
    ensureWeakRegistries();
    const uint32_t block = blockIdOf(registry.get());
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
    const uint32_t block = blockIdOf(registry.get());
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
        if (g_registries[entry.blockId].isUndefined()) continue;
        Rooted<Value> registry{g_registries[entry.blockId]};
        Rooted<Value> held{entry.heldValue};
        Rooted<Value> callback{registry.get().asObject<ObjectHeader>()->internalSlot(
            RegistrySlot::CleanupCallback)};
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
            const std::string text = rtUncaughtText(Value(bronze_tls_block_addr()->exception_cell));
            rtClearException();
            std::fflush(stdout);
            std::fprintf(stderr, "%s in a FinalizationRegistry cleanup callback\n", text.c_str());
            std::fflush(stderr);
        }
    }
}

size_t rtKeptObjectCount() { return g_kept.size(); }

size_t rtFinalizationCellCount() {
    size_t total = 0;
    for (size_t b = 0; b < g_cells.size(); ++b) {
        if (b < g_registries.size() && g_registries[b].isUndefined()) continue;
        total += g_cells[b].size();
    }
    return total;
}

size_t rtFinalizationPendingCount() { return g_pending.size(); }

}  // namespace bronze::runtime
