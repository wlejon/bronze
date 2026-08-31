// The two passes that step the live space as a gapless run of headers: the
// opt-in structural audit (BRONZE_HEAP_VERIFY=1, run inside the collection
// pause on the space that was just copied) and the object walk a caller asks
// for. They share a precondition and so they share a file — a space parses as
// a header run only when it holds live, fully-built objects and nothing else,
// which is true of to-space at the end of the copy phase and of from-space
// immediately after the swap, and of neither at any other moment.

#include "runtime/heap.h"

#include "runtime/fatal.h"
#include "runtime/heap_internal.h"
#include "runtime/object.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace bronze {

// Diagnostic name for a Tag::Object header's HeapKind word. Pinned to Count
// so adding a kind extends this table instead of printing an index.
static const char* heap_kind_name(uint16_t kind) noexcept {
    static const char* const names[] = {
        "Plain", "Array", "Function", "TypedArray", "ArrayBuffer", "DataView",
        "Map", "Set", "WeakMap", "WeakSet", "Iterator", "RegExp", "Env",
        "ModuleNamespace", "Proxy", "PrivateTable", "WeakRef",
        "FinalizationRegistry", "SlotBlock", "ValueBlock",
    };
    static_assert(sizeof(names) / sizeof(names[0]) == HeapKind::Count,
                  "a new HeapKind needs a name here");
    return kind < HeapKind::Count ? names[kind] : "?";
}

void Heap::walk_objects(const std::function<void(HeapObjectHeader*)>& fn) {
    // The same header-run stepping verify_space's pass 1 validates — and the
    // same precondition: the space is a gapless run of live, fully-built
    // objects only immediately after collect() (heap.h has the contract).
    uint8_t* end = from_space_.bump_ptr;
    for (uint8_t* p = from_space_.base; p < end; ) {
        auto* hdr = reinterpret_cast<HeapObjectHeader*>(p);
        fn(hdr);
        p += hdr->size;
    }
}

void Heap::verify_space(const Semispace& space) const {
    const uint8_t* end = space.bump_ptr;
    const uint64_t reservation_lo = reinterpret_cast<uint64_t>(reserved_base_);
    const uint64_t reservation_hi = reservation_lo + reserved_bytes_;

    // Pass 1: the space must parse as a gapless run of headers, because pass 2
    // answers "is this pointer a live object?" by exact membership in this
    // list — a corrupt size here would silently shift every boundary after it.
    std::vector<uint64_t> headers;
    for (const uint8_t* p = space.base; p < end;) {
        auto* hdr = reinterpret_cast<const HeapObjectHeader*>(p);
        const uint16_t t = hdr->tag;
        const bool heap_tag = t == static_cast<uint16_t>(Tag::Object) ||
                              t == static_cast<uint16_t>(Tag::String) ||
                              t == static_cast<uint16_t>(Tag::RawBytes) ||
                              t == static_cast<uint16_t>(Tag::BigInt);
        if (!heap_tag || hdr->size < sizeof(HeapObjectHeader) || (hdr->size & 7) != 0 ||
            p + hdr->size > end) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "heap verify: unwalkable header at +0x%zX: tag=0x%04X flags=%u "
                          "size=%u (live space holds %zu bytes)",
                          static_cast<size_t>(p - space.base), t, hdr->flags, hdr->size,
                          static_cast<size_t>(end - space.base));
            fatal(buf);
        }
        headers.push_back(reinterpret_cast<uint64_t>(p));
        p += hdr->size;
    }

    // Pass 2: every word the collector scans must parse cleanly as a Value.
    // Of the four heap header tags only Tag::Object payloads hold Values
    // (payload_holds_values), so every owner below is an object and its
    // flags word names a HeapKind — which is exactly the name the padding
    // bug class needs reported: the struct type whose scanned word is dirty.
    for (uint64_t addr : headers) {
        auto* hdr = reinterpret_cast<const HeapObjectHeader*>(addr);
        if (!payload_holds_values(hdr->tag)) {
            continue;
        }
        // THE STAGE R1 REPRESENTATION INVARIANT, checked where the collector's
        // other structural invariants are checked: every slot a shape calls a
        // double must hold a Number. It is the tripwire for a write that
        // reached a double slot WITHOUT going through `ObjectHeader::setSlot` —
        // a generated raw store whose arm should have tested the value for
        // Number, or a runtime path that learned to write a slot and not to
        // ask (llvm_prop_set.cpp, llvm_static_slot.cpp). Such a
        // store is invisible in ordinary running (the bits are still a legal
        // Value) and a type confusion the moment stage R2 loads them as an f64.
        if (hdr->tag == static_cast<uint16_t>(Tag::Object) && hdr->flags == HeapKind::Plain) {
            const auto* obj = reinterpret_cast<const ObjectHeader*>(hdr);
            const Shape* shape = obj->shape;
            if (shape != nullptr && shape->double_slots != 0) {
                const uint32_t highest =
                    ObjectHeader::kInlineSlots + obj->overflowCapacity();
                for (uint32_t slot = 0; slot < highest && slot < 64; ++slot) {
                    if ((shape->double_slots & (uint64_t{1} << slot)) == 0) continue;
                    const Value v = obj->rawSlot(slot);
                    if (v.isNumber()) continue;
                    char buf[224];
                    std::snprintf(buf, sizeof(buf),
                                  "heap verify: object at +0x%zX holds 0x%016llX in slot %u, "
                                  "which its shape declares a DOUBLE — some write path reached "
                                  "the slot without ObjectHeader::setSlot",
                                  static_cast<size_t>(addr -
                                                      reinterpret_cast<uint64_t>(space.base)),
                                  static_cast<unsigned long long>(v.rawBits()), slot);
                    fatal(buf);
                }
            }
        }
        const Value* slots = hdr->payload<const Value>();
        const size_t num_slots = (hdr->size - sizeof(HeapObjectHeader)) / sizeof(Value);
        for (size_t i = 0; i < num_slots; ++i) {
            const Value v = slots[i];
            if (v.isNumber()) {
                continue;
            }
            const char* why = nullptr;
            switch (v.tag()) {
                case static_cast<uint16_t>(Tag::Int32):
                    break;
                case static_cast<uint16_t>(Tag::Bool):
                    if (v.payload() > 1) why = "Bool word whose payload is not 0 or 1";
                    break;
                case static_cast<uint16_t>(Tag::Null):
                case static_cast<uint16_t>(Tag::Undefined):
                case static_cast<uint16_t>(Tag::Hole):
                case static_cast<uint16_t>(Tag::Uninitialized):
                    if (v.payload() != 0) why = "singleton tag carrying a payload";
                    break;
                case static_cast<uint16_t>(Tag::Object):
                case static_cast<uint16_t>(Tag::String):
                case static_cast<uint16_t>(Tag::Symbol):
                case static_cast<uint16_t>(Tag::BigInt): {
                    const uint64_t target = v.payload();
                    // Null is legal (a hardware NaN is 0xFFF8'0000'0000'0000,
                    // which parses as Symbol with payload 0 — forward_value
                    // tolerates it for the same reason). So is anything
                    // outside the reservation: arena-interned symbols,
                    // strings and every C++-owned structure live there.
                    if (target == 0 || target < reservation_lo || target >= reservation_hi) {
                        break;
                    }
                    if (target < reinterpret_cast<uint64_t>(space.base) ||
                        target >= reinterpret_cast<uint64_t>(end)) {
                        why = "pointer into the heap but outside the live space "
                              "(stale semispace reference)";
                    } else if (!std::binary_search(headers.begin(), headers.end(), target)) {
                        why = "pointer into the live space that is not an object header";
                    } else {
                        // A reference's Value tag and its target's header tag
                        // agree, with one designed exception: Tag::Object may
                        // name a RawBytes header (ArrayBuffer stores, WeakRef —
                        // the payloads the scan must skip).
                        const uint16_t vt = v.tag();
                        const uint16_t tt = reinterpret_cast<const HeapObjectHeader*>(target)->tag;
                        const bool ok = vt == static_cast<uint16_t>(Tag::Object)
                                            ? (tt == static_cast<uint16_t>(Tag::Object) ||
                                               tt == static_cast<uint16_t>(Tag::RawBytes))
                                            : tt == vt;
                        if (!ok) why = "pointer whose target header carries a different tag";
                    }
                    break;
                }
                case static_cast<uint16_t>(Tag::Forwarded):
                    why = "Forwarded tag in a scanned word after the copy phase";
                    break;
                default:
                    why = "tag the value model does not define";
                    break;
            }
            if (why) {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "heap verify: %s: object at +0x%zX kind=%s size=%u, "
                              "slot %zu = 0x%016llX — a heap struct must zero every "
                              "byte of every word the collector scans",
                              why, static_cast<size_t>(addr - reinterpret_cast<uint64_t>(space.base)),
                              heap_kind_name(hdr->flags), hdr->size, i,
                              static_cast<unsigned long long>(v.rawBits()));
                fatal(buf);
            }
        }
    }
}

}  // namespace bronze
