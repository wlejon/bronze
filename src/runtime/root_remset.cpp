// The remembered-set machinery root_remset.h describes; the tables themselves
// keep their sets beside them (rt_state.cpp, buffer_views.cpp, weak_ref.cpp).

#include "runtime/root_remset.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime/fatal.h"
#include "runtime/rt_state.h"

namespace bronze::runtime {

namespace {

// -1: not read yet. Per thread, like every other runtime switch, so a test
// can flip it without reaching into another thread's collections.
thread_local int8_t t_enabled = -1;
thread_local RootRemsetStats t_stats;

}  // namespace

bool rtRootRemsetEnabled() noexcept {
    if (t_enabled < 0) {
        const char* v = std::getenv("BRONZE_NO_ROOT_REMSET");
        t_enabled = (v && *v && std::strcmp(v, "0") != 0) ? 0 : 1;
    }
    return t_enabled != 0;
}

void rtSetRootRemsetEnabled(bool enabled) noexcept { t_enabled = enabled ? 1 : 0; }

RootRemsetStats& rtRootRemsetStats() noexcept { return t_stats; }

std::string rtRootRemsetMissText(const char* table, size_t index, uint64_t bits) {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "root remembered set: %s cell %zu holds the young reference 0x%016" PRIx64
                  " but is on no remembered set (a writer of the table missed it; "
                  "runtime/root_remset.h)",
                  table, index, bits);
    return buf;
}

void rtRootRemsetMissed(const char* table, size_t index, uint64_t bits) {
    fatal(rtRootRemsetMissText(table, index, bits).c_str());
}

void RootCellSet::visitMinor(brass::gc::Tracer& t) {
    // Sorted so a cell written many times between two minors is visited once;
    // the set is what was written since the last minor plus what was still
    // young at it, so this is small on a quiet heap.
    std::sort(cells_.begin(), cells_.end());
    cells_.erase(std::unique(cells_.begin(), cells_.end()), cells_.end());
    size_t kept = 0;
    for (uint64_t* cell : cells_) {
        t.visit(cell);
        if (t.is_young(cell)) cells_[kept++] = cell;
    }
    t_stats.rememberedVisits += cells_.size();
    cells_.resize(kept);
}

std::vector<uint64_t*> RootCellSet::sorted() const {
    std::vector<uint64_t*> out = cells_;
    std::sort(out.begin(), out.end());
    return out;
}

void RootIndexSet::normalize() {
    std::sort(indices_.begin(), indices_.end());
    indices_.erase(std::unique(indices_.begin(), indices_.end()), indices_.end());
}

bool RootIndexSet::contains(uint32_t index) const {
    return std::binary_search(indices_.begin(), indices_.end(), index);
}

std::string rtRootRemsetCheck() {
    const brass::gc::Heap& heap = rtHeap().gc();
    std::string miss = rtModuleRootsRemsetCheck(heap);
    if (miss.empty()) miss = rtBufferViewRemsetCheck(heap);
    if (miss.empty()) miss = rtWeakTableRemsetCheck(heap);
    return miss;
}

}  // namespace bronze::runtime
