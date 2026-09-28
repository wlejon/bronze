#pragma once

// The stack walk both samplers share: the env-armed one (sampler.cpp,
// BRONZE_SAMPLE) and the script-armed one (profiler.cpp). Walks a SUSPENDED
// thread's context through the modules' x64 unwind data and, for JIT code,
// through brass's own copy of the tables it registered
// (brass/debug/jit_unwind_registry.hpp) — never RtlLookupFunctionEntry's
// dynamic-table path, whose lock the suspended thread may hold.
//
// Windows x64 only; elsewhere the walk finds nothing.

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

// Walks the suspended thread whose context `context` (a CONTEXT captured with
// CONTEXT_CONTROL | CONTEXT_INTEGER) holds, innermost first, into `pcs`, and
// each frame's stack pointer into `sps` when non-null. Frame 0's pc is the
// instruction the thread stopped on; every later one is a return address.
// Reads only the target's stack and the unwind tables; allocates nothing.
uint32_t walkStack(const ModuleRanges& modules, const void* context, uint64_t* pcs, uint64_t* sps,
                   uint32_t maxFrames);

}  // namespace bronze::runtime::sampling
