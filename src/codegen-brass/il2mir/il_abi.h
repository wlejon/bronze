#pragma once

// The runtime ABI facts the translator bakes into the MIR it emits, taken
// from the ABI headers themselves so a change there moves the translator
// with it.

#include "abi/bronze_abi.h"
#include "il_ast.h"
#include <cstdint>
#include <string>

namespace brass {
class Module;
}

namespace il2mir {

constexpr uint64_t kUndefinedTag = BRONZE_ABI_UNDEFINED_BITS;
constexpr uint64_t kNullTag = uint64_t{BRONZE_ABI_TAG_NULL} << 48;
constexpr uint64_t kBoolTag = uint64_t{BRONZE_ABI_TAG_BOOL} << 48;
constexpr uint64_t kInt32Tag = uint64_t{BRONZE_ABI_TAG_INT32} << 48;
// Marker values a NameResolve of `print` / `print.err` lowers to.
constexpr uint64_t kPrintTag = 0xFFFE000000000001ULL;
constexpr uint64_t kPrintErrTag = 0xFFFE000000000002ULL;

// The thread-local block, as pinned-register code reads it.
constexpr int32_t kBronzeTlsStackLimitOff = BRONZE_TLS_STACK_LIMIT_OFF;
// The thread's module-delta array, indexed by a module's slot cell: what
// `TranslatorOptions::per_thread_module_data` adds to every module-data
// address.
constexpr int32_t kBronzeTlsModuleDeltasOff = BRONZE_TLS_MODULE_DELTAS_OFF;
// One inline-cache SITE in the module's `__bronze_ic_table`. The table is
// indexed by site, and a helper takes the address of a site's way 0.
constexpr uint32_t kBronzeIcSiteSize = BRONZE_ABI_IC_SITE_SIZE;

// Declare every runtime helper and module data symbol the lowered module
// may reference, with the optimizer contracts (SymbolRole) of the ones it
// has them for. `entry_symbol` names the module-suffixed data symbols.
void register_all_module_external_symbols(brass::Module* mod, const std::string& entry_symbol);

// The module's code-range table and its length (bronze_abi.h,
// bronze_code_range), as the object emitter names them for `entry_symbol`.
std::string code_ranges_symbol(const std::string& entry_symbol);
std::string code_range_count_symbol(const std::string& entry_symbol);

} // namespace il2mir
