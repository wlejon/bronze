#include "check/static_checker.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <sstream>
#include <unordered_set>

#include "check/static_checker_domain.h"
#include "lower/lower.h"
#include "modules/modules.h"
#include "types/host_contracts.h"
#include "types/infer.h"

namespace bronze::check {

using detail::AbstractVal;
using detail::describeNullState;
using detail::EnvMap;
using detail::exprEnvKey;
using detail::isArrayMutatingMethod;
using detail::isExplicitNaNSentinel;
using detail::isTypedArrayCtorName;
using detail::joinEnv;
using detail::joinVal;
using detail::mergeLoopEnv;
using detail::narrowForCondition;
using detail::NullState;
using detail::recordCorrelatedNonNull;
using detail::shortExprName;
using detail::stmtListTerminates;
using detail::stmtTerminates;

const char* bugKindTag(BugKind kind) {
    switch (kind) {
        case BugKind::NullDeref: return "null-deref";
        case BugKind::OutOfBounds: return "out-of-bounds";
        case BugKind::NaNPoison: return "nan-poison";
    }
    return "check";
}

namespace {

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

    CheckReport run(const ast::Module& mod) {
        detail::collectClosureAssignedNames(mod.body, false, closureAssigned_);
        EnvMap env;
        checkStmtList(mod.body, env);
        return std::move(report_);
    }

private:
    const types::InferenceResult* inferred_ = nullptr;
    const il::Module* ilMod_ = nullptr;
    DiagnosticSink* diags_ = nullptr;
    bool strictDomNullability_ = true;
    CheckReport report_;
    std::unordered_set<std::string> closureAssigned_;
    std::unordered_set<uint64_t> emittedKeys_;

    void reportFinding(BugKind kind, Span span, std::string message) {
        uint64_t dedupKey = (static_cast<uint64_t>(span.file) << 48) ^
                            (static_cast<uint64_t>(span.begin) << 16) ^
                            static_cast<uint64_t>(kind);
        if (!emittedKeys_.insert(dedupKey).second) return;

        report_.findings.push_back(BugFinding{kind, span, message});
        if (kind == BugKind::NullDeref) ++report_.nullDerefCount;
        else if (kind == BugKind::OutOfBounds) ++report_.outOfBoundsCount;
        else if (kind == BugKind::NaNPoison) ++report_.nanPoisonCount;
        if (diags_) diags_->warning(span, "[" + std::string(bugKindTag(kind)) + "] " + message);
    }

    void checkNotNullishReceiver(const AbstractVal& recvVal, const ast::Expr& recvExpr,
                                 Span accessSpan, const std::string& actionDesc) {
        if (!recvVal.isNullishOrMaybe()) return;
        reportFinding(BugKind::NullDeref, accessSpan,
                      "cannot " + actionDesc + " because " +
                          describeNullState(recvVal, shortExprName(recvExpr)));
    }

    void checkNumericOperandForNaN(const AbstractVal& val, const ast::Expr& operandExpr,
                                   Span opSpan, const std::string& contextDesc) {
        if (val.nullState == NullState::DefinitelyUndefined) {
            std::string name = shortExprName(operandExpr);
            std::string detail = !val.nullOrigin.empty() ? val.nullOrigin : ("'" + name + "' is undefined");
            reportFinding(BugKind::NaNPoison, opSpan, detail + " in " + contextDesc + " produces NaN");
        } else if (val.isDefinitelyNaN) {
            std::string detail = !val.nanOrigin.empty() ? val.nanOrigin
                                                        : ("'" + shortExprName(operandExpr) + "' is NaN");
            reportFinding(BugKind::NaNPoison, opSpan,
                          detail + " flowing into " + contextDesc + " poisons result with NaN");
        }
    }

    AbstractVal evalExpr(const ast::Expr* expr, EnvMap& env, bool isWriteTarget = false) {
        AbstractVal res;
        if (!expr) return res;

        if (const auto* num = ast::exactAs<const ast::NumberLit*>(expr)) {
            res.nullState = NullState::DefinitelyNonNullish;
            res.isNumber = true;
            if (std::isnan(num->value)) {
                res.isDefinitelyNaN = true;
                res.nanOrigin = "NaN literal";
            } else {
                res.hasConstNum = true;
                res.constNum = num->value;
                if (std::isfinite(num->value) && std::floor(num->value) == num->value &&
                    num->value >= -9007199254740992.0 && num->value <= 9007199254740992.0) {
                    res.hasIntBounds = true;
                    res.minInt = res.maxInt = static_cast<int64_t>(num->value);
                }
            }
            return res;
        }
        if (ast::exactAs<const ast::StringLit*>(expr) || ast::exactAs<const ast::TemplateLit*>(expr)) {
            if (const auto* tpl = ast::exactAs<const ast::TemplateLit*>(expr)) {
                for (const auto& e : tpl->exprs) evalExpr(e.get(), env);
            }
            res.nullState = NullState::DefinitelyNonNullish;
            res.isString = true;
            return res;
        }
        if (ast::exactAs<const ast::BoolLit*>(expr) || ast::exactAs<const ast::BigIntLit*>(expr) ||
            ast::exactAs<const ast::RegExpLit*>(expr) || ast::exactAs<const ast::ThisExpr*>(expr)) {
            res.nullState = NullState::DefinitelyNonNullish;
            return res;
        }
        if (ast::exactAs<const ast::NullLit*>(expr)) {
            res.nullState = NullState::DefinitelyNull;
            res.nullOrigin = "null literal";
            return res;
        }
        if (ast::exactAs<const ast::UndefinedLit*>(expr)) {
            res.nullState = NullState::DefinitelyUndefined;
            res.nullOrigin = "'undefined'";
            return res;
        }
        if (const auto* id = ast::exactAs<const ast::Ident*>(expr)) {
            if (id->name == "undefined") {
                res.nullState = NullState::DefinitelyUndefined;
                res.nullOrigin = "'undefined'";
                return res;
            }
            if (id->name == "NaN") {
                res.nullState = NullState::DefinitelyNonNullish;
                res.isNumber = res.isDefinitelyNaN = true;
                res.nanOrigin = "'NaN'";
                return res;
            }
            if (id->name == "Infinity") {
                res.nullState = NullState::DefinitelyNonNullish;
                res.isNumber = res.hasConstNum = true;
                res.constNum = INFINITY;
                return res;
            }
            auto it = env.find(id->name);
            return it != env.end() ? it->second : res;
        }
        if (const auto* arr = ast::exactAs<const ast::ArrayLit*>(expr)) {
            bool hasSpread = false;
            for (const auto& el : arr->elements) {
                if (const auto* sp = ast::exactAs<const ast::SpreadElement*>(el.get())) {
                    hasSpread = true;
                    evalExpr(sp->argument.get(), env);
                } else {
                    evalExpr(el.get(), env);
                }
            }
            res.nullState = NullState::DefinitelyNonNullish;
            res.isContainer = true;
            if (!hasSpread && !arr->elements.empty()) {
                res.hasExactLength = true;
                res.exactLength = static_cast<int64_t>(arr->elements.size());
            }
            return res;
        }
        if (const auto* obj = ast::exactAs<const ast::ObjectLit*>(expr)) {
            for (const auto& p : obj->props) {
                evalExpr(p.keyExpr.get(), env);
                evalExpr(p.value.get(), env);
            }
            res.nullState = NullState::DefinitelyNonNullish;
            res.isPlainObject = true;
            return res;
        }
        if (const auto* fn = ast::exactAs<const ast::FunctionExpr*>(expr)) {
            checkFunctionBody(fn->params, fn->body, env);
            res.nullState = NullState::DefinitelyNonNullish;
            return res;
        }
        if (const auto* ce = ast::exactAs<const ast::ClassExpr*>(expr)) {
            for (const auto& m : ce->methods) {
                if (m.init) evalExpr(m.init.get(), env);
                if (m.fn) checkFunctionBody(m.fn->params, m.fn->body, env);
            }
            res.nullState = NullState::DefinitelyNonNullish;
            return res;
        }

        if (const auto* nw = ast::exactAs<const ast::NewExpr*>(expr)) {
            std::vector<AbstractVal> argVals;
            argVals.reserve(nw->args.size());
            for (const auto& a : nw->args) {
                argVals.push_back(evalExpr(a.get(), env));
            }
            evalExpr(nw->callee.get(), env);
            res.nullState = NullState::DefinitelyNonNullish;
            if (const auto* cid = ast::exactAs<const ast::Ident*>(nw->callee.get())) {
                if (isTypedArrayCtorName(cid->name)) {
                    res.isContainer = true;
                    res.isTypedArray = true;
                    if (argVals.empty()) {
                        res.hasExactLength = true;
                        res.exactLength = 0;
                    } else if (argVals.size() == 1 && argVals[0].hasConstNum &&
                               std::isfinite(argVals[0].constNum) &&
                               argVals[0].constNum >= 0 &&
                               std::floor(argVals[0].constNum) == argVals[0].constNum) {
                        res.hasExactLength = true;
                        res.exactLength = static_cast<int64_t>(argVals[0].constNum);
                    }
                } else if (cid->name == "Array" && argVals.size() == 1 &&
                           argVals[0].hasConstNum && std::isfinite(argVals[0].constNum) &&
                           argVals[0].constNum >= 0 &&
                           std::floor(argVals[0].constNum) == argVals[0].constNum) {
                    res.isContainer = true;
                    res.isTypedArray = false;
                    res.hasExactLength = true;
                    res.exactLength = static_cast<int64_t>(argVals[0].constNum);
                }
            }
            return res;
        }

        if (const auto* un = ast::exactAs<const ast::Unary*>(expr)) {
            if (un->op == ast::UnaryOp::TypeOf) {
                res.nullState = NullState::DefinitelyNonNullish;
                res.isString = true;
                return res;
            }
            AbstractVal opVal = evalExpr(un->operand.get(), env);
            if (un->op == ast::UnaryOp::Void) {
                res.nullState = NullState::DefinitelyUndefined;
                res.nullOrigin = "'void' expression";
                return res;
            }
            if (un->op == ast::UnaryOp::Not) {
                res.nullState = NullState::DefinitelyNonNullish;
                return res;
            }
            if (un->op == ast::UnaryOp::Negate || un->op == ast::UnaryOp::Posate ||
                un->op == ast::UnaryOp::PreInc || un->op == ast::UnaryOp::PreDec ||
                un->op == ast::UnaryOp::PostInc || un->op == ast::UnaryOp::PostDec) {
                if (un->operand) {
                    checkNumericOperandForNaN(opVal, *un->operand, un->span,
                                              std::string("unary '") + ast::unaryOpName(un->op) + "'");
                }
                res.nullState = NullState::DefinitelyNonNullish;
                res.isNumber = true;
                if (opVal.isDefinitelyNaN || opVal.nullState == NullState::DefinitelyUndefined) {
                    res.isDefinitelyNaN = true;
                    res.nanOrigin = !opVal.nanOrigin.empty() ? opVal.nanOrigin : "NaN unary operation";
                } else if (un->op == ast::UnaryOp::Negate && opVal.hasConstNum) {
                    res.hasConstNum = true;
                    res.constNum = -opVal.constNum;
                    if (opVal.hasIntBounds) {
                        res.hasIntBounds = true;
                        res.minInt = -opVal.maxInt;
                        res.maxInt = -opVal.minInt;
                    }
                } else if (un->op == ast::UnaryOp::Posate && opVal.hasConstNum) {
                    res.hasConstNum = true;
                    res.constNum = opVal.constNum;
                    res.hasIntBounds = opVal.hasIntBounds;
                    res.minInt = opVal.minInt;
                    res.maxInt = opVal.maxInt;
                }
                return res;
            }
            res.nullState = NullState::DefinitelyNonNullish;
            return res;
        }

        if (const auto* mem = ast::exactAs<const ast::MemberAccess*>(expr)) {
            AbstractVal objVal = evalExpr(mem->object.get(), env);
            bool inOptionalChain = mem->optional ||
                                   (mem->object && !mem->object->parenthesized &&
                                    ast::containsOptionalLink(*mem->object));
            if (!inOptionalChain && mem->object) {
                checkNotNullishReceiver(
                    objVal, *mem->object, mem->span, "read property '" + mem->property + "'");
            }

            if (const auto* baseId = ast::exactAs<const ast::Ident*>(mem->object.get())) {
                if (baseId->name == "Number" && mem->property == "NaN") {
                    res.nullState = NullState::DefinitelyNonNullish;
                    res.isNumber = true;
                    res.isDefinitelyNaN = true;
                    res.nanOrigin = "'Number.NaN'";
                    return res;
                }
            }

            std::string key = exprEnvKey(*mem);
            if (!key.empty()) {
                auto it = env.find(key);
                if (it != env.end()) {
                    res = it->second;
                    if (inOptionalChain && objVal.isNullishOrMaybe()) {
                        res.nullState = NullState::MaybeNullish;
                    }
                    return res;
                }
            }

            if (mem->property == "length" && objVal.isContainer && objVal.hasExactLength) {
                res.nullState = NullState::DefinitelyNonNullish;
                res.isNumber = true;
                res.hasConstNum = true;
                res.constNum = static_cast<double>(objVal.exactLength);
                res.hasIntBounds = true;
                res.minInt = objVal.exactLength;
                res.maxInt = objVal.exactLength;
                return res;
            }

            types::HostNullability propNull = types::classifyHostPropertyNullability(
                *mem, [](const std::string&) { return false; });
            if (strictDomNullability_ &&
                (propNull == types::HostNullability::MaybeNullish ||
                 propNull == types::HostNullability::AlwaysNullish)) {
                res.nullState = NullState::MaybeNullish;
                res.nullOrigin = types::describeHostNullabilitySource(*mem);
                return res;
            }
            if (propNull == types::HostNullability::DefinitelyNonNullish) {
                res.nullState = NullState::DefinitelyNonNullish;
                return res;
            }

            if (inOptionalChain && objVal.isNullishOrMaybe()) {
                res.nullState = NullState::MaybeNullish;
                res.nullOrigin = "optional chain on nullable receiver";
            }
            return res;
        }

        if (const auto* idx = ast::exactAs<const ast::IndexAccess*>(expr)) {
            AbstractVal objVal = evalExpr(idx->object.get(), env);
            AbstractVal idxVal = evalExpr(idx->index.get(), env);
            bool inOptionalChain = idx->optional ||
                                   (idx->object && !idx->object->parenthesized &&
                                    ast::containsOptionalLink(*idx->object));
            if (!inOptionalChain && idx->object) {
                checkNotNullishReceiver(objVal, *idx->object, idx->span, "index into element");
            }
            if (idx->object && idx->index) {
                checkIndexBounds(objVal, idxVal, *idx->object, *idx->index, idx->span, isWriteTarget);
            }
            if (inOptionalChain && objVal.isNullishOrMaybe()) {
                res.nullState = NullState::MaybeNullish;
            }
            return res;
        }

        if (const auto* call = ast::exactAs<const ast::Call*>(expr)) {
            return evalCall(*call, env);
        }

        if (const auto* tern = ast::exactAs<const ast::Ternary*>(expr)) {
            evalExpr(tern->condition.get(), env);
            EnvMap thenEnv = narrowForCondition(env, tern->condition.get(), true);
            EnvMap elseEnv = narrowForCondition(env, tern->condition.get(), false);
            AbstractVal tv = evalExpr(tern->thenExpr.get(), thenEnv);
            AbstractVal ev = evalExpr(tern->elseExpr.get(), elseEnv);
            AbstractVal joined = joinVal(tv, ev);
            if (tern->condition && closureAssigned_.count(exprEnvKey(*tern->condition)) > 0 &&
                ev.nullState == NullState::DefinitelyNull) {
                joined.nullState = tv.nullState;
                joined.nullOrigin = tv.nullOrigin;
            }
            return joined;
        }

        if (const auto* bin = ast::exactAs<const ast::Binary*>(expr)) {
            return evalBinary(*bin, env);
        }

        if (const auto* yld = ast::exactAs<const ast::YieldExpr*>(expr)) {
            evalExpr(yld->argument.get(), env);
            return res;
        }

        return res;
    }

    void checkIndexBounds(const AbstractVal& objVal,
                          const AbstractVal& idxVal,
                          const ast::Expr& objExpr,
                          const ast::Expr& idxExpr,
                          Span span,
                          bool isWriteTarget) {
        std::string arrName = shortExprName(objExpr);

        if (const auto* mem = ast::exactAs<const ast::MemberAccess*>(&idxExpr)) {
            if (mem->property == "length" && mem->object) {
                std::string lenObj = exprEnvKey(*mem->object);
                std::string baseObj = exprEnvKey(objExpr);
                if (!lenObj.empty() && lenObj == baseObj && (!isWriteTarget || objVal.isTypedArray)) {
                    reportFinding(BugKind::OutOfBounds, span,
                                  "index '" + arrName + ".length' is out of bounds for '" + arrName +
                                      "' (valid indices are 0 .. " + arrName + ".length - 1)");
                    return;
                }
            }
        }
        if (!idxVal.symBoundArray.empty() && idxVal.symBoundInclusive) {
            std::string baseObj = exprEnvKey(objExpr);
            if (!baseObj.empty() && baseObj == idxVal.symBoundArray) {
                reportFinding(BugKind::OutOfBounds, span,
                              "index '" + shortExprName(idxExpr) + "' reaches '" + arrName +
                                  ".length' (loop bound uses '<=' instead of '<')");
                return;
            }
        }
        if (idxVal.hasIntBounds && idxVal.maxInt < 0) {
            if (!objVal.isPlainObject &&
                (objVal.isContainer || ast::exactAs<const ast::Unary*>(&idxExpr) != nullptr)) {
                reportFinding(BugKind::OutOfBounds, span,
                              "negative index " + std::to_string(idxVal.maxInt) +
                                  " is out of bounds for '" + arrName + "'");
                return;
            }
        }
        if (objVal.isContainer && objVal.hasExactLength && idxVal.hasIntBounds) {
            if (idxVal.minInt < 0) {
                reportFinding(BugKind::OutOfBounds, span,
                              "index " + std::to_string(idxVal.minInt) + " is negative for '" + arrName + "'");
                return;
            }
            bool isLoopRange = idxVal.minInt < idxVal.maxInt;
            if (idxVal.maxInt >= objVal.exactLength && (objVal.isTypedArray || !isWriteTarget || isLoopRange)) {
                reportFinding(BugKind::OutOfBounds, span,
                              "index " + std::to_string(idxVal.maxInt) + " is out of bounds for '" + arrName +
                                  "' of length " + std::to_string(objVal.exactLength));
            }
        }
    }

    AbstractVal evalCall(const ast::Call& call, EnvMap& env) {
        AbstractVal res;
        bool inOptionalChain = call.optional ||
                               (call.callee && !call.callee->parenthesized &&
                                ast::containsOptionalLink(*call.callee));
        std::vector<AbstractVal> argVals;
        argVals.reserve(call.args.size());
        for (const auto& a : call.args) argVals.push_back(evalExpr(a.get(), env));

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
                    res.nullState = NullState::DefinitelyNonNullish;
                    res.isNumber = true;
                    for (size_t i = 0; i < argVals.size(); ++i) {
                        checkNumericOperandForNaN(
                            argVals[i], *call.args[i], call.span, "'Math." + mem->property + "(...)'");
                        if (argVals[i].isDefinitelyNaN ||
                            argVals[i].nullState == NullState::DefinitelyUndefined) {
                            res.isDefinitelyNaN = true;
                            res.nanOrigin = "'Math." + mem->property + "(...)' with NaN/undefined";
                        }
                    }
                    if (!argVals.empty() && argVals[0].hasConstNum) {
                        double c = argVals[0].constNum;
                        if ((mem->property == "sqrt" || mem->property == "log" ||
                             mem->property == "log2" || mem->property == "log10") &&
                            c < 0.0) {
                            res.isDefinitelyNaN = true;
                            res.nanOrigin = "'Math." + mem->property + "(" + std::to_string(c) + ")'";
                            reportFinding(BugKind::NaNPoison,
                                          call.span,
                                          "'Math." + mem->property + "' called with negative argument " +
                                              std::to_string(c) + " produces NaN");
                        } else if ((mem->property == "acos" || mem->property == "asin") &&
                                   (c < -1.0 || c > 1.0)) {
                            res.isDefinitelyNaN = true;
                            res.nanOrigin = "'Math." + mem->property + "(" + std::to_string(c) + ")'";
                            reportFinding(BugKind::NaNPoison,
                                          call.span,
                                          "'Math." + mem->property + "' called with out-of-domain argument " +
                                              std::to_string(c) + " outside [-1, 1] produces NaN");
                        }
                    }
                    return res;
                }
            }
        } else if (call.callee) {
            AbstractVal calleeVal = evalExpr(call.callee.get(), env);
            if (!inOptionalChain) {
                checkNotNullishReceiver(calleeVal, *call.callee, call.span, "invoke function");
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

    AbstractVal evalBinary(const ast::Binary& bin, EnvMap& env) {
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
                    if (std::isfinite(c) && std::floor(c) == c &&
                        c >= -9007199254740992.0 && c <= 9007199254740992.0) {
                        res.hasIntBounds = true;
                        res.minInt = static_cast<int64_t>(c);
                        res.maxInt = res.minInt;
                    }
                }
            }
        }
        return res;
    }

    void checkFunctionBody(const std::vector<ast::Param>& params,
                           const std::vector<ast::StmtPtr>& body,
                           const EnvMap& outerEnv) {
        EnvMap fnEnv;
        for (const auto& [k, v] : outerEnv) {
            if (v.nullState == NullState::DefinitelyNonNullish || v.isContainer) {
                fnEnv[k] = v;
            }
        }
        for (const auto& p : params) {
            if (!p.name.empty()) {
                AbstractVal pv;
                if (p.defaultValue) {
                    AbstractVal dv = evalExpr(p.defaultValue.get(), fnEnv);
                    if (dv.nullState == NullState::DefinitelyNonNullish) {
                        pv.nullState = NullState::DefinitelyNonNullish;
                    }
                }
                fnEnv[p.name] = pv;
            }
        }
        detail::collectClosureAssignedNames(body, false, closureAssigned_);
        checkStmtList(body, fnEnv);
    }

    void checkStmtList(const std::vector<ast::StmtPtr>& stmts, EnvMap& env) {
        for (const auto& st : stmts) {
            if (const auto* fd = ast::exactAs<const ast::FunctionDecl*>(st.get())) {
                if (!fd->name.empty()) {
                    AbstractVal fv;
                    fv.nullState = NullState::DefinitelyNonNullish;
                    env[fd->name] = fv;
                }
            }
        }
        for (const auto& st : stmts) {
            if (!st) continue;
            checkStmt(*st, env);
            if (stmtTerminates(*st)) break;
        }
    }

    void checkStmt(const ast::Stmt& stmt, EnvMap& env) {
        if (const auto* vd = ast::exactAs<const ast::VarDecl*>(&stmt)) {
            if (vd->init) {
                AbstractVal iv = evalExpr(vd->init.get(), env);
                if (!vd->name.empty()) {
                    recordCorrelatedNonNull(env, vd->name, vd->init.get(), iv);
                    if (closureAssigned_.count(vd->name) > 0) {
                        if (iv.nullState == NullState::DefinitelyNull ||
                            iv.nullState == NullState::DefinitelyUndefined) {
                            iv.nullState = NullState::Unknown;
                            iv.nullOrigin.clear();
                        }
                        iv.hasConstNum = false;
                        iv.hasIntBounds = false;
                        iv.hasExactLength = false;
                    }
                    env[vd->name] = iv;
                }
            } else if (!vd->name.empty()) {
                AbstractVal uv;
                if (closureAssigned_.count(vd->name) == 0) {
                    uv.nullState = NullState::DefinitelyUndefined;
                    uv.nullOrigin = "uninitialized variable '" + vd->name + "'";
                }
                env[vd->name] = uv;
            }
            return;
        }
        if (const auto* es = ast::exactAs<const ast::ExprStmt*>(&stmt)) {
            evalExpr(es->expr.get(), env);
            if (const auto* call = ast::exactAs<const ast::Call*>(es->expr.get())) {
                if (!call->args.empty() && call->callee) {
                    std::string cn = shortExprName(*call->callee);
                    if (cn == "assert" || cn == "check" || cn.ends_with(".assert") || cn.ends_with(".check")) {
                        env = narrowForCondition(env, call->args[0].get(), true);
                    }
                }
            }
            return;
        }
        if (const auto* rs = ast::exactAs<const ast::ReturnStmt*>(&stmt)) { evalExpr(rs->value.get(), env); return; }
        if (const auto* ts = ast::exactAs<const ast::ThrowStmt*>(&stmt)) { evalExpr(ts->value.get(), env); return; }
        if (const auto* bs = ast::exactAs<const ast::BlockStmt*>(&stmt)) { checkStmtList(bs->stmts, env); return; }
        if (const auto* ifs = ast::exactAs<const ast::IfStmt*>(&stmt)) {
            evalExpr(ifs->condition.get(), env);
            EnvMap thenEnv = narrowForCondition(env, ifs->condition.get(), true);
            EnvMap elseEnv = narrowForCondition(env, ifs->condition.get(), false);
            checkStmtList(ifs->thenBody, thenEnv);
            if (!ifs->elseBody.empty()) checkStmtList(ifs->elseBody, elseEnv);
            bool thenTerm = stmtListTerminates(ifs->thenBody);
            bool elseTerm = !ifs->elseBody.empty() && stmtListTerminates(ifs->elseBody);
            if (thenTerm && !elseTerm) env = std::move(elseEnv);
            else if (elseTerm && !thenTerm) env = std::move(thenEnv);
            else if (!thenTerm && !elseTerm) env = joinEnv(thenEnv, elseEnv);
            return;
        }
        if (const auto* fs = ast::exactAs<const ast::ForStmt*>(&stmt)) {
            EnvMap loopEnv = env;
            checkStmtList(fs->init, loopEnv);
            evalExpr(fs->condition.get(), loopEnv);
            EnvMap bodyEnv = narrowForCondition(loopEnv, fs->condition.get(), true);
            for (auto& [k, v] : bodyEnv) {
                auto initIt = loopEnv.find(k);
                if (initIt != loopEnv.end() && initIt->second.hasIntBounds && v.hasIntBounds) {
                    v.minInt = std::min(initIt->second.minInt, v.maxInt);
                    if (v.minInt < v.maxInt) v.hasConstNum = false;
                }
            }
            checkStmtList(fs->body, bodyEnv);
            evalExpr(fs->update.get(), bodyEnv);
            mergeLoopEnv(env, bodyEnv, fs->body);
            return;
        }
        if (const auto* ws = ast::exactAs<const ast::WhileStmt*>(&stmt)) {
            evalExpr(ws->condition.get(), env);
            EnvMap bodyEnv = narrowForCondition(env, ws->condition.get(), true);
            checkStmtList(ws->body, bodyEnv);
            mergeLoopEnv(env, bodyEnv, ws->body);
            return;
        }
        if (const auto* dws = ast::exactAs<const ast::DoWhileStmt*>(&stmt)) {
            EnvMap bodyEnv = env;
            checkStmtList(dws->body, bodyEnv);
            evalExpr(dws->condition.get(), bodyEnv);
            mergeLoopEnv(env, bodyEnv, dws->body);
            return;
        }
        if (const auto* fis = ast::exactAs<const ast::ForInStmt*>(&stmt)) {
            evalExpr(fis->object.get(), env);
            EnvMap bodyEnv = env;
            if (!fis->name.empty()) {
                bodyEnv[fis->name].nullState = NullState::DefinitelyNonNullish;
                bodyEnv[fis->name].isString = true;
            }
            checkStmtList(fis->body, bodyEnv);
            mergeLoopEnv(env, bodyEnv, fis->body);
            return;
        }
        if (const auto* fos = ast::exactAs<const ast::ForOfStmt*>(&stmt)) {
            AbstractVal iterVal = evalExpr(fos->iterable.get(), env);
            if (fos->iterable) checkNotNullishReceiver(iterVal, *fos->iterable, fos->span, "iterate over value");
            EnvMap bodyEnv = env;
            if (!fos->name.empty()) bodyEnv[fos->name] = AbstractVal{};
            checkStmtList(fos->body, bodyEnv);
            mergeLoopEnv(env, bodyEnv, fos->body);
            return;
        }
        if (const auto* ts = ast::exactAs<const ast::TryStmt*>(&stmt)) {
            EnvMap tryEnv = env, catchEnv = env;
            checkStmtList(ts->body, tryEnv);
            if (ts->hasCatch) {
                if (ts->hasCatchParam && !ts->catchName.empty()) {
                    catchEnv[ts->catchName].nullState = NullState::DefinitelyNonNullish;
                }
                checkStmtList(ts->catchBody, catchEnv);
            }
            bool tryTerm = stmtListTerminates(ts->body);
            bool catchTerm = !ts->hasCatch || stmtListTerminates(ts->catchBody);
            if (!tryTerm && catchTerm) env = std::move(tryEnv);
            else if (tryTerm && !catchTerm) env = std::move(catchEnv);
            else env = joinEnv(tryEnv, catchEnv);
            if (ts->hasFinally) checkStmtList(ts->finallyBody, env);
            return;
        }
        if (const auto* sw = ast::exactAs<const ast::SwitchStmt*>(&stmt)) {
            evalExpr(sw->discriminant.get(), env);
            EnvMap merged = env;
            for (const auto& c : sw->cases) {
                evalExpr(c.test.get(), env);
                EnvMap caseEnv = env;
                checkStmtList(c.body, caseEnv);
                if (!stmtListTerminates(c.body)) merged = joinEnv(merged, caseEnv);
            }
            env = std::move(merged);
            return;
        }
        if (const auto* ls = ast::exactAs<const ast::LabeledStmt*>(&stmt)) {
            if (ls->body) checkStmt(*ls->body, env);
            return;
        }
        if (const auto* fd = ast::exactAs<const ast::FunctionDecl*>(&stmt)) {
            if (!fd->name.empty()) env[fd->name].nullState = NullState::DefinitelyNonNullish;
            checkFunctionBody(fd->params, fd->body, env);
            return;
        }
        if (const auto* cd = ast::exactAs<const ast::ClassDecl*>(&stmt)) {
            if (!cd->name.empty()) env[cd->name].nullState = NullState::DefinitelyNonNullish;
            for (const auto& m : cd->methods) {
                if (m.init) evalExpr(m.init.get(), env);
                if (m.fn) checkFunctionBody(m.fn->params, m.fn->body, env);
            }
            return;
        }
    }
};

}  // namespace

std::string CheckReport::render(const SourceBuffer& buffer) const {
    std::ostringstream out;
    for (const auto& f : findings) {
        auto lc = buffer.lineCol(f.span.begin);
        out << buffer.name() << ":" << lc.line << ":" << lc.column << ": warning: ["
            << bugKindTag(f.kind) << "] " << f.message << "\n";
    }
    return out.str();
}

std::string CheckReport::render(const SourceSet& sources) const {
    std::ostringstream out;
    for (const auto& f : findings) {
        const SourceBuffer& buf = sources.at(f.span.file);
        auto lc = buf.lineCol(f.span.begin);
        out << buf.name() << ":" << lc.line << ":" << lc.column << ": warning: ["
            << bugKindTag(f.kind) << "] " << f.message << "\n";
    }
    return out.str();
}

CheckReport checkModule(const ast::Module& astMod, const types::InferenceResult* inferred,
                        const il::Module* ilMod, DiagnosticSink* diags, bool strictDomNullability) {
    return StaticAnalyzer(inferred, ilMod, diags, strictDomNullability).run(astMod);
}

int runCheckFile(const std::string& sourcePath, std::string* outString,
                 const std::vector<modules::ModuleRoot>& moduleRoots,
                 const std::string& importMapPath) {
    SourceSet sources;
    DiagnosticSink parseDiags;
    auto astMod = modules::loadProgram(
        sourcePath, sources, parseDiags, {moduleRoots, importMapPath});
    if (!astMod || parseDiags.hasErrors()) {
        std::string rendered = parseDiags.render(sources);
        if (outString) *outString = rendered;
        else std::cerr << rendered;
        return 1;
    }

    DiagnosticSink inferDiags;
    auto inferred = types::inferModule(*astMod, inferDiags);
    std::optional<il::Module> ilMod;
    if (inferred) {
        DiagnosticSink lowerDiags;
        ilMod = lower::lowerModule(*astMod, lowerDiags, &*inferred, nullptr, &sources);
    }

    CheckReport report = checkModule(
        *astMod, inferred ? &*inferred : nullptr, ilMod ? &*ilMod : nullptr, nullptr, false);
    std::ostringstream out;
    out << report.render(sources);
    out << "bronze check: " << report.findings.size() << " finding(s) ("
        << report.nullDerefCount << " null-deref, "
        << report.outOfBoundsCount << " out-of-bounds, "
        << report.nanPoisonCount << " nan-poison)\n";

    if (outString) *outString = out.str();
    else std::cout << out.str();
    return report.empty() ? 0 : 1;
}

}  // namespace bronze::check
