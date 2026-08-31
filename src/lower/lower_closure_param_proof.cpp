// The closure PARAMETER proof: which parameters of a nested `function f() {}`
// are Numbers at every call, the one walk of the enclosing scope that decides
// it, and the point at which the answer reaches the declaration's IL skeleton.

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/assigned.h"
#include "ast/queries.h"
#include "ast/query_walk.h"
#include "lower/lowerer.h"

namespace bronze::lower {

bool Lowerer::closureParamProofDisabled() {
    static const bool disabled = [] {
        const char* env = std::getenv("BRONZE_NO_CLOSURE_PARAM_PROOF");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return disabled;
}

namespace {

// Every mention of one name under a function body, split into the two things a
// mention can be: the CALLEE of an ordinary call — whose argument list is then
// evidence about the parameters — and anything else at all, which is the
// function value leaving through a door this analysis cannot follow.
//
// Built on `ast::detail::IdentVisitor` and not beside it, because the walk that
// decides "is this every mention?" must be THE walk that finds mentions:
// a form this one forgot to descend into is not a missed optimization, it is an
// unsound proof. Two overrides, and both are removals:
//
//   - `Call`: a callee that is exactly the bare name is recorded as a site and
//     NOT as an escape; the callee of anything else, and every argument, walk
//     normally. `?.()` is a site too — it either calls with these arguments or
//     does not call at all. `f(...xs)` is refused outright, because a spread
//     breaks the one-argument-per-parameter correspondence the claim IS.
//   - `FunctionDecl`: the declaration's own name is a binding, not a mention.
//     The base class records it (it is a name an enclosing scope must put in a
//     record), which would make every function an escape of itself.
//
// A `VarDecl` of the name still counts as a mention, and so as an escape. That
// is deliberately blunt and in the safe direction: a redeclaration means the
// name does not denote one binding throughout, and modelling shadowing here
// would be a scope resolver, not a proof.
class CalleeOnlyScan final : public ast::detail::IdentVisitor {
public:
    explicit CalleeOnlyScan(std::string name) : name_(std::move(name)) {}

    std::vector<const ast::Call*> sites;
    bool escaped = false;
    bool spread = false;

    void visit(const ast::Call& c) override {
        const auto* id = dynamic_cast<const ast::Ident*>(c.callee.get());
        if (id != nullptr && id->name == name_) {
            for (const auto& arg : c.args) {
                if (dynamic_cast<const ast::SpreadElement*>(arg.get()) != nullptr) spread = true;
            }
            sites.push_back(&c);
        } else {
            c.callee->accept(*this);
        }
        for (const auto& arg : c.args) arg->accept(*this);
    }

    void visit(const ast::FunctionDecl& f) override {
        ast::detail::visitParamExprs(f.params, *this);
        for (const auto& s : f.body) {
            if (s) s->accept(*this);
        }
    }

    void visit(const ast::Ident& i) override {
        if (i.name == name_) escaped = true;
        ast::detail::IdentVisitor::visit(i);
    }

    void visit(const ast::VarDecl& v) override {
        if (!v.pattern && v.name == name_) escaped = true;
        ast::detail::IdentVisitor::visit(v);
    }

private:
    std::string name_;
};

}  // namespace

// THE PARAMETER PROOF FOR A CLOSURE, and the general form of what `param
// f(x): number` does in a manifest by hand.
//
// Stage E3 wrote down the gap: a class method's parameters are typed by joining
// what every call site passes (types/flow_expr.cpp, `contributeArgs`), a module
// function's by the same join over its enumerated callers (types/infer.cpp),
// and a nested `function f() {}` by nothing — it is reached through a function
// value, so inference has no `functionIndex` for it and no signature that can
// speak for its result or its arguments. `bench/pins/env-slot-kernel.pins`
// carries five `param` lines that exist only because of that hole.
//
// But a nested declaration's callers ARE enumerable, on exactly the terms the
// static call plan (lower_static_call_plan.cpp) already establishes for its
// VALUE. The declaration is
// the value, it is installed before the scope's first statement runs, and the
// scope's whole lexical reach is one subtree this compilation holds. So: walk
// that subtree once, and if every mention of the name is the callee of an
// ordinary call, those calls are ALL the calls, and the join over their
// arguments at position k is a fact about every value parameter k will ever
// hold — the same join, over the same lattice, that the two paths above run.
//
// This is a PROOF and not a pin. Every clause below is a refusal in the safe
// direction, and a refusal costs the f64 slot and nothing else:
//
//   - a plain declaration only: a generator or an async function's parameters
//     are bound by a resume edge, not by the call.
//   - no default, rest or pattern parameter, because the value bound is then
//     not the value passed — the same three positions `applySignaturePins`
//     refuses, and for the same reason: there is no `undefined` in an f64.
//   - the name is not rebound anywhere in the subtree (`getDeeplyAssignedNames`,
//     the stable-plan's test), and every mention of it is a callee.
//   - no spread at any site.
//   - every site supplies position k, and the argument there is a proven
//     Number. A SHORT call binds `undefined`, which is not a Number, so a
//     single one refuses the position.
//
// What it deliberately does NOT reach: a closure that escapes. `env_slot_kernel`
// ends `return render;`, so `render`'s own parameter stays unproven and stays
// pinned — the manifest says so. That is the honest boundary of this mechanism:
// it types the closures a factory calls, not the closure the factory hands out.
void Lowerer::planClosureParamNumbers(const std::vector<ast::Param>& params,
                                      const std::vector<ast::StmtPtr>& stmts) {
    if (inference_ == nullptr || closureParamProofDisabled()) return;
    // `lowerFunctionBody` opened this body's frame before calling in.
    if (provenClosureParams_.empty()) return;

    bool anyDeclaration = false;
    for (const auto& stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt.get());
        if (fnDecl != nullptr && !fnDecl->isGenerator && !fnDecl->isAsync &&
            !fnDecl->params.empty()) {
            anyDeclaration = true;
            break;
        }
    }
    if (!anyDeclaration) return;

    std::unordered_set<std::string> rebound = ast::getDeeplyAssignedNames(stmts);
    for (const auto& p : params) {
        if (!p.defaultValue) continue;
        for (auto& name : ast::getDeeplyAssignedNames(*p.defaultValue)) {
            rebound.insert(std::move(name));
        }
    }

    for (const auto& stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt.get());
        if (fnDecl == nullptr || fnDecl->isGenerator || fnDecl->isAsync) continue;
        if (fnDecl->name.empty() || fnDecl->params.empty()) continue;
        if (rebound.contains(fnDecl->name)) continue;
        bool shapeFits = true;
        for (const auto& p : fnDecl->params) {
            if (p.defaultValue || p.isRest || p.pattern || p.name.empty()) shapeFits = false;
        }
        if (!shapeFits) continue;

        CalleeOnlyScan scan(fnDecl->name);
        for (const auto& s : stmts) {
            if (s) s->accept(scan);
        }
        for (const auto& p : params) {
            if (p.defaultValue) p.defaultValue->accept(scan);
            if (p.pattern) ast::detail::visitPatternExprs(p.pattern.get(), scan);
        }
        if (scan.escaped || scan.spread || scan.sites.empty()) continue;

        std::vector<bool> proven(fnDecl->params.size(), true);
        for (const ast::Call* site : scan.sites) {
            for (size_t k = 0; k < proven.size(); ++k) {
                if (k >= site->args.size() || !provenNumber(*site->args[k])) proven[k] = false;
            }
        }
        bool any = false;
        for (const bool p : proven) any = any || p;
        if (any) provenClosureParams_.back().emplace(fnDecl, std::move(proven));
    }
}

void Lowerer::applyProvenClosureParams(const ast::Node& site,
                                       const std::vector<ast::Param>& params,
                                       il::Function& fn) const {
    const auto* decl = dynamic_cast<const ast::FunctionDecl*>(&site);
    if (decl == nullptr) return;
    // The innermost open frame is the one planned for the statement list this
    // declaration is a member of: the enclosing body is mid-lowering, and every
    // body nested inside it has closed its own frame again. The module top level
    // is not lowered through `lowerFunctionBody` and so opens no frame at all.
    if (provenClosureParams_.empty()) return;
    const auto& plan = provenClosureParams_.back();
    const auto it = plan.find(decl);
    if (it == plan.end()) return;
    const size_t base = fn.firstSourceParam();
    for (size_t i = 0; i < params.size() && i < it->second.size() && i + base < fn.params.size();
         ++i) {
        // Only ever Dynamic to F64. A `--pins` entry that already said the same
        // thing has run just above; the proof and the promise agree, and
        // neither can be reached by a slot the other typed differently.
        if (it->second[i] && fn.params[i + base].type == il::Type::Dynamic) {
            fn.params[i + base].type = il::Type::F64;
        }
    }
}

}  // namespace bronze::lower
