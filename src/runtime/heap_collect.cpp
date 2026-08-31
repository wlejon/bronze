// The copying collection itself: forwarding one reference, scanning one
// object's payload, the pause that does both over every root and every
// survivor, and the forwarding marks it leaves behind for a post-collection
// hook to read. Everything here runs with from-space frozen — nothing may
// allocate on the heap between the first forward and the semispace swap.

#include "runtime/heap.h"

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/heap_internal.h"
#include "runtime/object.h"
#include "runtime/profile.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace bronze {

static bool is_valid_object_tag(uint16_t tag) noexcept {
    return (tag >= 0xFFF1 && tag <= 0xFFF9) ||
           tag == static_cast<uint16_t>(Tag::BigInt) ||
           tag == static_cast<uint16_t>(Tag::Forwarded);
}

void Heap::forward_value(Value& val) {
    if (!val.isPointer()) {
        return;
    }

    void* payload_ptr = val.asObject<void>();
    if (!payload_ptr) {
        return;
    }

    auto* raw_ptr = static_cast<uint8_t*>(payload_ptr);
    if (raw_ptr < from_space_.base || raw_ptr >= from_space_.bump_ptr) {
        return;
    }

    // Every heap reference in a Value points at the object's HEADER, never
    // its payload — including the ones in out-of-line blocks (object
    // overflow slots, array elements). That invariant is what lets this
    // dereference rather than guess: probing ptr-8 and falling back to ptr,
    // accepting whichever carried a plausible tag, is ambiguous by
    // construction, since an object's last payload word can hold a Value and
    // a Value's low 16 bits can be anything, including a valid-looking tag.
    auto* header = reinterpret_cast<HeapObjectHeader*>(raw_ptr);
    if (!is_valid_object_tag(header->tag)) {
        return;
    }

    if (header->tag == static_cast<uint16_t>(Tag::Forwarded)) {
        auto* new_hdr = *reinterpret_cast<HeapObjectHeader**>(header->payload());
        val = Value::fromTagAndPayload(val.tag(), reinterpret_cast<uintptr_t>(new_hdr));
        return;
    }

    size_t total_size = header->size;
    uint8_t* new_mem = static_cast<uint8_t*>(allocate_in_space(to_space_, total_size));
    std::memcpy(new_mem, header, total_size);
    // THIS is the relocation, so this is where the epoch moves. Anything that
    // hashes an object by its address is wrong from this line onward, and
    // putting the increment at the end of `collect()` instead would make that
    // invalidation depend on a cycle completing rather than on an object
    // actually moving.
    ++relocations_;
    auto* new_hdr = reinterpret_cast<HeapObjectHeader*>(new_mem);

    header->tag = static_cast<uint16_t>(Tag::Forwarded);
    *reinterpret_cast<HeapObjectHeader**>(header->payload()) = new_hdr;

    val = Value::fromTagAndPayload(val.tag(), reinterpret_cast<uintptr_t>(new_hdr));
}

// One plain object's payload, with the slots its shape declares to be raw
// doubles left alone (runtime/slot_repr.h).
//
// WHY IT IS SAFE TO SKIP THEM. A double slot's eight bytes are an f64, and an
// f64 can spell any bit pattern — including one that reads as a heap pointer.
// Tracing such a word would forward a number, which corrupts the number and
// relocates memory nothing owns. So the shape has to be consulted, and it can
// be: shapes are arena-allocated and immortal, so the word this object carries
// is as good after the copy as before it.
//
// WHY IT IS SAFE NOT TO. In stage R1 a double slot only ever receives a
// Number: through `ObjectHeader::setSlot`, which canonicalizes, or from a
// generated arm that tested for one first — and bronze boxes a Number as its
// own bits. So the word is also a legal number-tagged Value and
// `forward_value` would have returned from it anyway.
// The precision is therefore a no-op TODAY and load-bearing the moment stage
// R2's codegen writes a slot without going through the runtime. Landing it now
// is what makes that a codegen change rather than a collector change.
//
// THE SLOT BLOCK is scanned HERE and nowhere else. Exactly one word in the
// program names it — this object's `overflow` — so this scan is the only pass
// that can reach it while knowing which of its words are Values. Ordering holds
// by construction: the block is copied into to-space by the `overflow` forward
// three lines down, which happens during THIS object's scan, so the block's
// address is above this object's and the scan loop has not passed it yet.
void Heap::scan_plain_object(ObjectHeader* obj, size_t obj_size) {
    const Shape* shape = obj->shape;
    // `HeapKind::Plain` is now a CLAIM that a `Shape*` is at offset 8, and this
    // is where it is cashed. A header that says Plain and is not an object —
    // a Value block left at the zero `Heap::allocate` writes, a header whose
    // kind its creator sets a few lines late — would hand a Value or a code
    // pointer to the dereference below.
    //
    // Shapes are arena-allocated, so a shape address is never inside the heap
    // reservation. Two compares per plain object per collection, against a
    // memcpy: the price of turning that mistake into a sentence instead of a
    // fault somewhere else entirely.
    const uint64_t shape_bits = reinterpret_cast<uint64_t>(shape);
    const uint64_t res_lo = reinterpret_cast<uint64_t>(reserved_base_);
    if (BRONZE_UNLIKELY(shape == nullptr ||
                        (shape_bits >= res_lo && shape_bits < res_lo + reserved_bytes_))) {
        char buf[224];
        std::snprintf(buf, sizeof(buf),
                      "gc: a header carrying HeapKind::Plain has 0x%016llX where its shape "
                      "should be — some Tag::Object allocation left `flags` at zero, which "
                      "reads as Plain (heap.h, HeapKind::ValueBlock)",
                      static_cast<unsigned long long>(shape_bits));
        fatal(buf);
    }
    const uint64_t doubles = shape->double_slots;

    Value* words = reinterpret_cast<Value*>(obj->header.payload());
    const size_t num_words = (obj_size - sizeof(HeapObjectHeader)) / sizeof(Value);

    // Word 0 is the shape pointer and word 1 the overflow reference; the inline
    // property slots follow, and any internal slots follow those. Only the
    // property slots have a representation — an internal slot is never named by
    // a shape and is always a Value.
    constexpr size_t kFirstSlotWord =
        (sizeof(ObjectHeader) - sizeof(HeapObjectHeader)) / sizeof(Value);
    static_assert(kFirstSlotWord == 2, "the shape and overflow words precede the inline slots");

    if (BRONZE_LIKELY(doubles == 0)) {
        for (size_t i = 0; i < num_words; ++i) forward_value(words[i]);
    } else {
        for (size_t i = 0; i < num_words; ++i) {
            if (i >= kFirstSlotWord && i < kFirstSlotWord + ObjectHeader::kInlineSlots) {
                const uint32_t slot = static_cast<uint32_t>(i - kFirstSlotWord);
                if ((doubles & (uint64_t{1} << slot)) != 0) continue;
            }
            forward_value(words[i]);
        }
    }

    // `overflow` has just been forwarded, so this reads the block's NEW address.
    const Value ovf = words[1];
    if (!ovf.isPointer()) return;
    auto* block = ovf.asObject<HeapObjectHeader>();
    if (block == nullptr || block->flags != HeapKind::SlotBlock) return;
    const size_t block_words = (block->size - sizeof(HeapObjectHeader)) / sizeof(Value);
    Value* block_slots = block->payload<Value>();
    for (size_t j = 0; j < block_words; ++j) {
        const uint32_t slot = ObjectHeader::kInlineSlots + static_cast<uint32_t>(j);
        if (slot < 64 && (doubles & (uint64_t{1} << slot)) != 0) continue;
        forward_value(block_slots[j]);
    }
}

HeapObjectHeader* Heap::survivor_of(HeapObjectHeader* header) const noexcept {
    auto* raw_ptr = reinterpret_cast<uint8_t*>(header);
    // Outside from-space it is not this collection's business at all: an
    // arena-interned key, a to-space address a caller already updated. Reported
    // as a survivor unchanged, because "not in the space being collected" and
    // "died" are different facts and only the second may clear a weak slot.
    if (raw_ptr < from_space_.base || raw_ptr >= from_space_.bump_ptr) {
        return header;
    }
    if (!is_valid_object_tag(header->tag)) {
        return header;
    }
    if (header->tag == static_cast<uint16_t>(Tag::Forwarded)) {
        return *reinterpret_cast<HeapObjectHeader**>(header->payload());
    }
    // Its header still carries the tag it was allocated with, so the copy phase
    // never reached it: nothing live refers to it any more.
    return nullptr;
}

void Heap::collect() {
    if (in_gc_) {
        return;
    }

    in_gc_ = true;

    // The inline-allocation window points into from-space, which this
    // collection is about to abandon — invalidate it FIRST, so nothing
    // (hooks included) can see a window over memory whose objects are being
    // forwarded out from under it. bronze_construct refills it later.
    bronze_tls_block_addr()->alloc_cursor = 0;
    bronze_tls_block_addr()->alloc_limit = 0;

    std::chrono::steady_clock::time_point gc_t0;
    if (g_gcLog.enabled) gc_t0 = std::chrono::steady_clock::now();

    if (collection_hook_) {
        collection_hook_(*this);
    }

    to_space_.bump_ptr = to_space_.base;

    for (Value* slot : permanent_roots_) {
        forward_value(*slot);
    }

    RootVisitor visit = [this](Value& slot) { forward_value(slot); };
    for (const RootSource& src : root_sources_) {
        src(visit);
    }

    for (ShadowStackFrame* frame = ShadowStackFrame::current(); frame != nullptr; frame = frame->prev()) {
        Value** root_slots = frame->roots();
        size_t count = frame->count();
        for (size_t i = 0; i < count; ++i) {
            if (root_slots[i]) {
                forward_value(*root_slots[i]);
            }
        }
    }

    // Generated code's root frames: contiguous slot arrays in compiled
    // functions' own stack frames, linked inline by compiled code.
    for (bronze_gc_frame* frame = bronze_tls_block_addr()->frame_top; frame != nullptr;
         frame = frame->prev) {
        Value* slots = reinterpret_cast<Value*>(frame->slots);
        for (uint64_t i = 0; i < frame->count; ++i) {
            forward_value(slots[i]);
        }
    }

    uint8_t* scan_ptr = to_space_.base;
    while (scan_ptr < to_space_.bump_ptr) {
        auto* scan_hdr = reinterpret_cast<HeapObjectHeader*>(scan_ptr);
        size_t obj_size = scan_hdr->size;

        if (payload_holds_values(scan_hdr->tag)) {
            const bool object_kind = scan_hdr->tag == static_cast<uint16_t>(Tag::Object);
            if (object_kind && scan_hdr->flags == HeapKind::SlotBlock) {
                // Its OWNER scanned it, precisely, on the pass that copied it —
                // see scan_plain_object. Skipping it here is not an
                // optimization: this loop cannot know which of a block's words
                // are doubles, because the shape that says so belongs to an
                // object this header has no way back to.
            } else if (object_kind && scan_hdr->flags == HeapKind::Plain) {
                scan_plain_object(reinterpret_cast<ObjectHeader*>(scan_hdr), obj_size);
            } else {
                uint8_t* payload_start = reinterpret_cast<uint8_t*>(scan_hdr->payload());
                size_t payload_bytes = obj_size - sizeof(HeapObjectHeader);
                size_t num_slots = payload_bytes / sizeof(Value);
                auto* slots = reinterpret_cast<Value*>(payload_start);
                for (size_t i = 0; i < num_slots; ++i) {
                    forward_value(slots[i]);
                }
            }
        }

        scan_ptr += obj_size;
    }

    // Every reachable object has been copied: a from-space header now reads
    // Tag::Forwarded if its object survived and its original tag if it did
    // not, which is exactly the question a finalizer sweep has to ask. It runs
    // BEFORE the swap because that is when "from-space" still names the space
    // the registered pointers point into.
    for (const PostCollectionHook& hook : post_collection_hooks_) {
        hook();
    }

    // After the hooks: they are part of the collection (weak sweeps re-point
    // and clear cells), and the state being certified is the one the mutator
    // resumes on.
    if (gc_verify_mode_) {
        verify_space(to_space_);
    }

    if (g_gcLog.enabled) {
        ++g_gcLog.collections;
        g_gcLog.copied_bytes += to_space_.bump_ptr - to_space_.base;
        g_gcLog.gc_nanos += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - gc_t0)
                                .count();
    }

    // Poison AFTER the hooks: they are the last legitimate reader of the
    // abandoned space (survivor_of decodes its forwarding marks). Anything
    // that reads it after this line was holding a reference across a move.
    if (gc_poison_mode_) {
        std::memset(from_space_.base, 0xDB,
                    static_cast<size_t>(from_space_.bump_ptr - from_space_.base));
    }

    const size_t live_bytes = to_space_.bump_ptr - to_space_.base;
    constexpr size_t kMinThreshold = 16 * 1024 * 1024;
    constexpr size_t kMinHeadroom = 8 * 1024 * 1024;
    const size_t next_threshold = live_bytes + std::max(live_bytes, kMinHeadroom);
    gc_threshold_bytes_ = std::min(semispace_size_, std::max(kMinThreshold, next_threshold));

    from_space_.bump_ptr = from_space_.base;
    std::swap(from_space_, to_space_);

    ++collections_;
    in_gc_ = false;
}

}  // namespace bronze
