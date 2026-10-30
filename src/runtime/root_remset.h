#pragma once

// Remembered sets for the runtime's off-heap root tables.
//
// A minor collection used to visit every cell of every root table the runtime
// keeps outside the heap: each module's template cells (a thousand and more
// per module), its method-site env words, its global caches and fn slots, the
// function singletons, the root shapes' and user prototype shapes'
// prototypes, the buffer-view rows and the FinalizationRegistry cells. Nearly
// all of them are written once and hold an old object (or nothing) from the
// minor after that on, so on a quiet heap that walk was almost the whole
// pause.
//
// A minor collection only has to visit a cell that may name a YOUNG object.
// So each of those tables keeps the set of its cells that may: a writer that
// stores a young reference (gcHoldsYoung, heap.h) puts the cell on the set, a
// minor visits only the set, and a cell leaves the set once a minor finds it
// no longer young afterwards (promoted, or overwritten with something old).
// That is brass's root-source contract (brass docs/gc_contract.md, "Roots of a
// Collection"); a full collection and a verification walk still visit every
// cell, and a full collection's first phase promotes everything young, which
// empties every set.
//
// Recording only YOUNG stores, rather than every store, is what keeps the
// writers cheap: the one writer that is ever warm (a method site relatched on
// an IC miss) pays a range test, and finds the cell's table only on the rare
// store of a young env.
//
// A missed writer would leave a young object in a cell the minor never
// forwards. So under brass's verification (BRASS_GC_VERIFY=1, or
// BRONZE_HEAP_VERIFY=1) a minor also reads every cell it skips and stops the
// process naming the table and cell if one holds a young address.
//
// BRONZE_NO_ROOT_REMSET=1 makes a minor visit every cell again (the sets are
// still kept, and visited too, so the switch can flip mid-run), for A/B.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/heap.h"

namespace bronze::runtime {

// Whether minor collections visit only the remembered cells (the default) or
// every cell (BRONZE_NO_ROOT_REMSET=1). Per thread; read from the
// environment at the thread's first ask.
bool rtRootRemsetEnabled() noexcept;
void rtSetRootRemsetEnabled(bool enabled) noexcept;

// Whether a root source called with `t` may visit only its remembered cells:
// a minor pass (which a full collection's first phase is too) with the sets
// on.
inline bool rtRootRemsetPass(const brass::gc::Tracer& t) noexcept {
    return t.purpose() == brass::gc::Tracer::Purpose::Minor && rtRootRemsetEnabled();
}

// Whether a minor pass with `t` must also check the cells it skips.
inline bool rtRootRemsetVerifying(const brass::gc::Tracer& t) noexcept {
    return t.heap().verifying();
}

// The thread's counters, for tests and the A/B: cumulative, never reset.
struct RootRemsetStats {
    // Cells visited from a remembered set by minor passes.
    uint64_t rememberedVisits = 0;
    // Skipped cells a verifying minor pass read.
    uint64_t verifiedCells = 0;
};
RootRemsetStats& rtRootRemsetStats() noexcept;

// Stops the process: `table`'s cell `index` holds the young word `bits` but
// is on no remembered set, so a writer of that table missed the set.
[[noreturn]] void rtRootRemsetMissed(const char* table, size_t index, uint64_t bits);
// The same report as text, for a checker that returns it rather than stops.
std::string rtRootRemsetMissText(const char* table, size_t index, uint64_t bits);

// The remembered set of a table whose cells never move (module data, a row
// held by index in a stable vector is not one: see RootIndexSet).
class RootCellSet {
public:
    void add(uint64_t* cell) {
        if (cells_.empty() || cells_.back() != cell) cells_.push_back(cell);
    }
    // A minor pass: every cell on the set once, keeping those still young.
    void visitMinor(brass::gc::Tracer& t);
    // Keeps only the cells `keep` accepts (a span dropped, its memory going).
    template <typename Keep>
    void retainIf(Keep keep) {
        std::erase_if(cells_, [&keep](uint64_t* cell) { return !keep(cell); });
    }
    void clear() noexcept { cells_.clear(); }
    size_t size() const noexcept { return cells_.size(); }
    // The set sorted, for a verifying pass's membership tests.
    std::vector<uint64_t*> sorted() const;

private:
    std::vector<uint64_t*> cells_;
};

// The remembered set of a table held as a vector whose entries move when it
// grows: the entries' indices. Whoever erases from the table rebuilds the set
// (rebuild), since every index after the erased one shifts.
class RootIndexSet {
public:
    void add(uint32_t index) {
        if (indices_.empty() || indices_.back() != index) indices_.push_back(index);
    }
    // A minor pass: `slotAt(i)` is entry i's cell; strong visits, keeping the
    // entries still young.
    template <typename SlotAt>
    void visitMinor(brass::gc::Tracer& t, SlotAt slotAt) {
        normalize();
        size_t kept = 0;
        for (uint32_t i : indices_) {
            uint64_t* slot = slotAt(i);
            t.visit(slot);
            if (t.is_young(slot)) indices_[kept++] = i;
        }
        rtRootRemsetStats().rememberedVisits += indices_.size();
        indices_.resize(kept);
    }
    // The set rebuilt from the table's `count` entries: those `young(i)` says
    // hold a young reference.
    template <typename Young>
    void rebuild(size_t count, Young young) {
        indices_.clear();
        for (size_t i = 0; i < count; ++i) {
            if (young(i)) indices_.push_back(static_cast<uint32_t>(i));
        }
    }
    // Sorted and without repeats, for a visit or a membership test.
    void normalize();
    bool contains(uint32_t index) const;  // after normalize()
    void clear() noexcept { indices_.clear(); }
    size_t size() const noexcept { return indices_.size(); }
    const std::vector<uint32_t>& indices() const noexcept { return indices_; }

private:
    std::vector<uint32_t> indices_;
};

// Every remembered table's check, as a minor pass under verification runs
// it: the first cell that holds a young word and is on no set, described, or
// "" when there is none. For tests, which write a cell behind the writers'
// back and expect it named; a collection stops the process instead.
std::string rtRootRemsetCheck();
// The per-file halves of it (rt_state.cpp, buffer_views.cpp, weak_ref.cpp).
std::string rtModuleRootsRemsetCheck(const brass::gc::Heap& heap);
std::string rtBufferViewRemsetCheck(const brass::gc::Heap& heap);
std::string rtWeakTableRemsetCheck(const brass::gc::Heap& heap);

// A method site's two env words (bronze_abi.h's method-site contract,
// BRONZE_ABI_METHOD_IC_ENV_WORD and its way-1 copy), after a latch wrote
// them: each one that now holds a young env goes on its module's set.
void rtNoteMethodIcEnvWords(uint64_t* icEntry) noexcept;
// A registered root cell (a template cell, a global cache cell, a fn slot's
// value) a runtime writer has just stored into: on its module's set when it
// now holds a young reference. A cell in no remembered span is not one of
// those tables' (an always-visited value cell, or no root at all) and is left
// alone.
void rtNoteRootCellWrite(uint64_t* cell) noexcept;

}  // namespace bronze::runtime
