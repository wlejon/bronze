#pragma once

// The packed way-0 copies of a module's property sites (ic_hot.cpp,
// bronze_abi_layout.h "the HOT array"). The write hook itself,
// rtIcHotNoteWrite, is declared in object.h beside InlineCache, whose fills
// call it.

#include <cstddef>

namespace bronze::runtime {

// Forget every registered range overlapping [begin, begin + size): an
// instance's run is going away (rtDropModuleCellsWithin).
void rtIcHotDropWithin(const void* begin, size_t size) noexcept;
// Forget them all: the thread's instances are going away.
void rtIcHotClearThread() noexcept;
// How many ranges this thread has (tests).
size_t rtIcHotRangeCount() noexcept;

}  // namespace bronze::runtime
