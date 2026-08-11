#include "lower/captures.h"

#include <algorithm>

namespace bronze::lower {
namespace {

// Every identifier mentioned anywhere below a node, descending into nested
// functions. Used to decide what an enclosing scope must put in an
// environment record.
class IdentVisitor final : public ast::Visitor {
public:
    std::unordered_set<std::string> names;

    void visit(const ast::NumberLit&) override {}
    void visit(const ast::StringLit&) override {}
    void visit(const ast::BoolLit&) override {}
    void visit(const ast::NullLit&) override {}
    void visit(const ast::UndefinedLit&) override {}
    void visit(const ast::ThisExpr&) override {}
    void visit(const ast::Ident& i) override { names.insert(i.name); }

    void visit(const ast::Unary& u) override { u.operand->accept(*this); }
    void visit(const ast::Binary& b) override {
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
        for (const auto& prop : o.props) prop.value->accept(*this);
    }
    void visit(const ast::ArrayLit& a) override {
        for (const auto& elem : a.elements) elem->accept(*this);
    }
    void visit(const ast::FunctionExpr& f) override {
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const ast::BlockStmt& b) override {
        for (const auto& s : b.stmts) s->accept(*this);
    }
    void visit(const ast::VarDecl& v) override {
        names.insert(v.name);
        if (v.init) v.init->accept(*this);
    }
    void visit(const ast::ReturnStmt& r) override {
        if (r.value) r.value->accept(*this);
    }
    void visit(const ast::ExprStmt& e) override { e.expr->accept(*this); }
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
        if (f.init) f.init->accept(*this);
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const ast::BreakStmt&) override {}
    void visit(const ast::ContinueStmt&) override {}
    void visit(const ast::SwitchStmt&) override {}
    void visit(const ast::ForInStmt&) override {}
    void visit(const ast::ForOfStmt&) override {}
    void visit(const ast::TryStmt&) override {}
    void visit(const ast::ThrowStmt&) override {}
    void visit(const ast::FunctionDecl& f) override {
        names.insert(f.name);
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const ast::Module& m) override {
        for (const auto& s : m.body) s->accept(*this);
    }
};

// Walks a scope looking for nested functions; everything mentioned inside
// one is a candidate capture. Does not descend into a nested function
// itself — IdentVisitor already covers it to every depth.
class CaptureVisitor : public ast::Visitor {
public:
    std::unordered_set<std::string> captured;

    void addFunctionBody(const std::vector<ast::StmtPtr>& body) {
        IdentVisitor idents;
        for (const auto& s : body) {
            if (s) s->accept(idents);
        }
        captured.insert(idents.names.begin(), idents.names.end());
    }

    void visit(const ast::NumberLit&) override {}
    void visit(const ast::StringLit&) override {}
    void visit(const ast::BoolLit&) override {}
    void visit(const ast::NullLit&) override {}
    void visit(const ast::UndefinedLit&) override {}
    void visit(const ast::ThisExpr&) override {}
    void visit(const ast::Ident&) override {}

    void visit(const ast::Unary& u) override { u.operand->accept(*this); }
    void visit(const ast::Binary& b) override {
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
        for (const auto& prop : o.props) prop.value->accept(*this);
    }
    void visit(const ast::ArrayLit& a) override {
        for (const auto& elem : a.elements) elem->accept(*this);
    }
    void visit(const ast::FunctionExpr& f) override { addFunctionBody(f.body); }
    void visit(const ast::FunctionDecl& f) override { addFunctionBody(f.body); }

    void visit(const ast::BlockStmt& b) override {
        for (const auto& s : b.stmts) s->accept(*this);
    }
    void visit(const ast::VarDecl& v) override {
        if (v.init) v.init->accept(*this);
    }
    void visit(const ast::ReturnStmt& r) override {
        if (r.value) r.value->accept(*this);
    }
    void visit(const ast::ExprStmt& e) override { e.expr->accept(*this); }
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
        if (f.init) f.init->accept(*this);
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const ast::BreakStmt&) override {}
    void visit(const ast::ContinueStmt&) override {}
    void visit(const ast::SwitchStmt&) override {}
    void visit(const ast::ForInStmt&) override {}
    void visit(const ast::ForOfStmt&) override {}
    void visit(const ast::TryStmt&) override {}
    void visit(const ast::ThrowStmt&) override {}
    void visit(const ast::Module& m) override {
        for (const auto& s : m.body) s->accept(*this);
    }
};

// Finds `this` in a function body, stopping at any nested function: each
// binds its own receiver. Reuses CaptureVisitor's traversal shape, which
// already stops at function boundaries — it just records something else.
class ThisVisitor final : public CaptureVisitor {
public:
    bool found = false;
    void visit(const ast::ThisExpr&) override { found = true; }
    void visit(const ast::FunctionExpr&) override {}
    void visit(const ast::FunctionDecl&) override {}
};

void collectHoistedVars(const std::vector<ast::StmtPtr>& stmts, std::vector<std::string>& out);

void collectHoistedVarsIn(const ast::Stmt& stmt, std::vector<std::string>& out) {
    if (const auto* v = dynamic_cast<const ast::VarDecl*>(&stmt)) {
        if (v->isVar) out.push_back(v->name);
        return;
    }
    if (const auto* b = dynamic_cast<const ast::BlockStmt*>(&stmt)) {
        collectHoistedVars(b->stmts, out);
        return;
    }
    if (const auto* i = dynamic_cast<const ast::IfStmt*>(&stmt)) {
        collectHoistedVars(i->thenBody, out);
        collectHoistedVars(i->elseBody, out);
        return;
    }
    if (const auto* w = dynamic_cast<const ast::WhileStmt*>(&stmt)) {
        collectHoistedVars(w->body, out);
        return;
    }
    if (const auto* d = dynamic_cast<const ast::DoWhileStmt*>(&stmt)) {
        collectHoistedVars(d->body, out);
        return;
    }
    if (const auto* f = dynamic_cast<const ast::ForStmt*>(&stmt)) {
        if (f->init) collectHoistedVarsIn(*f->init, out);
        collectHoistedVars(f->body, out);
        return;
    }
    // A nested function's `var`s belong to that function, not this one.
}

void collectHoistedVars(const std::vector<ast::StmtPtr>& stmts, std::vector<std::string>& out) {
    for (const auto& s : stmts) {
        if (s) collectHoistedVarsIn(*s, out);
    }
}

}  // namespace

std::unordered_set<std::string> getCapturedNames(const std::vector<ast::StmtPtr>& stmts) {
    CaptureVisitor v;
    for (const auto& s : stmts) {
        if (s) s->accept(v);
    }
    return v.captured;
}

std::vector<std::string> getScopeDeclarations(const std::vector<ast::StmtPtr>& stmts) {
    std::vector<std::string> names;
    for (const auto& s : stmts) {
        if (!s) continue;
        if (const auto* v = dynamic_cast<const ast::VarDecl*>(s.get())) {
            if (!v->isVar) names.push_back(v->name);
        } else if (const auto* f = dynamic_cast<const ast::FunctionDecl*>(s.get())) {
            names.push_back(f->name);
        }
    }
    return names;
}

std::vector<std::string> getHoistedVarDeclarations(const std::vector<ast::StmtPtr>& stmts) {
    std::vector<std::string> names;
    collectHoistedVars(stmts, names);
    return names;
}

// The module's top level is lowered from a borrowed-pointer list (the
// function declarations having been split out), so each entry point takes
// that shape too.
std::unordered_set<std::string> getCapturedNames(const std::vector<const ast::Stmt*>& stmts) {
    CaptureVisitor v;
    for (const auto* s : stmts) {
        if (s) s->accept(v);
    }
    return v.captured;
}

std::vector<std::string> getScopeDeclarations(const std::vector<const ast::Stmt*>& stmts) {
    std::vector<std::string> names;
    for (const auto* s : stmts) {
        if (!s) continue;
        if (const auto* v = dynamic_cast<const ast::VarDecl*>(s)) {
            if (!v->isVar) names.push_back(v->name);
        } else if (const auto* f = dynamic_cast<const ast::FunctionDecl*>(s)) {
            names.push_back(f->name);
        }
    }
    return names;
}

bool usesThis(const std::vector<ast::StmtPtr>& stmts) {
    ThisVisitor v;
    for (const auto& s : stmts) {
        if (s) s->accept(v);
    }
    return v.found;
}

bool usesThis(const std::vector<const ast::Stmt*>& stmts) {
    ThisVisitor v;
    for (const auto* s : stmts) {
        if (s) s->accept(v);
    }
    return v.found;
}

std::vector<std::string> getHoistedVarDeclarations(const std::vector<const ast::Stmt*>& stmts) {
    std::vector<std::string> names;
    for (const auto* s : stmts) {
        if (s) collectHoistedVarsIn(*s, names);
    }
    return names;
}

}  // namespace bronze::lower
