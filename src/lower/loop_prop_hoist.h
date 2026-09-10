#pragma once

#include "il/il.h"

namespace bronze::lower {

// Hoists loop-invariant property reads out of pure loops into their preheaders.
bool hoistLoopInvariantProps(il::Module& module);

}  // namespace bronze::lower
