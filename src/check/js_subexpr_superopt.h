#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "check/cegis_superopt.h"
#include "check/js_rewrite_ast.h"

namespace bronze::check::jsrw {

// Identifies pure, side-effect-free compound 32-bit integer sub-expressions in `p`
// and runs bounded CEGIS superoptimization (`superoptimizeJsFunction`) on each maximal
// candidate sub-expression to replace redundant bit-twiddling or branchy ternaries
// in-place with strictly lower-cost equivalent DAGs.
void collectSubexprSuperoptEdits(const ParsedJs& p,
                                 const std::string& fallbackFn,
                                 std::vector<Edit>& out,
                                 int32_t domainMin = -10000,
                                 int32_t domainMax = 10000,
                                 CegisSuperoptStats* accumStats = nullptr);

}  // namespace bronze::check::jsrw
