// EvalOptions::classicScript: a classic script's top-level `var`s and function
// declarations are the global object's properties (ECMA-262 16.1.7
// GlobalDeclarationInstantiation), not bindings of the unit the way a module's
// are. Done to the tree before inference and lowering see it, so both read
// ordinary code: every reference to such a name is a bare global reference
// (the realm's global read/write cache answers it), and nothing downstream
// learns a new kind of binding.
//
// Per script of a joined unit, at the script's start (after its marker):
//   - each function declaration, defined on the global object as
//     `({ f: function (...) {...} }).f`: a property definition names the
//     function `f` (NamedEvaluation) without binding `f` inside it, so the
//     body's `f` is the global, as a declaration's is, and a script that
//     reassigns it is seen;
//   - each `var` name the global object does not already have, defined as
//     undefined, writable, enumerable and not configurable (16.1.7 step 17's
//     CreateGlobalVarBinding with D false); functions first, so a var of a
//     function's name finds it.
// Each `var` declarator becomes the assignment of its initializer, or nothing.
// `let`, `const` and `class` are untouched: the unit's own bindings.

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eval/eval_internal.h"

namespace bronze::eval::detail {

namespace {

template <typename T>
std::unique_ptr<T> node(Span span) {
    auto n = std::make_unique<T>();
    n->span = span;
    return n;
}

ast::ExprPtr ident(const std::string& name, Span span) {
    auto id = node<ast::Ident>(span);
    id->name = name;
    return id;
}

ast::ExprPtr member(ast::ExprPtr object, const std::string& prop, Span span) {
    auto m = node<ast::MemberAccess>(span);
    m->object = std::move(object);
    m->property = prop;
    return m;
}

ast::ExprPtr str(const std::string& s, Span span) {
    auto lit = node<ast::StringLit>(span);
    lit->value = s;
    return lit;
}

ast::ExprPtr call(ast::ExprPtr callee, std::vector<ast::ExprPtr> args, Span span) {
    auto c = node<ast::Call>(span);
    c->callee = std::move(callee);
    c->args = std::move(args);
    return c;
}

ast::ExprPtr binary(ast::BinaryOp op, ast::ExprPtr lhs, ast::ExprPtr rhs, Span span) {
    auto b = node<ast::Binary>(span);
    b->op = op;
    b->lhs = std::move(lhs);
    b->rhs = std::move(rhs);
    return b;
}

ast::StmtPtr exprStmt(ast::ExprPtr e, Span span) {
    auto s = node<ast::ExprStmt>(span);
    s->expr = std::move(e);
    return s;
}

ast::ExprPtr objectWith(std::vector<std::pair<std::string, ast::ExprPtr>> props, Span span) {
    auto obj = node<ast::ObjectLit>(span);
    for (auto& [key, value] : props) {
        ast::ObjectProp p;
        p.key = key;
        p.value = std::move(value);
        obj->props.push_back(std::move(p));
    }
    return obj;
}

// `Object.hasOwn(globalThis, "x") || Object.defineProperty(globalThis, "x",
// { value: undefined, writable: true, enumerable: true })`
ast::StmtPtr declareGlobalVar(const std::string& name, Span span) {
    std::vector<ast::ExprPtr> hasArgs;
    hasArgs.push_back(ident("globalThis", span));
    hasArgs.push_back(str(name, span));
    auto has = call(member(ident("Object", span), "hasOwn", span), std::move(hasArgs), span);
    std::vector<std::pair<std::string, ast::ExprPtr>> desc;
    desc.emplace_back("value", node<ast::UndefinedLit>(span));
    {
        auto t = node<ast::BoolLit>(span);
        t->value = true;
        desc.emplace_back("writable", std::move(t));
    }
    {
        auto t = node<ast::BoolLit>(span);
        t->value = true;
        desc.emplace_back("enumerable", std::move(t));
    }
    std::vector<ast::ExprPtr> defArgs;
    defArgs.push_back(ident("globalThis", span));
    defArgs.push_back(str(name, span));
    defArgs.push_back(objectWith(std::move(desc), span));
    auto define = call(member(ident("Object", span), "defineProperty", span), std::move(defArgs), span);
    return exprStmt(binary(ast::BinaryOp::LogicalOr, std::move(has), std::move(define), span), span);
}

// `Object.defineProperty(globalThis, "f", { value: ({ f: function (...) {...} }).f,
// writable: true, enumerable: true })`, taking the declaration's parts: 16.1.7's
// CreateGlobalFunctionBinding, which defines it non-configurable, or throws
// the TypeError CanDeclareGlobalFunction would where the global object holds
// a non-configurable property that is not a writable, enumerable one.
ast::StmtPtr defineGlobalFunction(ast::FunctionDecl& fd) {
    const Span span = fd.span;
    auto fn = node<ast::FunctionExpr>(span);
    fn->params = std::move(fd.params);
    fn->returnType = std::move(fd.returnType);
    fn->body = std::move(fd.body);
    fn->strict = fd.strict;
    fn->isGenerator = fd.isGenerator;
    fn->isAsync = fd.isAsync;
    fn->kind = ast::FunctionKind::Normal;
    std::vector<std::pair<std::string, ast::ExprPtr>> props;
    props.emplace_back(fd.name, std::move(fn));
    auto named = member(objectWith(std::move(props), span), fd.name, span);
    std::vector<std::pair<std::string, ast::ExprPtr>> desc;
    desc.emplace_back("value", std::move(named));
    for (const char* flag : {"writable", "enumerable"}) {
        auto t = node<ast::BoolLit>(span);
        t->value = true;
        desc.emplace_back(flag, std::move(t));
    }
    std::vector<ast::ExprPtr> args;
    args.push_back(ident("globalThis", span));
    args.push_back(str(fd.name, span));
    args.push_back(objectWith(std::move(desc), span));
    return exprStmt(call(member(ident("Object", span), "defineProperty", span), std::move(args), span),
                    span);
}

class VarRewriter {
public:
    std::vector<std::string> names;  // in source order, each once

    void list(std::vector<ast::StmtPtr>& stmts) {
        for (auto& s : stmts) stmt(s);
    }

    void stmt(ast::StmtPtr& s) {
        if (!s) return;
        if (auto* vd = dynamic_cast<ast::VarDecl*>(s.get())) {
            if (vd->isVar) s = declarator(*vd);
            return;
        }
        if (auto* b = dynamic_cast<ast::BlockStmt*>(s.get())) return list(b->stmts);
        if (auto* i = dynamic_cast<ast::IfStmt*>(s.get())) {
            list(i->thenBody);
            return list(i->elseBody);
        }
        if (auto* w = dynamic_cast<ast::WhileStmt*>(s.get())) return list(w->body);
        if (auto* d = dynamic_cast<ast::DoWhileStmt*>(s.get())) return list(d->body);
        if (auto* f = dynamic_cast<ast::ForStmt*>(s.get())) {
            forInit(*f);
            return list(f->body);
        }
        if (auto* f = dynamic_cast<ast::ForInStmt*>(s.get())) {
            if (f->isVar) {
                f->isVar = false;
                note(f->name, f->pattern.get());
            }
            return list(f->body);
        }
        if (auto* f = dynamic_cast<ast::ForOfStmt*>(s.get())) {
            if (f->isVar) {
                f->isVar = false;
                note(f->name, f->pattern.get());
            }
            return list(f->body);
        }
        if (auto* sw = dynamic_cast<ast::SwitchStmt*>(s.get())) {
            for (auto& c : sw->cases) list(c.body);
            return;
        }
        if (auto* l = dynamic_cast<ast::LabeledStmt*>(s.get())) return stmt(l->body);
        if (auto* t = dynamic_cast<ast::TryStmt*>(s.get())) {
            list(t->body);
            list(t->catchBody);
            return list(t->finallyBody);
        }
    }

private:
    std::unordered_set<std::string> seen_;

    void note(const std::string& name, const ast::BindingPattern* pattern) {
        if (pattern) {
            for (auto& n : ast::patternBoundNames(*pattern)) add(n);
        } else {
            add(name);
        }
    }
    void add(const std::string& n) {
        if (!n.empty() && seen_.insert(n).second) names.push_back(n);
    }

    // The assignment a `var` declarator is once its name is the global's;
    // an empty block for one with no initializer.
    ast::ExprPtr assignment(ast::VarDecl& vd) {
        note(vd.name, vd.pattern.get());
        if (!vd.init) return nullptr;
        if (vd.pattern) {
            auto d = node<ast::DestructuringAssign>(vd.span);
            d->pattern = std::move(vd.pattern);
            d->value = std::move(vd.init);
            return d;
        }
        return binary(ast::BinaryOp::Assign, ident(vd.name, vd.span), std::move(vd.init), vd.span);
    }

    ast::StmtPtr declarator(ast::VarDecl& vd) {
        const Span span = vd.span;
        if (auto e = assignment(vd)) return exprStmt(std::move(e), span);
        return node<ast::BlockStmt>(span);
    }

    // `for (var i = 0, j = n; ...)`: the header's declarators become one
    // comma expression, the single ExprStmt a ForStmt's init may be.
    void forInit(ast::ForStmt& f) {
        bool anyVar = false;
        for (auto& s : f.init) {
            if (auto* vd = dynamic_cast<ast::VarDecl*>(s.get()); vd && vd->isVar) anyVar = true;
        }
        if (!anyVar) return;
        ast::ExprPtr combined;
        Span span{};
        for (auto& s : f.init) {
            auto* vd = dynamic_cast<ast::VarDecl*>(s.get());
            if (!vd) continue;
            span = vd->span;
            auto e = assignment(*vd);
            if (!e) continue;
            combined = combined ? binary(ast::BinaryOp::Comma, std::move(combined), std::move(e), span)
                                : std::move(e);
        }
        f.init.clear();
        if (combined) f.init.push_back(exprStmt(std::move(combined), span));
    }
};

bool isMarker(const ast::Stmt* s, const std::string& marker) {
    if (marker.empty()) return false;
    const auto* es = dynamic_cast<const ast::ExprStmt*>(s);
    if (!es) return false;
    const auto* b = dynamic_cast<const ast::Binary*>(es->expr.get());
    if (!b || b->op != ast::BinaryOp::Assign) return false;
    const auto* m = dynamic_cast<const ast::MemberAccess*>(b->lhs.get());
    if (!m || m->property != marker) return false;
    const auto* g = dynamic_cast<const ast::Ident*>(m->object.get());
    return g && g->name == "globalThis";
}

// One script: its statements, its declarations ahead of them.
void instantiateScript(std::vector<ast::StmtPtr>& script, std::vector<ast::StmtPtr>& out,
                       Span at) {
    std::vector<ast::StmtPtr> functions;
    VarRewriter vars;
    std::vector<ast::StmtPtr> body;
    for (auto& s : script) {
        if (auto* fd = dynamic_cast<ast::FunctionDecl*>(s.get()); fd && !fd->name.empty()) {
            // A later declaration of one name replaces an earlier one
            // (16.1.7 step 10 keeps the last), and defining both in order
            // leaves the last.
            functions.push_back(defineGlobalFunction(*fd));
            continue;
        }
        vars.stmt(s);
        body.push_back(std::move(s));
    }
    for (auto& f : functions) out.push_back(std::move(f));
    for (const auto& n : vars.names) out.push_back(declareGlobalVar(n, at));
    for (auto& s : body) out.push_back(std::move(s));
}

}  // namespace

void instantiateClassicScripts(ast::Module& astModule, const std::string& marker) {
    std::vector<ast::StmtPtr> out;
    std::vector<ast::StmtPtr> script;
    Span at = astModule.span;
    for (auto& s : astModule.body) {
        if (isMarker(s.get(), marker)) {
            instantiateScript(script, out, at);
            script.clear();
            at = s->span;
            out.push_back(std::move(s));
            continue;
        }
        script.push_back(std::move(s));
    }
    instantiateScript(script, out, at);
    astModule.body = std::move(out);
}

}  // namespace bronze::eval::detail
