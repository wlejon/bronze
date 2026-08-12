#include "types/escape.h"

#include "types/walk.h"

namespace bronze::types {
namespace {

class EscapeWalker final : public Walker {
public:
    std::set<std::string> escaping;

    void visit(const ast::Ident& n) override { escaping.insert(n.name); }

    void visit(const ast::Call& n) override {
        // The one non-escaping position. Everything else about the call —
        // its arguments, and a callee that is not a bare name — still walks.
        if (!dynamic_cast<const ast::Ident*>(n.callee.get())) {
            n.callee->accept(*this);
        }
        for (const auto& a : n.args) a->accept(*this);
    }

    // `new` has no override: unlike a call, its callee is NOT a non-escaping
    // position — the constructor's `.prototype` outlives the site and every
    // instance holds it — so the base walk, which reaches the callee as an
    // ordinary expression, records exactly what is wanted.

    void visit(const ast::VarDecl& n) override {
        escaping.insert(n.name);
        Walker::visit(n);
    }

    void visit(const ast::FunctionDecl& n) override {
        escaping.insert(n.name);
        for (const auto& p : n.params) escaping.insert(p.name);
        Walker::visit(n);
    }

    void visit(const ast::FunctionExpr& n) override {
        if (!n.name.empty()) escaping.insert(n.name);
        for (const auto& p : n.params) escaping.insert(p.name);
        Walker::visit(n);
    }
};

}  // namespace

std::set<std::string> escapingNames(const ast::Module& module) {
    EscapeWalker w;
    for (const auto& stmt : module.body) {
        if (!stmt) continue;
        // A module-level function declaration is the definition of the name,
        // not a reference to it — so its body and parameters are walked
        // directly, bypassing the override that would record the name.
        if (const auto* fn = dynamic_cast<const ast::FunctionDecl*>(stmt.get())) {
            // Unless it is exported. A signature is specialized by joining over
            // every call site, and its licence to do so is "this function has
            // no unknown callers". An exported symbol has one by definition: a
            // caller outside this compilation, which contributed nothing to the
            // join and is free to pass anything. So the name escapes here, in
            // the analysis, rather than being re-tested by every consumer of
            // the result.
            if (fn->isExported) w.escaping.insert(fn->name);
            for (const auto& p : fn->params) w.escaping.insert(p.name);
            w.walkList(fn->body);
        } else {
            stmt->accept(w);
        }
    }
    return std::move(w.escaping);
}

}  // namespace bronze::types
