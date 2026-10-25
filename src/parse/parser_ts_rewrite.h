#pragma once

// AST rewrites the TypeScript lowerings need after a construct has been
// parsed: qualifying the names a namespace or an enum exports, and placing a
// constructor's parameter-property assignments after its `super()` call. Both
// are one walk — every expression slot below a statement list, with the
// scopes that shadow a name tracked on the way down — so there is one copy of
// it here, shaped after the module linker's renamer (src/modules/rename.cpp):
// the shadow set for each scope kind comes from `ast::queries`, the same code
// lowering resolves names with, and a node kind the walk does not recognise
// is a diagnosed internal error rather than a subtree left alone.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "support/diagnostics.h"

namespace bronze::ts_rewrite {

class ScopedWalk {
public:
    explicit ScopedWalk(DiagnosticSink& diags) : diags_(diags) {}
    virtual ~ScopedWalk() = default;

    // A statement list with no scope of its own pushed: the caller's scope
    // (a namespace body, a constructor body) is what the rewrite is about.
    bool walkStmts(std::vector<ast::StmtPtr>& list) {
        stmtList(list);
        return ok_;
    }
    bool walkExpr(ast::ExprPtr& p) {
        slot(p);
        return ok_;
    }

protected:
    bool shadowed(const std::string& name) const;
    void fail(Span span, const std::string& message);

    // An expression slot, before its children are walked. Returns true when
    // the hook replaced or fully handled the node, so the walk does not
    // descend into it.
    virtual bool visitSlot(ast::ExprPtr& p) {
        (void)p;
        return false;
    }
    // A pattern element that names a binding being ASSIGNED (a destructuring
    // assignment's target), not declared.
    virtual void assignTarget(ast::PatternElement& elem) { (void)elem; }
    // A `for (x of …)` / `for (x in …)` head with no declaration keyword.
    virtual void iterationTarget(const std::string& name, Span span) {
        (void)name;
        (void)span;
    }
    // A function about to be walked. Returns false to leave its parameters and
    // body alone.
    virtual bool enterFunction(const ast::FunctionExpr* fn, bool isArrow) {
        (void)fn;
        (void)isArrow;
        return true;
    }
    // The function enterFunction admitted has been walked.
    virtual void exitFunction(bool isArrow) { (void)isArrow; }
    // A class body about to be walked (its heritage is walked regardless).
    virtual bool enterClassBody() { return true; }

    bool ok_ = true;

private:
    void stmtList(std::vector<ast::StmtPtr>& list);
    void blockList(std::vector<ast::StmtPtr>& list);
    void functionBody(std::vector<ast::Param>& params, std::vector<ast::StmtPtr>& body,
                      const std::string& ownName);
    void pattern(ast::BindingPattern& p, bool declaring);
    void stmt(ast::Stmt& s);
    void iterationHead(const std::string& name, ast::BindingPattern* pat, bool declares,
                       std::vector<ast::StmtPtr>& body, Span span);
    void classBody(const std::string& ownName, ast::ExprPtr& superClass, std::string& superName,
                   std::vector<ast::ClassMethod>& methods);
    void slot(ast::ExprPtr& p);
    void expr(ast::Expr& e);

    DiagnosticSink& diags_;
    std::vector<std::set<std::string>> shadow_;
};

// Rewrites every reference to one of `names` that resolves to the scope the
// walk starts in into `object.name` — a namespace's exported variables read
// through its object, an enum's members through the enum. A destructuring
// target becomes the member reference too; a bare `for (x of …)` head naming
// one is refused, since its target cannot be a member here.
class QualifyNames final : public ScopedWalk {
public:
    QualifyNames(DiagnosticSink& diags, std::set<std::string> names, std::string object)
        : ScopedWalk(diags), names_(std::move(names)), object_(std::move(object)) {}

    ast::ExprPtr member(const std::string& name, Span span) const;

private:
    bool qualifies(const std::string& name) const {
        return names_.count(name) != 0 && !shadowed(name);
    }
    bool visitSlot(ast::ExprPtr& p) override;
    void assignTarget(ast::PatternElement& elem) override;
    void iterationTarget(const std::string& name, Span span) override;

    std::set<std::string> names_;
    std::string object_;
};

// Node builders for the JavaScript the TypeScript lowerings emit. Every node
// gets the span of the declaration it stands for, so a diagnostic or a stack
// trace inside the emitted code points at the source that produced it.
ast::ExprPtr makeIdent(const std::string& name, Span span);
ast::ExprPtr makeString(const std::string& value, Span span);
ast::ExprPtr makeNumber(double value, Span span);
ast::ExprPtr makeMember(ast::ExprPtr object, const std::string& property, Span span);
ast::ExprPtr makeIndex(ast::ExprPtr object, ast::ExprPtr index, Span span);
ast::ExprPtr makeBinary(ast::BinaryOp op, ast::ExprPtr lhs, ast::ExprPtr rhs, Span span);
ast::StmtPtr makeExprStmt(ast::ExprPtr expr, Span span);
// `(function (param) { body })(arg)`: the shape TypeScript gives every enum
// and namespace body. A plain function and not an arrow, as TypeScript emits
// it, so `this` and `arguments` inside mean what they mean there.
ast::ExprPtr makeIife(const std::string& param, std::vector<ast::StmtPtr> body,
                      ast::ExprPtr arg, bool strict, Span span);

// ECMA-262 6.1.6.1.20 Number::toString(x) in radix 10: the spelling a
// computed enum key or a folded string concatenation gets at run time.
std::string numberToString(double v);
// ECMA-262 7.1.6 / 7.1.7, ToInt32 and ToUint32 — defined for every double,
// NaN and the infinities included, which a cast is not.
int32_t toInt32(double v);
uint32_t toUint32(double v);

// The dotted spelling of a heritage expression (`Base`, `ns.Base`), the name a
// class records for its parent; empty for anything else.
std::string heritageName(const ast::Expr* expr);

}  // namespace bronze::ts_rewrite
