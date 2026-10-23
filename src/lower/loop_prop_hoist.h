#pragma once

#include "il/il.h"

namespace bronze::lower {

class DirectMethodTable;

// Hoists loop-invariant property reads out of pure loops into their preheaders.
bool hoistLoopInvariantProps(il::Module& module, const DirectMethodTable* directMethods = nullptr);

}  // namespace bronze::lower
