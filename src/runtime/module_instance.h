#pragma once

#include <cstdint>

namespace bronze::runtime {

// Whether the calling thread has registered its instance of the module whose
// slot cell is `slotCell` (bronze_abi.h, `bronze_module_instance`): it has
// run the module's entry, so its copy of the module's data is no longer the
// pristine one a first run starts from.
bool rtThreadHasModuleInstance(const uint64_t* slotCell);

}  // namespace bronze::runtime
