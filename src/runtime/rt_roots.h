#pragma once

#include <cstdint>

#include "runtime/gc.h"
#include "runtime/root_slots.h"
#include "runtime/value.h"

// The two argument blocks that are GC ROOTS: one a callee copies its parameters
// into, one the runtime fills before it calls.
//
// bronze_dynamic_call hands a builtin an argument block that is rooted only as
// long as the CALLER's frame is, and a block the runtime builds is plain stack
// memory. Both become safe the same way — every slot is pushed onto the shadow
// stack for the block's lifetime — so the two classes live together and a
// reader comparing them can see that they are one idea in two directions.
//
// The storage under both is runtime/root_slots.h's RootValueBlock: the first
// kRootBlockInline slots inline, the C heap only past them. What that changes
// is where the slots live, and nothing else — a block is still sized once and
// never resized, because the frame holds pointers INTO it for its lifetime.

namespace bronze::runtime {

// A native builtin's prologue. bronze_dynamic_call hands a builtin an argument
// block that is rooted only as long as the CALLER's frame is — generated code's
// block lives in its GC root frame, but FunctionHeader::call's arity-adaptation
// vector and the blocks builtins build for callbacks are plain stack memory.
// The contract that makes both safe is that the callee copies its parameters
// into roots of its own before it allocates, and this is that copy, made
// explicit: after constructing one, read arguments from HERE and never from
// `argv` again.
//
// Any allocation the block itself makes is C++'s, not the bronze heap's, so no
// collection can happen between the copy and the rooting.
class RootedArgs {
public:
    RootedArgs(uint32_t argc, const uint64_t* argv) : block_(argc) {
        Value* slots = block_.data();
        for (uint32_t i = 0; i < argc; ++i) slots[i] = Value(argv[i]);
        // Rooted<>'s rule, for the same reason (gc.h): a block that roots
        // nothing is worse than no block at all, because it reads as safe.
        frame_ = requireFrameForRoot();
        for (uint32_t i = 0; i < argc; ++i) frame_->push(&slots[i]);
    }

    ~RootedArgs() {
        if (!frame_) return;
        // Backwards, so every pop is the frame's top and none of them searches.
        // Forwards, a three-argument builtin popped slot 0 while slot 2 was on
        // top and paid a scan for each — an O(argc^2) shape hiding inside a
        // destructor that reads as symmetric with the constructor.
        Value* slots = block_.data();
        for (uint32_t i = block_.count(); i-- > 0;) frame_->pop(&slots[i]);
    }

    RootedArgs(const RootedArgs&) = delete;
    RootedArgs& operator=(const RootedArgs&) = delete;

    uint32_t count() const noexcept { return block_.count(); }

    // Out of range is `undefined`, which is what a JS call site that omitted
    // the argument means — so a builtin never has to bounds-check first.
    Value operator[](uint32_t i) const noexcept {
        return i < block_.count() ? block_.data()[i] : Value::fromUndefined();
    }
    Value at(uint32_t i, Value fallback) const noexcept {
        return i < block_.count() ? block_.data()[i] : fallback;
    }

    // The rooted slots themselves, for a caller that hands them onward as a
    // contiguous view: the collector updates these in place, so a span over
    // them stays CURRENT across anything the callee allocates — which a copy
    // taken with operator[] would not. The embed module's host callbacks read
    // their arguments through exactly this.
    const Value* data() const noexcept { return block_.data(); }

private:
    RootValueBlock block_;
    ShadowStackFrame* frame_{nullptr};
};

// RootedArgs in the other direction: an argument block the RUNTIME builds and
// hands to a callee, rather than one a callee copies out of.
//
// Most blocks the runtime builds need nothing like this — `builtin_array`'s
// `Value block[3]`, the JSON replacer's and the regexp replacer's are all
// filled from roots on the statement before the call, and `bronze_dynamic_call`
// reaches the callee without allocating, so nothing can move in between.
//
// `bronze_construct` is the exception and the reason this class exists: it
// allocates the INSTANCE before it reads the block, so a block that is not
// rooted holds pre-collection addresses by the time the constructor is
// entered. Every slot here is pushed onto the shadow stack for the block's
// lifetime, which makes it safe to pass to a helper that allocates first.
// The block is sized once and never resized, so the pushed pointers stay
// valid — the same reason RootedArgs above may push into its own storage.
class RootedBlock {
public:
    explicit RootedBlock(uint32_t count) : block_(count) {
        // Explicitly, and not left to the storage: the inline half of a
        // RootValueBlock default-constructs to `undefined`, the heap half is
        // raw bytes, and a slot the frame is about to root has to be a real
        // Value before the first collection can read it.
        Value* slots = block_.data();
        for (uint32_t i = 0; i < count; ++i) slots[i] = Value::fromUndefined();
        frame_ = requireFrameForRoot();
        for (uint32_t i = 0; i < count; ++i) frame_->push(&slots[i]);
    }

    ~RootedBlock() {
        if (!frame_) return;
        Value* slots = block_.data();
        for (uint32_t i = block_.count(); i-- > 0;) frame_->pop(&slots[i]);
    }

    RootedBlock(const RootedBlock&) = delete;
    RootedBlock& operator=(const RootedBlock&) = delete;

    void set(uint32_t i, Value v) { block_.data()[i] = v; }
    uint32_t count() const noexcept { return block_.count(); }
    const uint64_t* data() const noexcept {
        return reinterpret_cast<const uint64_t*>(block_.data());
    }

private:
    RootValueBlock block_;
    ShadowStackFrame* frame_{nullptr};
};

}  // namespace bronze::runtime
