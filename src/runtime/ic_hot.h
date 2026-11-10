#pragma once

// The packed way-0 copies of a module's property sites (ic_hot.cpp,
// bronze_abi_layout.h "the HOT array"). The write hook itself,
// rtIcHotNoteWrite, is declared in object.h beside InlineCache, whose fills
// call it.

#include <cstddef>
#include <cstdint>

namespace bronze::runtime {

// Forget every registered range overlapping [begin, begin + size): an
// instance's run is going away (rtDropModuleCellsWithin).
void rtIcHotDropWithin(const void* begin, size_t size) noexcept;
// Forget them all: the thread's instances are going away.
void rtIcHotClearThread() noexcept;
// How many ranges this thread has (tests).
size_t rtIcHotRangeCount() noexcept;

// After a write to a METHOD-CALL site's way 0 (rt_method_call.cpp's latches):
// copy its guard words to the site's method row (bronze_abi_layout.h, "the
// METHOD ROWS"), when a registered range names it.
void rtMethodRowNoteWrite(const uint64_t* icEntry) noexcept;
// How many method-row ranges this thread has (tests).
size_t rtMethodRowRangeCount() noexcept;

// Hands every range this thread has registered with an identity record to
// the harvester (ic_harvest.h rtIcHarvestFold): the inline caches as they
// stand. A range is also folded as it is dropped, while harvesting is armed.
void rtIcHotFoldLiveRanges();

}  // namespace bronze::runtime
