// The packed way-0 copies of a module's property sites (bronze_abi_layout.h,
// "the HOT array"): where each instance's rows are, and the one write that
// keeps a row equal to its site's way 0.
//
// A site's 96 bytes are four ways; generated code's guards read way 0's shape
// and slot words first and, on the scenes that matter, almost only those. A
// frame walks thousands of sites once each, so the line a guard misses on
// held one site and 72 bytes it never read. The rows put those 16 bytes of
// every site in site order, four to a line, and the guards read them instead.
//
// The table stays the truth. Every runtime path fills an entry through
// InlineCache::fill*, and each fill reports here; a fill of a way-0 entry of
// a registered table copies the two words to the row. Everything else (a
// later way, an element cache's entry, a scratch site on the stack, a table
// whose module never registered rows, as an image compiled before them)
// costs one range lookup and changes nothing, and such a module's generated
// code reads way 0 itself, as it always did.
//
// Ranges are per (thread, realm) instance, exactly as the tables are
// (module_instance.cpp): registered by each instance's init, forgotten with
// the instance's cells (rtDropModuleCellsWithin) or the thread.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/ic_harvest.h"
#include "runtime/ic_hot.h"
#include "runtime/object.h"

namespace bronze {

namespace {

struct HotRange {
    uintptr_t lo;   // the first site
    uintptr_t hi;   // one past the last
    uint64_t* rows;
    // The module's IC identity record (bronze_abi_layout.h): what a harvest
    // of this range is filed under (ic_harvest.h).
    const uint64_t* identity;
};

thread_local std::vector<HotRange> t_ranges;
// The range the last write found: a frame's fills come from one module.
thread_local size_t t_last = 0;
// Envelope [t_minLo, t_maxHi) across all live `t_ranges` on this thread
// (`UINTPTR_MAX` / `0` when empty), so stack-allocated scratch sites and
// unharvested tables reject in a single comparison without touching the vector.
thread_local uintptr_t t_minLo = UINTPTR_MAX;
thread_local uintptr_t t_maxHi = 0;

// A module instance's METHOD ROWS (bronze_abi_layout.h): its table's range,
// the rows, and the method sites in the sorted order the rows follow.
struct MethodRange {
    uintptr_t lo;
    uintptr_t hi;
    uint64_t* rows;
    std::vector<uint64_t> sites;
};

thread_local std::vector<MethodRange> t_methodRanges;
thread_local size_t t_methodLast = 0;
thread_local uintptr_t t_methodMinLo = UINTPTR_MAX;
thread_local uintptr_t t_methodMaxHi = 0;

void recomputeHotBounds() noexcept {
    uintptr_t minLo = UINTPTR_MAX;
    uintptr_t maxHi = 0;
    for (const HotRange& r : t_ranges) {
        if (r.lo < minLo) minLo = r.lo;
        if (r.hi > maxHi) maxHi = r.hi;
    }
    t_minLo = minLo;
    t_maxHi = maxHi;
}

void recomputeMethodBounds() noexcept {
    uintptr_t minLo = UINTPTR_MAX;
    uintptr_t maxHi = 0;
    for (const MethodRange& r : t_methodRanges) {
        if (r.lo < minLo) minLo = r.lo;
        if (r.hi > maxHi) maxHi = r.hi;
    }
    t_methodMinLo = minLo;
    t_methodMaxHi = maxHi;
}

static_assert(BRONZE_ABI_METHOD_ROW_SIZE == 4 * sizeof(uint64_t) && BRONZE_ABI_METHOD_ROW_SHAPE_OFFSET == 0 &&
                  BRONZE_ABI_METHOD_ROW_CODE_OFFSET == 8 && BRONZE_ABI_METHOD_ROW_ARITY_OFFSET == 16 &&
                  BRONZE_ABI_METHOD_ROW_EPOCH_OFFSET == 24,
              "a method row is way 0's shape, code, arity and epoch words, in that order");

void copyMethodRow(const MethodRange& r, uintptr_t at) noexcept {
    const uintptr_t off = at - r.lo;
    if (off % BRONZE_ABI_IC_SITE_SIZE != 0) return;
    const uint64_t site = off / BRONZE_ABI_IC_SITE_SIZE;
    const auto it = std::lower_bound(r.sites.begin(), r.sites.end(), site);
    if (it == r.sites.end() || *it != site) return;  // not a method site
    const auto* words = reinterpret_cast<const uint64_t*>(at);
    uint64_t* row = r.rows + static_cast<size_t>(it - r.sites.begin()) * 4;
    row[0] = words[0];
    row[1] = words[BRONZE_ABI_METHOD_IC_CODE_WORD];
    row[2] = words[BRONZE_ABI_METHOD_IC_ARITY_WORD];
    row[3] = words[BRONZE_ABI_METHOD_IC_EPOCH_WORD];
}

static_assert(BRONZE_ABI_IC_HOT_ENTRY_SIZE == 2 * sizeof(uint64_t), "a row is the shape and slot words");
static_assert(BRONZE_ABI_IC_HOT_SHAPE_OFFSET == 0 && BRONZE_ABI_IC_HOT_SLOT_OFFSET == 8, "in that order");
static_assert(BRONZE_ABI_IC_SHAPE_OFFSET == 0 && BRONZE_ABI_IC_SLOTWORD_OFFSET == 8,
              "a row copies an entry's first two words");

void copyRow(const HotRange& r, uintptr_t at) noexcept {
    const uintptr_t off = at - r.lo;
    if (off % BRONZE_ABI_IC_SITE_SIZE != 0) return;  // a later way
    const auto* words = reinterpret_cast<const uint64_t*>(at);
    uint64_t* row = r.rows + (off / BRONZE_ABI_IC_SITE_SIZE) * 2;
    row[0] = words[0];
    row[1] = words[1];
}

// One range to the harvester, with its method sites (the method-row range
// that starts at the same table, if the module has method sites).
void foldRange(const HotRange& r) {
    if (r.identity == nullptr) return;
    const std::vector<uint64_t>* methods = nullptr;
    for (const MethodRange& m : t_methodRanges) {
        if (m.lo == r.lo) methods = &m.sites;
    }
    runtime::rtIcHarvestFold(reinterpret_cast<const uint64_t*>(r.lo), (r.hi - r.lo) / BRONZE_ABI_IC_SITE_SIZE,
                             r.identity, methods);
}

}  // namespace

void rtIcHotNoteWrite(const InlineCache* entry) noexcept {
    const auto at = reinterpret_cast<uintptr_t>(entry);
    if (at < t_minLo || at >= t_maxHi) return;
    const size_t n = t_ranges.size();
    if (t_last < n) {
        const HotRange& r = t_ranges[t_last];
        if (at >= r.lo && at < r.hi) {
            copyRow(r, at);
            return;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const HotRange& r = t_ranges[i];
        if (at >= r.lo && at < r.hi) {
            t_last = i;
            copyRow(r, at);
            return;
        }
    }
}

namespace runtime {

void rtMethodRowNoteWrite(const uint64_t* icEntry) noexcept {
    const auto at = reinterpret_cast<uintptr_t>(icEntry);
    if (at < t_methodMinLo || at >= t_methodMaxHi) return;
    const size_t n = t_methodRanges.size();
    if (t_methodLast < n) {
        const MethodRange& r = t_methodRanges[t_methodLast];
        if (at >= r.lo && at < r.hi) {
            copyMethodRow(r, at);
            return;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const MethodRange& r = t_methodRanges[i];
        if (at >= r.lo && at < r.hi) {
            t_methodLast = i;
            copyMethodRow(r, at);
            return;
        }
    }
}

void rtIcHotDropWithin(const void* begin, size_t size) noexcept {
    const auto lo = reinterpret_cast<uintptr_t>(begin);
    const uintptr_t hi = lo + size;
    // The caches are about to be forgotten or put back as they were (a
    // reload, a realm's end): a module being harvested keeps what they hold.
    if (rtIcHarvestArmed()) {
        for (const HotRange& r : t_ranges) {
            if (r.lo < hi && r.hi > lo) foldRange(r);
        }
    }
    std::erase_if(t_ranges, [lo, hi](const HotRange& r) { return r.lo < hi && r.hi > lo; });
    std::erase_if(t_methodRanges, [lo, hi](const MethodRange& r) { return r.lo < hi && r.hi > lo; });
    t_last = 0;
    t_methodLast = 0;
    recomputeHotBounds();
    recomputeMethodBounds();
}

void rtIcHotClearThread() noexcept {
    t_ranges.clear();
    t_ranges.shrink_to_fit();
    t_last = 0;
    t_minLo = UINTPTR_MAX;
    t_maxHi = 0;
    t_methodRanges.clear();
    t_methodRanges.shrink_to_fit();
    t_methodLast = 0;
    t_methodMinLo = UINTPTR_MAX;
    t_methodMaxHi = 0;
}

size_t rtMethodRowRangeCount() noexcept { return t_methodRanges.size(); }

size_t rtIcHotRangeCount() noexcept { return t_ranges.size(); }

void rtIcHotFoldLiveRanges() {
    for (const HotRange& r : t_ranges) foldRange(r);
}

}  // namespace runtime

}  // namespace bronze

extern "C" void bronze_register_ic_hot(uint64_t* icTable, uint64_t* hot, uint64_t siteCount,
                                       const uint64_t* identity) {
    using namespace bronze;
    if (!icTable || !hot || siteCount == 0) return;
    const auto lo = reinterpret_cast<uintptr_t>(icTable);
    const uintptr_t hi = lo + siteCount * BRONZE_ABI_IC_SITE_SIZE;
    // An instance registered again (a module whose init runs twice in one
    // instance) replaces its range rather than adding a second copy of it.
    runtime::rtIcHotDropWithin(icTable, static_cast<size_t>(hi - lo));
    t_ranges.push_back({lo, hi, hot, identity});
    recomputeHotBounds();
    // A standalone program built with `--harvest-out` names its file here.
    runtime::rtIcHarvestNoteIdentity(identity);
    // The rows start as copies of what the sites hold now: zero for a fresh
    // instance, and whatever a realm copy's pristine snapshot carried.
    for (uintptr_t at = lo; at < hi; at += BRONZE_ABI_IC_SITE_SIZE) copyRow(t_ranges.back(), at);
}

extern "C" void bronze_register_method_ic_rows(uint64_t* icTable, uint64_t* rows, const uint64_t* siteIndexes,
                                               uint64_t count) {
    using namespace bronze;
    if (!icTable || !rows || !siteIndexes || count == 0) return;
    MethodRange r{reinterpret_cast<uintptr_t>(icTable), 0, rows,
                  std::vector<uint64_t>(siteIndexes, siteIndexes + count)};
    // The list is the lowering's sorted one (il.h methodIcSites); sorted here
    // all the same, since a row's index is its site's rank.
    std::sort(r.sites.begin(), r.sites.end());
    r.hi = r.lo + (r.sites.back() + 1) * BRONZE_ABI_IC_SITE_SIZE;
    std::erase_if(t_methodRanges, [&r](const MethodRange& m) { return m.lo < r.hi && m.hi > r.lo; });
    t_methodLast = 0;
    t_methodRanges.push_back(std::move(r));
    recomputeMethodBounds();
    // The rows start as copies of what the sites hold now.
    const MethodRange& added = t_methodRanges.back();
    for (uint64_t site : added.sites) copyMethodRow(added, added.lo + site * BRONZE_ABI_IC_SITE_SIZE);
}
