#pragma once

#include <cstddef>
#include <cstdint>

struct bronze_fn_desc;

namespace bronze::runtime {

// One JS frame an interpreter is running on the calling thread, as the stack
// walker (stack_trace.cpp) merges it among the native frames: compiled code
// is found by its return addresses, an interpreted frame only through the
// interpreter that runs it.
struct InterpretedFrame {
    // Where the interpreter keeps the frame's record: on this thread's native
    // stack, inside the interpreter's own native frame, so the address orders
    // it among the native frames (the stack grows down: a smaller address is
    // a more recent frame).
    uintptr_t stackAddress = 0;
    const bronze_fn_desc* desc = nullptr;
    // The source position the frame is at (0 = the descriptor's own), and
    // its file (null = the descriptor's).
    uint32_t line = 0;
    uint32_t col = 0;
    const char* file = nullptr;
};

// Fills `out` with up to `capacity` of the calling thread's interpreted JS
// frames, innermost first, and returns how many there are in all (which may
// exceed `capacity`). Frames of functions that are not JS (no descriptor)
// are left out.
using InterpretedFrameWalker = size_t (*)(InterpretedFrame* out, size_t capacity);

// Process-wide: installed by the engine that interprets (the tiered JIT),
// read by every stack walk. Null (the default) means no frame is interpreted.
void rtSetInterpretedFrameWalker(InterpretedFrameWalker walker) noexcept;
InterpretedFrameWalker rtGetInterpretedFrameWalker() noexcept;

}  // namespace bronze::runtime
