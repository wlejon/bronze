#include "runtime/fn_source.h"

#include <unordered_map>

namespace bronze::runtime {

namespace {

struct SourceSlice {
    const char* text;
    uint32_t begin;
    uint32_t length;
};

// thread_local, like every other module-registration table in the runtime: a
// process can run more than one bronze thread, each with its own runtime, and
// each one's module init registers what its own modules hold. The DATA is
// immutable read-only image bytes either way, so the duplication is one map
// entry per function per thread and nothing more.
thread_local std::unordered_map<const void*, SourceSlice> g_fnSources;

}  // namespace

std::string_view rtFunctionSourceText(const void* code) {
    auto it = g_fnSources.find(code);
    if (it == g_fnSources.end()) return {};
    return std::string_view(it->second.text + it->second.begin, it->second.length);
}

extern "C" void bronze_register_fn_sources(const char* text, uint32_t textLen,
                                           const uint64_t* entries, uint32_t count) {
    if (!text || !entries) return;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t code = entries[i * 2];
        const uint64_t range = entries[i * 2 + 1];
        const uint32_t begin = static_cast<uint32_t>(range >> 32);
        const uint32_t length = static_cast<uint32_t>(range);
        // A range past the end of its own file would slice image bytes that
        // are not this file's. It cannot happen from a build of this compiler
        // — lowering takes both offsets from one parsed span — so the check is
        // against a MISMATCHED pair of module and runtime, where dropping the
        // entry leaves `toString` reporting a missing one rather than handing
        // back neighbouring rodata.
        if (begin > textLen || length > textLen - begin) continue;
        g_fnSources.insert_or_assign(reinterpret_cast<const void*>(static_cast<uintptr_t>(code)),
                                     SourceSlice{text, begin, length});
    }
}

}  // namespace bronze::runtime
