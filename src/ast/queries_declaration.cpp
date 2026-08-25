// What a scope DECLARES: the names a statement list binds directly, the `var`s
// it hoists out of every block under it, and the lexical half of the first.
//
// One analysis and so one unit. The rule that makes it its own is the depth each
// question stops at: a `let` belongs to the list that spells it, a `var` belongs
// to the enclosing FUNCTION however deeply it is written (ECMA-262 8.6.2), and
// nothing at all crosses a nested function boundary. Three depths, one walk.

// For `getenv`, which MSVC deprecates and every other toolchain does not. The
// one seam here is read exactly once, from a single-threaded driver.
#define _CRT_SECURE_NO_WARNINGS

#include <cstdlib>
#include <cstring>
#include <unordered_set>

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

std::vector<std::string> getConstDeclarations(const std::vector<StmtPtr>& stmts) {
    std::vector<std::string> names;
    for (const auto& s : stmts) {
        if (s) appendConstNames(*s, names);
    }
    return names;
}

std::vector<std::string> getConstDeclarations(const std::vector<const Stmt*>& stmts) {
    std::vector<std::string> names;
    for (const auto* s : stmts) {
        if (s) appendConstNames(*s, names);
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

namespace {

// An initializer that runs no user code at all: a literal. Stage E3's whole
// rule, kept as the fallback for a scope the widening below refuses.
bool inertInitializer(const Expr& e) {
    return dynamic_cast<const NumberLit*>(&e) != nullptr ||
           dynamic_cast<const StringLit*>(&e) != nullptr ||
           dynamic_cast<const BoolLit*>(&e) != nullptr ||
           dynamic_cast<const NullLit*>(&e) != nullptr ||
           dynamic_cast<const UndefinedLit*>(&e) != nullptr ||
           dynamic_cast<const BigIntLit*>(&e) != nullptr ||
           dynamic_cast<const RegExpLit*>(&e) != nullptr ||
           (dynamic_cast<const TemplateLit*>(&e) != nullptr &&
            static_cast<const TemplateLit&>(e).exprs.empty());
}

// Every name mentioned anywhere under a node, plus the one question that stops
// the scan on sight: does the node BUILD a function or a class?
//
// A function expression written inside a prefix statement is a closure over
// this record that the statement can then enter — `(function () { return v })()`
// and `xs.forEach(x => v)` are both that, and neither mentions a dangerous
// name. A class is the same thing wearing method syntax. So the scan refuses
// any statement whose subtree contains one; the single position where creating
// a function is harmless — a declaration's initializer being wholly one — is
// handled by the caller, which never sends that expression here.
//
// Built on IdentVisitor because the mention question has to be answered by THE
// walk that finds mentions: a form this one forgot to descend into would be a
// dropped check, which is a ReferenceError that never fires. `ast::Visitor`
// declares one PURE virtual per node kind, so that walk cannot silently skip a
// form — a new node type stops the build until someone says how to descend it.
//
// A nested `function` DECLARATION is deliberately NOT flagged below, and that
// is load-bearing rather than an omission: IdentVisitor descends into its body,
// so every name the declaration could ever read is collected here and a
// dangerous one refuses the statement — while a declaration whose body names
// nothing dangerous has no way to read one either, whatever is done with the
// value afterwards. `tests/oracle/cases/dead_zone_reach_boundary.js` cases 1
// and 2 are that argument as programs, including the two-hop form where the
// declaration reaches the binding through one of the outer list's own hoisted
// functions.
class ReachScan final : public detail::IdentVisitor {
public:
    bool buildsFunction = false;

    void visit(const FunctionExpr& f) override {
        buildsFunction = true;
        detail::IdentVisitor::visit(f);
    }
    void visit(const ClassExpr& c) override {
        buildsFunction = true;
        detail::IdentVisitor::visit(c);
    }
    void visit(const ClassDecl& c) override {
        buildsFunction = true;
        detail::IdentVisitor::visit(c);
    }
};

// Does `node` mention any name in `stop`, or build a function?
bool reachesAnythingDangerous(const Node& node, const std::unordered_set<std::string>& stop) {
    ReachScan scan;
    node.accept(scan);
    if (scan.buildsFunction) return true;
    for (const auto& name : scan.names) {
        if (stop.count(name) != 0) return true;
    }
    return false;
}

// Whether a parameter default can put a closure over this record into a
// PARAMETER. The scan below treats every name that is not a declaration of
// this list as harmless, and a parameter holding such a closure is the one way
// that is wrong — so the whole widening is refused for the scope rather than
// modelled. Null for a list that has no parameters.
bool paramDefaultsBuildFunctions(const std::vector<Param>* params) {
    if (params == nullptr) return false;
    for (const auto& p : *params) {
        ReachScan scan;
        if (p.defaultValue) p.defaultValue->accept(scan);
        if (p.pattern) detail::visitPatternExprs(p.pattern.get(), scan);
        if (scan.buildsFunction) return true;
    }
    return false;
}

// BRONZE_NO_DEFINITE_REACH=1, the A/B seam for stage E4's half of this: with
// it the scan is stage E3's again — literal initializers only, and a stop at
// the first statement that could call anything.
bool reachWideningDisabled() {
    static const bool disabled = [] {
        const char* env = std::getenv("BRONZE_NO_DEFINITE_REACH");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return disabled;
}

std::vector<std::string> definitelyAssigned(const std::vector<const Stmt*>& stmts,
                                            const std::vector<Param>* params) {
    std::vector<std::string> names;
    const bool widen = !reachWideningDisabled() && !paramDefaultsBuildFunctions(params);

    // Everything the scan may not walk past. It starts as every hoisted
    // `function` of this list — all of them created, over this record, before
    // statement one — plus every lexical name the list declares, because a
    // mention of one before its declaration is a read in its own dead zone.
    // A name leaves the set as its declaration is passed; a function name
    // never does, and neither does one the prefix filled with a closure.
    std::unordered_set<std::string> stop;
    for (const Stmt* stmt : stmts) {
        if (const auto* f = dynamic_cast<const FunctionDecl*>(stmt)) stop.insert(f->name);
    }
    for (const auto& name : getLexicalDeclarations(stmts)) stop.insert(name);

    for (const Stmt* stmt : stmts) {
        if (stmt == nullptr) continue;
        // A hoisted function declaration is instantiated for the whole scope
        // before the first statement runs (8.6.2), so it initializes nothing
        // here and delays nothing after it.
        if (dynamic_cast<const FunctionDecl*>(stmt) != nullptr) continue;
        // Nor does an export clause: 16.2.3 names bindings and evaluates
        // nothing. It is here because the module sites hand this the WHOLE
        // module body — the only way the hoisted module functions, which are
        // exactly the closures over the module record, reach the stop set.
        if (dynamic_cast<const ExportNamesDecl*>(stmt) != nullptr) continue;
        const auto* v = dynamic_cast<const VarDecl*>(stmt);
        const bool plainDecl = v != nullptr && !v->name.empty() && v->init != nullptr;
        // A declaration whose initializer is wholly a function expression: it
        // runs no user code, and the closure it makes goes into this binding —
        // which is what keeps the NAME in `stop` from here down.
        if (plainDecl && dynamic_cast<const FunctionExpr*>(v->init.get()) != nullptr) {
            if (!v->isVar) names.push_back(v->name);
            stop.insert(v->name);
            continue;
        }
        if (plainDecl && inertInitializer(*v->init)) {
            if (!v->isVar) names.push_back(v->name);
            stop.erase(v->name);
            continue;
        }
        if (!widen) break;
        // A `var` is not lexical and has no dead zone, so it neither qualifies
        // nor disqualifies — but its initializer is code like any other. A
        // destructuring declaration binds no single name and is refused with
        // everything else that falls through to the break.
        if (plainDecl && !reachesAnythingDangerous(*v->init, stop)) {
            if (!v->isVar) names.push_back(v->name);
            stop.erase(v->name);
            continue;
        }
        // Not a declaration at all. Stage E3 stopped here unconditionally; now
        // a statement that can neither reach a closure over this record nor
        // read a binding still in its dead zone carries the scan on — which is
        // what lets the declarations BELOW a setup loop be proven.
        if (v == nullptr && !reachesAnythingDangerous(*stmt, stop)) continue;
        break;
    }
    return names;
}

}  // namespace

std::vector<std::string> getDefinitelyAssignedLexicalNames(const std::vector<StmtPtr>& stmts,
                                                           const std::vector<Param>* params) {
    std::vector<const Stmt*> raw;
    raw.reserve(stmts.size());
    for (const auto& s : stmts) raw.push_back(s.get());
    return definitelyAssigned(raw, params);
}

std::vector<std::string> getDefinitelyAssignedLexicalNames(const std::vector<const Stmt*>& stmts,
                                                           const std::vector<Param>* params) {
    return definitelyAssigned(stmts, params);
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

std::vector<std::string> getTopLevelVarDeclarations(const std::vector<StmtPtr>& stmts) {
    std::vector<std::string> names;
    for (const auto& s : stmts) {
        if (!s) continue;
        if (const auto* v = dynamic_cast<const VarDecl*>(s.get())) {
            if (v->isVar) appendDeclaredNames(*v, names);
        }
    }
    return names;
}

std::vector<std::string> getTopLevelVarDeclarations(const std::vector<const Stmt*>& stmts) {
    std::vector<std::string> names;
    for (const auto* s : stmts) {
        if (!s) continue;
        if (const auto* v = dynamic_cast<const VarDecl*>(s)) {
            if (v->isVar) appendDeclaredNames(*v, names);
        }
    }
    return names;
}
}  // namespace bronze::ast
