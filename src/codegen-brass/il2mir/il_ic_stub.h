#pragma once

// Tier 2's inverse of the lowering's small forms. A read in the lower tiers
// scans its site's other ways through a call (kPolyScanHelper,
// il_property.h); tier 2 rewrites each such call back into the inline scan,
// with the site's test reading the hit and the slot load indexed, so its code
// is what the inline form always was.

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <cstddef>

namespace il2mir {

// Expands every kPolyScanHelper call of `fn`; returns how many.
size_t expand_ic_stubs(Function& fn);

} // namespace il2mir
