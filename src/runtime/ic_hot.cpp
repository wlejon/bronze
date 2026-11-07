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

#include <cstdint>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/ic_hot.h"
#include "runtime/object.h"

namespace bronze {

namespace {

struct HotRange {
    uintptr_t lo;   // the first site
    uintptr_t hi;   // one past the last
    uint64_t* rows;
};

thread_local std::vector<HotRange> t_ranges;
// The range the last write found: a frame's fills come from one module.
thread_local size_t t_last = 0;

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

}  // namespace

void rtIcHotNoteWrite(const InlineCache* entry) noexcept {
    const auto at = reinterpret_cast<uintptr_t>(entry);
    const size_t n = t_ranges.size();
    if (n == 0) return;
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

void rtIcHotDropWithin(const void* begin, size_t size) noexcept {
    const auto lo = reinterpret_cast<uintptr_t>(begin);
    const uintptr_t hi = lo + size;
    std::erase_if(t_ranges, [lo, hi](const HotRange& r) { return r.lo < hi && r.hi > lo; });
    t_last = 0;
}

void rtIcHotClearThread() noexcept {
    t_ranges.clear();
    t_ranges.shrink_to_fit();
    t_last = 0;
}

size_t rtIcHotRangeCount() noexcept { return t_ranges.size(); }

}  // namespace runtime

}  // namespace bronze

extern "C" void bronze_register_ic_hot(uint64_t* icTable, uint64_t* hot, uint64_t siteCount) {
    using namespace bronze;
    if (!icTable || !hot || siteCount == 0) return;
    const auto lo = reinterpret_cast<uintptr_t>(icTable);
    const uintptr_t hi = lo + siteCount * BRONZE_ABI_IC_SITE_SIZE;
    // An instance registered again (a module whose init runs twice in one
    // instance) replaces its range rather than adding a second copy of it.
    runtime::rtIcHotDropWithin(icTable, static_cast<size_t>(hi - lo));
    t_ranges.push_back({lo, hi, hot});
    // The rows start as copies of what the sites hold now: zero for a fresh
    // instance, and whatever a realm copy's pristine snapshot carried.
    for (uintptr_t at = lo; at < hi; at += BRONZE_ABI_IC_SITE_SIZE) copyRow(t_ranges.back(), at);
}
