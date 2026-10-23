#include "check/static_checker.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <sstream>
#include <unordered_set>

#include "check/static_checker_domain.h"
#include "lex/lexer.h"
#include "lower/lower.h"
#include "modules/modules.h"
#include "parse/parser.h"
#include "types/host_contracts.h"
#include "types/infer.h"

namespace bronze::check {

const char* bugKindTag(BugKind kind) {
    switch (kind) {
        case BugKind::NullDeref: return "null-deref";
        case BugKind::OutOfBounds: return "out-of-bounds";
        case BugKind::NaNPoison: return "nan-poison";
    }
    return "check";
}

namespace detail {

CheckReport StaticAnalyzer::run(const ast::Module& mod) {
    collectClosureAssignedNames(mod.body, false, closureAssigned_);
    EnvMap env;
    checkStmtList(mod.body, env);
    return std::move(report_);
}

void StaticAnalyzer::reportFinding(BugKind kind, Span span, std::string message) {
    uint64_t dedupKey = (static_cast<uint64_t>(span.file) << 48) ^
                        (static_cast<uint64_t>(span.begin) << 16) ^
                        static_cast<uint64_t>(kind) ^
                        (static_cast<uint64_t>(message.empty() ? 0 : message[0]) << 8);
    if (!emittedKeys_.insert(dedupKey).second) return;

    report_.findings.push_back(BugFinding{kind, span, message});
    if (kind == BugKind::NullDeref) {
        ++report_.nullDerefCount;
    } else if (kind == BugKind::OutOfBounds) {
        ++report_.outOfBoundsCount;
    } else if (kind == BugKind::NaNPoison) {
        ++report_.nanPoisonCount;
        if (message.find("[domain-error]") != std::string::npos) ++report_.domainErrorCount;
        if (message.find("[shift-overflow]") != std::string::npos) ++report_.shiftOverflowCount;
        if (message.find("[div-by-zero]") != std::string::npos ||
            message.find("[zero-norm]") != std::string::npos) {
            ++report_.divByZeroCount;
        }
    }
    if (diags_) diags_->warning(span, "[" + std::string(bugKindTag(kind)) + "] " + message);
}

void StaticAnalyzer::checkNotNullishReceiver(const AbstractVal& recvVal,
                                             const ast::Expr& recvExpr,
                                             Span accessSpan,
                                             const std::string& actionDesc) {
    if (!recvVal.isNullishOrMaybe()) return;
    reportFinding(BugKind::NullDeref,
                  accessSpan,
                  "cannot " + actionDesc + " because " +
                      describeNullState(recvVal, shortExprName(recvExpr)));
}

void StaticAnalyzer::checkNumericOperandForNaN(const AbstractVal& val,
                                               const ast::Expr& operandExpr,
                                               Span opSpan,
                                               const std::string& contextDesc) {
    if (val.nullState == NullState::DefinitelyUndefined) {
        std::string name = shortExprName(operandExpr);
        std::string detail = !val.nullOrigin.empty() ? val.nullOrigin : ("'" + name + "' is undefined");
        reportFinding(BugKind::NaNPoison, opSpan, detail + " in " + contextDesc + " produces NaN");
    } else if (val.isDefinitelyNaN) {
        std::string detail = !val.nanOrigin.empty() ? val.nanOrigin
                                                    : ("'" + shortExprName(operandExpr) + "' is NaN");
        reportFinding(BugKind::NaNPoison,
                      opSpan,
                      detail + " flowing into " + contextDesc + " poisons result with NaN");
    }
}

AbstractVal StaticAnalyzer::evalUnary(const ast::Unary& un, EnvMap& env) {
    AbstractVal res;
    if (un.op == ast::UnaryOp::TypeOf) {
        res.nullState = NullState::DefinitelyNonNullish;
        res.isString = true;
        return res;
    }
    AbstractVal opVal = evalExpr(un.operand.get(), env);
    if (un.op == ast::UnaryOp::Void) {
        res.nullState = NullState::DefinitelyUndefined;
        res.nullOrigin = "'void' expression";
        return res;
    }
    if (un.op == ast::UnaryOp::Not) {
        res.nullState = NullState::DefinitelyNonNullish;
        return res;
    }
    if (un.op == ast::UnaryOp::Negate || un.op == ast::UnaryOp::Posate ||
        un.op == ast::UnaryOp::PreInc || un.op == ast::UnaryOp::PreDec ||
        un.op == ast::UnaryOp::PostInc || un.op == ast::UnaryOp::PostDec) {
        if (un.operand) {
            checkNumericOperandForNaN(opVal, *un.operand, un.span,
                                      std::string("unary '") + ast::unaryOpName(un.op) + "'");
        }
        res.nullState = NullState::DefinitelyNonNullish;
        res.isNumber = true;
        if (opVal.isDefinitelyNaN || opVal.nullState == NullState::DefinitelyUndefined) {
            res.isDefinitelyNaN = true;
            res.nanOrigin = !opVal.nanOrigin.empty() ? opVal.nanOrigin : "NaN unary operation";
        } else if (un.op == ast::UnaryOp::Negate) {
            if (opVal.hasConstNum) {
                res.hasConstNum = true;
                res.constNum = -opVal.constNum;
            }
            if (opVal.hasIntBounds) {
                res.hasIntBounds = true;
                res.minInt = -opVal.maxInt;
                res.maxInt = -opVal.minInt;
            }
            if (opVal.hasNumBounds) {
                res.hasNumBounds = true;
                res.minNum = -opVal.maxNum;
                res.maxNum = -opVal.minNum;
            }
            res.isNonZero = opVal.provablyNonZero();
            res.isClampedUnit = opVal.provablyInUnitInterval();
        } else if (un.op == ast::UnaryOp::Posate) {
            res = opVal;
            res.nullState = NullState::DefinitelyNonNullish;
            res.isNumber = true;
        }
        return res;
    }
    res.nullState = NullState::DefinitelyNonNullish;
    return res;
}

void StaticAnalyzer::checkIndexBounds(const AbstractVal& objVal,
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

AbstractVal StaticAnalyzer::evalExpr(const ast::Expr* expr, EnvMap& env, bool isWriteTarget) {
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
            if (std::isfinite(num->value)) {
                res.hasNumBounds = true;
                res.minNum = res.maxNum = num->value;
                res.isNonZero = (num->value != 0.0);
                res.isNonNegative = (num->value >= 0.0);
                res.isStrictlyPositive = (num->value > 0.0);
                res.isClampedUnit = (num->value >= -1.0 && num->value <= 1.0);
                res.isByteClamped = (num->value >= 0.0 && num->value <= 255.0);
                if (std::floor(num->value) == num->value &&
                    num->value >= -9007199254740992.0 && num->value <= 9007199254740992.0) {
                    res.hasIntBounds = true;
                    res.minInt = res.maxInt = static_cast<int64_t>(num->value);
                }
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
            res.isNonZero = res.isNonNegative = res.isStrictlyPositive = true;
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
        return evalUnary(*un, env);
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
            if (baseId->name == "Number" && mem->property == "EPSILON") {
                res.nullState = NullState::DefinitelyNonNullish;
                res.isNumber = true;
                res.hasConstNum = true;
                res.constNum = 2.220446049250313e-16;
                res.hasNumBounds = true;
                res.minNum = res.maxNum = res.constNum;
                res.isNonZero = res.isNonNegative = res.isStrictlyPositive = res.isClampedUnit = true;
                return res;
            }
            if (baseId->name == "Math") {
                if (mem->property == "PI" || mem->property == "LN2" ||
                    mem->property == "LN10" || mem->property == "E" ||
                    mem->property == "SQRT2" || mem->property == "SQRT1_2") {
                    double mc = (mem->property == "PI") ? 3.141592653589793
                                : (mem->property == "LN2") ? 0.6931471805599453
                                : (mem->property == "LN10") ? 2.302585092994046
                                : (mem->property == "E") ? 2.718281828459045
                                : (mem->property == "SQRT2") ? 1.4142135623730951
                                                             : 0.7071067811865476;
                    res.nullState = NullState::DefinitelyNonNullish;
                    res.isNumber = true;
                    res.hasConstNum = true;
                    res.constNum = mc;
                    res.hasNumBounds = true;
                    res.minNum = res.maxNum = mc;
                    res.isNonZero = res.isNonNegative = res.isStrictlyPositive = true;
                    res.isClampedUnit = (mc <= 1.0);
                    return res;
                }
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
            res.hasNumBounds = true;
            res.minNum = res.maxNum = res.constNum;
            res.isNonNegative = true;
            res.isNonZero = (objVal.exactLength > 0);
            res.isStrictlyPositive = (objVal.exactLength > 0);
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

void StaticAnalyzer::checkFunctionBody(const std::vector<ast::Param>& params,
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
    collectClosureAssignedNames(body, false, closureAssigned_);
    checkStmtList(body, fnEnv);
}

void StaticAnalyzer::checkStmtList(const std::vector<ast::StmtPtr>& stmts, EnvMap& env) {
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

void StaticAnalyzer::checkStmt(const ast::Stmt& stmt, EnvMap& env) {
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

}  // namespace detail

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

CheckReport checkModule(const ast::Module& astMod,
                        const types::InferenceResult* inferred,
                        const il::Module* ilMod,
                        DiagnosticSink* diags,
                        bool strictDomNullability) {
    return detail::StaticAnalyzer(inferred, ilMod, diags, strictDomNullability).run(astMod);
}

CheckReport checkSource(std::string_view src,
                        bool strictDomNullability,
                        std::string* errorOut) {
    SourceSet sources;
    const auto& buf = sources.add("check_input.js", std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) {
        if (errorOut) *errorOut = diags.render(buf);
        return {};
    }
    auto mod = Parser(std::move(tokens), diags).parseModule("check_input.js");
    if (diags.hasErrors() || !mod) {
        if (errorOut) *errorOut = diags.render(buf);
        return {};
    }
    DiagnosticSink inferDiags;
    auto inferred = types::inferModule(*mod, inferDiags);
    std::optional<il::Module> ilMod;
    if (inferred) {
        DiagnosticSink lowerDiags;
        ilMod = lower::lowerModule(*mod, lowerDiags, &*inferred, nullptr, &sources);
    }
    return checkModule(*mod,
                       inferred ? &*inferred : nullptr,
                       ilMod ? &*ilMod : nullptr,
                       nullptr,
                       strictDomNullability);
}

int runCheckFile(const std::string& sourcePath,
                 std::string* outString,
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
