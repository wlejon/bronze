#pragma once

// The traversal primitives the `ast::` queries are built out of, shared because
// four analyses walk the same tree with the same boundary rules and a second
// copy of one of them would eventually disagree with the first. Nothing here is
// a query: each of these answers "what is under this node", and the files that
// include it decide what to do with the answer.
//
// `detail` and not an anonymous namespace, because these have to be ONE
// definition across the units that share them.

#include <string>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "ast/queries.h"

namespace bronze::ast::detail {

// A pattern's key expressions and defaults are ordinary expressions that run
// where the pattern does, so every walk over a scope has to reach them. The
// NAMES a pattern binds are a separate question, answered by
// `patternBoundNames`.
inline void visitPatternExprs(const BindingPattern* pattern, Visitor& v) {
    if (!pattern) return;
    for (const auto& elem : pattern->elements) {
        if (elem.keyExpr) elem.keyExpr->accept(v);
        // A member target's base is code too — `({ a: obj[k()] } = src)` calls
        // `k` where the pattern runs, and reads `obj` from the enclosing scope.
        if (elem.target) elem.target->accept(v);
        if (elem.defaultValue) elem.defaultValue->accept(v);
        visitPatternExprs(elem.pattern.get(), v);
    }
}

// The same, for a parameter list: a default is a piece of code that runs in
// the function's own scope on every call that omits the argument.
inline void visitParamExprs(const std::vector<Param>& params, Visitor& v) {
    for (const auto& p : params) {
        if (p.defaultValue) p.defaultValue->accept(v);
        visitPatternExprs(p.pattern.get(), v);
    }
}

// Every identifier mentioned anywhere below a node, descending into nested
// functions. Used to decide what an enclosing scope must put in an
// environment record. Not final: lowering's typed-element binding scan
// derives from it to reuse exactly this traversal — a walk that misses a
// mention there is an unsound proof, so it must be THIS walk.
class IdentVisitor : public Visitor {
public:
    std::unordered_set<std::string> names;

    // Every name this walk records goes through here — a declaration's own
    // name as much as a read — so a derivation that needs to know WHERE a
    // mention sits (FreeNameVisitor, which drops the ones a scope it is
    // inside binds) overrides one function rather than the whole walk.
    virtual void mention(const std::string& name) { names.insert(name); }

    void visit(const NumberLit&) override {}
    void visit(const BigIntLit&) override {}
    void visit(const StringLit&) override {}
    void visit(const RegExpLit&) override {}
    void visit(const BoolLit&) override {}
    void visit(const NullLit&) override {}
    void visit(const UndefinedLit&) override {}
    void visit(const ThisExpr&) override {}
    void visit(const Ident& i) override { mention(i.name); }

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
    void visit(const NewTargetExpr&) override {}
    void visit(const ImportMetaExpr&) override {}
    void visit(const TaggedTemplate& t) override {
        t.tag->accept(*this);
        for (const auto& e : t.templateLit->exprs) e->accept(*this);
    }
    void visit(const SuperCall& c) override {
        if (c.baseExpr) c.baseExpr->accept(*this);
        else if (!c.baseName.empty()) mention(c.baseName);
        for (const auto& arg : c.args) arg->accept(*this);
    }
    void visit(const SuperMember& m) override {
        if (m.baseExpr) m.baseExpr->accept(*this);
        else if (!m.baseName.empty()) mention(m.baseName);
    }
    void visit(const SpreadElement& s) override { s.argument->accept(*this); }
    void visit(const YieldExpr& y) override { y.argument->accept(*this); }
    void visit(const DynamicImportExpr& d) override { if (d.specifier) d.specifier->accept(*this); }
    void visit(const DestructuringAssign& d) override {
        for (const auto& n : patternBoundNames(*d.pattern)) mention(n);
        visitPatternExprs(d.pattern.get(), *this);
        d.value->accept(*this);
    }
    void visit(const ClassDecl& c) override {
        mention(c.name);
        if (c.superClass) c.superClass->accept(*this);
        else if (!c.superName.empty()) mention(c.superName);
        for (const auto& m : c.methods) {
            // A computed member name is an expression of the ENCLOSING scope,
            // evaluated where the class is defined rather than where the method
            // is called, so what it mentions belongs here and not in the body.
            if (m.keyExpr) m.keyExpr->accept(*this);
            if (m.fn) m.fn->accept(*this);
            if (m.init) m.init->accept(*this);
        }
    }
    void visit(const ClassExpr& c) override {
        if (c.superClass) c.superClass->accept(*this);
        else if (!c.superName.empty()) mention(c.superName);
        for (const auto& m : c.methods) {
            if (m.keyExpr) m.keyExpr->accept(*this);
            if (m.fn) m.fn->accept(*this);
            if (m.init) m.init->accept(*this);
        }
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
            for (const auto& n : patternBoundNames(*v.pattern)) mention(n);
            visitPatternExprs(v.pattern.get(), *this);
        } else {
            mention(v.name);
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
    // The head's name is a mention whether it declares or assigns — and when
    // it assigns (`for (last of xs)`, 14.7.5.7 with lhsKind assignment) it is
    // the ONLY mention of an outer binding a closure may hold, so leaving it
    // out resolved the name against the global object and threw.
    void visit(const ForInStmt& n) override {
        if (!n.name.empty()) mention(n.name);
        if (n.pattern) {
            for (const auto& bound : patternBoundNames(*n.pattern)) mention(bound);
            visitPatternExprs(n.pattern.get(), *this);
        }
        if (n.object) n.object->accept(*this);
        for (const auto& s : n.body) s->accept(*this);
    }
    void visit(const ForOfStmt& n) override {
        if (!n.name.empty()) mention(n.name);
        if (n.pattern) {
            for (const auto& bound : patternBoundNames(*n.pattern)) mention(bound);
            visitPatternExprs(n.pattern.get(), *this);
        }
        if (n.iterable) n.iterable->accept(*this);
        for (const auto& s : n.body) s->accept(*this);
    }
    void visit(const TryStmt& n) override {
        for (const auto& s : n.body) s->accept(*this);
        if (n.hasCatchParam) {
            if (n.catchPattern) {
                for (const auto& bound : patternBoundNames(*n.catchPattern)) mention(bound);
                visitPatternExprs(n.catchPattern.get(), *this);
            } else {
                mention(n.catchName);
            }
        }
        for (const auto& s : n.catchBody) s->accept(*this);
        for (const auto& s : n.finallyBody) s->accept(*this);
    }
    void visit(const ThrowStmt& n) override {
        if (n.value) n.value->accept(*this);
    }
    void visit(const FunctionDecl& f) override {
        mention(f.name);
        visitParamExprs(f.params, *this);
        for (const auto& s : f.body) s->accept(*this);
    }
    void visit(const Module& m) override {
        for (const auto& s : m.body) s->accept(*this);
    }
};

// A declarator contributes its name, or — when it is a pattern — every name
// the pattern binds. One helper, because a scope that saw only the outermost
// level of a pattern would leave the inner names with no slot to live in.
inline void appendDeclaredNames(const VarDecl& decl, std::vector<std::string>& out) {
    if (decl.pattern) {
        for (auto& name : patternBoundNames(*decl.pattern)) out.push_back(std::move(name));
    } else {
        out.push_back(decl.name);
    }
}

// Walks a scope looking for nested functions; everything mentioned inside
// one is a candidate capture. Does not descend into a nested function
// itself — IdentVisitor already covers it to every depth.
class CaptureVisitor : public Visitor {
public:
    std::unordered_set<std::string> captured;

    // A nested function reaches this scope through its body AND through its
    // parameter defaults, which are code that runs on every call that omits the
    // argument and can name anything in scope where the function was written.
    void addFunctionBody(const std::vector<StmtPtr>& body,
                         const std::vector<Param>* params = nullptr);

    void visit(const NumberLit&) override {}
    void visit(const BigIntLit&) override {}
    void visit(const StringLit&) override {}
    void visit(const RegExpLit&) override {}
    void visit(const BoolLit&) override {}
    void visit(const NullLit&) override {}
    void visit(const UndefinedLit&) override {}
    void visit(const ThisExpr&) override {}
    void visit(const Ident&) override {}

    void visit(const YieldExpr& y) override { y.argument->accept(*this); }
    void visit(const DynamicImportExpr& d) override { if (d.specifier) d.specifier->accept(*this); }
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
    void visit(const NewTargetExpr&) override {}
    void visit(const ImportMetaExpr&) override {}
    void visit(const TaggedTemplate& t) override {
        t.tag->accept(*this);
        for (const auto& e : t.templateLit->exprs) e->accept(*this);
    }
    void visit(const SuperCall& c) override {
        if (c.baseExpr) c.baseExpr->accept(*this);
        for (const auto& arg : c.args) arg->accept(*this);
    }
    void visit(const SuperMember& m) override {
        if (m.baseExpr) m.baseExpr->accept(*this);
    }
    void visit(const SpreadElement& s) override { s.argument->accept(*this); }
    void visit(const DestructuringAssign& d) override {
        visitPatternExprs(d.pattern.get(), *this);
        d.value->accept(*this);
    }
    // Every method of a class is a closure over this scope, so what its body
    // mentions is a candidate capture - including the parent class name that a
    // `super` inside it resolves against.
    void visit(const ClassDecl& c) override {
        if (c.superClass) c.superClass->accept(*this);
        for (const auto& m : c.methods) {
            if (m.keyExpr) {
                m.keyExpr->accept(*this);
                if (m.isField && !m.isStatic) {
                    IdentVisitor idents;
                    m.keyExpr->accept(idents);
                    captured.insert(idents.names.begin(), idents.names.end());
                }
            }
            if (m.fn) addFunctionBody(m.fn->body, &m.fn->params);
            if (m.init) {
                m.init->accept(*this);
                if (m.isField && !m.isStatic) {
                    IdentVisitor idents;
                    m.init->accept(idents);
                    captured.insert(idents.names.begin(), idents.names.end());
                }
            }
        }
    }
    void visit(const ClassExpr& c) override {
        if (c.superClass) c.superClass->accept(*this);
        for (const auto& m : c.methods) {
            if (m.keyExpr) {
                m.keyExpr->accept(*this);
                if (m.isField && !m.isStatic) {
                    IdentVisitor idents;
                    m.keyExpr->accept(idents);
                    captured.insert(idents.names.begin(), idents.names.end());
                }
            }
            if (m.fn) addFunctionBody(m.fn->body, &m.fn->params);
            if (m.init) {
                m.init->accept(*this);
                if (m.isField && !m.isStatic) {
                    IdentVisitor idents;
                    m.init->accept(idents);
                    captured.insert(idents.names.begin(), idents.names.end());
                }
            }
        }
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
    void visit(const FunctionExpr& f) override {
        addFunctionBody(f.body, &f.params);
        // An arrow's `this` is the enclosing function's receiver, so it is
        // captured like a free variable — under the one name no source binding
        // can collide with, because `this` is a keyword.
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

// The names a function reaches OUTSIDE itself: IdentVisitor's walk, with a
// mention kept only when no scope it sits inside binds that name.
//
// "Inside" is the whole point. The previous form subtracted every name
// declared ANYWHERE in the body from every mention in it, so `{ let x; }
// return x;` in a closure lost the outer `x` — the inner block's binding hid
// a read it never covered, and the read resolved against the global object.
// Each list of statements is a scope here, holding the declarations
// `getScopeDeclarations` gives it; a function adds its parameters, its own
// name when it is a named expression, and the `var`s hoisted from every block
// under it (8.6.2); a loop head, a catch parameter and a class name bind over
// the body they front. Anything this walk does not know to be bound is free,
// which errs toward a capture — the direction that costs a slot, never a wrong
// resolution.
class FreeNameVisitor final : public IdentVisitor {
public:
    using IdentVisitor::visit;

    void mention(const std::string& name) override {
        for (const auto& scope : scopes_) {
            if (scope.count(name) != 0) return;
        }
        names.insert(name);
    }

    // The function whose free names are wanted: its parameters and body form
    // the outermost scope of the walk, so a default like `(a, b = a)` binds.
    void walkFunction(const std::vector<StmtPtr>& body, const std::vector<Param>* params,
                      const std::string& ownName = {}) {
        std::unordered_set<std::string> scope;
        if (params) {
            for (const auto& p : *params) {
                if (!p.name.empty()) scope.insert(p.name);
                if (p.pattern) {
                    for (const auto& bound : patternBoundNames(*p.pattern)) scope.insert(bound);
                }
            }
        }
        if (!ownName.empty()) scope.insert(ownName);
        for (const auto& name : getScopeDeclarations(body)) scope.insert(name);
        for (const auto& name : getHoistedVarDeclarations(body)) scope.insert(name);
        scopes_.push_back(std::move(scope));
        if (params) visitParamExprs(*params, *this);
        for (const auto& s : body) {
            if (s) s->accept(*this);
        }
        scopes_.pop_back();
    }

    void visit(const FunctionExpr& f) override {
        // `function fact() { … fact … }` sees itself (15.2.5). A method's node
        // carries its property key as a name and binds nothing; the kind is
        // what tells the two apart, exactly as lowering's `bindsOwnName` site
        // does.
        const bool ownName = f.kind == FunctionKind::Normal && !f.isArrow;
        walkFunction(f.body, &f.params, ownName ? f.name : std::string{});
    }
    void visit(const FunctionDecl& f) override {
        mention(f.name);
        walkFunction(f.body, &f.params);
    }
    // A declaration's body resolves the class name to the DECLARATION's
    // binding (lowering opens no inner record for it), so the name stays a
    // mention of the enclosing scope; an expression's name lives only in the
    // record the class opens for it, and binds over its body here.
    void visit(const ClassDecl& c) override {
        mention(c.name);
        if (c.superClass) c.superClass->accept(*this);
        else if (!c.superName.empty()) mention(c.superName);
        walkClassBody(std::string{}, c.methods);
    }
    void visit(const ClassExpr& c) override {
        if (c.superClass) c.superClass->accept(*this);
        else if (!c.superName.empty()) mention(c.superName);
        walkClassBody(c.name, c.methods);
    }
    void visit(const BlockStmt& b) override { walkList(b.stmts); }
    void visit(const IfStmt& i) override {
        i.condition->accept(*this);
        walkList(i.thenBody);
        walkList(i.elseBody);
    }
    void visit(const WhileStmt& w) override {
        w.condition->accept(*this);
        walkList(w.body);
    }
    void visit(const DoWhileStmt& d) override {
        walkList(d.body);
        d.condition->accept(*this);
    }
    // The head's declarations cover the condition, the update and the body.
    void visit(const ForStmt& f) override {
        scopes_.push_back(declaredIn(f.init));
        for (const auto& s : f.init) s->accept(*this);
        if (f.condition) f.condition->accept(*this);
        if (f.update) f.update->accept(*this);
        walkList(f.body);
        scopes_.pop_back();
    }
    // The subject is evaluated OUTSIDE the head's scope (14.7.5.6 uses the
    // TDZ environment only for the expression's own lookups of the head
    // names, which bronze reports as a TDZ error either way); a declared head
    // then binds over the body, and an assigning head is a mention.
    void visit(const ForInStmt& n) override {
        if (n.object) n.object->accept(*this);
        walkIterationHead(n.name, n.pattern.get(), n.isConst || n.isLet || n.isVar, n.body);
    }
    void visit(const ForOfStmt& n) override {
        if (n.iterable) n.iterable->accept(*this);
        walkIterationHead(n.name, n.pattern.get(), n.isConst || n.isLet || n.isVar, n.body);
    }
    // The switch body is one block (14.12.2), so every case shares one scope.
    void visit(const SwitchStmt& n) override {
        if (n.discriminant) n.discriminant->accept(*this);
        std::unordered_set<std::string> scope;
        for (const auto& c : n.cases) {
            for (const auto& name : getScopeDeclarations(c.body)) scope.insert(name);
        }
        scopes_.push_back(std::move(scope));
        for (const auto& c : n.cases) {
            if (c.test) c.test->accept(*this);
            for (const auto& s : c.body) s->accept(*this);
        }
        scopes_.pop_back();
    }
    void visit(const TryStmt& n) override {
        walkList(n.body);
        std::unordered_set<std::string> scope = declaredIn(n.catchBody);
        if (n.hasCatchParam) {
            if (n.catchPattern) {
                for (const auto& bound : patternBoundNames(*n.catchPattern)) scope.insert(bound);
            } else {
                scope.insert(n.catchName);
            }
        }
        scopes_.push_back(std::move(scope));
        if (n.catchPattern) visitPatternExprs(n.catchPattern.get(), *this);
        for (const auto& s : n.catchBody) s->accept(*this);
        scopes_.pop_back();
        walkList(n.finallyBody);
    }

private:
    static std::unordered_set<std::string> declaredIn(const std::vector<StmtPtr>& stmts) {
        std::unordered_set<std::string> scope;
        for (const auto& name : getScopeDeclarations(stmts)) scope.insert(name);
        return scope;
    }
    void walkList(const std::vector<StmtPtr>& stmts) {
        scopes_.push_back(declaredIn(stmts));
        for (const auto& s : stmts) {
            if (s) s->accept(*this);
        }
        scopes_.pop_back();
    }
    void walkIterationHead(const std::string& name, const BindingPattern* pattern, bool declares,
                           const std::vector<StmtPtr>& body) {
        std::unordered_set<std::string> scope;
        if (declares) {
            if (!name.empty()) scope.insert(name);
            if (pattern) {
                for (const auto& bound : patternBoundNames(*pattern)) scope.insert(bound);
            }
        } else {
            if (!name.empty()) mention(name);
            if (pattern) {
                for (const auto& bound : patternBoundNames(*pattern)) mention(bound);
            }
        }
        for (const auto& d : declaredIn(body)) scope.insert(d);
        scopes_.push_back(std::move(scope));
        if (pattern) visitPatternExprs(pattern, *this);
        for (const auto& s : body) {
            if (s) s->accept(*this);
        }
        scopes_.pop_back();
    }
    // A class's own name is bound inside its body; a computed member name is
    // the enclosing scope's expression and is walked before that binding.
    void walkClassBody(const std::string& name, const std::vector<ClassMethod>& methods) {
        for (const auto& m : methods) {
            if (m.keyExpr) m.keyExpr->accept(*this);
        }
        std::unordered_set<std::string> scope;
        if (!name.empty()) scope.insert(name);
        scopes_.push_back(std::move(scope));
        for (const auto& m : methods) {
            if (m.fn) m.fn->accept(*this);
            if (m.init) m.init->accept(*this);
        }
        scopes_.pop_back();
    }

    std::vector<std::unordered_set<std::string>> scopes_;
};

inline void CaptureVisitor::addFunctionBody(const std::vector<StmtPtr>& body,
                                           const std::vector<Param>* params) {
    FreeNameVisitor free;
    free.walkFunction(body, params);
    captured.insert(free.names.begin(), free.names.end());
}

// What one statement contributes to its scope's LEXICAL declarations. A
// `function` declaration is not one of them, which is the whole difference
// between this and `getScopeDeclarations`.
inline void appendConstNames(const Stmt& s, std::vector<std::string>& out) {
    if (const auto* v = dynamic_cast<const VarDecl*>(&s)) {
        if (v->isConst) appendDeclaredNames(*v, out);
    }
}

inline void appendLexicalNames(const Stmt& s, std::vector<std::string>& out) {
    if (const auto* v = dynamic_cast<const VarDecl*>(&s)) {
        if (!v->isVar) appendDeclaredNames(*v, out);
    } else if (const auto* c = dynamic_cast<const ClassDecl*>(&s)) {
        out.push_back(c->name);
    }
}

}  // namespace bronze::ast::detail
