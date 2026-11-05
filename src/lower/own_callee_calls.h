#pragma once

#include <cstdint>
#include <functional>
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

// The same split for `call.dynamic`, on its callee operand: `targets` names
// the module function a site's closure is expected to be (a type-feedback
// profile's one callee, opt/spec/feedback_plan.h), or UINT32_MAX. The hit
// enters that function directly with the call's `this` and arguments and the
// closure's environment; the miss is the original call. Returns how many.
using ClosureTargets = std::function<uint32_t(const il::Instruction&)>;
uint32_t applyClosureCalls(il::Module& module, const ClosureTargets& targets);

}  // namespace bronze::lower
