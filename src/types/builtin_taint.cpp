#include "types/builtin_taint.h"

#include "ast/query_walk.h"

namespace bronze::types {
namespace {

bool isEvaluatorOrGlobal(const std::string& n) {
    return n == "globalThis" || n == "eval" || n == "Function" || n == "AsyncFunction" ||
           n == "GeneratorFunction";
}

class BuiltinTaintScan final : public ast::detail::IdentVisitor {
public:
    BuiltinTaintScan(const std::string& name, bool aliasTaints)
        : name_(name), aliasTaints_(aliasTaints) {}

    bool tainted = false;

    void visit(const ast::Ident& i) override {
        if (isEvaluatorOrGlobal(i.name)) tainted = true;
        if (aliasTaints_ && i.name == name_) tainted = true;
    }
    // Every name the walk records other than through a plain read: a
    // declaration's own name, a `for (x in o)` target, a destructuring's
    // bound names, an `extends` base. Where the object itself may be passed
    // around, the one that matters is a binding OF the name, which in sloppy
    // code can be the global's own — so any such mention taints.
    void mention(const std::string& n) override {
        if (!aliasTaints_ && n == name_) tainted = true;
    }
    void visit(const ast::MemberAccess& m) override {
        if (isTarget(m.object.get())) return;
        // `globalThis.BENCH_CONFIG`: a named property of the global object,
        // read or written, is not the builtin unless it names the builtin,
        // another road back to the global object, or a way to run source text
        // — and `window.Math = f` rebinds the bare name as surely as
        // `globalThis.Math = f` does.
        if (globalRoad(m.object.get()) && !m.isPrivate) {
            if (reachesTarget(m.property)) tainted = true;
            if (isGlobalThis(m.object.get())) return;
        }
        ast::detail::IdentVisitor::visit(m);
    }
    void visit(const ast::IndexAccess& ix) override {
        if (isTarget(ix.object.get())) {
            ix.index->accept(*this);
            return;
        }
        // `window[k]` may name the builtin.
        if (globalRoad(ix.object.get())) tainted = true;
        ast::detail::IdentVisitor::visit(ix);
    }
    void visit(const ast::Binary& b) override {
        if (ast::isAssignOp(b.op)) {
            if (aliasTaints_ && memberBaseIsTarget(b.lhs.get())) tainted = true;
            if (isTarget(b.lhs.get())) tainted = true;
        }
        ast::detail::IdentVisitor::visit(b);
    }
    void visit(const ast::Unary& u) override {
        const bool mutating = u.op == ast::UnaryOp::Delete || u.op == ast::UnaryOp::PreInc ||
                              u.op == ast::UnaryOp::PreDec || u.op == ast::UnaryOp::PostInc ||
                              u.op == ast::UnaryOp::PostDec;
        if (mutating) {
            if (aliasTaints_ && memberBaseIsTarget(u.operand.get())) tainted = true;
            if (isTarget(u.operand.get())) tainted = true;
        }
        ast::detail::IdentVisitor::visit(u);
    }
    // A destructuring pattern's targets can be member expressions, and the
    // walk below cannot tell a target from a read — so the builtin anywhere in
    // the pattern taints, via a plain mention scan over its expressions.
    void visit(const ast::DestructuringAssign& d) override {
        ast::detail::IdentVisitor mentions;
        ast::detail::visitPatternExprs(d.pattern.get(), mentions);
        for (const auto& n : ast::patternBoundNames(*d.pattern)) mentions.names.insert(n);
        if (mentions.names.count(name_) != 0) tainted = true;
        for (const auto& n : mentions.names) {
            if (isEvaluatorOrGlobal(n)) tainted = true;
        }
        d.value->accept(*this);
    }

private:
    static bool isGlobalThis(const ast::Expr* e) {
        const auto* id = dynamic_cast<const ast::Ident*>(e);
        return id != nullptr && id->name == "globalThis";
    }
    // A name the global object answers with itself. Only `globalThis` taints
    // on a bare mention: the others are common names of a program's own
    // bindings (`function onFirstUse(self)`, `let parent = node.parent`),
    // which this scan cannot tell from the global's, so a member of them is
    // what it watches.
    static bool globalRoad(const ast::Expr* e) {
        const auto* id = dynamic_cast<const ast::Ident*>(e);
        if (id == nullptr) return false;
        const std::string& n = id->name;
        return n == "globalThis" || n == "window" || n == "self" || n == "top" || n == "parent" ||
               n == "frames";
    }
    // The global object's properties that hand back the builtin, the global
    // object itself, or an evaluator: what a member of `globalThis` must not
    // name for the mention to be harmless.
    bool reachesTarget(const std::string& p) const {
        return p == name_ || p == "globalThis" || p == "window" || p == "self" || p == "frames" ||
               p == "parent" || p == "top" || p == "eval" || p == "Function" || p == "Reflect" ||
               p == "Object";
    }
    bool isTarget(const ast::Expr* e) const {
        const auto* id = dynamic_cast<const ast::Ident*>(e);
        return id != nullptr && id->name == name_;
    }
    bool memberBaseIsTarget(const ast::Expr* e) const {
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(e)) {
            return isTarget(m->object.get());
        }
        if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(e)) {
            return isTarget(ix->object.get());
        }
        return false;
    }

    const std::string& name_;
    bool aliasTaints_;
};

}  // namespace

bool builtinTainted(const ast::Module& module, const std::string& name, bool aliasTaints) {
    BuiltinTaintScan scan(name, aliasTaints);
    for (const auto& stmt : module.body) {
        if (stmt) stmt->accept(scan);
        if (scan.tainted) return true;
    }
    return scan.tainted;
}

}  // namespace bronze::types
