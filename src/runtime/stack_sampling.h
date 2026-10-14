#pragma once

// The stack walk both samplers share: the env-armed one (sampler.cpp,
// BRONZE_SAMPLE) and the script-armed one (profiler.cpp). Walks a SUSPENDED
// thread's context through the modules' x64 unwind data and, for JIT code,
// through brass's own copy of the tables it registered
// (brass/debug/jit_unwind_registry.hpp) — never RtlLookupFunctionEntry's
// dynamic-table path, whose lock the suspended thread may hold.
//
// Windows x64, and arm64 macOS by the frame-pointer chain (Arm64Context);
// elsewhere the walk finds nothing.

#include <cstdint>
#include <vector>

namespace bronze::runtime::sampling {

// The loaded modules' address ranges, refreshed off the hot path: a pc is
// looked up in a module's unwind data only when it is inside one, so a
// garbage pc never reaches the OS lookup.
class ModuleRanges {
public:
    void refresh();
    bool contains(uint64_t pc) const;

private:
    struct Range {
        uint64_t base;
        uint64_t end;
    };
    std::vector<Range> ranges_;
};

// arm64 macOS: what walkStack takes for `context`. The walk follows the
// frame-pointer chain (the platform ABI keeps one; brass's JIT frames keep
// it too), and puts the link register in as frame 1 when it differs from
// the innermost frame record's return address: a frameless leaf's caller is
// only there. It sets lrFrame then; a profiler drops that frame when it
// names frame 0's own function (an LR left by a call it made earlier).
struct Arm64Context {
    uint64_t pc = 0, lr = 0, fp = 0, sp = 0;
    uint64_t stackLo = 0, stackHi = 0;  // the thread's stack; nothing outside it is read
    bool lrFrame = false;               // out
};

// Linux x64: what walkStack takes for `context`. Follows the 16-byte-aligned
// %rbp frame-pointer chain ([fp] = caller's fp, [fp + 8] = return address)
// within [stackLo, stackHi).
struct LinuxX64Context {
    uint64_t pc = 0, sp = 0, fp = 0;
    uint64_t stackLo = 0, stackHi = 0;
};

// Walks the suspended thread whose context `context` (Windows x64: a CONTEXT
// captured with CONTEXT_CONTROL | CONTEXT_INTEGER; arm64 macOS: an
// Arm64Context; Linux x64: a LinuxX64Context) holds, innermost first, into
// `pcs`, and each frame's stack pointer into `sps` when non-null. Frame 0's pc
// is the instruction the thread stopped on; every later one is a return address.
// Reads only the target's stack and the unwind tables; allocates nothing.
uint32_t walkStack(const ModuleRanges& modules, const void* context, uint64_t* pcs, uint64_t* sps,
                   uint32_t maxFrames);

}  // namespace bronze::runtime::sampling
