#pragma once

#include <brass/mir/builder.hpp>
#include <brass/mir/instruction.hpp>
#include <cstdint>
#include <vector>
#include "il_ast.h"

namespace il2mir {

// Object, array and environment allocation: an inline bump in the thread's
// allocation window (bronze_abi_tls.h) with the runtime helper as the slow
// path, or the helper alone when the TLAB fast path is off. The one
// exception is init_literal, a call of the module's tagged allocator
// (bronze_gc_alloc, brass Module::tagged_allocator) followed by the stores
// that initialize the object, which brass sees as an allocation; the fresh
// literal of a transition run (il_transition_run.cpp) is made with it, so
// brass can remove it. An escaping one costs more than the bump, so nothing
// that may escape is made with it.
class AllocLoweringHelper {
public:
    explicit AllocLoweringHelper(bool enable_tlab = true) : enable_tlab_(enable_tlab) {}

    [[nodiscard]] bool enable_tlab() const noexcept { return enable_tlab_; }
    void set_enable_tlab(bool enable) noexcept { enable_tlab_ = enable; }

    // A literal's object; `inline_slots` > 4 asks for that many inline slots
    // (create.object's immI32, its predicted size), from the thread's sized
    // Object.prototype root.
    Value* lower_create_object(Builder& b, uint32_t inline_slots = 0);
    Value* lower_create_array(Builder& b, Value* size_val, uint32_t param_count);
    Value* lower_env_create(Builder& b, Value* parent_val, Value* size_val, uint32_t param_count);
    // A literal asking for `want` inline slots: the root it is made on (0,
    // the default root, or a sized one) and that root's capacity.
    static uint32_t literal_root(uint32_t want);
    static uint32_t literal_capacity(uint32_t root);
    // The TLS block's entry for that root: its shape, or 0 until the runtime
    // has made it.
    Value* published_literal_root(Builder& b, uint32_t root);
    // A literal of that root, made by the module's tagged allocator and
    // initialized as ObjectHeader::create leaves one, except that its shape
    // is `shape` and inline slot i holds `slots[i]` where that is given
    // (undefined elsewhere). The allocation is a GC point; the stores follow
    // it directly. Answers the object's Value bits.
    Value* init_literal(Builder& b, uint32_t root, Value* shape, const std::vector<Value*>& slots);
    // A plain object with no internal slots and `shape`, bumped out of the
    // thread's allocation window when `precondition` holds and the window has
    // room; otherwise a branch to `miss`. Ends in `fast` (empty, the caller's
    // own block), or for a capacity read off the shape in a block after it,
    // with the object initialized as ObjectHeader::create leaves
    // one, and answers its Value bits. Nothing in it is a GC point, so the
    // caller's derived values stay valid up to its next call. `inline_slots`
    // is the shape's capacity when the caller knows it; 0 reads it from the
    // shape (shape.h `bare_header`), which `precondition` must guard.
    Value* bump_plain_object(Builder& b, Value* shape, Value* precondition, BasicBlock* miss, BasicBlock* fast,
                             uint32_t inline_slots = 0);
    // An object's overflow block of `capacity` (an i64) slots, every one
    // undefined, as ObjectHeader::ensureOverflow leaves a new one: bumped out
    // of the thread's window when it has room, else a branch to `miss`. Ends
    // in `fast` and answers the block's header address, untagged. No GC point.
    Value* bump_slot_block(Builder& b, Value* capacity, BasicBlock* miss, BasicBlock* fast);
    // $ArraySpeciesCreate(O, length) (bronze_sh_array_species_create, the
    // self-hosted map/filter/slice's result): for a plain Array O (no
    // property box: no subclass, no `constructor` of its own) and a length
    // that is a small integer Number, the array bronze_create_array(length)
    // makes, header and element block bumped out of the thread's window in
    // one allocation; otherwise the helper.
    Value* lower_array_species_create(Builder& b, Value* o, Value* length);

private:
    Value* lower_create_object_bronze(Builder& b, uint32_t inline_slots);
    Value* lower_create_array_bronze(Builder& b, Value* size_val, uint32_t param_count);
    Value* lower_env_create_bronze(Builder& b, Value* parent_val, Value* size_val, uint32_t param_count);

    bool enable_tlab_ = true;
};

} // namespace il2mir
