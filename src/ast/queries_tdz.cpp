// The temporal dead zone: which lexical bindings can be READ while they are
// still uninitialized, so that lowering has to give them a slot to hold the
// marker in and a checked read to look at it.
//
// Its own unit because it is the one query here that is about ORDER rather than
// about shape. Everything else asks what a subtree contains; this asks what has
// already run when a given statement is reached — which is a question about a
// statement LIST, and about nothing else.

#include "ast/queries.h"

#include "ast/query_walk.h"

namespace bronze::ast {

using namespace detail;

namespace {

bool initializerReads(const Stmt& s, const std::string& name) {
    // `class C extends C`, which 15.7.14 makes the same shape: the heritage
    // is evaluated at step 5 and the class binding initialized at step 17.
    if (const auto* c = dynamic_cast<const ClassDecl*>(&s)) return c->superName == name;
    const auto* v = dynamic_cast<const VarDecl*>(&s);
    if (!v || v->isVar) return false;
    IdentVisitor init;
    if (v->init) v->init->accept(init);
    visitPatternExprs(v->pattern.get(), init);
    return init.names.contains(name);
}

// The exposure scan. One statement LIST at a time, because "before" is a
// question about a list and about nothing else: a scope's activation runs its
// statements in order, and a nested list is a scope of its own whose first
// statement is a fresh beginning.
class TdzScan {
public:
    std::unordered_set<std::string> exposed;

    void list(const std::vector<StmtPtr>& stmts) {
        std::vector<const Stmt*> borrowed;
        borrowed.reserve(stmts.size());
        for (const auto& s : stmts) borrowed.push_back(s.get());
        list(borrowed);
    }

    void list(const std::vector<const Stmt*>& stmts) {
        // Every name the statements SO FAR have mentioned, nested functions
        // included — a function written above a declaration can be called
        // above it too.
        IdentVisitor seen;
        for (const auto* s : stmts) {
            if (!s) continue;
            std::vector<std::string> declared;
            appendLexicalNames(*s, declared);
            // Checked BEFORE this statement is folded into `seen`: an
            // `IdentVisitor` counts a declaration's own name as a mention, so
            // testing afterwards would report every `const` written in a
            // straight line as reading itself, and put the whole function's
            // locals in an environment record.
            for (const auto& name : declared) {
                if (seen.names.contains(name) || initializerReads(*s, name)) {
                    exposed.insert(name);
                }
            }
            s->accept(seen);
        }
        for (const auto* s : stmts) {
            if (s) nested(*s);
        }
    }

private:
    // The statement lists written INSIDE one statement, each its own scope.
    // Nested functions are deliberately not among them: a function body asks
    // this question for itself, against its own environment records.
    void nested(const Stmt& s) {
        if (const auto* b = dynamic_cast<const BlockStmt*>(&s)) {
            list(b->stmts);
        } else if (const auto* i = dynamic_cast<const IfStmt*>(&s)) {
            list(i->thenBody);
            list(i->elseBody);
        } else if (const auto* w = dynamic_cast<const WhileStmt*>(&s)) {
            list(w->body);
        } else if (const auto* d = dynamic_cast<const DoWhileStmt*>(&s)) {
            list(d->body);
        } else if (const auto* f = dynamic_cast<const ForStmt*>(&s)) {
            list(f->init);
            list(f->body);
        } else if (const auto* fi = dynamic_cast<const ForInStmt*>(&s)) {
            list(fi->body);
        } else if (const auto* fo = dynamic_cast<const ForOfStmt*>(&s)) {
            list(fo->body);
        } else if (const auto* l = dynamic_cast<const LabeledStmt*>(&s)) {
            if (l->body) nested(*l->body);
        } else if (const auto* t = dynamic_cast<const TryStmt*>(&s)) {
            list(t->body);
            list(t->catchBody);
            list(t->finallyBody);
        } else if (const auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
            switchBody(*sw);
        }
    }

    // 14.12.2: one scope, and one entry point per clause. A `case` jump can
    // enter above the clause that initializes a binding or below it, so every
    // lexical binding written directly in the body is exposed wherever it sits
    // and whatever the clauses above it say.
    void switchBody(const SwitchStmt& sw) {
        for (const auto& clause : sw.cases) {
            for (const auto& s : clause.body) {
                if (!s) continue;
                std::vector<std::string> declared;
                appendLexicalNames(*s, declared);
                for (auto& name : declared) exposed.insert(std::move(name));
            }
        }
        for (const auto& clause : sw.cases) list(clause.body);
    }
};
}  // namespace

std::unordered_set<std::string> getTdzExposedNames(const std::vector<StmtPtr>& stmts) {
    TdzScan scan;
    scan.list(stmts);
    return std::move(scan.exposed);
}

std::unordered_set<std::string> getTdzExposedNames(const std::vector<const Stmt*>& stmts) {
    TdzScan scan;
    scan.list(stmts);
    return std::move(scan.exposed);
}

// The module's top level is lowered from a borrowed-pointer list (the
// function declarations having been split out), so each entry point takes
}  // namespace bronze::ast
