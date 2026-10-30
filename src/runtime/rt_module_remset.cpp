// The module tables' remembered sets (rt_module_state.h, root_remset.h): what
// a minor collection visits of every module's root spans and of the function
// singletons, and the bookkeeping that keeps the sets exact as spans come and
// go.

#include <algorithm>
#include <string>

#include "runtime/fatal.h"
#include "runtime/rt_module_state.h"

namespace bronze::runtime {

namespace {

uint64_t* cellWord(Value* cell) noexcept { return reinterpret_cast<uint64_t*>(cell); }

void rebuildRanges(ThreadModuleState& s) {
    s.rememberedRanges.clear();
    auto add = [&s](const void* begin, size_t bytes) {
        const auto lo = reinterpret_cast<uintptr_t>(begin);
        s.rememberedRanges.emplace_back(lo, lo + bytes);
    };
    for (const auto& span : s.moduleRememberedCells) add(span.cells, span.count * sizeof(Value));
    for (const auto& span : s.globalCacheSpans) add(span.cells, span.count * sizeof(Value));
    for (const auto& span : s.moduleFnSlots) add(span.cells, span.count * sizeof(FnSingletonSlot));
    std::sort(s.rememberedRanges.begin(), s.rememberedRanges.end());
    s.rememberedRangesStale = false;
}

bool inRememberedSpan(ThreadModuleState& s, const uint64_t* cell) {
    if (s.rememberedRangesStale) rebuildRanges(s);
    const auto a = reinterpret_cast<uintptr_t>(cell);
    // The last range starting at or below the cell: spans never overlap.
    auto it = std::upper_bound(s.rememberedRanges.begin(), s.rememberedRanges.end(),
                               std::make_pair(a, UINTPTR_MAX));
    if (it == s.rememberedRanges.begin()) return false;
    --it;
    return a >= it->first && a < it->second;
}

}  // namespace

void moduleTablesVisitMinor(ThreadModuleState& s, brass::gc::Tracer& t) {
    if (rtRootRemsetVerifying(t)) {
        const std::string miss = moduleTablesRemsetCheck(s, t.heap());
        if (!miss.empty()) fatal(miss.c_str());
    }
    for (const auto& span : s.moduleValueCells) {
        for (uint64_t i = 0; i < span.count; ++i) t.visit(cellWord(&span.cells[i]));
    }
    s.youngCells.visitMinor(t);
    s.youngSingletons.visitMinor(
        t, [&s](uint32_t i) { return cellWord(&s.functionSingletons[i].value); });
}

void moduleRemsetSpanAdded(ThreadModuleState& s, const ModuleSpan<Value>& span) {
    s.rememberedRangesStale = true;
    for (uint64_t i = 0; i < span.count; ++i) {
        if (gcHoldsYoung(span.cells[i])) s.youngCells.add(cellWord(&span.cells[i]));
    }
}

void moduleRemsetFnSlotsAdded(ThreadModuleState& s, const ModuleSpan<FnSingletonSlot>& span) {
    s.rememberedRangesStale = true;
    for (uint64_t i = 0; i < span.count; ++i) {
        if (span.cells[i].code && gcHoldsYoung(span.cells[i].value)) {
            s.youngCells.add(cellWord(&span.cells[i].value));
        }
    }
}

void moduleRemsetSpansDropped(ThreadModuleState& s) {
    rebuildRanges(s);
    s.youngCells.retainIf([&s](uint64_t* cell) { return inRememberedSpan(s, cell); });
}

void moduleRemsetSingletonsChanged(ThreadModuleState& s) {
    s.youngSingletons.rebuild(s.functionSingletons.size(), [&s](size_t i) {
        return gcHoldsYoung(s.functionSingletons[i].value);
    });
}

void moduleRemsetNoteCell(ThreadModuleState& s, uint64_t* cell) noexcept {
    if (!gcHoldsYoung(Value(*cell))) return;
    if (inRememberedSpan(s, cell)) s.youngCells.add(cell);
}

std::string moduleTablesRemsetCheck(ThreadModuleState& s, const brass::gc::Heap& heap) {
    RootRemsetStats& stats = rtRootRemsetStats();
    const std::vector<uint64_t*> remembered = s.youngCells.sorted();
    std::string miss;
    // False (and `miss` set) at the first young cell on no set.
    auto check = [&](const char* table, size_t index, uint64_t* cell) {
        ++stats.verifiedCells;
        if (!heap.holds_young(*cell)) return true;
        if (std::binary_search(remembered.begin(), remembered.end(), cell)) return true;
        miss = rtRootRemsetMissText(table, index, *cell);
        return false;
    };
    for (const auto& span : s.moduleRememberedCells) {
        for (uint64_t i = 0; i < span.count; ++i) {
            if (!check("a module's template or method-site env", i, cellWord(&span.cells[i]))) {
                return miss;
            }
        }
    }
    for (const auto& span : s.globalCacheSpans) {
        for (uint64_t i = 0; i < span.count; ++i) {
            if (!check("a module's global cache", i, cellWord(&span.cells[i]))) return miss;
        }
    }
    for (const auto& span : s.moduleFnSlots) {
        for (uint64_t i = 0; i < span.count; ++i) {
            if (span.cells[i].code && !check("a module's fn slot", i, cellWord(&span.cells[i].value))) {
                return miss;
            }
        }
    }
    s.youngSingletons.normalize();
    for (size_t i = 0; i < s.functionSingletons.size(); ++i) {
        const uint64_t bits = s.functionSingletons[i].value.rawBits();
        ++stats.verifiedCells;
        if (heap.holds_young(bits) && !s.youngSingletons.contains(static_cast<uint32_t>(i))) {
            return rtRootRemsetMissText("the function singletons", i, bits);
        }
    }
    return miss;
}

}  // namespace bronze::runtime
