#pragma once

#include <cstdint>
#include <cstdlib>

#include "runtime/tls_block.h"
#include "runtime/value.h"

// The storage under every GC root block, and the reason it is not a
// std::vector.
//
// A host call is the shape that decides this. `hostTrampoline`
// (embed/embed_function.cpp) opens a ShadowStackFrame, copies its arguments
// into a RootedArgs and roots the receiver — and each of those three was a
// container that began EMPTY and was immediately filled, so a single
// `gl.drawElements(...)` reached `malloc` four or five times and `free` as
// many. At 10,006 GL calls per `many_meshes` frame the chunk-4 sampler put the
// C heap at 10 % of the frame (4.49 ms), the largest identified line item
// below the JS line, and fitting the three scenes attributed it per DRAW
// rather than per object — which is exactly this path.
//
// So both shapes hold their first entries inline and reach the heap only past
// them. Nothing about the ROOTING contract changes: every slot is still pushed
// onto the shadow stack for the block's lifetime, the only allocation between
// a copy and its rooting is still C++'s and never the bronze heap's, and a
// block is still sized once and never resized.
//
// Seam: BRONZE_NO_INLINE_ROOTS=1 makes every block take the heap even when it
// would fit inline, which reproduces the old shape in the same binary.

namespace bronze {

// Whether a block may use its inline storage. Read once per block, from the
// thread's own ABI word through runtime/tls_block.h's inline accessor — this
// path cannot afford the call that `bronze_tls_block_addr` used to be, which
// is why that accessor exists.
//
// The word is lowered by Heap's constructor, which runs on a thread's FIRST
// touch of the runtime — so the entry frame an embed API function opens before
// that touch reads the default and keeps its inline storage even with the seam
// down. That is one frame per thread out of the millions the A/B is about, and
// it is stated here rather than worked around because the alternative — a
// second, earlier place that reads the environment — is a second answer to
// "what is this seam set to".
inline bool inlineRootsEnabled() noexcept {
    return runtime::g_tls_block.inline_roots_enabled != 0;
}

// How many slot POINTERS a shadow-stack frame holds inline.
//
// Sixteen because that is the hot shape and then some: a host trampoline roots
// the receiver plus its arguments — three.js's widest GL call passes four —
// and the binding body opens a handful of its own. It is not a number that has
// to be right. A frame that outgrows it spills to the heap and keeps working,
// and BRONZE_IC_LOG=1 counts the spills under `root_frame_spill`, so the
// choice is auditable rather than believed.
inline constexpr uint32_t kRootSlotsInline = 16;

// How many root SLOTS an argument block holds inline.
//
// Eight, because that is where JavaScript's own call sites sit: the widest
// builtin bronze runs takes six, and a JS callback receives at most three from
// the array iteration methods. Past it the block mallocs once — never twice,
// and never a growth sequence, because an argument block's size is known
// before its first store.
inline constexpr uint32_t kRootBlockInline = 8;

// Counted, not asserted: a spill is legal and rare, and the only way to know
// whether kRootSlotsInline / kRootBlockInline were chosen well is to see how
// often the real workload passes them. Defined in gc.cpp; the recorders are
// BRONZE_UNLIKELY-guarded there, so a build with the log off pays a predicted
// branch on the cold edge only.
void recordRootFrameSpill(uint32_t reached) noexcept;
void recordRootBlockSpill(uint32_t count) noexcept;

// A shadow-stack frame's list of root slot POINTERS.
//
// Growth moves the LIST and never a slot, which is what makes growth safe
// here and not in the block below: what a frame holds is the addresses of
// Values that live in Rooted<> objects and argument blocks elsewhere, so a
// reallocation of this array is invisible both to the collector — which reads
// through `data()` every time — and to every root already pushed.
class RootSlotList {
public:
    RootSlotList() noexcept {
        if (!inlineRootsEnabled()) {
            // The seam, and it has to be spelled this way rather than as a
            // branch inside push(): what the A/B has to reproduce is a list
            // that owns no inline storage at all, so the very first push
            // allocates the way push_back did.
            slots_ = nullptr;
            capacity_ = 0;
        }
    }

    ~RootSlotList() {
        if (slots_ != inline_) std::free(slots_);
    }

    RootSlotList(const RootSlotList&) = delete;
    RootSlotList& operator=(const RootSlotList&) = delete;

    Value** data() const noexcept { return const_cast<Value**>(slots_); }
    uint32_t size() const noexcept { return count_; }

    // Whether this list is still living inside itself. The only honest way to
    // assert "no allocation happened", because it is not a count of mallocs
    // that happens to be zero — it is the storage saying it never needed one.
    // tests/runtime/root_slots_test.cpp reads it; nothing else may.
    bool usesInlineStorage() const noexcept { return slots_ == inline_; }
    uint32_t capacity() const noexcept { return capacity_; }

    void push(Value* slot) {
        if (count_ == capacity_) {
            growAndPush(slot);
            return;
        }
        slots_[count_++] = slot;
    }

    // Popping is by IDENTITY, not by position, because Rooted<>'s move
    // constructor can register a slot out of order. The top of the list is
    // the answer for every block in this file — they pop in reverse — and the
    // search is the cold half for everything else.
    void pop(Value* slot) noexcept {
        if (count_ != 0 && slots_[count_ - 1] == slot) {
            --count_;
            return;
        }
        popOutOfOrder(slot);
    }

private:
    void growAndPush(Value* slot);
    void popOutOfOrder(Value* slot) noexcept;

    Value* inline_[kRootSlotsInline];
    Value** slots_{inline_};
    uint32_t count_{0};
    uint32_t capacity_{kRootSlotsInline};
};

// A fixed-size block of root SLOTS: the storage under RootedArgs and
// RootedBlock (runtime/rt_roots.h).
//
// Sized once at construction and never resized, and that is a correctness
// requirement rather than an optimization — the frame is holding pointers
// INTO this array for the block's whole lifetime, so a reallocation would
// leave the shadow stack naming freed memory and the collector writing a
// forwarded address into it.
class RootValueBlock {
public:
    explicit RootValueBlock(uint32_t count) : count_(count) {
        if (count <= kRootBlockInline && inlineRootsEnabled()) {
            slots_ = inline_;
            return;
        }
        if (count > kRootBlockInline) recordRootBlockSpill(count);
        // A zero-length block still needs a non-null, never-dereferenced
        // address for `data()`; malloc(0) may answer null, so the inline
        // buffer stands in and the destructor's `!= inline_` test frees
        // nothing.
        if (count == 0) {
            slots_ = inline_;
            return;
        }
        slots_ = static_cast<Value*>(std::malloc(sizeof(Value) * count));
        if (!slots_) blockAllocationFailed(count);
    }

    ~RootValueBlock() {
        if (slots_ != inline_) std::free(slots_);
    }

    RootValueBlock(const RootValueBlock&) = delete;
    RootValueBlock& operator=(const RootValueBlock&) = delete;

    Value* data() noexcept { return slots_; }
    const Value* data() const noexcept { return slots_; }
    uint32_t count() const noexcept { return count_; }

    // As RootSlotList::usesInlineStorage above: the storage's own answer to
    // "did this block reach the C heap", which is the thing the chunk claims
    // and the thing a wall clock on this machine cannot resolve.
    bool usesInlineStorage() const noexcept { return slots_ == inline_; }

private:
    [[noreturn]] static void blockAllocationFailed(uint32_t count);

    Value inline_[kRootBlockInline];
    Value* slots_{nullptr};
    uint32_t count_{0};
};

}  // namespace bronze
