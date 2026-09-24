#include "runtime/gc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include <brass/gc/card_table.hpp>
#include <brass/gc/runtime_gc.hpp>

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"
#include "runtime/ic_log.h"

// Generated code's own frame chain lives in the per-thread bronze_tls_block
// (tls_block.cpp, `frame_top`): compiled code links and unlinks against the
// block its prologue fetched, and the collector walks the calling thread's
// chain (heap.cpp). What remains here is the C++ side's shadow stack — and of
// that, only the parts that are NOT on the hot path. The frame's link, its
// slot pushes and pops and the "is there a frame at all" check are inline in
// gc.h; what is left below is the growth, the out-of-order pop, and the two
// named deaths.

namespace bronze {

thread_local ShadowStackFrame* ShadowStackFrame::top_frame_ = nullptr;

void fatalNoRootFrame() {
    fatal("Rooted<> with no ShadowStackFrame open: every entry into bronze opens "
          "one (rt.cpp's main, embed::runMain, embed::runEntry, and each embed:: "
          "API function), and generated code is only reached through them — a host "
          "calling raw bronze_* helpers must open a bronze::ShadowStackFrame first");
}

void recordRootFrameSpill(uint32_t reached) noexcept {
    if (BRONZE_UNLIKELY(runtime::g_icLogEnabled)) {
        runtime::icLogRecordRootSpill("root_frame_spill", reached);
    }
}

void recordRootBlockSpill(uint32_t count) noexcept {
    if (BRONZE_UNLIKELY(runtime::g_icLogEnabled)) {
        runtime::icLogRecordRootSpill("root_block_spill", count);
    }
}

void RootSlotList::growAndPush(Value* slot) {
    // Doubling from the inline capacity, and from 8 when the seam took the
    // inline storage away — the same geometric shape std::vector had, so the
    // A/B compares two allocation policies rather than two growth curves.
    const uint32_t next = capacity_ == 0 ? 8u : capacity_ * 2u;
    if (slots_ == inline_) recordRootFrameSpill(count_);

    auto* grown = static_cast<Value**>(std::malloc(sizeof(Value*) * next));
    if (!grown) {
        fatal("bronze: out of C++ heap growing a GC root frame — the shadow stack "
              "cannot spill, because a root it cannot hold is a value the "
              "collector will not find");
    }
    if (count_ != 0) std::memcpy(grown, slots_, sizeof(Value*) * count_);
    if (slots_ != inline_) std::free(slots_);
    slots_ = grown;
    capacity_ = next;
    slots_[count_++] = slot;
}

void RootSlotList::popOutOfOrder(Value* slot) noexcept {
    // Rooted<>'s move constructor registers the moved-to slot without
    // unregistering in order, so a pop that is not the top is legal. Searching
    // from the top keeps the common near-top case short, and a slot that is
    // not present at all is a no-op — the same answer the vector's
    // `if (!roots_.empty())` guard gave.
    for (uint32_t i = count_; i-- > 0;) {
        if (slots_[i] != slot) continue;
        const uint32_t tail = count_ - i - 1;
        if (tail != 0) std::memmove(&slots_[i], &slots_[i + 1], sizeof(Value*) * tail);
        --count_;
        return;
    }
}

void RootValueBlock::blockAllocationFailed(uint32_t count) {
    (void)count;
    fatal("bronze: out of C++ heap building a rooted argument block — the copy a "
          "callee reads its parameters from cannot be elided, because `argv` is "
          "only rooted for as long as the caller's frame is");
}

static thread_local WriteBarrierStats g_writeBarrierStats;

WriteBarrierStats& get_write_barrier_stats() noexcept { return g_writeBarrierStats; }
void reset_write_barrier_stats() noexcept { g_writeBarrierStats.reset(); }

// The card table a thread's barrier marks, per thread like the heap it
// describes: a table installed by one thread's collector names that thread's
// old and young spaces, which mean nothing to a store on another thread.
static thread_local ActiveCardTableDescriptor g_activeCardTableStorage;
static thread_local const ActiveCardTableDescriptor* g_activeCardTable = nullptr;

void set_active_card_table(const ActiveCardTableDescriptor* desc) noexcept {
    g_activeCardTable = desc;
}

void set_active_card_table(brass::CardTable* ct,
                           uintptr_t old_space_base, size_t old_space_size,
                           uintptr_t young_space_base, size_t young_space_size) noexcept {
    if (!ct) {
        g_activeCardTable = nullptr;
        return;
    }
    g_activeCardTableStorage.card_table = ct;
    g_activeCardTableStorage.old_space_base = old_space_base;
    g_activeCardTableStorage.old_space_size = old_space_size;
    g_activeCardTableStorage.young_space_base = young_space_base;
    g_activeCardTableStorage.young_space_size = young_space_size;
    g_activeCardTableStorage.is_old_fn = nullptr;
    g_activeCardTableStorage.is_young_fn = nullptr;
    g_activeCardTable = &g_activeCardTableStorage;
}

void set_active_card_table(brass::CardTable* ct,
                           bool (*is_old)(uintptr_t),
                           bool (*is_young)(uintptr_t)) noexcept {
    if (!ct) {
        g_activeCardTable = nullptr;
        return;
    }
    g_activeCardTableStorage.card_table = ct;
    g_activeCardTableStorage.old_space_base = 0;
    g_activeCardTableStorage.old_space_size = 0;
    g_activeCardTableStorage.young_space_base = 0;
    g_activeCardTableStorage.young_space_size = 0;
    g_activeCardTableStorage.is_old_fn = is_old;
    g_activeCardTableStorage.is_young_fn = is_young;
    g_activeCardTable = &g_activeCardTableStorage;
}

const ActiveCardTableDescriptor* get_active_card_table_descriptor() noexcept {
    return g_activeCardTable;
}

brass::CardTable* get_active_card_table() noexcept {
    return g_activeCardTable ? g_activeCardTable->card_table : nullptr;
}

static inline bool is_gc_log_enabled() noexcept {
    static const bool enabled = (std::getenv("BRONZE_GC_LOG") != nullptr);
    return enabled;
}

}  // namespace bronze

extern "C" void brass_gc_write_barrier(uint64_t obj, uint64_t val) {
    bronze::g_writeBarrierStats.total_invocations++;

    // Unmask obj to extract raw heap address (handles both raw pointer and NaN-tagged pointer)
    const uintptr_t obj_addr = static_cast<uintptr_t>(obj & bronze::kPayloadMask);
    if (obj_addr == 0) {
        return;
    }

    // Inspect val: is it a pointer to a young-generation GC object?
    if (val == 0) {
        bronze::g_writeBarrierStats.filtered_non_pointer++;
        return;
    }

    uintptr_t ptr_val = 0;
    const uint64_t high16 = val >> bronze::kTagShift;

    if (high16 == 0) {
        // Raw 48-bit address in user space
        ptr_val = static_cast<uintptr_t>(val);
    } else if (high16 == static_cast<uint64_t>(bronze::Tag::Object) ||
               high16 == static_cast<uint64_t>(bronze::Tag::String) ||
               high16 == static_cast<uint64_t>(bronze::Tag::BigInt)) {
        // Bronze NaN-boxed pointer payload
        ptr_val = static_cast<uintptr_t>(val & bronze::kPayloadMask);
        if (ptr_val == 0) {
            bronze::g_writeBarrierStats.filtered_non_pointer++;
            return;
        }
    } else {
        // Non-pointer / scalar: IEEE double, Bool, Undefined, Null, Int32, Hole, etc.
        bronze::g_writeBarrierStats.filtered_non_pointer++;
        return;
    }

    // Bronze owns the heap, so the card table is bronze's: brass's own
    // generational collector never holds a bronze object, and this barrier —
    // the process's one definition of brass_gc_write_barrier, overriding
    // brass's brass_default_gc_write_barrier — never consults it.
    if (auto* desc = bronze::get_active_card_table_descriptor()) {
        if (desc->card_table) {
            bool is_old = false;
            if (desc->is_old_fn) {
                is_old = desc->is_old_fn(obj_addr);
            } else if (desc->old_space_size > 0) {
                is_old = (obj_addr >= desc->old_space_base &&
                          obj_addr < desc->old_space_base + desc->old_space_size);
            }
            if (!is_old) {
                bronze::g_writeBarrierStats.filtered_non_old_obj++;
                return;
            }

            bool is_young = false;
            if (desc->is_young_fn) {
                is_young = desc->is_young_fn(ptr_val);
            } else if (desc->young_space_size > 0) {
                is_young = (ptr_val >= desc->young_space_base &&
                            ptr_val < desc->young_space_base + desc->young_space_size);
            }
            if (!is_young) {
                bronze::g_writeBarrierStats.filtered_non_young_val++;
                return;
            }

            desc->card_table->mark_card(obj_addr);
            bronze::g_writeBarrierStats.old_to_young_marked++;
            if (BRONZE_UNLIKELY(bronze::is_gc_log_enabled())) {
                std::fprintf(stderr, "[BRONZE_GC_WRITE_BARRIER] old=0x%llx -> young=0x%llx (card %zu marked)\n",
                             static_cast<unsigned long long>(obj_addr),
                             static_cast<unsigned long long>(ptr_val),
                             desc->card_table->card_index(obj_addr));
            }
            return;
        }
    }

    // No active card table
    bronze::g_writeBarrierStats.filtered_non_old_obj++;
}
