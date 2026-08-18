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

    void visit(const NumberLit&) override {}
    void visit(const BigIntLit&) override {}
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
    void visit(const NewTargetExpr&) override {}
    void visit(const ImportMetaExpr&) override {}
    void visit(const TaggedTemplate& t) override {
        t.tag->accept(*this);
        for (const auto& e : t.templateLit->exprs) e->accept(*this);
    }
    void visit(const SuperCall& c) override {
        names.insert(c.baseName);
        for (const auto& arg : c.args) arg->accept(*this);
    }
    void visit(const SuperMember& m) override { names.insert(m.baseName); }
    void visit(const SpreadElement& s) override { s.argument->accept(*this); }
    void visit(const YieldExpr& y) override { y.argument->accept(*this); }
    void visit(const DynamicImportExpr& d) override { if (d.specifier) d.specifier->accept(*this); }
    void visit(const DestructuringAssign& d) override {
        for (const auto& n : patternBoundNames(*d.pattern)) names.insert(n);
        visitPatternExprs(d.pattern.get(), *this);
        d.value->accept(*this);
    }
    void visit(const ClassDecl& c) override {
        names.insert(c.name);
        if (!c.superName.empty()) names.insert(c.superName);
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
        if (!c.superName.empty()) names.insert(c.superName);
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
        for (const auto& arg : c.args) arg->accept(*this);
    }
    void visit(const SuperMember&) override {}
    void visit(const SpreadElement& s) override { s.argument->accept(*this); }
    void visit(const DestructuringAssign& d) override {
        visitPatternExprs(d.pattern.get(), *this);
        d.value->accept(*this);
    }
    // Every method of a class is a closure over this scope, so what its body
    // mentions is a candidate capture - including the parent class name that a
    // `super` inside it resolves against.
    void visit(const ClassDecl& c) override {
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

// Collects all identifier names declared anywhere within a scope (stopping at nested functions).
class DeclaredNamesVisitor final : public CaptureVisitor {
public:
    std::unordered_set<std::string> names;

    void visit(const VarDecl& v) override {
        std::vector<std::string> declared;
        appendDeclaredNames(v, declared);
        names.insert(declared.begin(), declared.end());
    }
    void visit(const FunctionDecl& f) override { names.insert(f.name); }
    void visit(const ClassDecl& c) override { names.insert(c.name); }

    void visit(const FunctionExpr&) override {}
    void visit(const ClassExpr&) override {}

    void visit(const TryStmt& t) override {
        for (const auto& s : t.body) if (s) s->accept(*this);
        if (t.catchPattern) {
            for (const auto& b : patternBoundNames(*t.catchPattern)) names.insert(b);
        }
        for (const auto& s : t.catchBody) if (s) s->accept(*this);
        for (const auto& s : t.finallyBody) if (s) s->accept(*this);
    }
};

inline void CaptureVisitor::addFunctionBody(const std::vector<StmtPtr>& body,
                                           const std::vector<Param>* params) {
    IdentVisitor idents;
    if (params) visitParamExprs(*params, idents);
    for (const auto& s : body) {
        if (s) s->accept(idents);
    }
    if (params) {
        for (const auto& p : *params) {
            if (!p.name.empty()) idents.names.erase(p.name);
            if (p.pattern) {
                for (const auto& bound : patternBoundNames(*p.pattern)) {
                    idents.names.erase(bound);
                }
            }
        }
    }
    DeclaredNamesVisitor decls;
    for (const auto& s : body) {
        if (s) s->accept(decls);
    }
    for (const auto& d : decls.names) {
        idents.names.erase(d);
    }
    captured.insert(idents.names.begin(), idents.names.end());
}

// What one statement contributes to its scope's LEXICAL declarations. A
// `function` declaration is not one of them, which is the whole difference
// between this and `getScopeDeclarations`.
inline void appendLexicalNames(const Stmt& s, std::vector<std::string>& out) {
    if (const auto* v = dynamic_cast<const VarDecl*>(&s)) {
        if (!v->isVar) appendDeclaredNames(*v, out);
    } else if (const auto* c = dynamic_cast<const ClassDecl*>(&s)) {
        out.push_back(c->name);
    }
}

}  // namespace bronze::ast::detail
