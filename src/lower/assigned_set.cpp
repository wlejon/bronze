#include "lower/assigned_set.h"

namespace bronze::lower {
namespace {

class AssignedVisitor final : public ast::Visitor {
public:
    std::unordered_set<std::string> assigned;

    void visit(const ast::NumberLit&) override {}
    void visit(const ast::StringLit&) override {}
    void visit(const ast::TemplateLit& n) override {
        for (const auto& e : n.exprs) e->accept(*this);
    }
    void visit(const ast::BoolLit&) override {}
    void visit(const ast::NullLit&) override {}
    void visit(const ast::UndefinedLit&) override {}
    void visit(const ast::ThisExpr&) override {}
    void visit(const ast::Ident&) override {}

    void visit(const ast::Unary& u) override {
        if (u.op == ast::UnaryOp::PreInc || u.op == ast::UnaryOp::PreDec ||
            u.op == ast::UnaryOp::PostInc || u.op == ast::UnaryOp::PostDec) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(u.operand.get())) {
                assigned.insert(id->name);
            }
        }
        u.operand->accept(*this);
    }

    void visit(const ast::Binary& b) override {
        if (ast::isAssignOp(b.op)) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(b.lhs.get())) {
                assigned.insert(id->name);
            }
        }
        b.lhs->accept(*this);
        b.rhs->accept(*this);
    }

    void visit(const ast::Ternary& t) override {
        t.condition->accept(*this);
        t.thenExpr->accept(*this);
        t.elseExpr->accept(*this);
    }

    void visit(const ast::MemberAccess& m) override { m.object->accept(*this); }
    void visit(const ast::IndexAccess& i) override {
        i.object->accept(*this);
        i.index->accept(*this);
    }

    void visit(const ast::Call& c) override {
        c.callee->accept(*this);
        for (const auto& arg : c.args) arg->accept(*this);
    }

    void visit(const ast::NewExpr& n) override {
        for (const auto& arg : n.args) arg->accept(*this);
    }

    void visit(const ast::ObjectLit& o) override {
        for (const auto& prop : o.props) {
            if (prop.keyExpr) prop.keyExpr->accept(*this);
            prop.value->accept(*this);
        }
    }

    void visit(const ast::ArrayLit& a) override {
        for (const auto& elem : a.elements) elem->accept(*this);
    }

    void visit(const ast::FunctionExpr&) override {
        // Nested function scope assignments do not affect outer local variable SSA join
    }

    void visit(const ast::SuperCall& c) override {
        for (const auto& arg : c.args) arg->accept(*this);
    }
    void visit(const ast::SuperMember&) override {}

    void visit(const ast::ClassDecl&) override {
        // A class body is nothing but methods, and a method's assignments
        // are its own scope's, exactly like a nested function's above. The
        // binding the class introduces is handled where declarations are.
    }

    void visit(const ast::BlockStmt& b) override {
        for (const auto& s : b.stmts) s->accept(*this);
    }

    void visit(const ast::VarDecl& v) override {
        assigned.insert(v.name);
        if (v.init) v.init->accept(*this);
    }

    void visit(const ast::ReturnStmt& r) override {
        if (r.value) r.value->accept(*this);
    }

    void visit(const ast::ExprStmt& e) override {
        e.expr->accept(*this);
    }

    void visit(const ast::IfStmt& i) override {
        i.condition->accept(*this);
        for (const auto& s : i.thenBody) s->accept(*this);
        for (const auto& s : i.elseBody) s->accept(*this);
    }

    void visit(const ast::WhileStmt& w) override {
        w.condition->accept(*this);
        for (const auto& s : w.body) s->accept(*this);
    }

    void visit(const ast::DoWhileStmt& d) override {
        for (const auto& s : d.body) s->accept(*this);
        d.condition->accept(*this);
    }

    void visit(const ast::ForStmt& f) override {
        for (const auto& s : f.init) s->accept(*this);
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        for (const auto& s : f.body) s->accept(*this);
    }

    void visit(const ast::BreakStmt&) override {}
    void visit(const ast::ContinueStmt&) override {}
    void visit(const ast::SwitchStmt&) override {}
    void visit(const ast::ForInStmt&) override {}
    void visit(const ast::ForOfStmt& f) override {
        if (f.iterable) f.iterable->accept(*this);
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const ast::TryStmt&) override {}
    void visit(const ast::ThrowStmt&) override {}
    void visit(const ast::FunctionDecl&) override {}
    void visit(const ast::Module&) override {}
};

}  // namespace

std::unordered_set<std::string> getAssignedVariables(const ast::Node& node) {
    AssignedVisitor v;
    node.accept(v);
    return v.assigned;
}

std::unordered_set<std::string> getAssignedVariables(const std::vector<ast::StmtPtr>& stmts) {
    AssignedVisitor v;
    for (const auto& s : stmts) {
        if (s) s->accept(v);
    }
    return v.assigned;
}

}  // namespace bronze::lower
