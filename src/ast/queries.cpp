// What a subtree REFERS TO: the names it reads freely, the subset of those a
// nested function reaches (and so must be able to outlive this frame), and the
// two implicit references — `this` and `arguments` — that decide whether a
// frame has to materialize either.
//
// One unit because every question here is the same walk with the same stopping
// rule: descend everywhere, and at a nested function boundary ask whether the
// name is rebound there before continuing. The sibling files answer questions
// this walk cannot: what a scope DECLARES (queries_declaration.cpp), which
// binding can be read before it is initialized (queries_tdz.cpp), and what a
// suspension has to keep alive (queries_yield.cpp).

#include "ast/queries.h"

#include <algorithm>

#include "ast/query_walk.h"

namespace bronze::ast {

using namespace detail;

namespace {

// Finds `this` in a function body, stopping at any nested function: each binds
// its own receiver. Reuses CaptureVisitor's traversal shape, which already
// stops at function boundaries — it just records something else. Does this body
// need a receiver? An ordinary nested function has its own `this` and is not
// descended into; an ARROW does not, so its `this` is this body's and has to be
// found.
class ThisVisitor final : public CaptureVisitor {
public:
    bool found = false;
    void visit(const ThisExpr&) override { found = true; }
    // `super(...)` and `super.m()` both RUN on the current receiver, so a
    // method whose body never writes `this` still needs one. Missing this made
    // `super.describe()` in an override report `this` outside a function.
    void visit(const SuperCall& c) override {
        found = true;
        for (const auto& arg : c.args) arg->accept(*this);
    }
    void visit(const SuperMember&) override { found = true; }
    void visit(const FunctionExpr& f) override {
        if (!f.isArrow) return;
        for (const auto& s : f.body) {
            if (s) s->accept(*this);
        }
    }
    void visit(const FunctionDecl&) override {}
    void visit(const ClassDecl&) override {}
    void visit(const ClassExpr&) override {}
};

// Finds `arguments` in a function body, descending into ARROWS and stopping at
// every other function — the same boundary `ThisVisitor` walks, because it is
// the same rule: an arrow has no `arguments` of its own and sees the enclosing
// function's, exactly as it does for `this`.
//
// `arguments` is an ordinary Identifier, not a keyword, so the name can be
// bound — and where it is, the binding wins and no arguments object exists.
// That test is `usesArguments`'s, below: this visitor only finds mentions.
class ArgumentsVisitor final : public CaptureVisitor {
public:
    bool found = false;
    void visit(const Ident& i) override {
        if (i.name == "arguments") found = true;
    }
    void visit(const FunctionExpr& f) override {
        if (!f.isArrow) return;
        visitParamExprs(f.params, *this);
        for (const auto& s : f.body) {
            if (s) s->accept(*this);
        }
    }
    void visit(const FunctionDecl&) override {}
    void visit(const ClassDecl&) override {}
    void visit(const ClassExpr&) override {}
};

// Does a function body bind `name` itself, so that every reference to it
// anywhere inside resolves to that binding (or to an inner shadow of it)
// rather than reaching out to the enclosing scope?
//
// Three sources, and no more: parameters, the body's own top-level lexical and
// function declarations, and a top-level `var`. Anything declared deeper is
// deliberately not counted, in BOTH directions of the spec:
//
//   - a `let` inside an inner block covers only that block, so `{ let x; }
//     use(x)` still reaches outward and counting it would hide a real capture;
//   - a `var` inside an inner block is function-scoped by 8.6.2 and so DOES
//     cover the whole body — but bronze does not hoist one out of the block it
//     is written in, and this predicate must describe the bindings lowering
//     actually makes rather than the ones the spec describes. Claiming the
//     shadow that lowering will not create is how `for (let i…)` came to be
//     captured by a closure whose `var i` sat one block down.
//
// Erring toward "not bound" costs a diagnostic; erring the other way is a
// silently wrong capture.
bool functionBindsName(const std::vector<Param>& params, const std::vector<StmtPtr>& body,
                       const std::string& name) {
    for (const auto& p : params) {
        if (p.name == name) return true;
        if (p.pattern) {
            for (const auto& bound : patternBoundNames(*p.pattern)) {
                if (bound == name) return true;
            }
        }
    }
    for (const auto& declared : getScopeDeclarations(body)) {
        if (declared == name) return true;
    }
    for (const auto& s : body) {
        const auto* decl = dynamic_cast<const VarDecl*>(s.get());
        if (!decl || !decl->isVar) continue;
        std::vector<std::string> declared;
        appendDeclaredNames(*decl, declared);
        for (const auto& d : declared) {
            if (d == name) return true;
        }
    }
    return false;
}

bool functionFreelyReferences(const std::vector<Param>& params, const std::vector<StmtPtr>& body,
                              const std::string& name);

// Does a function nested under this node reach `name` in the scope the node
// sits in? CaptureVisitor's traversal already stops at every function
// boundary, so overriding only the boundaries turns "what could a closure
// capture" into "does a closure capture THIS name" — and the answer is no
// whenever the closure binds the name itself.
class NestedFunctionRefVisitor : public CaptureVisitor {
public:
    explicit NestedFunctionRefVisitor(std::string name) : name_(std::move(name)) {}
    bool found = false;

    void visit(const FunctionExpr& f) override {
        if (functionFreelyReferences(f.params, f.body, name_)) found = true;
    }
    void visit(const FunctionDecl& f) override {
        if (functionFreelyReferences(f.params, f.body, name_)) found = true;
    }
    // `super` in a method resolves against the class's BASE, so a method body
    // reaches the base name without ever spelling it as an identifier.
    void visit(const ClassDecl& c) override {
        if (c.superName == name_) found = true;
        for (const auto& m : c.methods) {
            // A computed member name is not inside a function at all, so this
            // over-approximates for the nested-function question and answers
            // the free-mention one exactly. Over-approximating "free" costs an
            // environment slot; the other direction is a wrong capture.
            if (m.keyExpr) m.keyExpr->accept(*this);
            if (functionFreelyReferences(m.fn->params, m.fn->body, name_)) found = true;
        }
    }
    void visit(const ClassExpr& c) override {
        if (c.superName == name_) found = true;
        for (const auto& m : c.methods) {
            if (m.keyExpr) m.keyExpr->accept(*this);
            if (functionFreelyReferences(m.fn->params, m.fn->body, name_)) found = true;
        }
    }

protected:
    std::string name_;
};

// The same, widened to the scope's OWN mentions: inside a function, a
// reference in the body counts as much as one two closures down.
//
// Over-approximates in the direction of "free": a mention inside an inner
// BLOCK that redeclares the name still counts, because `functionBindsName`
// looks only at bindings that cover the whole body. That costs a diagnostic a
// fully scope-resolved walk would not raise, and never the reverse — and the
// reverse is a silently wrong capture.
class FreeMentionVisitor final : public NestedFunctionRefVisitor {
public:
    using NestedFunctionRefVisitor::NestedFunctionRefVisitor;

    void visit(const Ident& i) override {
        if (i.name == name_) found = true;
    }
    // A destructuring assignment's TARGETS are writes of those names, which is
    // a reference like any read. CaptureVisitor walks only the pattern's
    // expressions, because for its own question a target is a declaration.
    void visit(const DestructuringAssign& d) override {
        for (const auto& bound : patternBoundNames(*d.pattern)) {
            if (bound == name_) found = true;
        }
        CaptureVisitor::visit(d);
    }
};

bool functionFreelyReferences(const std::vector<Param>& params, const std::vector<StmtPtr>& body,
                              const std::string& name) {
    if (functionBindsName(params, body, name)) return false;
    FreeMentionVisitor v{name};
    visitParamExprs(params, v);
    for (const auto& s : body) {
        if (s) s->accept(v);
    }
    return v.found;
}

// Finds a `return <expr>;` in a function body, stopping at any nested
// function: an inner `return` returns from that function, not this one.
// Same traversal shape as ThisVisitor, recording something else again.
class ValueReturnVisitor final : public CaptureVisitor {
public:
    bool found = false;
    void visit(const ReturnStmt& r) override {
        if (r.value) found = true;
    }
    void visit(const FunctionExpr&) override {}
    void visit(const FunctionDecl&) override {}
    void visit(const ClassDecl&) override {}
    void visit(const ClassExpr&) override {}
};

}  // namespace

std::unordered_set<std::string> getCapturedNames(const std::vector<StmtPtr>& stmts) {
    CaptureVisitor v;
    for (const auto& s : stmts) {
        if (s) s->accept(v);
    }
    return v.captured;
}

std::unordered_set<std::string> getReferencedNames(const std::vector<StmtPtr>& stmts) {
    IdentVisitor v;
    for (const auto& s : stmts) {
        if (s) s->accept(v);
    }
    return v.names;
}

std::unordered_set<std::string> getParamReferencedNames(const std::vector<Param>& params) {
    IdentVisitor v;
    visitParamExprs(params, v);
    return v.names;
}

std::unordered_set<std::string> getCapturedNames(const std::vector<const Stmt*>& stmts) {
    CaptureVisitor v;
    for (const auto* s : stmts) {
        if (s) s->accept(v);
    }
    return v.captured;
}

bool closureCapturesLoopBinding(const ForStmt& forStmt, const std::string& name) {
    NestedFunctionRefVisitor v{name};
    // The head is walked too: `for (let i = 0, f = () => i; …)` puts a closure
    // over the loop binding in the declaration list itself.
    for (const auto& s : forStmt.init) {
        if (s) s->accept(v);
    }
    if (forStmt.condition) forStmt.condition->accept(v);
    if (forStmt.update) forStmt.update->accept(v);
    for (const auto& s : forStmt.body) {
        if (s) s->accept(v);
    }
    return v.found;
}

bool returnsAValue(const std::vector<StmtPtr>& stmts) {
    ValueReturnVisitor v;
    for (const auto& s : stmts) {
        if (s) s->accept(v);
    }
    return v.found;
}

bool usesArguments(const std::vector<Param>& params, const std::vector<StmtPtr>& body) {
    // A declaration of the name wins over the arguments object, and there is
    // no object at all in that case (10.2.11 CreateMappedArgumentsObject runs
    // only when `arguments` is not already bound). Checked first, so a
    // function whose parameter is called `arguments` never grows the synthetic
    // binding that would then be a redeclaration of it.
    for (const auto& p : params) {
        if (p.pattern) {
            for (const auto& bound : patternBoundNames(*p.pattern)) {
                if (bound == "arguments") return false;
            }
        } else if (p.name == "arguments") {
            return false;
        }
    }
    for (const auto& name : getScopeDeclarations(body)) {
        if (name == "arguments") return false;
    }
    for (const auto& name : getHoistedVarDeclarations(body)) {
        if (name == "arguments") return false;
    }
    ArgumentsVisitor v;
    // A parameter DEFAULT runs inside the function and appears nowhere in its
    // body, so it is scanned here for the reason `getParamReferencedNames`
    // exists.
    visitParamExprs(params, v);
    for (const auto& s : body) {
        if (s) s->accept(v);
    }
    return v.found;
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

}  // namespace bronze::ast
