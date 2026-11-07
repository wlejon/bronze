#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "check/prove_equiv.h"

namespace bronze::check {

// Proves equivalence of a single-loop function vs. a closed-form function (or
// two single-loop functions) over unbounded symbolic trip counts in
// `[domainMin, domainMax]` via k-induction over `brass::verify::BVContext`.
FunctionEquivResult compareIlLoopFunctionPairInductive(const il::Module& oldMod,
                                                       const il::Function& oldFn,
                                                       const il::Module& newMod,
                                                       const il::Function& newFn,
                                                       bool strictJs32Bitwise,
                                                       int32_t domainMin,
                                                       int32_t domainMax);

// Parses and lowers `loopSrc` and `closedFormSrc`, and runs inductive loop
// equivalence verification over `[domainMin, domainMax]`.
EquivReport proveLoopClosedFormInductive(std::string_view loopSrc,
                                         std::string_view closedFormSrc,
                                         int32_t domainMin,
                                         int32_t domainMax,
                                         std::string* errorOut);

}  // namespace bronze::check
