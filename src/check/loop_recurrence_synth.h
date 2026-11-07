#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ast/ast.h"

namespace bronze::check {

struct SynthesizedClosedForm {
    std::string expression;       // e.g. "((n * (n - 1)) / 2)" or "(n * n)"
    std::string returnStatement;  // e.g. "return (n <= 0) ? 0 : ((n * (n - 1)) / 2);"
    int64_t initVal = 0;
    int64_t d0 = 0;
    int64_t d1 = 0;
    int64_t d2 = 0;
    int64_t d3 = 0;
    int degree = 0;
};

// Evaluates `stepExpr` as an integer polynomial in induction variable `ivName`
// at `ivVal`. Returns std::nullopt if `stepExpr` is not a pure integer
// polynomial expression in `ivName`.
std::optional<int64_t> evalPolynomialStepExpr(const ast::Expr* stepExpr,
                                              std::string_view ivName,
                                              int64_t ivVal);

// Given the initial accumulator `initVal`, induction variable name `ivName`,
// upper bound identifier `boundName`, loop step expression `stepExpr`, and
// whether the accumulation wraps with `| 0` (`wrapsInt32`), synthesizes a
// closed-form polynomial expression in `boundName` via Newton's forward
// finite-difference formula and verifies it inductively via
// `proveLoopClosedFormInductive`.
std::optional<SynthesizedClosedForm> synthesizePolynomialLoopClosedForm(
    int64_t initVal,
    std::string_view ivName,
    std::string_view boundName,
    const ast::Expr* stepExpr,
    bool wrapsInt32 = false);

}  // namespace bronze::check
