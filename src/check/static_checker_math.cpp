#include "check/static_checker_domain.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "types/host_contracts.h"

namespace bronze::check::detail {

namespace {

bool isStructuralSquareExpr(const ast::Expr& a, const ast::Expr& b) {
    std::string ka = exprEnvKey(a);
    std::string kb = exprEnvKey(b);
    if (!ka.empty() && ka == kb) return true;
    const auto* ba = ast::exactAs<const ast::Binary*>(&a);
    const auto* bb = ast::exactAs<const ast::Binary*>(&b);
    if (ba && bb && ba->op == bb->op && ba->lhs && ba->rhs && bb->lhs && bb->rhs) {
        std::string al = exprEnvKey(*ba->lhs);
        std::string ar = exprEnvKey(*ba->rhs);
        std::string bl = exprEnvKey(*bb->lhs);
        std::string br = exprEnvKey(*bb->rhs);
        if (!al.empty() && !ar.empty() && al == bl && ar == br) return true;
    }
    return false;
}

bool isNormOrDetIdentifier(const std::string& name) {
    return name == "len" || name == "length" || name == "l" || name == "norm" ||
           name == "mag" || name == "radius" || name == "this.radius" ||
           name == "det" || name == "denom" || name == "denominator" ||
           name == "s" || name == "lenA" || name == "lenB" ||
           name.ends_with(".length()");
}

}  // namespace

AbstractVal StaticAnalyzer::evalMathCall(const ast::Call& call,
                                         const std::string& prop,
                                         const std::vector<AbstractVal>& argVals) {
    AbstractVal res;
    res.nullState = NullState::DefinitelyNonNullish;
    res.isNumber = true;

    for (size_t i = 0; i < argVals.size(); ++i) {
        checkNumericOperandForNaN(argVals[i], *call.args[i], call.span, "'Math." + prop + "(...)'");
        if (argVals[i].isDefinitelyNaN || argVals[i].nullState == NullState::DefinitelyUndefined) {
            res.isDefinitelyNaN = true;
            res.nanOrigin = "'Math." + prop + "(...)' with NaN/undefined";
        }
    }

    if (argVals.empty()) return res;
    const AbstractVal& a0 = argVals[0];
    std::string a0Name = call.args[0] ? shortExprName(*call.args[0]) : "arg";

    if (prop == "acos" || prop == "asin") {
        if (a0.hasConstNum) {
            double c = a0.constNum;
            if (c < -1.0 || c > 1.0) {
                res.isDefinitelyNaN = true;
                res.nanOrigin = "'Math." + prop + "(" + std::to_string(c) + ")'";
                reportFinding(BugKind::NaNPoison,
                              call.span,
                              "[domain-error] 'Math." + prop + "' called with out-of-domain argument " +
                                  std::to_string(c) + " outside [-1, 1] produces NaN");
            } else {
                res.hasConstNum = true;
                res.constNum = (prop == "acos") ? std::acos(c) : std::asin(c);
            }
        } else if (!a0.provablyInUnitInterval()) {
            reportFinding(BugKind::NaNPoison,
                          call.span,
                          "[domain-error] 'Math." + prop + "' argument '" + a0Name +
                              "' is not clamped to [-1, 1] and produces NaN on floating-point drift");
        }
        res.isNonNegative = (prop == "acos");
        return res;
    }

    if (prop == "sqrt") {
        if (a0.hasConstNum) {
            double c = a0.constNum;
            if (c < 0.0) {
                res.isDefinitelyNaN = true;
                res.nanOrigin = "'Math.sqrt(" + std::to_string(c) + ")'";
                reportFinding(BugKind::NaNPoison,
                              call.span,
                              "[domain-error] 'Math.sqrt' called with negative argument " +
                                  std::to_string(c) + " produces NaN");
            } else {
                res.hasConstNum = true;
                res.constNum = std::sqrt(c);
                res.isNonZero = (c > 0.0);
                res.isStrictlyPositive = (c > 0.0);
            }
        } else if ((a0.hasNumBounds && a0.minNum < 0.0) ||
                   (a0.hasIntBounds && a0.minInt < 0) ||
                   (a0.isPotentiallyNegativeDiff && !a0.provablyNonNegative())) {
            reportFinding(BugKind::NaNPoison,
                          call.span,
                          "[domain-error] 'Math.sqrt' argument '" + a0Name +
                              "' can be negative (< 0) without clamping and produces NaN");
        }
        res.isNonNegative = true;
        res.isVectorNormExpr = true;
        if (a0.provablyStrictlyPositive()) {
            res.isStrictlyPositive = true;
            res.isNonZero = true;
        }
        return res;
    }

    if (prop == "log" || prop == "log2" || prop == "log10") {
        if (a0.hasConstNum) {
            double c = a0.constNum;
            if (c < 0.0) {
                res.isDefinitelyNaN = true;
                res.nanOrigin = "'Math." + prop + "(" + std::to_string(c) + ")'";
                reportFinding(BugKind::NaNPoison,
                              call.span,
                              "[domain-error] 'Math." + prop + "' called with negative argument " +
                                  std::to_string(c) + " produces NaN");
            } else if (c == 0.0) {
                reportFinding(BugKind::NaNPoison,
                              call.span,
                              "[domain-error] 'Math." + prop + "' called with 0 produces -Infinity");
            }
        } else if (!a0.provablyStrictlyPositive()) {
            reportFinding(BugKind::NaNPoison,
                          call.span,
                          "[domain-error] 'Math." + prop + "' argument '" + a0Name +
                              "' is not guarded > 0 (<= 0 produces -Infinity or NaN)");
        }
        return res;
    }

    if (prop == "abs") {
        res.isNonNegative = true;
        if (a0.hasConstNum) {
            res.hasConstNum = true;
            res.constNum = std::abs(a0.constNum);
        }
        if (a0.provablyInUnitInterval()) {
            res.isClampedUnit = true;
            res.hasNumBounds = true;
            res.minNum = 0.0;
            res.maxNum = 1.0;
        }
        if (a0.provablyNonZero()) {
            res.isNonZero = true;
            res.isStrictlyPositive = true;
        }
        return res;
    }

    if (prop == "sin" || prop == "cos") {
        res.hasNumBounds = true;
        res.minNum = -1.0;
        res.maxNum = 1.0;
        res.isClampedUnit = true;
        return res;
    }

    if (prop == "exp") {
        res.isNonNegative = true;
        res.isStrictlyPositive = true;
        res.isNonZero = true;
        return res;
    }

    if (prop == "pow" && argVals.size() >= 2) {
        if (a0.provablyStrictlyPositive()) {
            res.isNonNegative = true;
            res.isStrictlyPositive = true;
            res.isNonZero = true;
        } else if (argVals[1].hasConstNum && argVals[1].constNum == 2.0) {
            res.isNonNegative = true;
        }
        return res;
    }

    if (prop == "hypot") {
        res.isNonNegative = true;
        res.isVectorNormExpr = true;
        for (const auto& av : argVals) {
            if (av.provablyNonZero()) {
                res.isNonZero = true;
                res.isStrictlyPositive = true;
            }
        }
        return res;
    }

    if (prop == "clz32") {
        res.hasIntBounds = true;
        res.minInt = 0;
        res.maxInt = 32;
        res.hasNumBounds = true;
        res.minNum = 0.0;
        res.maxNum = 32.0;
        res.isNonNegative = true;
        res.isByteClamped = true;
        return res;
    }

    if (prop == "round" || prop == "floor" || prop == "ceil" || prop == "trunc") {
        res.isNonNegative = a0.provablyNonNegative();
        res.isClampedUnit = a0.provablyInUnitInterval();
        res.isByteClamped = a0.provablyByteRange();
        if (a0.hasNumBounds) {
            res.hasNumBounds = true;
            res.minNum = (prop == "floor") ? std::floor(a0.minNum)
                         : (prop == "ceil") ? std::ceil(a0.minNum)
                         : (prop == "round") ? std::round(a0.minNum)
                                             : std::trunc(a0.minNum);
            res.maxNum = (prop == "floor") ? std::floor(a0.maxNum)
                         : (prop == "ceil") ? std::ceil(a0.maxNum)
                         : (prop == "round") ? std::round(a0.maxNum)
                                             : std::trunc(a0.maxNum);
            if (std::isfinite(res.minNum) && std::isfinite(res.maxNum)) {
                res.hasIntBounds = true;
                res.minInt = static_cast<int64_t>(res.minNum);
                res.maxInt = static_cast<int64_t>(res.maxNum);
            }
        }
        return res;
    }

    if (prop == "min" && argVals.size() >= 2) {
        double maxB = INFINITY;
        double minB = -INFINITY;
        bool anyMax = false;
        bool allMin = true;
        for (const auto& av : argVals) {
            if (av.hasConstNum && std::isfinite(av.constNum)) {
                maxB = std::min(maxB, av.constNum);
                anyMax = true;
                minB = (minB == -INFINITY) ? av.constNum : std::min(minB, av.constNum);
            } else if (av.hasNumBounds) {
                if (std::isfinite(av.maxNum)) {
                    maxB = std::min(maxB, av.maxNum);
                    anyMax = true;
                }
                if (std::isfinite(av.minNum)) {
                    minB = (minB == -INFINITY) ? av.minNum : std::min(minB, av.minNum);
                } else {
                    allMin = false;
                }
            } else {
                allMin = false;
            }
        }
        if (anyMax || allMin) {
            res.hasNumBounds = true;
            res.maxNum = anyMax ? maxB : INFINITY;
            res.minNum = allMin ? minB : -INFINITY;
            if (res.minNum >= -1.0 && res.maxNum <= 1.0) res.isClampedUnit = true;
            if (res.minNum >= 0.0 && res.maxNum <= 255.0) res.isByteClamped = true;
            if (res.minNum >= 0.0) res.isNonNegative = true;
            if (res.minNum > 0.0) {
                res.isStrictlyPositive = true;
                res.isNonZero = true;
            }
        }
        return res;
    }

    if (prop == "max" && argVals.size() >= 2) {
        double minB = -INFINITY;
        double maxB = INFINITY;
        bool anyMin = false;
        bool anyBoundedMax = false;
        for (const auto& av : argVals) {
            if (av.hasConstNum && std::isfinite(av.constNum)) {
                minB = std::max(minB, av.constNum);
                anyMin = true;
            } else if (av.hasNumBounds) {
                if (std::isfinite(av.minNum)) {
                    minB = std::max(minB, av.minNum);
                    anyMin = true;
                }
                if (std::isfinite(av.maxNum)) {
                    maxB = std::min(maxB, av.maxNum);
                    anyBoundedMax = true;
                }
            }
        }
        if (anyMin) {
            res.hasNumBounds = true;
            res.minNum = minB;
            if (anyBoundedMax && maxB >= minB) {
                res.maxNum = maxB;
            }
            if (res.minNum >= -1.0 && res.maxNum <= 1.0) res.isClampedUnit = true;
            if (res.minNum >= 0.0 && res.maxNum <= 255.0) res.isByteClamped = true;
            if (res.minNum >= 0.0) res.isNonNegative = true;
            if (res.minNum > 0.0) {
                res.isStrictlyPositive = true;
                res.isNonZero = true;
            }
        }
        return res;
    }

    return res;
}

AbstractVal StaticAnalyzer::evalCall(const ast::Call& call, EnvMap& env) {
    AbstractVal res;
    bool inOptionalChain = call.optional ||
                           (call.callee && !call.callee->parenthesized &&
                            ast::containsOptionalLink(*call.callee));
    std::vector<AbstractVal> argVals;
    argVals.reserve(call.args.size());
    for (const auto& a : call.args) argVals.push_back(evalExpr(a.get(), env));

    auto applyClampBounds = [&](const AbstractVal& loArg, const AbstractVal& hiArg) -> bool {
        if (!loArg.hasConstNum || !hiArg.hasConstNum) return false;
        double lo = loArg.constNum;
        double hi = hiArg.constNum;
        if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi) return false;
        res.nullState = NullState::DefinitelyNonNullish;
        res.isNumber = true;
        res.hasNumBounds = true;
        res.minNum = lo;
        res.maxNum = hi;
        if (lo >= -1.0 && hi <= 1.0) res.isClampedUnit = true;
        if (lo >= 0.0 && hi <= 255.0) res.isByteClamped = true;
        if (lo >= 0.0) res.isNonNegative = true;
        if (lo > 0.0) {
            res.isStrictlyPositive = true;
            res.isNonZero = true;
        }
        return true;
    };

    if (const auto* mem = ast::exactAs<const ast::MemberAccess*>(call.callee.get())) {
        AbstractVal recvVal = evalExpr(mem->object.get(), env);
        bool memOptional = inOptionalChain || mem->optional ||
                           (mem->object && !mem->object->parenthesized &&
                            ast::containsOptionalLink(*mem->object));
        if (!memOptional && mem->object) {
            checkNotNullishReceiver(recvVal, *mem->object, call.span, "call method '" + mem->property + "'");
        }
        if (mem->object && isArrayMutatingMethod(mem->property)) {
            std::string key = exprEnvKey(*mem->object);
            if (!key.empty()) {
                auto it = env.find(key);
                if (it != env.end()) it->second.hasExactLength = false;
            }
        }

        if (const auto* baseId = ast::exactAs<const ast::Ident*>(mem->object.get())) {
            if (baseId->name == "Math") {
                return evalMathCall(call, mem->property, argVals);
            }
        }

        if (mem->property == "clamp" && argVals.size() == 3 && applyClampBounds(argVals[1], argVals[2])) {
            return res;
        }
        if (mem->property == "clampScalar" && argVals.size() == 2 && applyClampBounds(argVals[0], argVals[1])) {
            return res;
        }
        if ((mem->property == "length" || mem->property == "lengthSq" ||
             mem->property == "manhattanLength") &&
            argVals.empty()) {
            res.nullState = NullState::DefinitelyNonNullish;
            res.isNumber = true;
            res.isNonNegative = true;
            res.isVectorNormExpr = true;
            return res;
        }
        if (mem->property == "dot" && argVals.size() == 1 && mem->object && call.args[0]) {
            res.nullState = NullState::DefinitelyNonNullish;
            res.isNumber = true;
            if (isStructuralSquareExpr(*mem->object, *call.args[0])) {
                res.isNonNegative = true;
                res.isVectorNormExpr = true;
            }
            return res;
        }
        if (mem->property == "divideScalar" && argVals.size() == 1 && call.args[0]) {
            if (!argVals[0].provablyNonZero()) {
                reportFinding(BugKind::NaNPoison,
                              call.span,
                              "[zero-norm] [div-by-zero] 'divideScalar' called with unguarded divisor '" +
                                  shortExprName(*call.args[0]) + "' that can be 0");
            }
            res.nullState = NullState::DefinitelyNonNullish;
            return res;
        }
    } else if (call.callee) {
        AbstractVal calleeVal = evalExpr(call.callee.get(), env);
        if (!inOptionalChain) {
            checkNotNullishReceiver(calleeVal, *call.callee, call.span, "invoke function");
        }
        if (const auto* cid = ast::exactAs<const ast::Ident*>(call.callee.get())) {
            if (cid->name == "clamp" && argVals.size() == 3 && applyClampBounds(argVals[1], argVals[2])) {
                return res;
            }
        }
    }

    std::string callKey = exprEnvKey(call);
    if (!callKey.empty()) {
        auto it = env.find(callKey);
        if (it != env.end()) {
            return it->second;
        }
    }

    types::HostNullability hostNull = types::classifyHostCallNullability(
        call, [](const std::string&) { return false; });
    if (strictDomNullability_ &&
        (hostNull == types::HostNullability::MaybeNullish ||
         hostNull == types::HostNullability::AlwaysNullish)) {
        res.nullState = NullState::MaybeNullish;
        res.nullOrigin = types::describeHostNullabilitySource(call);
        return res;
    }
    if (hostNull == types::HostNullability::DefinitelyNonNullish) {
        res.nullState = NullState::DefinitelyNonNullish;
        return res;
    }

    if (inOptionalChain) {
        res.nullState = NullState::MaybeNullish;
    }
    return res;
}

AbstractVal StaticAnalyzer::evalBinary(const ast::Binary& bin, EnvMap& env) {
    AbstractVal res;

    if (bin.op == ast::BinaryOp::LogicalAnd) {
        AbstractVal lv = evalExpr(bin.lhs.get(), env);
        EnvMap rhsEnv = narrowForCondition(env, bin.lhs.get(), true);
        AbstractVal rv = evalExpr(bin.rhs.get(), rhsEnv);
        return joinVal(lv, rv);
    }
    if (bin.op == ast::BinaryOp::LogicalOr || bin.op == ast::BinaryOp::NullishCoalescing) {
        AbstractVal lv = evalExpr(bin.lhs.get(), env);
        EnvMap rhsEnv = narrowForCondition(env, bin.lhs.get(), false);
        AbstractVal rv = evalExpr(bin.rhs.get(), rhsEnv);
        res = joinVal(lv, rv);
        if (lv.nullState == NullState::DefinitelyNonNullish ||
            rv.nullState == NullState::DefinitelyNonNullish) {
            res.nullState = NullState::DefinitelyNonNullish;
            res.nullOrigin.clear();
        } else if (!rv.isNullishOrMaybe()) {
            res.nullState = rv.nullState;
            res.nullOrigin = rv.nullOrigin;
        }
        if (bin.op == ast::BinaryOp::LogicalOr) {
            if (lv.provablyNonZero() || rv.provablyNonZero()) {
                res.isNonZero = true;
            }
            if (lv.provablyNonNegative() && rv.provablyStrictlyPositive()) {
                res.isNonNegative = true;
                res.isStrictlyPositive = true;
                res.isNonZero = true;
            }
        }
        return res;
    }

    if (bin.op == ast::BinaryOp::Assign) {
        AbstractVal rv = evalExpr(bin.rhs.get(), env);
        if (bin.lhs) {
            if (const auto* idx = ast::exactAs<const ast::IndexAccess*>(bin.lhs.get())) {
                AbstractVal objVal = evalExpr(idx->object.get(), env);
                AbstractVal idxVal = evalExpr(idx->index.get(), env);
                if (!idx->optional && idx->object) {
                    checkNotNullishReceiver(objVal, *idx->object, idx->span, "assign element");
                }
                if (idx->object && idx->index) {
                    checkIndexBounds(objVal, idxVal, *idx->object, *idx->index, idx->span, true);
                }
                if (objVal.isTypedArray && bin.rhs && !isExplicitNaNSentinel(rv)) {
                    checkNumericOperandForNaN(
                        rv, *bin.rhs, bin.span,
                        "TypedArray element store '" + shortExprName(*bin.lhs) + "'");
                }
                if (idx->object && !objVal.isTypedArray) {
                    std::string arrKey = exprEnvKey(*idx->object);
                    if (!arrKey.empty()) {
                        auto it = env.find(arrKey);
                        if (it != env.end()) it->second.hasExactLength = false;
                    }
                }
            } else if (const auto* mem = ast::exactAs<const ast::MemberAccess*>(bin.lhs.get())) {
                AbstractVal objVal = evalExpr(mem->object.get(), env);
                if (!mem->optional && mem->object) {
                    checkNotNullishReceiver(
                        objVal, *mem->object, mem->span, "assign property '" + mem->property + "'");
                }
                std::string key = exprEnvKey(*mem);
                if (!key.empty()) {
                    recordCorrelatedNonNull(env, key, bin.rhs.get(), rv);
                    env[key] = rv;
                }
            } else {
                std::string key = exprEnvKey(*bin.lhs);
                if (!key.empty()) {
                    recordCorrelatedNonNull(env, key, bin.rhs.get(), rv);
                    if (closureAssigned_.count(key) > 0 &&
                        (rv.nullState == NullState::DefinitelyNull ||
                         rv.nullState == NullState::DefinitelyUndefined)) {
                        rv.nullState = NullState::Unknown;
                        rv.nullOrigin.clear();
                    }
                    env[key] = rv;
                }
            }
        }
        return rv;
    }

    if (ast::isCompoundAssignOp(bin.op)) {
        AbstractVal lv = evalExpr(bin.lhs.get(), env);
        AbstractVal rv = evalExpr(bin.rhs.get(), env);
        ast::BinaryOp baseOp = ast::compoundAssignBase(bin.op);
        bool isStringConcat = (baseOp == ast::BinaryOp::Add && (lv.isString || rv.isString));
        if (!isStringConcat &&
            (baseOp == ast::BinaryOp::Add || baseOp == ast::BinaryOp::Sub ||
             baseOp == ast::BinaryOp::Mul || baseOp == ast::BinaryOp::Div ||
             baseOp == ast::BinaryOp::Mod || baseOp == ast::BinaryOp::Exp)) {
            if (bin.lhs) {
                checkNumericOperandForNaN(
                    lv, *bin.lhs, bin.span,
                    std::string("compound assignment '") + ast::binaryOpName(bin.op) + "'");
            }
            if (bin.rhs) {
                checkNumericOperandForNaN(
                    rv, *bin.rhs, bin.span,
                    std::string("compound assignment '") + ast::binaryOpName(bin.op) + "'");
            }
            if ((baseOp == ast::BinaryOp::Div || baseOp == ast::BinaryOp::Mod) &&
                bin.rhs && !rv.provablyNonZero()) {
                std::string rhsName = shortExprName(*bin.rhs);
                reportFinding(BugKind::NaNPoison,
                              bin.span,
                              "[div-by-zero] compound division/modulo by unguarded divisor '" +
                                  rhsName + "' that can be 0");
            }
        }
        res.nullState = NullState::DefinitelyNonNullish;
        if (!isStringConcat) {
            res.isNumber = true;
            if (lv.isDefinitelyNaN || rv.isDefinitelyNaN ||
                lv.nullState == NullState::DefinitelyUndefined ||
                rv.nullState == NullState::DefinitelyUndefined) {
                res.isDefinitelyNaN = true;
                res.nanOrigin = "compound arithmetic with NaN/undefined";
            }
        } else {
            res.isString = true;
        }
        if (bin.lhs) {
            std::string key = exprEnvKey(*bin.lhs);
            if (!key.empty()) env[key] = res;
        }
        return res;
    }

    AbstractVal lv = evalExpr(bin.lhs.get(), env);
    AbstractVal rv = evalExpr(bin.rhs.get(), env);
    res.nullState = NullState::DefinitelyNonNullish;

    if (bin.op == ast::BinaryOp::Shl) {
        res.isNumber = true;
        std::string lhsName = bin.lhs ? shortExprName(*bin.lhs) : "lhs";
        std::string rhsName = bin.rhs ? shortExprName(*bin.rhs) : "rhs";
        if (rv.hasConstNum && std::isfinite(rv.constNum)) {
            int64_t sh = static_cast<int64_t>(rv.constNum);
            if (sh >= 31 || sh < 0) {
                reportFinding(BugKind::NaNPoison,
                              bin.span,
                              "[shift-overflow] signed 32-bit shift '" + lhsName + " << " +
                                  std::to_string(sh) + "' overflows into sign bit or wraps mod 32");
            } else if ((sh == 8 || sh == 16 || sh == 24) && !lv.provablyByteRange() && !lv.hasConstNum) {
                reportFinding(BugKind::NaNPoison,
                              bin.span,
                              "[shift-overflow] byte-packing shift '" + lhsName + " << " +
                                  std::to_string(sh) + "' on unclamped channel can overflow into adjacent bits");
            }
        } else if (!rv.provablyShiftSafe30()) {
            reportFinding(BugKind::NaNPoison,
                          bin.span,
                          "[shift-overflow] signed 32-bit shift '" + lhsName + " << " + rhsName +
                              "' shift count can reach 31 (producing negative -2147483648 instead of +2147483648)");
        }
        return res;
    }

    if (bin.op == ast::BinaryOp::BitAnd) {
        res.isNumber = true;
        if (rv.hasConstNum && std::isfinite(rv.constNum) &&
            rv.constNum >= 0.0 && rv.constNum <= 2147483647.0) {
            int64_t mask = static_cast<int64_t>(rv.constNum);
            res.hasIntBounds = true;
            res.minInt = 0;
            res.maxInt = mask;
            res.hasNumBounds = true;
            res.minNum = 0.0;
            res.maxNum = static_cast<double>(mask);
            res.isNonNegative = true;
            if (mask <= 255) res.isByteClamped = true;
        }
        return res;
    }

    if (bin.op == ast::BinaryOp::BitOr) {
        res.isNumber = true;
        if (rv.hasConstNum && std::isfinite(rv.constNum) &&
            (static_cast<int64_t>(rv.constNum) & 0x7fffffff) != 0) {
            res.isNonZero = true;
        }
        return res;
    }

    if (bin.op == ast::BinaryOp::UShr) {
        res.isNumber = true;
        res.isNonNegative = true;
        res.hasNumBounds = true;
        res.minNum = 0.0;
        res.maxNum = 4294967295.0;
        if (rv.hasConstNum && rv.constNum >= 24.0 && rv.constNum <= 31.0) {
            res.isByteClamped = true;
        }
        return res;
    }

    if (bin.op == ast::BinaryOp::Add || bin.op == ast::BinaryOp::Sub ||
        bin.op == ast::BinaryOp::Mul || bin.op == ast::BinaryOp::Div ||
        bin.op == ast::BinaryOp::Mod || bin.op == ast::BinaryOp::Exp) {
        bool isStringAdd = (bin.op == ast::BinaryOp::Add && (lv.isString || rv.isString));
        if (isStringAdd) {
            res.isString = true;
            return res;
        }
        std::string opDesc = std::string("arithmetic '") + ast::binaryOpName(bin.op) + "'";
        if (bin.lhs) checkNumericOperandForNaN(lv, *bin.lhs, bin.span, opDesc);
        if (bin.rhs) checkNumericOperandForNaN(rv, *bin.rhs, bin.span, opDesc);

        res.isNumber = true;
        if (lv.isDefinitelyNaN || rv.isDefinitelyNaN ||
            lv.nullState == NullState::DefinitelyUndefined ||
            rv.nullState == NullState::DefinitelyUndefined) {
            res.isDefinitelyNaN = true;
            res.nanOrigin = !lv.nanOrigin.empty() ? lv.nanOrigin
                            : (!rv.nanOrigin.empty() ? rv.nanOrigin : opDesc);
            return res;
        }

        if (bin.op == ast::BinaryOp::Div) {
            bool isLiteralZeroOverZero =
                lv.hasConstNum && rv.hasConstNum && lv.constNum == 0.0 && rv.constNum == 0.0;
            if (!isLiteralZeroOverZero && bin.rhs) {
                std::string rhsName = shortExprName(*bin.rhs);
                if (rv.hasConstNum && rv.constNum == 0.0) {
                    reportFinding(BugKind::NaNPoison,
                                  bin.span,
                                  "[div-by-zero] division by constant zero produces Infinity or NaN");
                } else if (!rv.provablyNonZero()) {
                    bool isNorm = rv.isVectorNormExpr || isNormOrDetIdentifier(rhsName);
                    std::string tag = isNorm ? "[zero-norm] [div-by-zero]" : "[div-by-zero]";
                    reportFinding(BugKind::NaNPoison,
                                  bin.span,
                                  tag + " unguarded division by potentially zero '" + rhsName +
                                      "' produces NaN or Infinity");
                }
            }
        } else if (bin.op == ast::BinaryOp::Mod && bin.rhs) {
            if (rv.hasConstNum && rv.constNum == 0.0) {
                reportFinding(BugKind::NaNPoison,
                              bin.span,
                              "[div-by-zero] modulo by constant zero produces NaN");
            }
        }

        if (lv.hasConstNum && rv.hasConstNum) {
            double a = lv.constNum;
            double b = rv.constNum;
            double c = 0.0;
            switch (bin.op) {
                case ast::BinaryOp::Add: c = a + b; break;
                case ast::BinaryOp::Sub: c = a - b; break;
                case ast::BinaryOp::Mul: c = a * b; break;
                case ast::BinaryOp::Div: c = a / b; break;
                case ast::BinaryOp::Mod: c = std::fmod(a, b); break;
                case ast::BinaryOp::Exp: c = std::pow(a, b); break;
                default: break;
            }
            if (std::isnan(c)) {
                res.isDefinitelyNaN = true;
                res.nanOrigin = "'" + shortExprName(*bin.lhs) + " " +
                                ast::binaryOpName(bin.op) + " " + shortExprName(*bin.rhs) + "'";
            } else {
                res.hasConstNum = true;
                res.constNum = c;
                if (std::isfinite(c)) {
                    res.hasNumBounds = true;
                    res.minNum = res.maxNum = c;
                    res.isNonZero = (c != 0.0);
                    res.isNonNegative = (c >= 0.0);
                    res.isStrictlyPositive = (c > 0.0);
                    res.isClampedUnit = (c >= -1.0 && c <= 1.0);
                    res.isByteClamped = (c >= 0.0 && c <= 255.0);
                    if (std::floor(c) == c &&
                        c >= -9007199254740992.0 && c <= 9007199254740992.0) {
                        res.hasIntBounds = true;
                        res.minInt = static_cast<int64_t>(c);
                        res.maxInt = res.minInt;
                    }
                }
            }
            return res;
        }

        if (bin.op == ast::BinaryOp::Mul) {
            if (bin.lhs && bin.rhs && isStructuralSquareExpr(*bin.lhs, *bin.rhs)) {
                res.isNonNegative = true;
                res.isVectorNormExpr = true;
            } else if (lv.provablyNonNegative() && rv.provablyNonNegative()) {
                res.isNonNegative = true;
                if (lv.isVectorNormExpr || rv.isVectorNormExpr) res.isVectorNormExpr = true;
            }
            if (lv.provablyNonZero() && rv.provablyNonZero()) res.isNonZero = true;
            if (lv.provablyStrictlyPositive() && rv.provablyStrictlyPositive()) {
                res.isStrictlyPositive = true;
            }
            if (lv.provablyInUnitInterval() && rv.provablyInUnitInterval()) {
                res.isClampedUnit = true;
                res.hasNumBounds = true;
                res.minNum = res.isNonNegative ? 0.0 : -1.0;
                res.maxNum = 1.0;
            }
        } else if (bin.op == ast::BinaryOp::Add) {
            if (lv.provablyNonNegative() && rv.provablyNonNegative()) {
                res.isNonNegative = true;
                if (lv.isVectorNormExpr || rv.isVectorNormExpr) res.isVectorNormExpr = true;
                if (lv.provablyStrictlyPositive() || rv.provablyStrictlyPositive()) {
                    res.isStrictlyPositive = true;
                    res.isNonZero = true;
                }
            }
            if (lv.hasNumBounds && rv.hasNumBounds) {
                res.hasNumBounds = true;
                res.minNum = lv.minNum + rv.minNum;
                res.maxNum = lv.maxNum + rv.maxNum;
                if (res.minNum > 0.0 || res.maxNum < 0.0) res.isNonZero = true;
            }
        } else if (bin.op == ast::BinaryOp::Sub) {
            if (bin.lhs && bin.rhs) {
                std::string lk = exprEnvKey(*bin.lhs);
                std::string rk = exprEnvKey(*bin.rhs);
                if (!lk.empty() && !rk.empty()) {
                    auto neqIt = env.find("__neq__:" + lk + ":" + rk);
                    if (neqIt != env.end() && neqIt->second.isNonZero) {
                        res.isNonZero = true;
                    }
                }
            }
            if (lv.hasNumBounds && rv.hasNumBounds) {
                res.hasNumBounds = true;
                res.minNum = lv.minNum - rv.maxNum;
                res.maxNum = lv.maxNum - rv.minNum;
                if (res.minNum > 0.0 || res.maxNum < 0.0) res.isNonZero = true;
                if (res.minNum >= 0.0) res.isNonNegative = true;
                if (res.minNum > 0.0) res.isStrictlyPositive = true;
            }
            if (!res.provablyNonNegative()) {
                res.isPotentiallyNegativeDiff = true;
            }
            if (lv.isVectorNormExpr || rv.isVectorNormExpr) {
                res.isVectorNormExpr = true;
            }
        }
    }
    return res;
}

}  // namespace bronze::check::detail
