#include "ast/queries.h"

#include <algorithm>

namespace bronze::ast {
namespace {

// A pattern's key expressions and defaults are ordinary expressions that run
// where the pattern does, so every walk over a scope has to reach them. The
// NAMES a pattern binds are a separate question, answered by
// `patternBoundNames`.
void visitPatternExprs(const BindingPattern* pattern, Visitor& v) {
    if (!pattern) return;
    for (const auto& elem : pattern->elements) {
        if (elem.keyExpr) elem.keyExpr->accept(v);
        if (elem.defaultValue) elem.defaultValue->accept(v);
        visitPatternExprs(elem.pattern.get(), v);
    }
}

// The same, for a parameter list: a default is a piece of code that runs in
// the function's own scope on every call that omits the argument.
void visitParamExprs(const std::vector<Param>& params, Visitor& v) {
    for (const auto& p : params) {
        if (p.defaultValue) p.defaultValue->accept(v);
        visitPatternExprs(p.pattern.get(), v);
    }
}

// Every identifier mentioned anywhere below a node, descending into nested
// functions. Used to decide what an enclosing scope must put in an
// environment record.
class IdentVisitor final : public Visitor {
public:
    std::unordered_set<std::string> names;

    void visit(const NumberLit&) override {}
    void visit(const StringLit&) override {}
    void visit(const RegExpLit&) override {}
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
    void visit(const TemplateLit& t) override {
        for (const auto& e : t.exprs) e->accept(*this);
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
        // The CONSTRUCTOR is a mention of a name too. Without it, a closure
        // that does `new Point(...)` did not capture `Point`, which only
        // showed up once classes made the constructor an ordinary binding
        // rather than a module-level function declaration.
        n.callee->accept(*this);
        for (const auto& arg : n.args) arg->accept(*this);
    }
    void visit(const SuperCall& c) override {
        names.insert(c.baseName);
        for (const auto& arg : c.args) arg->accept(*this);
    }
    void visit(const SuperMember& m) override { names.insert(m.baseName); }
    void visit(const SpreadElement& s) override { s.argument->accept(*this); }
    void visit(const DestructuringAssign& d) override {
        for (const auto& n : patternBoundNames(*d.pattern)) names.insert(n);
        visitPatternExprs(d.pattern.get(), *this);
        d.value->accept(*this);
    }
    void visit(const ClassDecl& c) override {
        names.insert(c.name);
        if (!c.superName.empty()) names.insert(c.superName);
        for (const auto& m : c.methods) m.fn->accept(*this);
    }
    void visit(const ObjectLit& o) override {
        for (const auto& prop : o.props) {
            if (prop.keyExpr) prop.keyExpr->accept(*this);
            prop.value->accept(*this);
        }
    }
    void visit(const ArrayLit& a) override {
        for (const auto& elem : a.elements) elem->accept(*this);
    }
    // A parameter's default is code that runs inside this function, so what
    // it mentions is mentioned here; the parameter NAMES are declarations,
    // not references, and are deliberately not recorded.
    void visit(const FunctionExpr& f) override {
        visitParamExprs(f.params, *this);
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const BlockStmt& b) override {
        for (const auto& s : b.stmts) s->accept(*this);
    }
    void visit(const VarDecl& v) override {
        if (v.pattern) {
            for (const auto& n : patternBoundNames(*v.pattern)) names.insert(n);
            visitPatternExprs(v.pattern.get(), *this);
        } else {
            names.insert(v.name);
        }
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
        for (const auto& s : f.init) s->accept(*this);
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const BreakStmt&) override {}
    void visit(const ContinueStmt&) override {}
    void visit(const SwitchStmt& n) override {
        if (n.discriminant) n.discriminant->accept(*this);
        for (const auto& c : n.cases) {
            if (c.test) c.test->accept(*this);
            for (const auto& s : c.body) s->accept(*this);
        }
    }
    void visit(const LabeledStmt& n) override {
        if (n.body) n.body->accept(*this);
    }
    void visit(const ForInStmt& n) override {
        if (n.pattern) {
            for (const auto& bound : patternBoundNames(*n.pattern)) names.insert(bound);
            visitPatternExprs(n.pattern.get(), *this);
        }
        if (n.object) n.object->accept(*this);
        for (const auto& s : n.body) s->accept(*this);
    }
    void visit(const ForOfStmt& n) override {
        if (n.pattern) {
            for (const auto& bound : patternBoundNames(*n.pattern)) names.insert(bound);
            visitPatternExprs(n.pattern.get(), *this);
        }
        if (n.iterable) n.iterable->accept(*this);
        for (const auto& s : n.body) s->accept(*this);
    }
    void visit(const TryStmt& n) override {
        for (const auto& s : n.body) s->accept(*this);
        if (n.hasCatchParam) {
            if (n.catchPattern) {
                for (const auto& bound : patternBoundNames(*n.catchPattern)) names.insert(bound);
                visitPatternExprs(n.catchPattern.get(), *this);
            } else {
                names.insert(n.catchName);
            }
        }
        for (const auto& s : n.catchBody) s->accept(*this);
        for (const auto& s : n.finallyBody) s->accept(*this);
    }
    void visit(const ThrowStmt& n) override {
        if (n.value) n.value->accept(*this);
    }
    void visit(const FunctionDecl& f) override {
        names.insert(f.name);
        visitParamExprs(f.params, *this);
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

    // A nested function reaches this scope through its body AND through its
    // parameter defaults, which are code that runs on every call that omits
    // the argument and can name anything in scope where the function was
    // written (docs/0017 decision 1).
    void addFunctionBody(const std::vector<StmtPtr>& body,
                         const std::vector<Param>* params = nullptr) {
        IdentVisitor idents;
        if (params) visitParamExprs(*params, idents);
        for (const auto& s : body) {
            if (s) s->accept(idents);
        }
        captured.insert(idents.names.begin(), idents.names.end());
    }

    void visit(const NumberLit&) override {}
    void visit(const StringLit&) override {}
    void visit(const RegExpLit&) override {}
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
    void visit(const TemplateLit& t) override {
        for (const auto& e : t.exprs) e->accept(*this);
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
    void visit(const SuperCall& c) override {
        for (const auto& arg : c.args) arg->accept(*this);
    }
    void visit(const SuperMember&) override {}
    void visit(const SpreadElement& s) override { s.argument->accept(*this); }
    void visit(const DestructuringAssign& d) override {
        visitPatternExprs(d.pattern.get(), *this);
        d.value->accept(*this);
    }
    // Every method of a class is a closure over this scope, so what its body
    // mentions is a candidate capture - including the parent class name that
    // a `super` inside it resolves against (docs/0012 decision 5).
    void visit(const ClassDecl& c) override {
        for (const auto& m : c.methods) addFunctionBody(m.fn->body, &m.fn->params);
    }
    void visit(const ObjectLit& o) override {
        for (const auto& prop : o.props) {
            if (prop.keyExpr) prop.keyExpr->accept(*this);
            prop.value->accept(*this);
        }
    }
    void visit(const ArrayLit& a) override {
        for (const auto& elem : a.elements) elem->accept(*this);
    }
    void visit(const FunctionExpr& f) override {
        addFunctionBody(f.body, &f.params);
        // An arrow's `this` is the enclosing function's receiver, so it is
        // captured like a free variable — under the one name no source
        // binding can collide with, because `this` is a keyword
        // (docs/0012 decision 3).
        if (f.isArrow && usesThis(f.body)) captured.insert("this");
    }
    void visit(const FunctionDecl& f) override { addFunctionBody(f.body, &f.params); }

    void visit(const BlockStmt& b) override {
        for (const auto& s : b.stmts) s->accept(*this);
    }
    void visit(const VarDecl& v) override {
        visitPatternExprs(v.pattern.get(), *this);
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
        for (const auto& s : f.init) s->accept(*this);
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const BreakStmt&) override {}
    void visit(const ContinueStmt&) override {}
    void visit(const SwitchStmt& n) override {
        if (n.discriminant) n.discriminant->accept(*this);
        for (const auto& c : n.cases) {
            if (c.test) c.test->accept(*this);
            for (const auto& s : c.body) s->accept(*this);
        }
    }
    void visit(const LabeledStmt& n) override {
        if (n.body) n.body->accept(*this);
    }
    void visit(const ForInStmt& n) override {
        visitPatternExprs(n.pattern.get(), *this);
        if (n.object) n.object->accept(*this);
        for (const auto& s : n.body) s->accept(*this);
    }
    void visit(const ForOfStmt& n) override {
        visitPatternExprs(n.pattern.get(), *this);
        if (n.iterable) n.iterable->accept(*this);
        for (const auto& s : n.body) s->accept(*this);
    }
    void visit(const TryStmt& n) override {
        for (const auto& s : n.body) s->accept(*this);
        visitPatternExprs(n.catchPattern.get(), *this);
        for (const auto& s : n.catchBody) s->accept(*this);
        for (const auto& s : n.finallyBody) s->accept(*this);
    }
    void visit(const ThrowStmt& n) override {
        if (n.value) n.value->accept(*this);
    }
    void visit(const Module& m) override {
        for (const auto& s : m.body) s->accept(*this);
    }
};

// Finds `this` in a function body, stopping at any nested function: each
// binds its own receiver. Reuses CaptureVisitor's traversal shape, which
// already stops at function boundaries — it just records something else.
// Does this body need a receiver? An ordinary nested function has its own
// `this` and is not descended into; an ARROW does not, so its `this` is
// this body's and has to be found (docs/0012 decision 3).
class ThisVisitor final : public CaptureVisitor {
public:
    bool found = false;
    void visit(const ThisExpr&) override { found = true; }
    // `super(...)` and `super.m()` both RUN on the current receiver, so a
    // method whose body never writes `this` still needs one (docs/0012
    // decision 5). Missing this made `super.describe()` in an override
    // report `this` outside a function.
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
};

// Finds `arguments` in a function body, descending into ARROWS and stopping
// at every other function — the same boundary `ThisVisitor` walks, because it
// is the same rule: an arrow has no `arguments` of its own and sees the
// enclosing function's, exactly as it does for `this` (docs/0012 decision 3,
// applied by docs/0027 decision 3).
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
};

void appendDeclaredNames(const VarDecl& decl, std::vector<std::string>& out);

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
    // reaches the base name without ever spelling it as an identifier
    // (docs/0012 decision 5).
    void visit(const ClassDecl& c) override {
        if (c.superName == name_) found = true;
        for (const auto& m : c.methods) {
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
};

void collectHoistedVars(const std::vector<StmtPtr>& stmts, std::vector<std::string>& out);

// A declarator contributes its name, or — when it is a pattern — every name
// the pattern binds. One helper, because a scope that saw only the outermost
// level of a pattern would leave the inner names with no slot to live in.
void appendDeclaredNames(const VarDecl& decl, std::vector<std::string>& out) {
    if (decl.pattern) {
        for (auto& name : patternBoundNames(*decl.pattern)) out.push_back(std::move(name));
    } else {
        out.push_back(decl.name);
    }
}

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
            if (!v->isVar) appendDeclaredNames(*v, names);
        } else if (const auto* f = dynamic_cast<const FunctionDecl*>(s)) {
            names.push_back(f->name);
        } else if (const auto* c = dynamic_cast<const ClassDecl*>(s)) {
            names.push_back(c->name);
        }
    }
    return names;
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
    // exists (docs/0017 decision 9).
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

std::vector<std::string> getHoistedVarDeclarations(const std::vector<const Stmt*>& stmts) {
    std::vector<std::string> names;
    for (const auto* s : stmts) {
        if (s) collectHoistedVarsIn(*s, names);
    }
    return names;
}

}  // namespace bronze::ast
