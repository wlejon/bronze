#pragma once

#include <cstdint>
#include <unordered_map>

#include "il/il.h"

namespace bronze::lower {

// For a method-call site (by IC index) whose receivers the whole-program
// optimizer predicts hold its callee in an own slot, the one module function
// whose closures that slot holds (opt/wpo, `predictDirectTargets`).
using OwnCallees = std::unordered_map<uint32_t, uint32_t>;

// Splits each such `method.call` on its slot's closure: a test that the
// closure is one of the predicted function's (`closure.is`), whose hit calls
// that function's direct entry with the closure's environment, and whose miss
// is the original call (own_callee_calls.cpp). Returns how many sites.
uint32_t applyOwnCalleeCalls(il::Module& module, const OwnCallees& callees);

}  // namespace bronze::lower
