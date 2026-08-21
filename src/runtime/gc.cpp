#include "runtime/gc.h"

#include <cstdlib>
#include <cstring>
#include <iterator>

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

}  // namespace bronze
