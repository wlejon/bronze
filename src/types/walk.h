#pragma once

#include "ast/ast.h"

namespace bronze::types {

// The whole-AST traversal, written once.
//
// `ast::Visitor` is pure-virtual in every node kind, so each analysis would
// otherwise restate the same thirty child-walking methods — `src/lower`
// carries three near-identical copies of them. Subclasses here override only
// the nodes they care about and call the base method when they still want the
// children walked.
//
// The default descends into nested function bodies. An analysis that must
// stop at a function boundary (because each function binds its own `this`, or
// its own scope) overrides the two function nodes with an empty body.
class Walker : public ast::Visitor {
public:
    void visit(const ast::NumberLit&) override {}
    void visit(const ast::StringLit&) override {}
    void visit(const ast::BoolLit&) override {}
    void visit(const ast::NullLit&) override {}
    void visit(const ast::UndefinedLit&) override {}
    void visit(const ast::ThisExpr&) override {}
    void visit(const ast::Ident&) override {}
    void visit(const ast::BreakStmt&) override {}
    void visit(const ast::ContinueStmt&) override {}

    // The two statement kinds the parser accepts but models as empty nodes.
    // There is nothing under them to walk; every analysis has to treat them
    // as opaque, which is exactly what an empty visit means here.
    void visit(const ast::TryStmt&) override {}
    void visit(const ast::ThrowStmt&) override {}

    void visit(const ast::ForInStmt& n) override {
        walkPattern(n.pattern.get());
        if (n.object) n.object->accept(*this);
        for (const auto& s : n.body) s->accept(*this);
    }
    void visit(const ast::ForOfStmt& n) override {
        walkPattern(n.pattern.get());
        if (n.iterable) n.iterable->accept(*this);
        for (const auto& s : n.body) s->accept(*this);
    }
    void visit(const ast::LabeledStmt& n) override {
        if (n.body) n.body->accept(*this);
    }

    void visit(const ast::Unary& n) override { n.operand->accept(*this); }
    void visit(const ast::Binary& n) override {
        n.lhs->accept(*this);
        n.rhs->accept(*this);
    }
    void visit(const ast::TemplateLit& n) override {
        for (const auto& e : n.exprs) e->accept(*this);
    }
    void visit(const ast::Ternary& n) override {
        n.condition->accept(*this);
        n.thenExpr->accept(*this);
        n.elseExpr->accept(*this);
    }
    void visit(const ast::MemberAccess& n) override { n.object->accept(*this); }
    void visit(const ast::IndexAccess& n) override {
        n.object->accept(*this);
        n.index->accept(*this);
    }
    void visit(const ast::Call& n) override {
        n.callee->accept(*this);
        for (const auto& a : n.args) a->accept(*this);
    }
    void visit(const ast::NewExpr& n) override {
        for (const auto& a : n.args) a->accept(*this);
    }
    void visit(const ast::ObjectLit& n) override {
        for (const auto& p : n.props) p.value->accept(*this);
    }
    void visit(const ast::ArrayLit& n) override {
        for (const auto& e : n.elements) e->accept(*this);
    }
    void visit(const ast::SuperCall& n) override {
        for (const auto& a : n.args) a->accept(*this);
    }
    void visit(const ast::SuperMember&) override {}
    void visit(const ast::SpreadElement& n) override { n.argument->accept(*this); }
    void visit(const ast::DestructuringAssign& n) override {
        walkPattern(n.pattern.get());
        n.value->accept(*this);
    }
    void visit(const ast::ClassDecl& n) override {
        for (const auto& m : n.methods) m.fn->accept(*this);
    }
    void visit(const ast::FunctionExpr& n) override {
        walkParams(n.params);
        walkList(n.body);
    }
    void visit(const ast::FunctionDecl& n) override {
        walkParams(n.params);
        walkList(n.body);
    }

    void visit(const ast::BlockStmt& n) override { walkList(n.stmts); }
    void visit(const ast::VarDecl& n) override {
        walkPattern(n.pattern.get());
        if (n.init) n.init->accept(*this);
    }
    void visit(const ast::ReturnStmt& n) override {
        if (n.value) n.value->accept(*this);
    }
    void visit(const ast::ExprStmt& n) override { n.expr->accept(*this); }
    void visit(const ast::IfStmt& n) override {
        n.condition->accept(*this);
        walkList(n.thenBody);
        walkList(n.elseBody);
    }
    void visit(const ast::WhileStmt& n) override {
        n.condition->accept(*this);
        walkList(n.body);
    }
    void visit(const ast::DoWhileStmt& n) override {
        walkList(n.body);
        n.condition->accept(*this);
    }
    void visit(const ast::ForStmt& n) override {
        for (const auto& s : n.init) s->accept(*this);
        if (n.condition) n.condition->accept(*this);
        if (n.update) n.update->accept(*this);
        walkList(n.body);
    }
    void visit(const ast::SwitchStmt& n) override {
        if (n.discriminant) n.discriminant->accept(*this);
        for (const auto& c : n.cases) {
            // A `case` expression is an arbitrary expression that runs when
            // the switch does, so it is code this scope contains — leaving it
            // out hid its call sites from the widening pass, which is not a
            // missed optimization but an unsound proof (docs/0017 decision 9).
            if (c.test) c.test->accept(*this);
            for (const auto& s : c.body) s->accept(*this);
        }
    }
    void visit(const ast::Module& n) override { walkList(n.body); }

    // A pattern's key expressions and defaults are ordinary expressions that
    // evaluate where the pattern does; the names it binds are a separate
    // question, and `ast::patternBoundNames` is the one answer to it.
    void walkPattern(const ast::BindingPattern* pattern) {
        if (!pattern) return;
        for (const auto& elem : pattern->elements) {
            if (elem.keyExpr) elem.keyExpr->accept(*this);
            if (elem.defaultValue) elem.defaultValue->accept(*this);
            walkPattern(elem.pattern.get());
        }
    }
    void walkParams(const std::vector<ast::Param>& params) {
        for (const auto& p : params) {
            if (p.defaultValue) p.defaultValue->accept(*this);
            walkPattern(p.pattern.get());
        }
    }

    void walkList(const std::vector<ast::StmtPtr>& stmts) {
        for (const auto& s : stmts) {
            if (s) s->accept(*this);
        }
    }
    void walkList(const std::vector<const ast::Stmt*>& stmts) {
        for (const auto* s : stmts) {
            if (s) s->accept(*this);
        }
    }
};

}  // namespace bronze::types
