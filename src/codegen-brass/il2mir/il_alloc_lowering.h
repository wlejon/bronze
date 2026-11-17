#pragma once

#include <brass/mir/builder.hpp>
#include <brass/mir/instruction.hpp>
#include <cstdint>
#include <vector>
#include "il_ast.h"

namespace il2mir {

// Object, array and environment allocation: an inline bump in the thread's
// allocation window (bronze_abi_tls.h) with the runtime helper as the slow
// path, or the helper alone when the TLAB fast path is off.
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
    // A plain object with no internal slots and `shape`, bumped out of the
    // thread's allocation window when `precondition` holds and the window has
    // room; otherwise a branch to `miss`. Ends in `fast` (empty, the caller's
    // own block), or for a capacity read off the shape in a block after it,
    // with the object initialized as ObjectHeader::create leaves
    // one, and answers its Value bits. Nothing in it is a GC point, so the
    // caller's derived values stay valid up to its next call. `inline_slots`
    // is the shape's capacity when the caller knows it; 0 reads it from the
    // shape (shape.h `bare_header`), which `precondition` must guard.
    // `pretenure_site`: the site is pretenured (il.h Module::pretenureSites),
    // this its record (IlLowering::pretenure_site), and the object comes from
    // bronze_alloc_pretenured instead: old, or young when the collector
    // judged the site's objects die young or makes this one its probe. It
    // never collects either and answers 0, taken to `miss`, when neither
    // generation has room without a collection. An old one's card is dirty
    // and, while a marking cycle is open, it is grey, so stores that fill it
    // before the next GC point need no barrier, as a young one's need none.
    // `memento`: a tenured counter's address (IlLowering::alloc_memento), put
    // in an allocation memento bumped right behind the object.
    Value* bump_plain_object(Builder& b, Value* shape, Value* precondition, BasicBlock* miss, BasicBlock* fast,
                             uint32_t inline_slots = 0, Value* pretenure_site = nullptr, Value* memento = nullptr);

private:
    Value* lower_create_object_bronze(Builder& b, uint32_t inline_slots);
    Value* lower_create_array_bronze(Builder& b, Value* size_val, uint32_t param_count);
    Value* lower_env_create_bronze(Builder& b, Value* parent_val, Value* size_val, uint32_t param_count);

    bool enable_tlab_ = true;
};

} // namespace il2mir
