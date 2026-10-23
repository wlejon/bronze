#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bronze::check {

enum class SolveStatus : uint8_t {
    Sat,
    Unsat,
    Unknown,
    Error,
};

const char* solveStatusTag(SolveStatus status);

struct SolveOptions {
    int64_t targetReturn = 1;
    bool matchReturnValue = false;
    int32_t domainMin = INT32_MIN;
    int32_t domainMax = INT32_MAX;
    std::unordered_map<std::string, std::pair<int32_t, int32_t>> varBounds;
    bool minimizeResult = false;
    std::string fnName;
};

struct SolveResult {
    SolveStatus status = SolveStatus::Unknown;
    std::vector<int64_t> args;
    std::vector<std::string> paramNames;
    std::unordered_map<std::string, int64_t> modelMap;
    int64_t returnValue = 0;
    double solveTimeMs = 0.0;
    std::string error;

    bool sat() const { return status == SolveStatus::Sat; }
    bool unsat() const { return status == SolveStatus::Unsat; }
};

// Normalizes arrow functions `(a, b) => expr`, `(a, b) => { ... }`, `x => expr`,
// or anonymous `function(a, b) { ... }` into a valid named module function
// declaration `export function __solve_target(a, b) { ... }`.
std::string normalizeJsFunctionSource(std::string_view raw,
                                      std::string* outFnName = nullptr,
                                      std::string_view defaultName = "__solve_target");

// Symbolically lowers a JS function (or arrow function) to Brass bitvector SMT
// and solves for concrete 32-bit signed integer arguments that satisfy the
// predicate (`ret != 0` when `matchReturnValue == false`, or `ret == targetReturn`
// when `matchReturnValue == true`).
SolveResult solveJsFunction(std::string_view fnSource,
                            const SolveOptions& opts = {});

// Iteratively solves for up to `maxSolutions` distinct concrete argument tuples
// satisfying `fnSource` by adding blocking clauses `!(arg_0 == m_0 && ...)`
// after each satisfying assignment.
std::vector<std::vector<int64_t>> solveAllJsFunction(std::string_view fnSource,
                                                     size_t maxSolutions = 10,
                                                     const SolveOptions& opts = {});

// Evaluates a symbolically decidable JS function on concrete 32-bit integer
// arguments via Brass's bitvector DAG concrete evaluator.
bool evalConcreteJsFunction(std::string_view fnSource,
                            const std::vector<int64_t>& args,
                            int64_t* outReturn,
                            std::string* errorOut = nullptr,
                            std::string_view fnFilter = "");

}  // namespace bronze::check
