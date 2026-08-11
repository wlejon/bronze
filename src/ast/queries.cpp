#include "ast/queries.h"

#include <algorithm>

namespace bronze::ast {
namespace {

// Every identifier mentioned anywhere below a node, descending into nested
// functions. Used to decide what an enclosing scope must put in an
// environment record.
class IdentVisitor final : public Visitor {
public:
    std::unordered_set<std::string> names;

    void visit(const NumberLit&) override {}
    void visit(const StringLit&) override {}
    void visit(const BoolLit&) override {}
    void visit(const NullLit&) override {}
    void visit(const UndefinedLit&) override {}
    void visit(const ThisExpr&) override {}
    void visit(const Ident& i) override { names.insert(i.name); }

    void visit(const Unary& u) override { u.operand->accept(*this); }
    void visit(const Binary& b) override {
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
        for (const auto& arg : n.args) arg->accept(*this);
    }
    void visit(const ObjectLit& o) override {
        for (const auto& prop : o.props) prop.value->accept(*this);
    }
    void visit(const ArrayLit& a) override {
        for (const auto& elem : a.elements) elem->accept(*this);
    }
    void visit(const FunctionExpr& f) override {
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const BlockStmt& b) override {
        for (const auto& s : b.stmts) s->accept(*this);
    }
    void visit(const VarDecl& v) override {
        names.insert(v.name);
        if (v.init) v.init->accept(*this);
    }
    void visit(const ReturnStmt& r) override {
        if (r.value) r.value->accept(*this);
    }
    void visit(const ExprStmt& e) override { e.expr->accept(*this); }
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
        if (f.init) f.init->accept(*this);
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const BreakStmt&) override {}
    void visit(const ContinueStmt&) override {}
    void visit(const SwitchStmt&) override {}
    void visit(const ForInStmt&) override {}
    void visit(const ForOfStmt&) override {}
    void visit(const TryStmt&) override {}
    void visit(const ThrowStmt&) override {}
    void visit(const FunctionDecl& f) override {
        names.insert(f.name);
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const Module& m) override {
        for (const auto& s : m.body) s->accept(*this);
    }
};

// Walks a scope looking for nested functions; everything mentioned inside
// one is a candidate capture. Does not descend into a nested function
// itself — IdentVisitor already covers it to every depth.
class CaptureVisitor : public Visitor {
public:
    std::unordered_set<std::string> captured;

    void addFunctionBody(const std::vector<StmtPtr>& body) {
        IdentVisitor idents;
        for (const auto& s : body) {
            if (s) s->accept(idents);
        }
        captured.insert(idents.names.begin(), idents.names.end());
    }

    void visit(const NumberLit&) override {}
    void visit(const StringLit&) override {}
    void visit(const BoolLit&) override {}
    void visit(const NullLit&) override {}
    void visit(const UndefinedLit&) override {}
    void visit(const ThisExpr&) override {}
    void visit(const Ident&) override {}

    void visit(const Unary& u) override { u.operand->accept(*this); }
    void visit(const Binary& b) override {
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
        for (const auto& arg : n.args) arg->accept(*this);
    }
    void visit(const ObjectLit& o) override {
        for (const auto& prop : o.props) prop.value->accept(*this);
    }
    void visit(const ArrayLit& a) override {
        for (const auto& elem : a.elements) elem->accept(*this);
    }
    void visit(const FunctionExpr& f) override { addFunctionBody(f.body); }
    void visit(const FunctionDecl& f) override { addFunctionBody(f.body); }

    void visit(const BlockStmt& b) override {
        for (const auto& s : b.stmts) s->accept(*this);
    }
    void visit(const VarDecl& v) override {
        if (v.init) v.init->accept(*this);
    }
    void visit(const ReturnStmt& r) override {
        if (r.value) r.value->accept(*this);
    }
    void visit(const ExprStmt& e) override { e.expr->accept(*this); }
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
        if (f.init) f.init->accept(*this);
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const BreakStmt&) override {}
    void visit(const ContinueStmt&) override {}
    void visit(const SwitchStmt&) override {}
    void visit(const ForInStmt&) override {}
    void visit(const ForOfStmt&) override {}
    void visit(const TryStmt&) override {}
    void visit(const ThrowStmt&) override {}
    void visit(const Module& m) override {
        for (const auto& s : m.body) s->accept(*this);
    }
};

// Finds `this` in a function body, stopping at any nested function: each
// binds its own receiver. Reuses CaptureVisitor's traversal shape, which
// already stops at function boundaries — it just records something else.
class ThisVisitor final : public CaptureVisitor {
public:
    bool found = false;
    void visit(const ThisExpr&) override { found = true; }
    void visit(const FunctionExpr&) override {}
    void visit(const FunctionDecl&) override {}
};

void collectHoistedVars(const std::vector<StmtPtr>& stmts, std::vector<std::string>& out);

void collectHoistedVarsIn(const Stmt& stmt, std::vector<std::string>& out) {
    if (const auto* v = dynamic_cast<const VarDecl*>(&stmt)) {
        if (v->isVar) out.push_back(v->name);
        return;
    }
    if (const auto* b = dynamic_cast<const BlockStmt*>(&stmt)) {
        collectHoistedVars(b->stmts, out);
        return;
    }
    if (const auto* i = dynamic_cast<const IfStmt*>(&stmt)) {
        collectHoistedVars(i->thenBody, out);
        collectHoistedVars(i->elseBody, out);
        return;
    }
    if (const auto* w = dynamic_cast<const WhileStmt*>(&stmt)) {
        collectHoistedVars(w->body, out);
        return;
    }
    if (const auto* d = dynamic_cast<const DoWhileStmt*>(&stmt)) {
        collectHoistedVars(d->body, out);
        return;
    }
    if (const auto* f = dynamic_cast<const ForStmt*>(&stmt)) {
        if (f->init) collectHoistedVarsIn(*f->init, out);
        collectHoistedVars(f->body, out);
        return;
    }
    // A nested function's `var`s belong to that function, not this one.
}

void collectHoistedVars(const std::vector<StmtPtr>& stmts, std::vector<std::string>& out) {
    for (const auto& s : stmts) {
        if (s) collectHoistedVarsIn(*s, out);
    }
}

}  // namespace

std::unordered_set<std::string> getCapturedNames(const std::vector<StmtPtr>& stmts) {
    CaptureVisitor v;
    for (const auto& s : stmts) {
        if (s) s->accept(v);
    }
    return v.captured;
}

std::vector<std::string> getScopeDeclarations(const std::vector<StmtPtr>& stmts) {
    std::vector<std::string> names;
    for (const auto& s : stmts) {
        if (!s) continue;
        if (const auto* v = dynamic_cast<const VarDecl*>(s.get())) {
            if (!v->isVar) names.push_back(v->name);
        } else if (const auto* f = dynamic_cast<const FunctionDecl*>(s.get())) {
            names.push_back(f->name);
        }
    }
    return names;
}

std::vector<std::string> getHoistedVarDeclarations(const std::vector<StmtPtr>& stmts) {
    std::vector<std::string> names;
    collectHoistedVars(stmts, names);
    return names;
}

// The module's top level is lowered from a borrowed-pointer list (the
// function declarations having been split out), so each entry point takes
// that shape too.
std::unordered_set<std::string> getCapturedNames(const std::vector<const Stmt*>& stmts) {
    CaptureVisitor v;
    for (const auto* s : stmts) {
        if (s) s->accept(v);
    }
    return v.captured;
}

std::vector<std::string> getScopeDeclarations(const std::vector<const Stmt*>& stmts) {
    std::vector<std::string> names;
    for (const auto* s : stmts) {
        if (!s) continue;
        if (const auto* v = dynamic_cast<const VarDecl*>(s)) {
            if (!v->isVar) names.push_back(v->name);
        } else if (const auto* f = dynamic_cast<const FunctionDecl*>(s)) {
            names.push_back(f->name);
        }
    }
    return names;
}

bool usesThis(const std::vector<StmtPtr>& stmts) {
    ThisVisitor v;
    for (const auto& s : stmts) {
        if (s) s->accept(v);
    }
    return v.found;
}

bool usesThis(const std::vector<const Stmt*>& stmts) {
    ThisVisitor v;
    for (const auto* s : stmts) {
        if (s) s->accept(v);
    }
    return v.found;
}

std::vector<std::string> getHoistedVarDeclarations(const std::vector<const Stmt*>& stmts) {
    std::vector<std::string> names;
    for (const auto* s : stmts) {
        if (s) collectHoistedVarsIn(*s, names);
    }
    return names;
}

}  // namespace bronze::ast
