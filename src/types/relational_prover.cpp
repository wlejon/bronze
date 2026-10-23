#include "types/relational_prover.h"

#include <optional>

#include "ast/queries.h"

namespace bronze::types {
namespace {

bool isUndefinedExpr(const ast::Expr* e, const UserBindingPredicate& isUserBound) {
    if (e == nullptr) return false;
    if (ast::exactAs<const ast::UndefinedLit*>(e) != nullptr) return true;
    if (const auto* id = ast::exactAs<const ast::Ident*>(e)) {
        return id->name == "undefined" && (!isUserBound || !isUserBound("undefined"));
    }
    if (const auto* u = ast::exactAs<const ast::Unary*>(e)) {
        return u->op == ast::UnaryOp::Void;
    }
    return false;
}

bool isNullLitExpr(const ast::Expr* e) {
    return e != nullptr && ast::exactAs<const ast::NullLit*>(e) != nullptr;
}

std::optional<Type> typeofTagToType(std::string_view tag) {
    if (tag == "number") return Type::number();
    if (tag == "string") return Type::string();
    if (tag == "boolean") return Type::boolean();
    if (tag == "undefined") return Type::undefined();
    if (tag == "function") return Type::function();
    return std::nullopt;
}

const ast::Ident* extractTypeofIdent(const ast::Expr* side) {
    const auto* u = ast::exactAs<const ast::Unary*>(side);
    if (u == nullptr || u->op != ast::UnaryOp::TypeOf) return nullptr;
    return ast::exactAs<const ast::Ident*>(u->operand.get());
}

void mergeConsistentRefinement(std::unordered_map<std::string, Type>& dst,
                               const std::string& name, Type t) {
    const auto it = dst.find(name);
    if (it == dst.end()) {
        dst.emplace(name, t);
    } else if (it->second != t) {
        // When two conjoined conditions refine the same variable (`typeof x === "number" && x !== undefined`),
        // keep the more specific concrete primitive type over Dynamic.
        if (it->second.is(TypeKind::Dynamic) && !t.is(TypeKind::Dynamic)) {
            it->second = t;
        }
    }
}

void extractEqualityRefinement(const ast::Binary& b, const TypeLookup& lookupType,
                               const UserBindingPredicate& isUserBound,
                               ConditionRefinements& out) {
    const bool isNegated =
        (b.op == ast::BinaryOp::StrictNe || b.op == ast::BinaryOp::Ne);
    const bool isLoose =
        (b.op == ast::BinaryOp::Eq || b.op == ast::BinaryOp::Ne);

    auto& posMap = isNegated ? out.whenFalse : out.whenTrue;
    auto& negMap = isNegated ? out.whenTrue : out.whenFalse;

    // 1. `typeof x === "number"` / `typeof x !== "number"` / `typeof x === "undefined"`
    auto checkTypeofSide = [&](const ast::Expr* typeofSide, const ast::Expr* litSide) {
        const auto* id = extractTypeofIdent(typeofSide);
        const auto* strLit = ast::exactAs<const ast::StringLit*>(litSide);
        if (id == nullptr || strLit == nullptr) return false;
        if (const auto mapped = typeofTagToType(strLit->value)) {
            posMap[id->name] = *mapped;
            if (strLit->value == "undefined" && lookupType) {
                const Type cur = lookupType(id->name);
                if (cur.isDynamicNumOrUndef()) {
                    negMap[id->name] = Type::number();
                }
            } else if (strLit->value == "number" && lookupType) {
                const Type cur = lookupType(id->name);
                if (cur.isDynamicNumOrUndef()) {
                    negMap[id->name] = Type::undefined();
                }
            }
            return true;
        }
        return false;
    };
    if (checkTypeofSide(b.lhs.get(), b.rhs.get()) ||
        checkTypeofSide(b.rhs.get(), b.lhs.get())) {
        return;
    }

    // 2. `x === undefined`, `x === null`, `x == null`, `x === <primitive literal>`
    auto checkIdentVsExpr = [&](const ast::Ident* id, const ast::Expr* other) {
        if (id == nullptr || other == nullptr) return false;
        const Type cur = lookupType ? lookupType(id->name) : Type::dynamic();

        if (isUndefinedExpr(other, isUserBound)) {
            if (!isLoose) {
                posMap[id->name] = Type::undefined();
            }
            if (cur.isDynamicNumOrUndef()) {
                negMap[id->name] = Type::number();
            }
            return true;
        }
        if (isNullLitExpr(other)) {
            if (!isLoose) {
                posMap[id->name] = Type::null();
            } else if (cur.isDynamicNumOrUndef()) {
                // `x == null` is true iff `x === null || x === undefined`, so on
                // the falsy branch a `Number | undefined` variable is `Number`.
                negMap[id->name] = Type::number();
            }
            return true;
        }
        if (!isLoose) {
            if (ast::exactAs<const ast::NumberLit*>(other) != nullptr) {
                posMap[id->name] = Type::number();
                return true;
            }
            if (ast::exactAs<const ast::StringLit*>(other) != nullptr) {
                posMap[id->name] = Type::string();
                return true;
            }
            if (ast::exactAs<const ast::BoolLit*>(other) != nullptr) {
                posMap[id->name] = Type::boolean();
                return true;
            }
        }
        return false;
    };

    if (checkIdentVsExpr(ast::exactAs<const ast::Ident*>(b.lhs.get()), b.rhs.get())) return;
    checkIdentVsExpr(ast::exactAs<const ast::Ident*>(b.rhs.get()), b.lhs.get());
}

void extractCallRefinement(const ast::Call& call, const UserBindingPredicate& isUserBound,
                           ConditionRefinements& out) {
    if (call.optional || call.args.size() != 1) return;
    const auto* argId = ast::exactAs<const ast::Ident*>(call.args[0].get());
    if (argId == nullptr) return;

    if (const auto* calleeId = ast::exactAs<const ast::Ident*>(call.callee.get())) {
        if (isUserBound && isUserBound(calleeId->name)) return;
        // Global `isFinite(x)` and `isNaN(x)` coerce non-numbers, so they do NOT
        // prove `x` is a Number (`isFinite("0")` is true). Only `Number.isFinite`
        // and `Number.isNaN` prove `typeof x === "number"`.
        return;
    }

    const auto* member = ast::exactAs<const ast::MemberAccess*>(call.callee.get());
    if (member == nullptr || member->optional || member->isPrivate) return;
    const auto* baseId = ast::exactAs<const ast::Ident*>(member->object.get());
    if (baseId == nullptr || (isUserBound && isUserBound(baseId->name))) return;

    if (baseId->name == "Number") {
        if (member->property == "isFinite" || member->property == "isInteger" ||
            member->property == "isSafeInteger" || member->property == "isNaN") {
            out.whenTrue[argId->name] = Type::number();
        }
    } else if (baseId->name == "Array") {
        if (member->property == "isArray") {
            out.whenTrue[argId->name] = Type::array();
        }
    }
}

}  // namespace

ConditionRefinements extractConditionRefinements(const ast::Expr* cond,
                                                 const TypeLookup& lookupType,
                                                 const UserBindingPredicate& isUserBound,
                                                 bool mathPristine) {
    ConditionRefinements out;
    if (cond == nullptr) return out;

    if (const auto* u = ast::exactAs<const ast::Unary*>(cond)) {
        if (u->op == ast::UnaryOp::Not) {
            ConditionRefinements inner =
                extractConditionRefinements(u->operand.get(), lookupType, isUserBound, mathPristine);
            out.whenTrue = std::move(inner.whenFalse);
            out.whenFalse = std::move(inner.whenTrue);
        }
        return out;
    }

    if (const auto* b = ast::exactAs<const ast::Binary*>(cond)) {
        if (b->op == ast::BinaryOp::StrictEq || b->op == ast::BinaryOp::StrictNe ||
            b->op == ast::BinaryOp::Eq || b->op == ast::BinaryOp::Ne) {
            extractEqualityRefinement(*b, lookupType, isUserBound, out);
            return out;
        }
        if (b->op == ast::BinaryOp::LogicalAnd) {
            const ConditionRefinements lhs =
                extractConditionRefinements(b->lhs.get(), lookupType, isUserBound, mathPristine);
            const ConditionRefinements rhs =
                extractConditionRefinements(b->rhs.get(), lookupType, isUserBound, mathPristine);
            out.whenTrue = lhs.whenTrue;
            for (const auto& [name, t] : rhs.whenTrue) {
                mergeConsistentRefinement(out.whenTrue, name, t);
            }
            for (const auto& [name, tL] : lhs.whenFalse) {
                if (const auto it = rhs.whenFalse.find(name); it != rhs.whenFalse.end()) {
                    const Type joined = join(tL, it->second);
                    if (!joined.is(TypeKind::Dynamic)) {
                        out.whenFalse[name] = joined;
                    }
                }
            }
            return out;
        }
        if (b->op == ast::BinaryOp::LogicalOr) {
            const ConditionRefinements lhs =
                extractConditionRefinements(b->lhs.get(), lookupType, isUserBound, mathPristine);
            const ConditionRefinements rhs =
                extractConditionRefinements(b->rhs.get(), lookupType, isUserBound, mathPristine);
            out.whenFalse = lhs.whenFalse;
            for (const auto& [name, t] : rhs.whenFalse) {
                mergeConsistentRefinement(out.whenFalse, name, t);
            }
            for (const auto& [name, tL] : lhs.whenTrue) {
                if (const auto it = rhs.whenTrue.find(name); it != rhs.whenTrue.end()) {
                    const Type joined = join(tL, it->second);
                    if (!joined.is(TypeKind::Dynamic)) {
                        out.whenTrue[name] = joined;
                    }
                }
            }
            return out;
        }
        return out;
    }

    if (const auto* call = ast::exactAs<const ast::Call*>(cond)) {
        extractCallRefinement(*call, isUserBound, out);
        return out;
    }

    return out;
}

std::vector<std::pair<std::string, Type>> applyConditionRefinements(
    Env& env, const std::unordered_map<std::string, Type>& refinements,
    const std::function<bool(const std::string&)>& isCell) {
    std::vector<std::pair<std::string, Type>> saved;
    if (refinements.empty()) return saved;
    saved.reserve(refinements.size());
    for (const auto& [name, refinedType] : refinements) {
        if (isCell && isCell(name)) continue;
        const auto it = env.find(name);
        if (it == env.end()) continue;
        // Only refine a binding whose current type is Dynamic or DynamicNumOrUndef
        // (or matches the refinement); never clobber an Object shape identity with
        // a weaker type unless the refinement is concrete.
        if (it->second.is(TypeKind::Dynamic) || it->second.isDynamicNumOrUndef()) {
            saved.emplace_back(name, it->second);
            it->second = refinedType;
        }
    }
    return saved;
}

bool stmtDefinitelyTerminates(const ast::Stmt& stmt) {
    if (ast::exactAs<const ast::ReturnStmt*>(&stmt) != nullptr ||
        ast::exactAs<const ast::ThrowStmt*>(&stmt) != nullptr ||
        ast::exactAs<const ast::BreakStmt*>(&stmt) != nullptr ||
        ast::exactAs<const ast::ContinueStmt*>(&stmt) != nullptr) {
        return true;
    }
    if (const auto* blk = ast::exactAs<const ast::BlockStmt*>(&stmt)) {
        return stmtListDefinitelyTerminates(blk->stmts);
    }
    if (const auto* i = ast::exactAs<const ast::IfStmt*>(&stmt)) {
        return !i->thenBody.empty() && !i->elseBody.empty() &&
               stmtListDefinitelyTerminates(i->thenBody) &&
               stmtListDefinitelyTerminates(i->elseBody);
    }
    return false;
}

bool stmtListDefinitelyTerminates(const std::vector<ast::StmtPtr>& stmts) {
    for (const auto& s : stmts) {
        if (s && stmtDefinitelyTerminates(*s)) return true;
    }
    return false;
}

Type nullishCoalescingResult(Type lhs, Type rhs) {
    if (lhs.is(TypeKind::Never) || rhs.is(TypeKind::Never)) return Type::never();
    if (lhs.is(TypeKind::Undefined) || lhs.is(TypeKind::Null)) {
        return rhs;
    }
    if (lhs.is(TypeKind::Number) || lhs.is(TypeKind::String) || lhs.is(TypeKind::Bool) ||
        lhs.is(TypeKind::Object) || lhs.is(TypeKind::Array) || lhs.is(TypeKind::TypedArray) ||
        lhs.is(TypeKind::Function)) {
        return lhs;
    }
    if (lhs.isDynamicNumOrUndef() && rhs.is(TypeKind::Number)) {
        return Type::number();
    }
    return join(lhs, rhs);
}

Type logicalOrResult(Type lhs, Type rhs) {
    if (lhs.is(TypeKind::Never) || rhs.is(TypeKind::Never)) return Type::never();
    if (lhs.is(TypeKind::Undefined) || lhs.is(TypeKind::Null)) {
        return rhs;
    }
    if ((lhs.is(TypeKind::Number) || lhs.isDynamicNumOrUndef()) && rhs.is(TypeKind::Number)) {
        return Type::number();
    }
    return join(lhs, rhs);
}

}  // namespace bronze::types
