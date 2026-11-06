// The megamorphic read cache: one per thread, keyed by (shape, key), holding
// the entry a site's installer wrote for that pair (V8's stub cache).
//
// A read site keeps four ways. A site that sees more shapes than that — the
// renderer's `material.visible` over a dozen material classes — missed all
// four on most reads and paid the full by-name lookup each time, then evicted
// a healthy way to install an entry it would evict again four reads later.
// An entry is about its (shape, key) pair and nothing else about its site —
// its validity is the shape match plus, past depth 0, the prototype epoch
// (InlineCache::describes) — so the one a site installed answers any site
// reading that key off that shape.
//
// Shapes are immortal arena nodes of the thread's heap, so a slot never comes
// to name a different shape; the table is emptied when the thread's heap is
// made, because a new heap's arena may reuse an old one's addresses.

#pragma once

#include <cstdint>

namespace bronze {
struct InlineCache;
struct InlineCacheSite;
class Shape;
}  // namespace bronze

namespace bronze::runtime {

// The entry cached for (shape, key), or null. The caller still asks it
// `describes` / `describesAbsent`, exactly as a site's own way.
InlineCache* rtMegaPropProbe(const Shape* shape, uint32_t keyIndex) noexcept;

// Records the entry `site` holds for `shape` after a full lookup, if any.
void rtMegaPropFill(const InlineCacheSite* site, const Shape* shape, uint32_t keyIndex) noexcept;

// Records an OWN data slot of `shape` for `keyIndex`: what a method site's
// SLOT entry already proved (a shape is a key-to-slot map), put back in the
// table after a later read evicted the pair, so the site's stub answers it
// again without the helper.
void rtMegaPropFillOwnSlot(Shape* shape, uint32_t keyIndex, uint32_t slot) noexcept;

// The WRITE half: a set site's one entry (way 0) for `shape`, under a key
// space of its own — a read entry says nothing about writability, so the two
// must never answer each other. An entry for an adding store names the shape
// after the add and is not recorded.
InlineCache* rtMegaPropSetProbe(const Shape* shape, uint32_t keyIndex) noexcept;
void rtMegaPropSetFill(const InlineCache* way0, const Shape* shape, uint32_t keyIndex) noexcept;

void rtResetMegaPropCache() noexcept;

}  // namespace bronze::runtime
