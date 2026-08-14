#include "ast/assigned.h"

namespace bronze::ast {
namespace {

// Both queries below are the same walk. `onlyInsideTry` is what separates them:
// the sizing question ("what does this loop write?") wants every name, and the
// memory question ("what cannot live in SSA?") wants only the ones a handler
// could be entered in the middle of. One traversal, because two copies of
// "which expression forms write a name" would eventually disagree, and a
// disagreement is a missing join parameter.
class AssignedVisitor final : public Visitor {
public:
    explicit AssignedVisitor(bool onlyInsideTry = false) : onlyInsideTry_(onlyInsideTry) {}

    std::unordered_set<std::string> assigned;

    void record(const std::string& name) {
        if (!onlyInsideTry_ || tryDepth_ > 0) assigned.insert(name);
    }

    void visit(const NumberLit&) override {}
    void visit(const StringLit&) override {}
    void visit(const RegExpLit&) override {}
    void visit(const TemplateLit& n) override {
        for (const auto& e : n.exprs) e->accept(*this);
    }
    void visit(const BoolLit&) override {}
    void visit(const NullLit&) override {}
    void visit(const UndefinedLit&) override {}
    void visit(const ThisExpr&) override {}
    void visit(const Ident&) override {}

    void visit(const Unary& u) override {
        if (u.op == UnaryOp::PreInc || u.op == UnaryOp::PreDec ||
            u.op == UnaryOp::PostInc || u.op == UnaryOp::PostDec) {
            if (const auto* id = dynamic_cast<const Ident*>(u.operand.get())) {
                record(id->name);
            }
        }
        u.operand->accept(*this);
    }

    void visit(const Binary& b) override {
        if (isAssignOp(b.op)) {
            if (const auto* id = dynamic_cast<const Ident*>(b.lhs.get())) {
                record(id->name);
            }
        }
        b.lhs->accept(*this);
        b.rhs->accept(*this);
    }

    void visit(const Ternary& t) override {
        t.condition->accept(*this);
        t.thenExpr->accept(*this);
        t.elseExpr->accept(*this);
    }

    void visit(const MemberAccess& m) override { m.object->accept(*this); }
    void visit(const IndexAccess& i) override {
        i.object->accept(*this);
        i.index->accept(*this);
    }

    void visit(const Call& c) override {
        c.callee->accept(*this);
        for (const auto& arg : c.args) arg->accept(*this);
    }

    void visit(const NewExpr& n) override {
        n.callee->accept(*this);
        for (const auto& arg : n.args) arg->accept(*this);
    }

    void visit(const NewTargetExpr&) override {}

    void visit(const TaggedTemplate& t) override {
        t.tag->accept(*this);
        for (const auto& e : t.templateLit->exprs) e->accept(*this);
    }

    void visit(const ObjectLit& o) override {
        for (const auto& prop : o.props) {
            if (prop.keyExpr) prop.keyExpr->accept(*this);
            prop.value->accept(*this);
        }
    }

    void visit(const ArrayLit& a) override {
        for (const auto& elem : a.elements) {
            if (elem) elem->accept(*this);
        }
    }

    void visit(const FunctionExpr&) override {
        // Nested function scope assignments do not affect outer local variable SSA join
    }

    void visit(const SuperCall& c) override {
        for (const auto& arg : c.args) arg->accept(*this);
    }
    void visit(const SuperMember&) override {}
    void visit(const SpreadElement& s) override { s.argument->accept(*this); }
    // The operand is code that runs in this scope; the value the node produces
    // comes from outside it and writes nothing.
    void visit(const YieldExpr& y) override { y.argument->accept(*this); }
    // Every name a destructuring assignment's pattern binds is written by it,
    // which is exactly what sizes the SSA joins around it. A pattern that
    // contributed nothing here would leave those variables out of a loop's
    // block parameters and silently freeze them at their pre-loop values.
    void visit(const DestructuringAssign& d) override {
        for (const auto& name : patternBoundNames(*d.pattern)) record(name);
        visitPatternExprs(*d.pattern);
        d.value->accept(*this);
    }

    void visit(const ClassDecl&) override {
        // A class body is nothing but methods, and a method's assignments
        // are its own scope's, exactly like a nested function's above. The
        // binding the class introduces is handled where declarations are.
    }

    void visit(const ClassExpr&) override {}

    void visit(const BlockStmt& b) override {
        for (const auto& s : b.stmts) s->accept(*this);
    }

    void visit(const VarDecl& v) override {
        if (v.pattern) {
            for (const auto& name : patternBoundNames(*v.pattern)) record(name);
            visitPatternExprs(*v.pattern);
        } else {
            record(v.name);
        }
        if (v.init) v.init->accept(*this);
    }

    void visit(const ReturnStmt& r) override {
        if (r.value) r.value->accept(*this);
    }

    void visit(const ExprStmt& e) override {
        e.expr->accept(*this);
    }

    void visit(const IfStmt& i) override {
        i.condition->accept(*this);
        for (const auto& s : i.thenBody) s->accept(*this);
        for (const auto& s : i.elseBody) s->accept(*this);
    }

    void visit(const WhileStmt& w) override {
        w.condition->accept(*this);
        for (const auto& s : w.body) s->accept(*this);
    }

    void visit(const DoWhileStmt& d) override {
        for (const auto& s : d.body) s->accept(*this);
        d.condition->accept(*this);
    }

    void visit(const ForStmt& f) override {
        for (const auto& s : f.init) s->accept(*this);
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        for (const auto& s : f.body) s->accept(*this);
    }

    void visit(const BreakStmt&) override {}
    void visit(const ContinueStmt&) override {}
    // A switch writes from three places — the discriminant, the case
    // expressions and the case bodies — and every one of them runs where the
    // switch is written. A variable left out here would be missing from the
    // block parameters of every case body block and silently frozen at its
    // pre-switch value on the fall-through edges.
    void visit(const SwitchStmt& s) override {
        if (s.discriminant) s.discriminant->accept(*this);
        for (const auto& c : s.cases) {
            if (c.test) c.test->accept(*this);
            for (const auto& stmt : c.body) stmt->accept(*this);
        }
    }
    void visit(const LabeledStmt& l) override {
        if (l.body) l.body->accept(*this);
    }
    void visit(const ForInStmt& f) override {
        if (!f.isConst && !f.isLet && !f.isVar) {
            if (!f.name.empty()) record(f.name);
            if (f.pattern) {
                for (const auto& name : patternBoundNames(*f.pattern)) record(name);
            }
        }
        if (f.pattern) visitPatternExprs(*f.pattern);
        if (f.object) f.object->accept(*this);
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const ForOfStmt& f) override {
        if (!f.isConst && !f.isLet && !f.isVar) {
            if (!f.name.empty()) record(f.name);
            if (f.pattern) {
                for (const auto& name : patternBoundNames(*f.pattern)) record(name);
            }
        }
        if (f.pattern) visitPatternExprs(*f.pattern);
        if (f.iterable) f.iterable->accept(*this);
        for (const auto& s : f.body) s->accept(*this);
    }
    // All three parts run where the statement is written, and the catch
    // parameter is a binding this statement introduces — so every name it
    // binds is written here, exactly as a for-of head's names are.
    void visit(const TryStmt& t) override {
        ++tryDepth_;
        for (const auto& s : t.body) s->accept(*this);
        if (t.hasCatchParam) {
            if (t.catchPattern) {
                for (const auto& name : patternBoundNames(*t.catchPattern)) record(name);
                visitPatternExprs(*t.catchPattern);
            } else {
                record(t.catchName);
            }
        }
        for (const auto& s : t.catchBody) s->accept(*this);
        for (const auto& s : t.finallyBody) s->accept(*this);
        --tryDepth_;
    }
    void visit(const ThrowStmt& t) override {
        if (t.value) t.value->accept(*this);
    }
    void visit(const FunctionDecl&) override {}
    void visit(const Module&) override {}

private:
    const bool onlyInsideTry_;
    int tryDepth_ = 0;

    // A pattern's own expressions — computed keys and defaults — can assign
    // too, and they run in this scope.
    void visitPatternExprs(const BindingPattern& pattern) {
        for (const auto& elem : pattern.elements) {
            if (elem.keyExpr) elem.keyExpr->accept(*this);
            if (elem.target) elem.target->accept(*this);
            if (elem.defaultValue) elem.defaultValue->accept(*this);
            if (elem.pattern) visitPatternExprs(*elem.pattern);
        }
    }
};

}  // namespace

std::unordered_set<std::string> getAssignedNames(const Node& node) {
    AssignedVisitor v;
    node.accept(v);
    return v.assigned;
}

std::unordered_set<std::string> getAssignedNames(const std::vector<StmtPtr>& stmts) {
    AssignedVisitor v;
    for (const auto& s : stmts) {
        if (s) s->accept(v);
    }
    return v.assigned;
}

std::unordered_set<std::string> getTryAssignedNames(const std::vector<StmtPtr>& stmts) {
    AssignedVisitor v{/*onlyInsideTry=*/true};
    for (const auto& s : stmts) {
        if (s) s->accept(v);
    }
    return v.assigned;
}

std::unordered_set<std::string> getTryAssignedNames(const std::vector<const Stmt*>& stmts) {
    AssignedVisitor v{/*onlyInsideTry=*/true};
    for (const auto* s : stmts) {
        if (s) s->accept(v);
    }
    return v.assigned;
}

}  // namespace bronze::ast
