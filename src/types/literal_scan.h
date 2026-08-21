#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "types/walk.h"

namespace bronze::types {

// Collects lexical bindings that are only ever assigned string literals.
// Used by both MethodTable (to resolve computed calls) and FieldAudit (to resolve
// computed writes/deletes targeting a finite set of property names).
class LiteralNameScan final : public Walker {
public:
    using Walker::visit;

    std::map<std::string, std::set<std::string>> literals;
    std::set<std::string> unknown;

    void visit(const ast::VarDecl& n) override {
        if (!n.name.empty()) note(n.name, n.init.get());
        if (n.pattern) {
            for (const auto& name : ast::patternBoundNames(*n.pattern)) unknown.insert(name);
        }
        Walker::visit(n);
    }

    void visit(const ast::Binary& n) override {
        if (ast::isAssignOp(n.op)) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(n.lhs.get())) {
                note(id->name, n.op == ast::BinaryOp::Assign ? n.rhs.get() : nullptr);
            }
        }
        Walker::visit(n);
    }

    void visit(const ast::DestructuringAssign& n) override {
        for (const auto& name : ast::patternBoundNames(*n.pattern)) unknown.insert(name);
        Walker::visit(n);
    }

    void visit(const ast::FunctionExpr& n) override {
        noteParams(n.params);
        Walker::visit(n);
    }

    void visit(const ast::FunctionDecl& n) override {
        noteParams(n.params);
        Walker::visit(n);
    }

    void visit(const ast::ForInStmt& n) override {
        if (!n.name.empty()) unknown.insert(n.name);
        Walker::visit(n);
    }

    void visit(const ast::ForOfStmt& n) override {
        if (!n.name.empty()) unknown.insert(n.name);
        Walker::visit(n);
    }

private:
    void noteParams(const std::vector<ast::Param>& params) {
        for (const auto& p : params) {
            if (!p.name.empty()) unknown.insert(p.name);
            if (p.pattern) {
                for (const auto& b : ast::patternBoundNames(*p.pattern)) unknown.insert(b);
            }
        }
    }

    void note(const std::string& name, const ast::Expr* init) {
        if (init == nullptr) {
            if (name.empty()) return;
            return;
        }
        if (const auto* s = dynamic_cast<const ast::StringLit*>(init)) {
            literals[name].insert(s->value);
            return;
        }
        if (const auto* t = dynamic_cast<const ast::Ternary*>(init)) {
            const auto* thenStr = dynamic_cast<const ast::StringLit*>(t->thenExpr.get());
            const auto* elseStr = dynamic_cast<const ast::StringLit*>(t->elseExpr.get());
            if (thenStr != nullptr && elseStr != nullptr) {
                literals[name].insert(thenStr->value);
                literals[name].insert(elseStr->value);
                return;
            }
        }
        unknown.insert(name);
    }
};

inline bool possibleNames(const ast::Expr& index, const LiteralNameScan& env,
                          std::set<std::string>& out) {
    if (const auto* s = dynamic_cast<const ast::StringLit*>(&index)) {
        out.insert(s->value);
        return true;
    }
    if (const auto* t = dynamic_cast<const ast::Ternary*>(&index)) {
        return possibleNames(*t->thenExpr, env, out) && possibleNames(*t->elseExpr, env, out);
    }
    if (const auto* id = dynamic_cast<const ast::Ident*>(&index)) {
        if (env.unknown.count(id->name) != 0) return false;
        const auto it = env.literals.find(id->name);
        if (it == env.literals.end()) return false;
        out.insert(it->second.begin(), it->second.end());
        return true;
    }
    return false;
}

}  // namespace bronze::types
