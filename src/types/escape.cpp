#include "types/escape.h"

#include "types/walk.h"

namespace bronze::types {
namespace {

std::string matchGlobalThisFnExport(const ast::Stmt& stmt) {
    const auto* exprStmt = dynamic_cast<const ast::ExprStmt*>(&stmt);
    if (!exprStmt || !exprStmt->expr) return {};
    const auto* bin = dynamic_cast<const ast::Binary*>(exprStmt->expr.get());
    if (!bin || bin->op != ast::BinaryOp::Assign) return {};
    const auto* member = dynamic_cast<const ast::MemberAccess*>(bin->lhs.get());
    if (!member || member->optional || member->isPrivate || member->property.empty()) return {};
    const auto* base = dynamic_cast<const ast::Ident*>(member->object.get());
    if (!base || base->name != "globalThis") return {};
    const auto* rhs = dynamic_cast<const ast::Ident*>(bin->rhs.get());
    if (!rhs || rhs->name != member->property) return {};
    return member->property;
}

class GlobalExportSafetyScan final : public Walker {
public:
    std::set<std::string> directlyCalled;
    std::set<std::string> globalMemberProps;
    bool globalObjectEscaped = false;

    void visit(const ast::Ident& n) override {
        if (isGlobalObjectName(n.name)) globalObjectEscaped = true;
    }

    void visit(const ast::Call& n) override {
        if (!n.optional) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(n.callee.get())) {
                directlyCalled.insert(id->name);
            }
        }
        Walker::visit(n);
    }

    void visit(const ast::MemberAccess& n) override {
        if (const auto* id = dynamic_cast<const ast::Ident*>(n.object.get())) {
            if (isGlobalObjectName(id->name)) {
                globalMemberProps.insert(n.property);
                return;
            }
        }
        Walker::visit(n);
    }

private:
    static bool isGlobalObjectName(const std::string& name) {
        return name == "globalThis" || name == "window" || name == "self";
    }
};

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
    std::set<std::string> topLevelFns;
    for (const auto& stmt : module.body) {
        if (const auto* fn = dynamic_cast<const ast::FunctionDecl*>(stmt.get())) {
            if (!fn->isExported && !fn->name.empty()) topLevelFns.insert(fn->name);
        }
    }

    GlobalExportSafetyScan safety;
    for (const auto& stmt : module.body) {
        if (!stmt) continue;
        const std::string exported = matchGlobalThisFnExport(*stmt);
        if (!exported.empty() && topLevelFns.count(exported) != 0) continue;
        if (const auto* blk = dynamic_cast<const ast::BlockStmt*>(stmt.get())) {
            for (const auto& sub : blk->stmts) {
                if (!sub) continue;
                const std::string subExp = matchGlobalThisFnExport(*sub);
                if (!subExp.empty() && topLevelFns.count(subExp) != 0) continue;
                sub->accept(safety);
            }
            continue;
        }
        stmt->accept(safety);
    }

    auto isSafeGlobalExport = [&](const ast::Stmt& s) {
        const std::string fn = matchGlobalThisFnExport(s);
        return !fn.empty() && topLevelFns.count(fn) != 0 &&
               safety.directlyCalled.count(fn) != 0 &&
               safety.globalMemberProps.count(fn) == 0 && !safety.globalObjectEscaped;
    };

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
        } else if (isSafeGlobalExport(*stmt)) {
            continue;
        } else if (const auto* blk = dynamic_cast<const ast::BlockStmt*>(stmt.get())) {
            for (const auto& sub : blk->stmts) {
                if (sub && !isSafeGlobalExport(*sub)) sub->accept(w);
            }
        } else {
            stmt->accept(w);
        }
    }
    return std::move(w.escaping);
}

}  // namespace bronze::types
