// bronze_cold_resolve (abi/bronze_abi_functions.h): what an AOT image's stub
// of an interpreted cold function calls when its cell is still empty
// (codegen-brass/cold_companion.h). The work is the code generator's, which
// the runtime does not link: eval installs it (eval/aot_module.cpp).

#include "runtime/cold_resolve.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace bronze::runtime {

namespace {
std::atomic<ColdResolver> g_resolver{nullptr};
}  // namespace

void setColdResolver(ColdResolver resolver) { g_resolver.store(resolver, std::memory_order_release); }

}  // namespace bronze::runtime

extern "C" void bronze_cold_resolve(const void* link, uint32_t cell) {
    const bronze::runtime::ColdResolver resolver = bronze::runtime::g_resolver.load(std::memory_order_acquire);
    if (resolver == nullptr) {
        std::fprintf(stderr, "bronze: fatal: an interpreted cold function (cell %u) was called in a process "
                             "that cannot build its code\n", cell);
        std::fflush(stderr);
        std::abort();
    }
    resolver(link, cell);
}
