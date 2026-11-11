// The megamorphic property cache (rt_prop_mega.h), held in the thread's ABI
// block (bronze_abi_tls.h, mega_prop) so generated code probes the same table.

#include "runtime/rt_prop_mega.h"

#include <cstddef>

#include "abi/bronze_abi_tls.h"
#include "runtime/object.h"
#include "runtime/tls_block.h"

namespace bronze::runtime {

namespace {

// An entry is an IC way followed by its key word: the way's layout is what
// generated code reads with the BRONZE_ABI_IC_* offsets, and the probe hands
// the way itself to the site-cache path.
struct MegaEntry {
    InlineCache ic;
    uint64_t key;
};
static_assert(sizeof(MegaEntry) == BRONZE_ABI_MEGA_PROP_ENTRY_WORDS * sizeof(uint64_t));
static_assert(offsetof(MegaEntry, key) == BRONZE_ABI_MEGA_PROP_KEY_WORD * sizeof(uint64_t));

inline MegaEntry& entryFor(const Shape* shape, uint32_t key) noexcept {
    const uint64_t h =
        (reinterpret_cast<uintptr_t>(shape) ^ (uint64_t{key} * BRONZE_ABI_MEGA_PROP_HASH_MUL)) *
        BRONZE_ABI_MEGA_PROP_HASH_MUL;
    auto* table = reinterpret_cast<MegaEntry*>(rtTls()->mega_prop);
    return table[h >> (64 - BRONZE_ABI_MEGA_PROP_BITS)];
}

InlineCache* probe(const Shape* shape, uint32_t key) noexcept {
    MegaEntry& e = entryFor(shape, key);
    return (e.ic.cached_shape == shape && e.key == key) ? &e.ic : nullptr;
}

void record(const InlineCache& way, const Shape* shape, uint32_t key) noexcept {
    MegaEntry& e = entryFor(shape, key);
    e.ic = way;
    e.key = key;
}

}  // namespace

InlineCache* rtMegaPropProbe(const Shape* shape, uint32_t keyIndex) noexcept {
    return probe(shape, keyIndex);
}

void rtMegaPropFill(const InlineCacheSite* site, const Shape* shape, uint32_t keyIndex) noexcept {
    for (const InlineCache& way : site->ways) {
        if (way.isRealShape() && way.cached_shape == shape) {
            record(way, shape, keyIndex);
            return;
        }
    }
}

void rtMegaPropFillOwnSlot(Shape* shape, uint32_t keyIndex, uint32_t slot) noexcept {
    InlineCache way;
    way.fillLocal(shape, slot, /*depth=*/0);
    record(way, shape, keyIndex);
}

InlineCache* rtMegaPropSetProbe(const Shape* shape, uint32_t keyIndex) noexcept {
    return probe(shape, keyIndex | BRONZE_ABI_MEGA_PROP_SET_KEY_BIT);
}

void rtMegaPropSetFill(const InlineCacheSite* site, const Shape* shape, uint32_t keyIndex) noexcept {
    if (!site) return;
    for (uint32_t i = 0; i < BRONZE_ABI_IC_SET_WAYS; ++i) {
        const InlineCache& way = site->ways[i];
        if (way.isRealShape() && way.cached_shape == shape) {
            record(way, shape, keyIndex | BRONZE_ABI_MEGA_PROP_SET_KEY_BIT);
            return;
        }
    }
}

void rtResetMegaPropCache() noexcept {
    for (uint64_t& word : rtTls()->mega_prop) word = 0;
}

}  // namespace bronze::runtime
