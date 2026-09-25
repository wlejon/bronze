#pragma once

#include <cstdint>

#include "abi/bronze_abi.h"

namespace bronze::runtime {

// Registers the stack maps the ranges carry (bronze_code_range::stack_map)
// with brass's code registry, for the ranges whose code has none registered
// yet; a table registered already is left as it is. The maps stay
// registered until unregisterCodeStackMaps(ranges).
void registerCodeStackMaps(const bronze_code_range* ranges, uint32_t count);
void unregisterCodeStackMaps(const bronze_code_range* ranges);

}  // namespace bronze::runtime
