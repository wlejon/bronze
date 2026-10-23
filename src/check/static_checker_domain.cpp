#include "check/static_checker_domain.h"

#include <algorithm>
#include <cmath>

namespace bronze::check::detail {

AbstractVal joinVal(const AbstractVal& a, const AbstractVal& b) {
    AbstractVal out;
    if (a.nullState == b.nullState) {
        out.nullState = a.nullState;
        out.nullOrigin = !a.nullOrigin.empty() ? a.nullOrigin : b.nullOrigin;
    } else if (a.nullState == NullState::Unknown || b.nullState == NullState::Unknown) {
        if (a.isNullishOrMaybe()) {
            out.nullState = NullState::MaybeNullish;
            out.nullOrigin = a.nullOrigin;
        } else if (b.isNullishOrMaybe()) {
            out.nullState = NullState::MaybeNullish;
            out.nullOrigin = b.nullOrigin;
        } else {
            out.nullState = NullState::Unknown;
        }
    } else if (a.isNullishOrMaybe() || b.isNullishOrMaybe()) {
        out.nullState = NullState::MaybeNullish;
        out.nullOrigin = !a.nullOrigin.empty() ? a.nullOrigin : b.nullOrigin;
    } else {
        out.nullState = NullState::DefinitelyNonNullish;
    }

    out.isNumber = a.isNumber && b.isNumber;
    out.isString = a.isString && b.isString;
    if (a.isDefinitelyNaN || b.isDefinitelyNaN) {
        out.isDefinitelyNaN = a.isDefinitelyNaN && b.isDefinitelyNaN;
        out.nanOrigin = !a.nanOrigin.empty() ? a.nanOrigin : b.nanOrigin;
    }
    if (a.hasConstNum && b.hasConstNum && a.constNum == b.constNum) {
        out.hasConstNum = true;
        out.constNum = a.constNum;
    }
    if (a.hasIntBounds && b.hasIntBounds) {
        out.hasIntBounds = true;
        out.minInt = std::min(a.minInt, b.minInt);
        out.maxInt = std::max(a.maxInt, b.maxInt);
    }
    if (a.isContainer && b.isContainer) {
        out.isContainer = true;
        out.isTypedArray = a.isTypedArray && b.isTypedArray;
        if (a.hasExactLength && b.hasExactLength && a.exactLength == b.exactLength) {
            out.hasExactLength = true;
            out.exactLength = a.exactLength;
        }
    }
    return out;
}

EnvMap joinEnv(const EnvMap& a, const EnvMap& b) {
    EnvMap out = a;
    for (const auto& [k, vb] : b) {
        auto it = out.find(k);
        if (it == out.end()) {
            out.emplace(k, vb);
        } else {
            it->second = joinVal(it->second, vb);
        }
    }
    return out;
}

bool isTypedArrayCtorName(const std::string& name) {
    return name == "Int8Array" || name == "Uint8Array" || name == "Uint8ClampedArray" ||
           name == "Int16Array" || name == "Uint16Array" || name == "Int32Array" ||
           name == "Uint32Array" || name == "Float32Array" || name == "Float64Array" ||
           name == "BigInt64Array" || name == "BigUint64Array";
}

bool isArrayMutatingMethod(const std::string& prop) {
    return prop == "push" || prop == "pop" || prop == "shift" || prop == "unshift" ||
           prop == "splice" || prop == "fill" || prop == "copyWithin";
}

std::string exprEnvKey(const ast::Expr& expr) {
    if (const auto* id = ast::exactAs<const ast::Ident*>(&expr)) {
        return id->name;
    }
    if (const auto* mem = ast::exactAs<const ast::MemberAccess*>(&expr)) {
        if (!mem->optional && mem->object) {
            std::string base = exprEnvKey(*mem->object);
            if (!base.empty()) {
                return base + "." + mem->property;
            }
            if (ast::exactAs<const ast::ThisExpr*>(mem->object.get()) != nullptr) {
                return "this." + mem->property;
            }
        }
    }
    if (const auto* call = ast::exactAs<const ast::Call*>(&expr)) {
        if (!call->optional && call->args.size() == 1 && call->callee) {
            const auto* mem = ast::exactAs<const ast::MemberAccess*>(call->callee.get());
            const auto* argStr = ast::exactAs<const ast::StringLit*>(call->args[0].get());
            if (mem && argStr && mem->object &&
                (mem->property == "getElementById" || mem->property == "querySelector" ||
                 mem->property == "closest" || mem->property == "getAttribute" ||
                 mem->property == "getItem")) {
                std::string base = exprEnvKey(*mem->object);
                if (!base.empty()) {
                    return base + "." + mem->property + "(\"" + argStr->value + "\")";
                }
            }
        }
    }
    return {};
}

std::string shortExprName(const ast::Expr& expr) {
    std::string key = exprEnvKey(expr);
    if (!key.empty()) return key;
    if (ast::exactAs<const ast::NullLit*>(&expr)) return "null";
    if (ast::exactAs<const ast::UndefinedLit*>(&expr)) return "undefined";
    if (const auto* call = ast::exactAs<const ast::Call*>(&expr)) {
        if (call->callee) {
            std::string calleeName = shortExprName(*call->callee);
            if (!calleeName.empty()) return calleeName + "(...)";
        }
    }
    return "expression";
}

std::string describeNullState(const AbstractVal& v, const std::string& fallbackExpr) {
    if (v.nullState == NullState::DefinitelyNull) {
        return "'" + fallbackExpr + "' is null";
    }
    if (v.nullState == NullState::DefinitelyUndefined) {
        if (!v.nullOrigin.empty()) return v.nullOrigin;
        return "'" + fallbackExpr + "' is undefined";
    }
    if (!v.nullOrigin.empty()) {
        return "'" + fallbackExpr + "' may be null or undefined (from " + v.nullOrigin + ")";
    }
    return "'" + fallbackExpr + "' may be null or undefined";
}

namespace {

void collectClosureAssignedExpr(const ast::Expr* expr,
                                bool inNestedFn,
                                std::unordered_set<std::string>& outNames) {
    if (!expr) return;
    if (const auto* bin = ast::exactAs<const ast::Binary*>(expr)) {
        if (inNestedFn && ast::isAssignOp(bin->op) && bin->lhs) {
            if (const auto* id = ast::exactAs<const ast::Ident*>(bin->lhs.get())) {
                outNames.insert(id->name);
            }
        }
        collectClosureAssignedExpr(bin->lhs.get(), inNestedFn, outNames);
        collectClosureAssignedExpr(bin->rhs.get(), inNestedFn, outNames);
        return;
    }
    if (const auto* un = ast::exactAs<const ast::Unary*>(expr)) {
        if (inNestedFn &&
            (un->op == ast::UnaryOp::PreInc || un->op == ast::UnaryOp::PreDec ||
             un->op == ast::UnaryOp::PostInc || un->op == ast::UnaryOp::PostDec) &&
            un->operand) {
            if (const auto* id = ast::exactAs<const ast::Ident*>(un->operand.get())) {
                outNames.insert(id->name);
            }
        }
        collectClosureAssignedExpr(un->operand.get(), inNestedFn, outNames);
        return;
    }
    if (const auto* fn = ast::exactAs<const ast::FunctionExpr*>(expr)) {
        collectClosureAssignedNames(fn->body, true, outNames);
        return;
    }
    if (const auto* call = ast::exactAs<const ast::Call*>(expr)) {
        collectClosureAssignedExpr(call->callee.get(), inNestedFn, outNames);
        for (const auto& a : call->args) collectClosureAssignedExpr(a.get(), inNestedFn, outNames);
        return;
    }
    if (const auto* mem = ast::exactAs<const ast::MemberAccess*>(expr)) {
        collectClosureAssignedExpr(mem->object.get(), inNestedFn, outNames);
        return;
    }
    if (const auto* idx = ast::exactAs<const ast::IndexAccess*>(expr)) {
        collectClosureAssignedExpr(idx->object.get(), inNestedFn, outNames);
        collectClosureAssignedExpr(idx->index.get(), inNestedFn, outNames);
        return;
    }
    if (const auto* tern = ast::exactAs<const ast::Ternary*>(expr)) {
        collectClosureAssignedExpr(tern->condition.get(), inNestedFn, outNames);
        collectClosureAssignedExpr(tern->thenExpr.get(), inNestedFn, outNames);
        collectClosureAssignedExpr(tern->elseExpr.get(), inNestedFn, outNames);
        return;
    }
    if (const auto* obj = ast::exactAs<const ast::ObjectLit*>(expr)) {
        for (const auto& p : obj->props) {
            collectClosureAssignedExpr(p.keyExpr.get(), inNestedFn, outNames);
            collectClosureAssignedExpr(p.value.get(), inNestedFn, outNames);
        }
        return;
    }
    if (const auto* arr = ast::exactAs<const ast::ArrayLit*>(expr)) {
        for (const auto& el : arr->elements) collectClosureAssignedExpr(el.get(), inNestedFn, outNames);
        return;
    }
}

}  // namespace

void collectClosureAssignedNames(const std::vector<ast::StmtPtr>& stmts,
                                 bool inNestedFn,
                                 std::unordered_set<std::string>& outNames) {
    for (const auto& st : stmts) {
        if (!st) continue;
        if (const auto* vd = ast::exactAs<const ast::VarDecl*>(st.get())) {
            collectClosureAssignedExpr(vd->init.get(), inNestedFn, outNames);
        } else if (const auto* es = ast::exactAs<const ast::ExprStmt*>(st.get())) {
            collectClosureAssignedExpr(es->expr.get(), inNestedFn, outNames);
        } else if (const auto* rs = ast::exactAs<const ast::ReturnStmt*>(st.get())) {
            collectClosureAssignedExpr(rs->value.get(), inNestedFn, outNames);
        } else if (const auto* ifs = ast::exactAs<const ast::IfStmt*>(st.get())) {
            collectClosureAssignedExpr(ifs->condition.get(), inNestedFn, outNames);
            collectClosureAssignedNames(ifs->thenBody, inNestedFn, outNames);
            collectClosureAssignedNames(ifs->elseBody, inNestedFn, outNames);
        } else if (const auto* ws = ast::exactAs<const ast::WhileStmt*>(st.get())) {
            collectClosureAssignedExpr(ws->condition.get(), inNestedFn, outNames);
            collectClosureAssignedNames(ws->body, inNestedFn, outNames);
        } else if (const auto* dws = ast::exactAs<const ast::DoWhileStmt*>(st.get())) {
            collectClosureAssignedNames(dws->body, inNestedFn, outNames);
            collectClosureAssignedExpr(dws->condition.get(), inNestedFn, outNames);
        } else if (const auto* fs = ast::exactAs<const ast::ForStmt*>(st.get())) {
            collectClosureAssignedNames(fs->init, inNestedFn, outNames);
            collectClosureAssignedExpr(fs->condition.get(), inNestedFn, outNames);
            collectClosureAssignedExpr(fs->update.get(), inNestedFn, outNames);
            collectClosureAssignedNames(fs->body, inNestedFn, outNames);
        } else if (const auto* fis = ast::exactAs<const ast::ForInStmt*>(st.get())) {
            collectClosureAssignedExpr(fis->object.get(), inNestedFn, outNames);
            collectClosureAssignedNames(fis->body, inNestedFn, outNames);
        } else if (const auto* fos = ast::exactAs<const ast::ForOfStmt*>(st.get())) {
            collectClosureAssignedExpr(fos->iterable.get(), inNestedFn, outNames);
            collectClosureAssignedNames(fos->body, inNestedFn, outNames);
        } else if (const auto* bs = ast::exactAs<const ast::BlockStmt*>(st.get())) {
            collectClosureAssignedNames(bs->stmts, inNestedFn, outNames);
        } else if (const auto* ts = ast::exactAs<const ast::TryStmt*>(st.get())) {
            collectClosureAssignedNames(ts->body, inNestedFn, outNames);
            collectClosureAssignedNames(ts->catchBody, inNestedFn, outNames);
            collectClosureAssignedNames(ts->finallyBody, inNestedFn, outNames);
        } else if (const auto* sw = ast::exactAs<const ast::SwitchStmt*>(st.get())) {
            collectClosureAssignedExpr(sw->discriminant.get(), inNestedFn, outNames);
            for (const auto& c : sw->cases) collectClosureAssignedNames(c.body, inNestedFn, outNames);
        } else if (const auto* fd = ast::exactAs<const ast::FunctionDecl*>(st.get())) {
            collectClosureAssignedNames(fd->body, true, outNames);
        } else if (const auto* cd = ast::exactAs<const ast::ClassDecl*>(st.get())) {
            for (const auto& m : cd->methods) {
                if (m.fn) collectClosureAssignedNames(m.fn->body, true, outNames);
            }
        }
    }
}

bool stmtTerminates(const ast::Stmt& stmt) {
    if (ast::exactAs<const ast::ReturnStmt*>(&stmt) != nullptr ||
        ast::exactAs<const ast::ThrowStmt*>(&stmt) != nullptr ||
        ast::exactAs<const ast::BreakStmt*>(&stmt) != nullptr ||
        ast::exactAs<const ast::ContinueStmt*>(&stmt) != nullptr) {
        return true;
    }
    if (const auto* blk = ast::exactAs<const ast::BlockStmt*>(&stmt)) {
        return stmtListTerminates(blk->stmts);
    }
    if (const auto* ifs = ast::exactAs<const ast::IfStmt*>(&stmt)) {
        return !ifs->elseBody.empty() &&
               stmtListTerminates(ifs->thenBody) &&
               stmtListTerminates(ifs->elseBody);
    }
    return false;
}

bool stmtListTerminates(const std::vector<ast::StmtPtr>& stmts) {
    for (const auto& s : stmts) {
        if (s && stmtTerminates(*s)) return true;
    }
    return false;
}

EnvMap narrowForCondition(const EnvMap& env, const ast::Expr* cond, bool whenTrue) {
    EnvMap out = env;
    if (!cond) return out;

    if (const auto* un = ast::exactAs<const ast::Unary*>(cond)) {
        if (un->op == ast::UnaryOp::Not) {
            return narrowForCondition(env, un->operand.get(), !whenTrue);
        }
    }

    if (const auto* bin = ast::exactAs<const ast::Binary*>(cond)) {
        if (bin->op == ast::BinaryOp::LogicalAnd) {
            if (whenTrue) {
                EnvMap step1 = narrowForCondition(env, bin->lhs.get(), true);
                return narrowForCondition(step1, bin->rhs.get(), true);
            }
            return out;
        }
        if (bin->op == ast::BinaryOp::LogicalOr) {
            if (!whenTrue) {
                EnvMap step1 = narrowForCondition(env, bin->lhs.get(), false);
                return narrowForCondition(step1, bin->rhs.get(), false);
            }
            return out;
        }

        bool isEq = (bin->op == ast::BinaryOp::Eq || bin->op == ast::BinaryOp::StrictEq);
        bool isNe = (bin->op == ast::BinaryOp::Ne || bin->op == ast::BinaryOp::StrictNe);
        bool isLoose = (bin->op == ast::BinaryOp::Eq || bin->op == ast::BinaryOp::Ne);
        if ((isEq || isNe) && bin->lhs && bin->rhs) {
            const ast::Expr* target = nullptr;
            bool rhsNull = false;
            bool rhsUndef = false;
            auto checkNullishNode = [&](const ast::Expr* e) {
                if (ast::exactAs<const ast::NullLit*>(e)) {
                    rhsNull = true;
                    return true;
                }
                if (ast::exactAs<const ast::UndefinedLit*>(e)) {
                    rhsUndef = true;
                    return true;
                }
                if (const auto* id = ast::exactAs<const ast::Ident*>(e)) {
                    if (id->name == "undefined") {
                        rhsUndef = true;
                        return true;
                    }
                }
                return false;
            };
            if (checkNullishNode(bin->rhs.get())) {
                target = bin->lhs.get();
            } else if (checkNullishNode(bin->lhs.get())) {
                target = bin->rhs.get();
            }
            if (target) {
                std::string key = exprEnvKey(*target);
                if (!key.empty()) {
                    bool meansNullish = (isEq && whenTrue) || (isNe && !whenTrue);
                    auto& slot = out[key];
                    if (!meansNullish) {
                        slot.nullState = NullState::DefinitelyNonNullish;
                        slot.nullOrigin.clear();
                    } else if (isLoose) {
                        slot.nullState = NullState::MaybeNullish;
                    } else if (rhsNull) {
                        slot.nullState = NullState::DefinitelyNull;
                    } else if (rhsUndef) {
                        slot.nullState = NullState::DefinitelyUndefined;
                    }
                    return out;
                }
            }

            auto matchTypeof = [&](const ast::Expr* uSide, const ast::Expr* sSide) {
                const auto* u = ast::exactAs<const ast::Unary*>(uSide);
                const auto* s = ast::exactAs<const ast::StringLit*>(sSide);
                if (u && u->op == ast::UnaryOp::TypeOf && s && u->operand) {
                    std::string k = exprEnvKey(*u->operand);
                    if (!k.empty()) {
                        bool eqBranch = (isEq && whenTrue) || (isNe && !whenTrue);
                        if (s->value == "undefined") {
                            out[k].nullState = eqBranch ? NullState::DefinitelyUndefined
                                                        : NullState::DefinitelyNonNullish;
                        } else if (eqBranch &&
                                   (s->value == "object" || s->value == "function" ||
                                    s->value == "string" || s->value == "number" ||
                                    s->value == "boolean")) {
                            if (out[k].nullState == NullState::DefinitelyUndefined) {
                                out[k].nullState = NullState::DefinitelyNonNullish;
                            }
                        }
                    }
                }
            };
            matchTypeof(bin->lhs.get(), bin->rhs.get());
            matchTypeof(bin->rhs.get(), bin->lhs.get());
        }

        if (bin->lhs && bin->rhs) {
            if (const auto* lhsId = ast::exactAs<const ast::Ident*>(bin->lhs.get())) {
                auto& slot = out[lhsId->name];
                if (const auto* mem = ast::exactAs<const ast::MemberAccess*>(bin->rhs.get())) {
                    if (mem->property == "length" && mem->object) {
                        std::string arrKey = exprEnvKey(*mem->object);
                        if (!arrKey.empty()) {
                            if (whenTrue && bin->op == ast::BinaryOp::Less) {
                                slot.symBoundArray = arrKey;
                                slot.symBoundInclusive = false;
                                auto arrIt = env.find(arrKey);
                                if (arrIt != env.end() && arrIt->second.hasExactLength) {
                                    slot.hasIntBounds = true;
                                    slot.maxInt = arrIt->second.exactLength - 1;
                                }
                            } else if (whenTrue && bin->op == ast::BinaryOp::LessEqual) {
                                slot.symBoundArray = arrKey;
                                slot.symBoundInclusive = true;
                                auto arrIt = env.find(arrKey);
                                if (arrIt != env.end() && arrIt->second.hasExactLength) {
                                    slot.hasIntBounds = true;
                                    slot.maxInt = arrIt->second.exactLength;
                                }
                            } else if (!whenTrue &&
                                       (bin->op == ast::BinaryOp::GreaterEqual ||
                                        bin->op == ast::BinaryOp::Eq ||
                                        bin->op == ast::BinaryOp::StrictEq)) {
                                slot.symBoundArray = arrKey;
                                slot.symBoundInclusive = false;
                                auto arrIt = env.find(arrKey);
                                if (arrIt != env.end() && arrIt->second.hasExactLength) {
                                    slot.hasIntBounds = true;
                                    slot.maxInt = arrIt->second.exactLength - 1;
                                }
                            }
                        }
                    }
                }
                if (const auto* num = ast::exactAs<const ast::NumberLit*>(bin->rhs.get())) {
                    if (std::isfinite(num->value) && std::floor(num->value) == num->value) {
                        int64_t n = static_cast<int64_t>(num->value);
                        if (whenTrue && bin->op == ast::BinaryOp::Less) {
                            slot.hasIntBounds = true;
                            slot.maxInt = n - 1;
                        } else if (whenTrue && bin->op == ast::BinaryOp::LessEqual) {
                            slot.hasIntBounds = true;
                            slot.maxInt = n;
                        }
                    }
                }
            }
            // Narrow `host.firstChild` / `host.lastChild` when `host.children.length > k` (k >= 0)
            if (whenTrue &&
                (bin->op == ast::BinaryOp::Greater || bin->op == ast::BinaryOp::GreaterEqual)) {
                if (const auto* lenMem = ast::exactAs<const ast::MemberAccess*>(bin->lhs.get())) {
                    if (lenMem->property == "length" && lenMem->object) {
                        if (const auto* chMem =
                                ast::exactAs<const ast::MemberAccess*>(lenMem->object.get())) {
                            if ((chMem->property == "children" || chMem->property == "childNodes") &&
                                chMem->object) {
                                std::string hostKey = exprEnvKey(*chMem->object);
                                if (!hostKey.empty()) {
                                    out[hostKey + ".firstChild"].nullState =
                                        NullState::DefinitelyNonNullish;
                                    out[hostKey + ".lastChild"].nullState =
                                        NullState::DefinitelyNonNullish;
                                    out[hostKey + ".firstElementChild"].nullState =
                                        NullState::DefinitelyNonNullish;
                                    out[hostKey + ".lastElementChild"].nullState =
                                        NullState::DefinitelyNonNullish;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    std::string key = exprEnvKey(*cond);
    if (!key.empty()) {
        if (whenTrue) {
            out[key].nullState = NullState::DefinitelyNonNullish;
            out[key].nullOrigin.clear();
        }
    }
    return out;
}

}  // namespace bronze::check::detail
