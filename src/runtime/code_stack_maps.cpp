// The stack maps of compiled code that brass did not load itself: an object
// linked into a program, or a module image a host loaded. A JIT registers its
// maps in brass's code registry as it loads code; for the rest, each
// function's encoded map rides in its bronze_code_range (bronze_abi.h), and
// registering the ranges hands them to the same registry. A collection's
// stack walk then finds the Values these functions' frames hold, whichever
// way the code came to be in the process.

#include "runtime/code_stack_maps.h"

#include <brass/gc/code_stack_maps.hpp>
#include <brass/gc/stack_map.hpp>

#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>

namespace bronze::runtime {

namespace {

struct Registrations {
    std::mutex mu;
    // Per registered range table, the registry token keeping its maps.
    std::unordered_map<const bronze_code_range*, std::shared_ptr<const void>> tokens;
};

Registrations& registrations() {
    static auto* r = new Registrations();
    return *r;
}

}  // namespace

void registerCodeStackMaps(const bronze_code_range* ranges, uint32_t count) {
    if (!ranges || count == 0) return;
    Registrations& r = registrations();
    std::lock_guard<std::mutex> lock(r.mu);
    if (r.tokens.count(ranges)) return;
    brass::ModuleStackMap maps;
    for (uint32_t i = 0; i < count; ++i) {
        const bronze_code_range& range = ranges[i];
        if (!range.stack_map || range.stack_map_size == 0 || !range.code_start) continue;
        const auto start = reinterpret_cast<uintptr_t>(range.code_start);
        // Code whose maps its loader registered already (a JIT's).
        if (brass::code_stack_maps_cover(start)) continue;
        brass::ModuleStackMap one;
        if (!brass::decode_stack_maps_into(std::span<const uint8_t>(range.stack_map, range.stack_map_size), one)) {
            continue;
        }
        for (brass::FunctionStackMap& fn : one.functions()) {
            fn.function_address = start;
            fn.code_size = range.code_size;
            maps.add_function(std::move(fn));
        }
    }
    if (maps.empty()) return;
    r.tokens.emplace(ranges, brass::register_code_stack_maps(maps));
}

void unregisterCodeStackMaps(const bronze_code_range* ranges) {
    if (!ranges) return;
    Registrations& r = registrations();
    std::lock_guard<std::mutex> lock(r.mu);
    r.tokens.erase(ranges);
}

}  // namespace bronze::runtime
