#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"

namespace bronze::check::detail {

enum class NullState : uint8_t {
    Unknown,
    DefinitelyNull,
    DefinitelyUndefined,
    MaybeNullish,
    DefinitelyNonNullish,
};

struct AbstractVal {
    NullState nullState = NullState::Unknown;
    std::string nullOrigin;

    bool isNumber = false;
    bool isString = false;
    bool isDefinitelyNaN = false;
    std::string nanOrigin;

    bool hasConstNum = false;
    double constNum = 0.0;

    bool hasIntBounds = false;
    int64_t minInt = 0;
    int64_t maxInt = 0;

    // Symbolic upper bound from loop condition: `i < arr.length` (inclusive=false)
    // or `i <= arr.length` (inclusive=true).
    std::string symBoundArray;
    bool symBoundInclusive = false;

    // Known container length for Array literals and TypedArrays.
    bool isContainer = false;
    bool isTypedArray = false;
    bool hasExactLength = false;
    int64_t exactLength = 0;

    bool isNullishOrMaybe() const {
        return nullState == NullState::DefinitelyNull ||
               nullState == NullState::DefinitelyUndefined ||
               nullState == NullState::MaybeNullish;
    }
};

using EnvMap = std::unordered_map<std::string, AbstractVal>;

AbstractVal joinVal(const AbstractVal& a, const AbstractVal& b);
EnvMap joinEnv(const EnvMap& a, const EnvMap& b);

bool isTypedArrayCtorName(const std::string& name);
bool isArrayMutatingMethod(const std::string& prop);

std::string exprEnvKey(const ast::Expr& expr);
std::string shortExprName(const ast::Expr& expr);
std::string describeNullState(const AbstractVal& v, const std::string& fallbackExpr);

void collectClosureAssignedNames(const std::vector<ast::StmtPtr>& stmts,
                                 bool inNestedFn,
                                 std::unordered_set<std::string>& outNames);

bool stmtTerminates(const ast::Stmt& stmt);
bool stmtListTerminates(const std::vector<ast::StmtPtr>& stmts);

EnvMap narrowForCondition(const EnvMap& env, const ast::Expr* cond, bool whenTrue);

}  // namespace bronze::check::detail
