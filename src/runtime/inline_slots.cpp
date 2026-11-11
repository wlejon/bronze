// The inline capacity an object is allocated with (shape.h `inline_slots`),
// as the compiler predicted it: a class's instances get one Value per field
// its layout names (bronze_abi.h BRONZE_ABI_FN_INLINE_SLOTS_SHIFT, carried in
// the constructor's function flags), a class prototype gets kOverflowSlot
// inline slots, and everything else gets the default four.

#include <cstdint>
#include <unordered_map>

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"

namespace bronze::runtime {

// The compiler's per-code predictions (rtNoteInstanceInlineSlots). Per thread,
// as the arena and the shapes made from them are: a code pointer means the
// same function on every thread, and each thread records it when it makes a
// function object of that code.
static thread_local std::unordered_map<const void*, uint8_t>* g_instanceInlineSlots = nullptr;

void rtNoteInstanceInlineSlots(const void* code, uint32_t fnFlags) {
    const uint32_t want = (fnFlags >> BRONZE_ABI_FN_INLINE_SLOTS_SHIFT) & BRONZE_ABI_FN_INLINE_SLOTS_MASK;
    if (want == 0 || code == nullptr) return;
    if (!g_instanceInlineSlots) g_instanceInlineSlots = new std::unordered_map<const void*, uint8_t>();
    (*g_instanceInlineSlots)[code] = static_cast<uint8_t>(Shape::clampInlineSlots(want));
}

static uint32_t predictedInlineSlots(const FunctionHeader* fn) {
    if (fn == nullptr || !g_instanceInlineSlots) return 0;
    const auto it = g_instanceInlineSlots->find(reinterpret_cast<const void*>(fn->code));
    return it == g_instanceInlineSlots->end() ? 0u : it->second;
}

uint32_t rtInstanceInlineSlots(const FunctionHeader* fn) {
    if (fn == nullptr || fn->native_base != 0) return Shape::kDefaultInlineSlots;
    return Shape::clampInlineSlots(predictedInlineSlots(fn));
}

uint32_t rtPrototypeInlineSlots(const FunctionHeader* fn) {
    if (fn == nullptr || (fn->function_flags & BRONZE_ABI_FN_FLAG_CLASS_CTOR) == 0) {
        return Shape::kDefaultInlineSlots;
    }
    return Shape::kOverflowSlot;
}

}  // namespace bronze::runtime
