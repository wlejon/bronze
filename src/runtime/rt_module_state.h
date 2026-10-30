#pragma once

// The per-thread module tables (rt_state.cpp owns them; only it and
// rt_module_remset.cpp, which keeps their remembered sets, include this):
// the function singletons and every module's registered root spans.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/root_remset.h"
#include "runtime/value.h"

namespace bronze::runtime {

struct SingletonEntry {
    bronze_fn_code code{nullptr};
    Value value{Value::fromUndefined()};
    uint64_t epoch{0};
    // The realm the singleton belongs to (Realm::slot), 0 for a native's: a
    // compiled function object carries its realm's mutable state (its own
    // properties, its `prototype`) and must not be shared between realms,
    // while the natives are the frozen intrinsics every realm shares.
    uint32_t realm{0};
};

// functionSingletonIndex's key: the code pointer and the realm.
struct SingletonKey {
    const void* code;
    uint32_t realm;
    bool operator==(const SingletonKey& o) const noexcept { return code == o.code && realm == o.realm; }
};
struct SingletonKeyHash {
    size_t operator()(const SingletonKey& k) const noexcept {
        return std::hash<const void*>()(k.code) ^ (static_cast<size_t>(k.realm) * 0x9E3779B97F4A7C15ull);
    }
};

// One {code, value} entry of a module's fn-singleton table. The layout is the
// ABI's, because generated code reads the two words inline; this declaration
// exists so the runtime's writes and the collector's walk go through the same
// description the fast path does.
struct FnSingletonSlot {
    bronze_fn_code code{nullptr};
    Value value{Value::fromUndefined()};
};
static_assert(sizeof(FnSingletonSlot) == BRONZE_ABI_FNSLOT_SIZE);
static_assert(offsetof(FnSingletonSlot, code) == BRONZE_ABI_FNSLOT_CODE_OFFSET);
static_assert(offsetof(FnSingletonSlot, value) == BRONZE_ABI_FNSLOT_VALUE_OFFSET);

static_assert(sizeof(Value) == sizeof(uint64_t),
              "module cache spans are registered as raw u64 cells");

template <typename Cell>
struct ModuleSpan {
    Cell* cells;
    uint64_t count;
    uint64_t epoch;
};

struct DynamicModule {
    uint64_t epoch{0};
    std::shared_ptr<void> owner;
    std::vector<const void*> codeTokens;
    std::atomic<bool> marked{false};
};

struct ThreadModuleState {
    std::vector<SingletonEntry> functionSingletons;
    std::unordered_map<SingletonKey, size_t, SingletonKeyHash> functionSingletonIndex;
    // Cells visited by every collection: `bronze_register_value_cells`'s. A
    // module registers its environment cell here, which its own code stores
    // into directly (ModuleEnvSet), and a host may register cells of its own
    // that it writes however it likes; neither goes through a runtime writer
    // that could keep a remembered set, and there is one such cell per module.
    std::vector<ModuleSpan<Value>> moduleValueCells;
    // Cells only runtime writers store into, which a minor visits through
    // `youngCells` (root_remset.h): the template cells
    // (`bronze_template_object`) and the method sites' env words (the latches
    // in rt_method_call.cpp).
    std::vector<ModuleSpan<Value>> moduleRememberedCells;
    // Remembered too: a fn slot's value is written by
    // `bronze_function_singleton`, a global cache cell by
    // `bronze_global_get_cached` (and emptied to the hole, which is no
    // reference, by `rtInvalidateGlobalCaches`).
    std::vector<ModuleSpan<FnSingletonSlot>> moduleFnSlots;
    std::vector<ModuleSpan<Value>> globalCacheSpans;
    uint64_t moduleEpochCounter = 0;
    std::vector<uint64_t> activeModuleEpochs;
    std::vector<std::unique_ptr<DynamicModule>> dynamicModules;
    std::unordered_map<const void*, DynamicModule*> codeToModule;
    std::vector<const void*> activeTokens;

    // The remembered sets: the cells of the remembered spans above that may
    // hold a young reference, and the function singletons that may.
    RootCellSet youngCells;
    RootIndexSet youngSingletons;
    // The remembered spans' address ranges, sorted, so a writer handed a bare
    // cell pointer can tell a registered cell from any other word (a stack
    // scratch site, a site no module registered). Rebuilt on the first ask
    // after a span is registered or dropped.
    std::vector<std::pair<uintptr_t, uintptr_t>> rememberedRanges;
    bool rememberedRangesStale = true;
};

// rt_module_remset.cpp: the module tables' remembered sets.
//
// A minor pass over the module tables: the always-visited spans whole, then
// the remembered sets.
void moduleTablesVisitMinor(ThreadModuleState& s, brass::gc::Tracer& t);
// After a remembered span is registered: its cells that already hold a young
// reference go on the set (a module's data is fresh at its init, so this
// normally finds none), and the range index is rebuilt at its next use.
void moduleRemsetSpanAdded(ThreadModuleState& s, const ModuleSpan<Value>& span);
void moduleRemsetFnSlotsAdded(ThreadModuleState& s, const ModuleSpan<FnSingletonSlot>& span);
// After spans are dropped (their memory may be freed next): every
// remembered cell outside the remaining spans leaves the set.
void moduleRemsetSpansDropped(ThreadModuleState& s);
// After function singletons are erased (indices shift): the set rebuilt.
void moduleRemsetSingletonsChanged(ThreadModuleState& s);
// A cell written by a runtime writer: on the set if it now holds a young
// reference and lies in a remembered span.
void moduleRemsetNoteCell(ThreadModuleState& s, uint64_t* cell) noexcept;
// The verification walk: the first young cell on no set, or "".
std::string moduleTablesRemsetCheck(ThreadModuleState& s, const brass::gc::Heap& heap);


}  // namespace bronze::runtime
