#include "runtime/stack_sampling.h"

#include <algorithm>

#include <brass/debug/jit_unwind_registry.hpp>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace bronze::runtime::sampling {

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))

void ModuleRanges::refresh() {
    HMODULE mods[512];
    DWORD needed = 0;
    if (!::EnumProcessModules(::GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    const size_t n = std::min<size_t>(needed / sizeof(HMODULE), 512);
    ranges_.clear();
    ranges_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        MODULEINFO mi;
        if (::GetModuleInformation(::GetCurrentProcess(), mods[i], &mi, sizeof(mi))) {
            const uint64_t base = reinterpret_cast<uint64_t>(mi.lpBaseOfDll);
            ranges_.push_back({base, base + mi.SizeOfImage});
        }
    }
    std::sort(ranges_.begin(), ranges_.end(), [](const Range& a, const Range& b) { return a.base < b.base; });
}

bool ModuleRanges::contains(uint64_t pc) const {
    auto it = std::upper_bound(ranges_.begin(), ranges_.end(), pc,
                               [](uint64_t v, const Range& m) { return v < m.base; });
    if (it == ranges_.begin()) return false;
    --it;
    return pc >= it->base && pc < it->end;
}

uint32_t walkStack(const ModuleRanges& modules, const void* context, uint64_t* pcs, uint64_t* sps,
                   uint32_t maxFrames) {
    CONTEXT ctx = *static_cast<const CONTEXT*>(context);
    uint32_t n = 0;
    // The stack bounds, so the leaf-frame return-address read below can never
    // touch memory outside the target's stack.
    uint64_t stackLo = 0, stackHi = 0;
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (::VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(ctx.Rsp)), &mbi, sizeof(mbi)) ==
                sizeof(mbi) &&
            mbi.State == MEM_COMMIT) {
            stackLo = reinterpret_cast<uint64_t>(mbi.BaseAddress);
            stackHi = stackLo + mbi.RegionSize;
        }
    }
    while (n < maxFrames && ctx.Rip != 0) {
        if (sps) sps[n] = ctx.Rsp;
        pcs[n++] = ctx.Rip;
        // A return address names the call before it: look the function up
        // by the byte of the call, so a call that ends its function is still
        // inside it.
        const uint64_t lookup = n == 1 ? ctx.Rip : ctx.Rip - 1;
        PRUNTIME_FUNCTION rf = nullptr;
        DWORD64 imageBase = 0;
        if (modules.contains(lookup)) {
            rf = ::RtlLookupFunctionEntry(lookup, &imageBase, nullptr);
        } else {
            const void* entry = nullptr;
            uintptr_t base = 0;
            if (!brass::debug::find_jit_unwind_entry_nonblocking(static_cast<uintptr_t>(lookup), &entry, &base)) {
                break;
            }
            rf = const_cast<PRUNTIME_FUNCTION>(static_cast<const RUNTIME_FUNCTION*>(entry));
            imageBase = static_cast<DWORD64>(base);
        }
        if (rf == nullptr) {
            // A true leaf function: the return address is at RSP.
            if (ctx.Rsp < stackLo || ctx.Rsp + 8 > stackHi) break;
            ctx.Rip = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(ctx.Rsp));
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, rf, &ctx, &handlerData, &establisher, nullptr);
        if (ctx.Rsp != 0 && stackLo != 0 && (ctx.Rsp < stackLo || ctx.Rsp > stackHi)) break;
    }
    return n;
}

#else

void ModuleRanges::refresh() {}
bool ModuleRanges::contains(uint64_t) const { return false; }
uint32_t walkStack(const ModuleRanges&, const void*, uint64_t*, uint64_t*, uint32_t) { return 0; }

#endif

}  // namespace bronze::runtime::sampling
