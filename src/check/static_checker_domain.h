#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "check/static_checker.h"
#include "il/il.h"
#include "support/diagnostics.h"
#include "types/infer.h"

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

    // Real-valued interval and mathematical domain invariants
    bool hasNumBounds = false;
    double minNum = -INFINITY;
    double maxNum = INFINITY;
    bool isNonZero = false;
    bool isNonNegative = false;
    bool isStrictlyPositive = false;
    bool isClampedUnit = false;
    bool isByteClamped = false;
    bool isPotentiallyNegativeDiff = false;
    bool isVectorNormExpr = false;

    // Symbolic upper bound from loop condition: `i < arr.length` (inclusive=false)
    // or `i <= arr.length` (inclusive=true).
    std::string symBoundArray;
    bool symBoundInclusive = false;

    // Known container length for Array literals and TypedArrays.
    bool isContainer = false;
    bool isTypedArray = false;
    bool isPlainObject = false;
    bool hasExactLength = false;
    int64_t exactLength = 0;

    // Keys in EnvMap guaranteed to be non-nullish whenever this value is truthy.
    std::vector<std::string> impliedNonNullKeys;

    bool isNullishOrMaybe() const {
        return nullState == NullState::DefinitelyNull ||
               nullState == NullState::DefinitelyUndefined ||
               nullState == NullState::MaybeNullish;
    }

    bool provablyInUnitInterval() const {
        if (isClampedUnit) return true;
        if (hasConstNum && std::isfinite(constNum) && constNum >= -1.0 && constNum <= 1.0) {
            return true;
        }
        if (hasNumBounds && minNum >= -1.0 && maxNum <= 1.0) return true;
        if (hasIntBounds && minInt >= -1 && maxInt <= 1) return true;
        return false;
    }

    bool provablyNonNegative() const {
        if (isNonNegative || isStrictlyPositive || isByteClamped) return true;
        if (hasConstNum && std::isfinite(constNum) && constNum >= 0.0) return true;
        if (hasNumBounds && minNum >= 0.0) return true;
        if (hasIntBounds && minInt >= 0) return true;
        return false;
    }

    bool provablyStrictlyPositive() const {
        if (isStrictlyPositive) return true;
        if (isNonNegative && isNonZero) return true;
        if (hasConstNum && std::isfinite(constNum) && constNum > 0.0) return true;
        if (hasNumBounds && minNum > 0.0) return true;
        if (hasIntBounds && minInt > 0) return true;
        return false;
    }

    bool provablyNonZero() const {
        if (isNonZero || isStrictlyPositive) return true;
        if (hasConstNum && std::isfinite(constNum) && constNum != 0.0) return true;
        if (hasNumBounds && (minNum > 0.0 || maxNum < 0.0)) return true;
        if (hasIntBounds && (minInt > 0 || maxInt < 0)) return true;
        return false;
    }

    bool provablyShiftSafe30() const {
        if (hasConstNum && std::isfinite(constNum) && constNum >= 0.0 && constNum <= 30.0) {
            return true;
        }
        if (hasIntBounds && minInt >= 0 && maxInt <= 30) return true;
        if (hasNumBounds && minNum >= 0.0 && maxNum <= 30.0) return true;
        return false;
    }

    bool provablyByteRange() const {
        if (isByteClamped) return true;
        if (hasConstNum && std::isfinite(constNum) && constNum >= 0.0 && constNum <= 255.0) {
            return true;
        }
        if (hasIntBounds && minInt >= 0 && maxInt <= 255) return true;
        if (hasNumBounds && minNum >= 0.0 && maxNum <= 255.0) return true;
        return false;
    }
};

using EnvMap = std::unordered_map<std::string, AbstractVal>;

AbstractVal joinVal(const AbstractVal& a, const AbstractVal& b);
EnvMap joinEnv(const EnvMap& a, const EnvMap& b);
void mergeLoopEnv(EnvMap& env,
                  const EnvMap& bodyEnv,
                  const std::vector<ast::StmtPtr>& loopBody);
void recordCorrelatedNonNull(EnvMap& env,
                             const std::string& targetKey,
                             const ast::Expr* initExpr,
                             AbstractVal& initVal);
bool isExplicitNaNSentinel(const AbstractVal& val);

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

class StaticAnalyzer {
public:
    StaticAnalyzer(const types::InferenceResult* inferred,
                   const il::Module* ilMod,
                   DiagnosticSink* diags,
                   bool strictDomNullability)
        : inferred_(inferred),
          ilMod_(ilMod),
          diags_(diags),
          strictDomNullability_(strictDomNullability) {}

    CheckReport run(const ast::Module& mod);

private:
    const types::InferenceResult* inferred_ = nullptr;
    const il::Module* ilMod_ = nullptr;
    DiagnosticSink* diags_ = nullptr;
    bool strictDomNullability_ = true;
    CheckReport report_;
    std::unordered_set<std::string> closureAssigned_;
    std::unordered_set<uint64_t> emittedKeys_;

    // Core reporting and statement/expression traversal (in static_checker.cpp)
    void reportFinding(BugKind kind, Span span, std::string message);
    void checkNotNullishReceiver(const AbstractVal& recvVal,
                                 const ast::Expr& recvExpr,
                                 Span accessSpan,
                                 const std::string& actionDesc);
    void checkNumericOperandForNaN(const AbstractVal& val,
                                   const ast::Expr& operandExpr,
                                   Span opSpan,
                                   const std::string& contextDesc);
    AbstractVal evalExpr(const ast::Expr* expr, EnvMap& env, bool isWriteTarget = false);
    void checkFunctionBody(const std::vector<ast::Param>& params,
                           const std::vector<ast::StmtPtr>& body,
                           const EnvMap& outerEnv);
    void checkStmtList(const std::vector<ast::StmtPtr>& stmts, EnvMap& env);
    void checkStmt(const ast::Stmt& stmt, EnvMap& env);

    // Numeric, Math domain, division-by-zero, shift-overflow, and bounds analysis
    // (in static_checker_math.cpp)
    void checkIndexBounds(const AbstractVal& objVal,
                          const AbstractVal& idxVal,
                          const ast::Expr& objExpr,
                          const ast::Expr& idxExpr,
                          Span span,
                          bool isWriteTarget);
    AbstractVal evalUnary(const ast::Unary& un, EnvMap& env);
    AbstractVal evalCall(const ast::Call& call, EnvMap& env);
    AbstractVal evalMathCall(const ast::Call& call,
                             const std::string& prop,
                             const std::vector<AbstractVal>& argVals);
    AbstractVal evalBinary(const ast::Binary& bin, EnvMap& env);
};

}  // namespace bronze::check::detail
