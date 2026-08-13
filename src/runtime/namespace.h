#pragma once

#include <cstdint>

#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze {

// A MODULE NAMESPACE EXOTIC OBJECT (ECMA-262 10.4.6) — what `import * as ns`
// binds. It is its own heap kind and not a plain object, because three of its
// internal methods are not the ordinary ones and none of them can be expressed
// as a property attribute:
//
//   - 10.4.6.2 [[OwnPropertyKeys]] returns the export names SORTED by code
//     unit, so a module exporting `z` before `a` still answers `a` first. A
//     plain object answers in insertion order, which is a different list.
//   - 10.4.6.9 [[Set]] returns FALSE, always. Not "the property is
//     non-writable" — a namespace property IS writable (10.4.6.5, below); the
//     object simply refuses every write, so `ns.x = 1` is a TypeError in strict
//     code no matter what the descriptor says.
//   - 10.4.6.5 [[GetOwnProperty]] answers
//     `{ value, writable: true, enumerable: true, configurable: false }`.
//     `writable: true` is deliberate and is not a bug in this comment: the
//     exporting module can still assign to the binding, and a non-writable
//     non-configurable property whose value changed would break the invariant
//     6.1.7.3 places on every object.
//
// The VALUES are not stored. Each entry holds the export name and a GETTER
// closed over the exporting module's binding, so `ns.n` after the module
// reassigns `n` is the new value — 10.4.6.7's "read the binding out of the
// module environment record", reached with the machinery bronze already has for
// a live view. `src/modules/link.cpp` builds those getters; this object is the
// exotic wrapper the linker's object literal is turned into.
struct ModuleNamespaceHeader {
    HeapObjectHeader header;  // flags == kFlags
    // How many exports, and therefore how many (name, getter) pairs follow.
    // Two 32-bit fields and not one 64-bit one for the reason ArrayHeader has
    // the same pair: the collector scans an object's payload as an array of
    // Values, and a small integer pair is not a pointer under any tag.
    uint32_t count{0};
    uint32_t reserved{0};
    // 2 * count Values follow, sorted by name in code-unit order:
    // name0, getter0, name1, getter1, ...
    //
    // The names are ARENA-interned strings, which are immortal and never move,
    // so the sorted order is established once at construction and nothing can
    // disturb it. That is what makes the key order a function of the export
    // names and not of any table's iteration order.

    static constexpr uint16_t kFlags = HeapKind::ModuleNamespace;

    Value* entries() noexcept { return reinterpret_cast<Value*>(this + 1); }
    const Value* entries() const noexcept { return reinterpret_cast<const Value*>(this + 1); }

    Value name(uint32_t i) const { return entries()[2 * i]; }
    Value getter(uint32_t i) const { return entries()[2 * i + 1]; }

    // The index of `key` among the exports, or -1. A linear scan: an export
    // list is a handful of names, and the sorted order this walks is the
    // object's whole point.
    int32_t indexOf(const StringHeader* key) const;

    // Uninitialised entries; the caller fills them and must leave them sorted.
    static ModuleNamespaceHeader* create(Heap& heap, uint32_t count);
};

}  // namespace bronze
