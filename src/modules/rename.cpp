// The scope-aware renaming walk.
//
// A reference is renamed exactly when it resolves to its file's MODULE scope,
// so the walk carries a stack of shadowing sets and consults it before every
// rewrite. Getting that wrong in the direction of shadowing too little is a
// reference bound to the wrong variable, which produces no diagnostic at all
// — so the two things that could make it happen are removed rather than
// guarded: the shadow set for each scope kind is computed by `ast::queries`,
// the same code lowering resolves names with, and a node kind this walk does
// not recognise is an internal error rather than a subtree left alone.
//
// It is also where every span gets its file id stamped. Most spans arrive
// with one from the lexer, but the parser builds a few piecewise from
// `span.begin = ...` on a default-constructed Span, and one of those rendered
// against the wrong file is a diagnostic pointing at unrelated code.

#include <set>
#include <string>
#include <typeinfo>
#include <vector>

#include "ast/queries.h"
#include "modules/graph.h"

namespace bronze::modules {

namespace {

class Renamer {
public:
    Renamer(const std::map<std::string, std::string>& renames, uint16_t fileId,
            const std::map<std::string, std::string>& importedBindings, DiagnosticSink& diags)
        : renames_(renames), importedBindings_(importedBindings), diags_(diags), fileId_(fileId) {}

    bool run(std::vector<ast::StmtPtr>& stmts) {
        // The module's own top level pushes NO scope: its declarations are
        // exactly what is being renamed.
        stmtList(stmts);
        return ok_;
    }

private:
    bool shadowed(const std::string& name) const {
        for (const auto& scope : shadow_) {
            if (scope.count(name)) return true;
        }
        return false;
    }

    void rewrite(std::string& name) {
        if (shadowed(name)) return;
        auto it = renames_.find(name);
        if (it != renames_.end()) name = it->second;
    }

    void fail(const std::type_info& kind) {
        if (!ok_) return;
        ok_ = false;
        diags_.error(Span{},
                     std::string("internal error: the module linker cannot rename inside a ") +
                         kind.name() + " node");
    }

    // ---- scopes ----------------------------------------------------------

    void stmtList(std::vector<ast::StmtPtr>& list) {
        for (auto& s : list) {
            if (s) stmt(*s);
        }
    }

    // A statement list that IS a scope: a block, a loop body, an arm of an
    // `if`. Its own lexical declarations shadow the module's.
    void blockList(std::vector<ast::StmtPtr>& list) {
        std::set<std::string> scope;
        for (const auto& name : ast::getScopeDeclarations(list)) scope.insert(name);
        shadow_.push_back(std::move(scope));
        stmtList(list);
        shadow_.pop_back();
    }

    // A function's scope: its parameters, its own lexical declarations, and
    // every `var` hoisted to it from anywhere below. `ownName` is a named
    // function expression's own binding, which is visible inside its body and
    // nowhere else.
    void functionBody(std::vector<ast::Param>& params, std::vector<ast::StmtPtr>& body,
                      const std::string& ownName) {
        std::set<std::string> scope;
        if (!ownName.empty()) scope.insert(ownName);
        for (const auto& p : params) {
            if (p.pattern) {
                for (const auto& n : ast::patternBoundNames(*p.pattern)) scope.insert(n);
            } else {
                scope.insert(p.name);
            }
        }
        for (const auto& n : ast::getScopeDeclarations(body)) scope.insert(n);
        for (const auto& n : ast::getHoistedVarDeclarations(body)) scope.insert(n);
        shadow_.push_back(std::move(scope));
        for (auto& p : params) {
            if (p.defaultValue) expr(*p.defaultValue);
            if (p.pattern) pattern(*p.pattern);
            p.span.file = fileId_;
        }
        stmtList(body);
        shadow_.pop_back();
    }

    // A pattern's targets are renamed like any other name — shadowing decides,
    // and it decides correctly for both uses: in a DECLARATION the enclosing
    // scope has already pushed the names it binds, and in a destructuring
    // ASSIGNMENT it has not, because they are references to bindings that
    // already exist.
    void pattern(ast::BindingPattern& p) {
        p.span.file = fileId_;
        for (auto& elem : p.elements) {
            elem.span.file = fileId_;
            if (!elem.name.empty()) rewrite(elem.name);
            if (elem.keyExpr) expr(*elem.keyExpr);
            if (elem.defaultValue) expr(*elem.defaultValue);
            if (elem.pattern) pattern(*elem.pattern);
        }
    }

    // ---- statements -------------------------------------------------------

    void stmt(ast::Stmt& s) {
        if (!ok_) return;
        s.span.file = fileId_;

        if (auto* blk = dynamic_cast<ast::BlockStmt*>(&s)) {
            blockList(blk->stmts);
        } else if (auto* vd = dynamic_cast<ast::VarDecl*>(&s)) {
            if (vd->pattern) {
                pattern(*vd->pattern);
            } else {
                rewrite(vd->name);
            }
            if (vd->init) expr(*vd->init);
        } else if (auto* ret = dynamic_cast<ast::ReturnStmt*>(&s)) {
            if (ret->value) expr(*ret->value);
        } else if (auto* es = dynamic_cast<ast::ExprStmt*>(&s)) {
            expr(*es->expr);
        } else if (auto* ifs = dynamic_cast<ast::IfStmt*>(&s)) {
            expr(*ifs->condition);
            blockList(ifs->thenBody);
            blockList(ifs->elseBody);
        } else if (auto* wh = dynamic_cast<ast::WhileStmt*>(&s)) {
            expr(*wh->condition);
            blockList(wh->body);
        } else if (auto* dw = dynamic_cast<ast::DoWhileStmt*>(&s)) {
            blockList(dw->body);
            expr(*dw->condition);
        } else if (auto* fs = dynamic_cast<ast::ForStmt*>(&s)) {
            // The header's bindings belong to the loop and are visible to the
            // condition, the update and the body — which is exactly why
 // `ForStmt::init` is a list and not a block.
            std::set<std::string> scope;
            for (const auto& name : ast::getScopeDeclarations(fs->init)) scope.insert(name);
            shadow_.push_back(std::move(scope));
            stmtList(fs->init);
            if (fs->condition) expr(*fs->condition);
            if (fs->update) expr(*fs->update);
            blockList(fs->body);
            shadow_.pop_back();
        } else if (auto* fi = dynamic_cast<ast::ForInStmt*>(&s)) {
            expr(*fi->object);
            iterationHead(fi->name, fi->pattern.get(), fi->isConst || fi->isLet || fi->isVar, fi->body);
        } else if (auto* fo = dynamic_cast<ast::ForOfStmt*>(&s)) {
            expr(*fo->iterable);
            iterationHead(fo->name, fo->pattern.get(), fo->isConst || fo->isLet || fo->isVar, fo->body);
        } else if (auto* sw = dynamic_cast<ast::SwitchStmt*>(&s)) {
            expr(*sw->discriminant);
            // The whole switch body is one block scope (ECMA-262 14.12), so a
            // `let` in one clause shadows for every clause.
            std::set<std::string> scope;
            for (const auto& c : sw->cases) {
                for (const auto& name : ast::getScopeDeclarations(c.body)) scope.insert(name);
            }
            shadow_.push_back(std::move(scope));
            for (auto& c : sw->cases) {
                c.span.file = fileId_;
                if (c.test) expr(*c.test);
                stmtList(c.body);
            }
            shadow_.pop_back();
        } else if (auto* lab = dynamic_cast<ast::LabeledStmt*>(&s)) {
            // A label binds nothing; it names a jump target (ECMA-262 14.13).
            if (lab->body) stmt(*lab->body);
        } else if (auto* tr = dynamic_cast<ast::TryStmt*>(&s)) {
            blockList(tr->body);
            std::set<std::string> scope;
            if (tr->hasCatchParam) {
                if (tr->catchPattern) {
                    for (const auto& b : ast::patternBoundNames(*tr->catchPattern)) scope.insert(b);
                } else {
                    scope.insert(tr->catchName);
                }
            }
            for (const auto& name : ast::getScopeDeclarations(tr->catchBody)) scope.insert(name);
            shadow_.push_back(std::move(scope));
            if (tr->catchPattern) pattern(*tr->catchPattern);
            stmtList(tr->catchBody);
            shadow_.pop_back();
            blockList(tr->finallyBody);
        } else if (auto* th = dynamic_cast<ast::ThrowStmt*>(&s)) {
            expr(*th->value);
        } else if (auto* cd = dynamic_cast<ast::ClassDecl*>(&s)) {
            const std::string oldName = cd->name;
            rewrite(cd->name);
            rewrite(cd->superName);
            // The IL symbol of every method was built by the parser as
            // `<class>.<member>`, so it has to move with the class name or two
            // files' `Point.dot` would be one symbol. Rewriting the prefix
            // rather than rebuilding the name keeps the accessor spelling
            // (`Foo.get x`) the parser chose.
            if (cd->name != oldName) {
                const std::string oldPrefix = oldName + ".";
                for (auto& m : cd->methods) {
                    if (m.fn && m.fn->name.rfind(oldPrefix, 0) == 0) {
                        m.fn->name = cd->name + "." + m.fn->name.substr(oldPrefix.size());
                    }
                }
            }
            for (auto& m : cd->methods) {
                // The computed member name is an expression of this file's
                // module scope, so the names in it move with everything else.
                if (m.keyExpr) expr(*m.keyExpr);
                if (m.init) expr(*m.init);
                if (!m.fn) continue;
                m.fn->span.file = fileId_;
                functionBody(m.fn->params, m.fn->body, std::string());
            }
        } else if (auto* fd = dynamic_cast<ast::FunctionDecl*>(&s)) {
            rewrite(fd->name);
            functionBody(fd->params, fd->body, std::string());
        } else if (dynamic_cast<ast::BreakStmt*>(&s) || dynamic_cast<ast::ContinueStmt*>(&s)) {
            // A label is a jump target, not a binding (ECMA-262 14.13), and
            // neither statement has any other child.
        } else if (dynamic_cast<ast::ImportDecl*>(&s) ||
                   dynamic_cast<ast::ExportNamesDecl*>(&s)) {
            // Erased by the merge; they name no binding that survives.
        } else {
            fail(typeid(s));
        }
    }

    // `for (const x of it)` and `for (x of it)` differ in one way that
    // matters here: with a declaration keyword the head BINDS `x`, and
    // without one it ASSIGNS to a binding that already exists — which is a
    // reference, and must be renamed.
    void iterationHead(std::string& name, ast::BindingPattern* pat, bool declares,
                       std::vector<ast::StmtPtr>& body) {
        std::set<std::string> scope;
        if (declares) {
            if (pat) {
                for (const auto& b : ast::patternBoundNames(*pat)) scope.insert(b);
            } else {
                scope.insert(name);
            }
        } else {
            // No declaration keyword: the head ASSIGNS on every iteration, so
            // an import binding in it is refused exactly as `x = ...` is.
            if (pat) refuseImportedWrites(*pat, pat->span);
            if (!name.empty()) {
                refuseImportedWrite(name, Span{});
                rewrite(name);
            }
        }
        shadow_.push_back(std::move(scope));
        if (pat) pattern(*pat);
        blockList(body);
        shadow_.pop_back();
    }

    // ---- expressions ------------------------------------------------------

    void expr(ast::Expr& e) {
        if (!ok_) return;
        e.span.file = fileId_;

        if (auto* id = dynamic_cast<ast::Ident*>(&e)) {
            rewrite(id->name);
        } else if (dynamic_cast<ast::NumberLit*>(&e) || dynamic_cast<ast::StringLit*>(&e) ||
                   dynamic_cast<ast::BoolLit*>(&e) || dynamic_cast<ast::NullLit*>(&e) ||
                   dynamic_cast<ast::UndefinedLit*>(&e) || dynamic_cast<ast::ThisExpr*>(&e) ||
                   dynamic_cast<ast::RegExpLit*>(&e)) {
            // Nothing below them names anything.
        } else if (auto* tpl = dynamic_cast<ast::TemplateLit*>(&e)) {
            for (auto& sub : tpl->exprs) expr(*sub);
        } else if (auto* un = dynamic_cast<ast::Unary*>(&e)) {
            // `x++` writes `x` exactly as `x = x + 1` does (13.4.2.1 step 5
            // PutValue), so an import binding is refused under both spellings.
            if (un->op == ast::UnaryOp::PreInc || un->op == ast::UnaryOp::PreDec ||
                un->op == ast::UnaryOp::PostInc || un->op == ast::UnaryOp::PostDec) {
                if (const auto* target = dynamic_cast<const ast::Ident*>(un->operand.get())) {
                    refuseImportedWrite(target->name, un->span);
                }
            }
            expr(*un->operand);
        } else if (auto* bin = dynamic_cast<ast::Binary*>(&e)) {
            if (ast::isAssignOp(bin->op)) {
                if (const auto* target = dynamic_cast<const ast::Ident*>(bin->lhs.get())) {
                    refuseImportedWrite(target->name, bin->span);
                }
            }
            expr(*bin->lhs);
            expr(*bin->rhs);
        } else if (auto* ter = dynamic_cast<ast::Ternary*>(&e)) {
            expr(*ter->condition);
            expr(*ter->thenExpr);
            expr(*ter->elseExpr);
        } else if (auto* mem = dynamic_cast<ast::MemberAccess*>(&e)) {
            expr(*mem->object);  // `property` is a key, never a binding
        } else if (auto* idx = dynamic_cast<ast::IndexAccess*>(&e)) {
            expr(*idx->object);
            expr(*idx->index);
        } else if (auto* call = dynamic_cast<ast::Call*>(&e)) {
            expr(*call->callee);
            for (auto& arg : call->args) expr(*arg);
        } else if (auto* nw = dynamic_cast<ast::NewExpr*>(&e)) {
            // The callee is an expression, so it recurses like any other one.
            // A bare name still lands on the `Ident` branch above and is
            // renamed there; what recursion adds is the base of
            // `new imported.Ctor()`, which `rewrite` on a string could not
            // have reached and which would otherwise have bound to whatever
            // the importing file happened to call `imported`.
            expr(*nw->callee);
            for (auto& arg : nw->args) expr(*arg);
        } else if (auto* sc = dynamic_cast<ast::SuperCall*>(&e)) {
            rewrite(sc->baseName);
            for (auto& arg : sc->args) expr(*arg);
        } else if (auto* sm = dynamic_cast<ast::SuperMember*>(&e)) {
            rewrite(sm->baseName);
        } else if (auto* spr = dynamic_cast<ast::SpreadElement*>(&e)) {
            expr(*spr->argument);
        } else if (auto* y = dynamic_cast<ast::YieldExpr*>(&e)) {
            // The operand is ordinary code of this module; the value the node
            // produces comes from a caller and names nothing.
            expr(*y->argument);
        } else if (auto* di = dynamic_cast<ast::DynamicImportExpr*>(&e)) {
            if (di->specifier) expr(*di->specifier);
        } else if (auto* da = dynamic_cast<ast::DestructuringAssign*>(&e)) {
            // Every element of this pattern is an assignment TARGET — it
            // declares nothing — so each name it writes is refused like the
            // left side of an `=`.
            refuseImportedWrites(*da->pattern, da->span);
            pattern(*da->pattern);
            expr(*da->value);
        } else if (auto* obj = dynamic_cast<ast::ObjectLit*>(&e)) {
            for (auto& prop : obj->props) {
                if (prop.keyExpr) expr(*prop.keyExpr);
                if (prop.value) expr(*prop.value);
            }
        } else if (auto* arr = dynamic_cast<ast::ArrayLit*>(&e)) {
            for (auto& elem : arr->elements) {
                if (elem) expr(*elem);
            }
        } else if (auto* fe = dynamic_cast<ast::FunctionExpr*>(&e)) {
            // A named function expression's name is a binding visible only
            // inside its own body. The synthetic names the parser gives class
            // and object methods land here too and are harmless: they contain
            // a `.` or a space, so no reference can ever spell one.
            functionBody(fe->params, fe->body, fe->isArrow ? std::string() : fe->name);
        } else if (auto* ce = dynamic_cast<ast::ClassExpr*>(&e)) {
            rewrite(ce->superName);
            for (auto& m : ce->methods) {
                if (m.keyExpr) expr(*m.keyExpr);
                if (m.init) expr(*m.init);
                if (!m.fn) continue;
                m.fn->span.file = fileId_;
                functionBody(m.fn->params, m.fn->body, std::string());
            }
        } else if (auto* tt = dynamic_cast<ast::TaggedTemplate*>(&e)) {
            expr(*tt->tag);
            for (auto& exp : tt->templateLit->exprs) expr(*exp);
        } else if (dynamic_cast<ast::NewTargetExpr*>(&e)) {
            // new.target carries no identifier bindings
        } else {
            fail(typeid(e));
        }
    }

    // An assignment to an IMPORT BINDING. 16.2.1.6.1 CreateImportBinding makes
    // one an immutable indirect binding — the local is a second name for
    // another module's slot — and linking has renamed it to exactly that slot.
    // So a write bronze let through would not fail: it would succeed, into the
    // exporting module's binding, which is the silent wrong answer the house
    // rules rank worst.
    //
    // What this deliberately does NOT cover is `ns.z = 5`. That is a write to a
    // PROPERTY of an ordinary object value, not to the binding `ns`; ECMA-262
    // 10.4.6.9 answers it with a [[Set]] that returns false, which strict code
    // turns into a runtime TypeError. Conflating the two refused a whole
    // correct program at compile time.
    void refuseImportedWrite(const std::string& name, Span span) {
        if (name.empty() || shadowed(name)) return;
        auto it = importedBindings_.find(name);
        if (it == importedBindings_.end()) return;
        if (!ok_) return;
        ok_ = false;
        diags_.error(span, "cannot assign to '" + name +
                               "': it is an import binding of " + it->second +
                               ", and an import is an immutable view of that module's binding "
                               "(ECMA-262 16.2.1.6.1). Assign in the module that declares it.");
    }

    // Every name a destructuring TARGET writes. `patternBoundNames` is the same
    // walk a declaration uses to find what it binds, which is what keeps the
    // two spellings of "these names are written" from drifting.
    void refuseImportedWrites(const ast::BindingPattern& pat, Span span) {
        for (const auto& name : ast::patternBoundNames(pat)) refuseImportedWrite(name, span);
    }

    const std::map<std::string, std::string>& renames_;
    const std::map<std::string, std::string>& importedBindings_;
    DiagnosticSink& diags_;
    uint16_t fileId_ = 0;
    bool ok_ = true;
    std::vector<std::set<std::string>> shadow_;
};

}  // namespace

bool renameModuleScope(std::vector<ast::StmtPtr>& stmts,
                       const std::map<std::string, std::string>& renames, uint16_t fileId,
                       const std::map<std::string, std::string>& importedBindings,
                       DiagnosticSink& diags) {
    return Renamer(renames, fileId, importedBindings, diags).run(stmts);
}

}  // namespace bronze::modules
