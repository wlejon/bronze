#pragma once

#include <string>
#include "abi/bronze_abi.h"
#include "runtime/value.h"

namespace bronze::runtime {

void bronze_register_code_ranges_internal(const void* ranges, uint32_t count);
void bronze_unregister_code_ranges_internal(const void* ranges, uint32_t count);
const bronze_code_range* find_code_range(const void* pc);

// A pc inside compiled JS: the function's code range and the source position
// of the instruction there, read off the pc table the backend emitted
// (bronze_pc_entry). Falls back to the function's definition position when
// the table has no entry at or before the pc. `pc` is the instruction's own
// address, so a caller frame passes its return address minus one — the byte
// of the call, not the instruction after it. `file` is the source file of
// that position, which is not always the descriptor's: a program's merged top
// level holds every imported module's top-level statements.
struct CodeSite {
    const bronze_code_range* range = nullptr;
    // The function the position is in: the range's own, or, inside a call
    // compiled in place, that call's callee (bronze_inline_frame).
    const bronze_fn_desc* desc = nullptr;
    uint32_t line = 0;
    uint32_t col = 0;
    const char* file = nullptr;
    // The position's inline frame in `range` (1-based), 0 for the range's own
    // code.
    uint32_t inline_frame = 0;
};
// False when `pc` is in no registered code range, or in one with no descriptor.
// The site is the innermost frame at `pc`; outer_code_site steps out of it.
bool find_code_site(const void* pc, CodeSite& out);
// The frame around an inlined one: the function its call was compiled into,
// at the call's position. False for a range's own code, which has none.
bool outer_code_site(CodeSite& site);


std::string bronze_format_stack_trace(Value errorObj, Value skipFn = Value::fromUndefined());
void bronze_install_stack(Value errorObj, Value skipFn = Value::fromUndefined());

}  // namespace bronze::runtime
