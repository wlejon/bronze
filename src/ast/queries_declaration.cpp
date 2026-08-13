// What a scope DECLARES: the names a statement list binds directly, the `var`s
// it hoists out of every block under it, and the lexical half of the first.
//
// One analysis and so one unit. The rule that makes it its own is the depth each
// question stops at: a `let` belongs to the list that spells it, a `var` belongs
// to the enclosing FUNCTION however deeply it is written (ECMA-262 8.6.2), and
// nothing at all crosses a nested function boundary. Three depths, one walk.

#include "ast/queries.h"

#include "ast/query_walk.h"

namespace bronze::ast {

using namespace detail;

namespace {

void collectHoistedVars(const std::vector<StmtPtr>& stmts, std::vector<std::string>& out);

void collectHoistedVarsIn(const Stmt& stmt, std::vector<std::string>& out) {
    if (const auto* v = dynamic_cast<const VarDecl*>(&stmt)) {
        if (v->isVar) appendDeclaredNames(*v, out);
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
        collectHoistedVars(f->init, out);
        collectHoistedVars(f->body, out);
        return;
    }
    if (const auto* fo = dynamic_cast<const ForOfStmt*>(&stmt)) {
        collectHoistedVars(fo->body, out);
        return;
    }
    // A `var` in a switch case is the function's, exactly like a `var` in an
    // if-branch: the case clause is not a scope of its own, and only the
    // switch BODY is a block (ECMA-262 14.12.2). A switch missing from this
    // walk left `var m` inside a case with no function-level binding, so the
    // name read `undefined variable` after the switch.
    if (const auto* sw = dynamic_cast<const SwitchStmt*>(&stmt)) {
        for (const auto& c : sw->cases) collectHoistedVars(c.body, out);
        return;
    }
    if (const auto* fi = dynamic_cast<const ForInStmt*>(&stmt)) {
        collectHoistedVars(fi->body, out);
        return;
    }
    if (const auto* lb = dynamic_cast<const LabeledStmt*>(&stmt)) {
        if (lb->body) collectHoistedVarsIn(*lb->body, out);
        return;
    }
    // None of a try statement's three parts is a function boundary, so a
    // `var` in any of them is this function's. The catch PARAMETER is not one
    // of them: 14.15.2 gives it its own declarative environment, and it is a
    // lexical binding whatever the body does with it.
    if (const auto* tr = dynamic_cast<const TryStmt*>(&stmt)) {
        collectHoistedVars(tr->body, out);
        collectHoistedVars(tr->catchBody, out);
        collectHoistedVars(tr->finallyBody, out);
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

std::vector<std::string> getScopeDeclarations(const std::vector<StmtPtr>& stmts) {
    std::vector<std::string> names;
    for (const auto& s : stmts) {
        if (!s) continue;
        if (const auto* v = dynamic_cast<const VarDecl*>(s.get())) {
            if (!v->isVar) appendDeclaredNames(*v, names);
        } else if (const auto* f = dynamic_cast<const FunctionDecl*>(s.get())) {
            names.push_back(f->name);
        } else if (const auto* c = dynamic_cast<const ClassDecl*>(s.get())) {
            names.push_back(c->name);
        }
    }
    return names;
}

std::vector<std::string> getHoistedVarDeclarations(const std::vector<StmtPtr>& stmts) {
    std::vector<std::string> names;
    collectHoistedVars(stmts, names);
    return names;
}

std::vector<std::string> getLexicalDeclarations(const std::vector<StmtPtr>& stmts) {
    std::vector<std::string> names;
    for (const auto& s : stmts) {
        if (s) appendLexicalNames(*s, names);
    }
    return names;
}

std::vector<std::string> getLexicalDeclarations(const std::vector<const Stmt*>& stmts) {
    std::vector<std::string> names;
    for (const auto* s : stmts) {
        if (s) appendLexicalNames(*s, names);
    }
    return names;
}

std::vector<std::string> getScopeDeclarations(const std::vector<const Stmt*>& stmts) {
    std::vector<std::string> names;
    for (const auto* s : stmts) {
        if (!s) continue;
        if (const auto* v = dynamic_cast<const VarDecl*>(s)) {
            if (!v->isVar) appendDeclaredNames(*v, names);
        } else if (const auto* f = dynamic_cast<const FunctionDecl*>(s)) {
            names.push_back(f->name);
        } else if (const auto* c = dynamic_cast<const ClassDecl*>(s)) {
            names.push_back(c->name);
        }
    }
    return names;
}

std::vector<std::string> getHoistedVarDeclarations(const std::vector<const Stmt*>& stmts) {
    std::vector<std::string> names;
    for (const auto* s : stmts) {
        if (s) collectHoistedVarsIn(*s, names);
    }
    return names;
}
}  // namespace bronze::ast
