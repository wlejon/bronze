// Where a SUSPENSION is: `yield`, `yield*`, `await` and `for await`.
//
// The one interesting thing about the walk is its boundary rule — a nested
// function's body is not descended into. A `yield` written inside a nested
// generator is that generator's suspension point, and a non-generator function
// cannot hold one at all (the parser makes `yield` an ordinary identifier
// there), so a walk that descended would attribute an inner suspension to the
// outer body.

#include <algorithm>
#include <string>

#include "ast/queries.h"
#include "ast/query_walk.h"

namespace bronze::ast {
namespace {

class YieldScan final : public Visitor {
public:
    bool found = false;
    YieldForms forms = YieldForms::None;

    // `stopAtFirst` is the whole difference between the two questions this
    // walk answers. "Is there one" can quit at the first hit; "which forms are
    // there" cannot, because `yield` and `yield*` can share a position and a
    // refusal that named only the first one found would name the wrong one
    // half the time.
    explicit YieldScan(bool stopAtFirst = true) : stopAtFirst_(stopAtFirst) {}

    void visit(const YieldExpr& y) override {
        found = true;
        forms = forms | (y.isAwait     ? YieldForms::Await
                         : y.delegate  ? YieldForms::Delegating
                                       : YieldForms::Plain);
        // `yield* (yield x)` is two suspensions at one site, and the operand is
        // this generator's code like any other. `await (await x)` is the same
        // shape in an async body.
        walk(y.argument);
    }

    void visit(const NumberLit&) override {}
    void visit(const BigIntLit&) override {}
    void visit(const StringLit&) override {}
    void visit(const RegExpLit&) override {}
    void visit(const BoolLit&) override {}
    void visit(const NullLit&) override {}
    void visit(const UndefinedLit&) override {}
    void visit(const ThisExpr&) override {}
    void visit(const Ident&) override {}
    void visit(const BreakStmt&) override {}
    void visit(const ContinueStmt&) override {}
    void visit(const DebuggerStmt&) override {}
    void visit(const SuperMember&) override {}
    // The boundary. Both forms declare a function of their own, so nothing
    // under them suspends the body being scanned.
    void visit(const FunctionExpr&) override {}
    void visit(const FunctionDecl&) override {}
    void visit(const ClassDecl&) override {}
    void visit(const ClassExpr&) override {}

    void visit(const SpreadElement& n) override { walk(n.argument); }
    void visit(const Unary& n) override { walk(n.operand); }
    void visit(const Binary& n) override {
        walk(n.lhs);
        walk(n.rhs);
    }
    void visit(const TemplateLit& n) override {
        for (const auto& e : n.exprs) walk(e);
    }
    void visit(const TaggedTemplate& n) override {
        walk(n.tag);
        for (const auto& e : n.templateLit->exprs) walk(e);
    }
    void visit(const Ternary& n) override {
        walk(n.condition);
        walk(n.thenExpr);
        walk(n.elseExpr);
    }
    void visit(const MemberAccess& n) override { walk(n.object); }
    void visit(const IndexAccess& n) override {
        walk(n.object);
        walk(n.index);
    }
    void visit(const Call& n) override {
        walk(n.callee);
        for (const auto& a : n.args) walk(a);
    }
    void visit(const NewExpr& n) override {
        walk(n.callee);
        for (const auto& a : n.args) walk(a);
    }
    void visit(const NewTargetExpr&) override {}
    void visit(const ImportMetaExpr&) override {}
    void visit(const SuperCall& n) override {
        for (const auto& a : n.args) walk(a);
    }
    void visit(const DestructuringAssign& n) override {
        walkPattern(n.pattern.get());
        walk(n.value);
    }
    void visit(const DynamicImportExpr& n) override {
        walk(n.specifier);
    }
    void visit(const ObjectLit& n) override {
        for (const auto& p : n.props) {
            walk(p.keyExpr);
            walk(p.value);
        }
    }
    void visit(const ArrayLit& n) override {
        for (const auto& e : n.elements) walk(e);
    }
    void visit(const BlockStmt& n) override { walkList(n.stmts); }
    void visit(const VarDecl& n) override {
        walkPattern(n.pattern.get());
        walk(n.init);
    }
    void visit(const ReturnStmt& n) override { walk(n.value); }
    void visit(const ExprStmt& n) override { walk(n.expr); }
    void visit(const IfStmt& n) override {
        walk(n.condition);
        walkList(n.thenBody);
        walkList(n.elseBody);
    }
    void visit(const WhileStmt& n) override {
        walk(n.condition);
        walkList(n.body);
    }
    void visit(const DoWhileStmt& n) override {
        walkList(n.body);
        walk(n.condition);
    }
    void visit(const ForStmt& n) override {
        walkList(n.init);
        walk(n.condition);
        walk(n.update);
        walkList(n.body);
    }
    void visit(const SwitchStmt& n) override {
        walk(n.discriminant);
        for (const auto& c : n.cases) {
            walk(c.test);
            walkList(c.body);
        }
    }
    void visit(const ForInStmt& n) override {
        walkPattern(n.pattern.get());
        walk(n.object);
        walkList(n.body);
    }
    void visit(const ForOfStmt& n) override {
        if (n.isAwait) {
            found = true;
            forms = forms | YieldForms::Await;
        }
        walkPattern(n.pattern.get());
        walk(n.iterable);
        walkList(n.body);
    }
    void visit(const LabeledStmt& n) override {
        if (n.body) n.body->accept(*this);
    }
    void visit(const TryStmt& n) override {
        walkList(n.body);
        walkPattern(n.catchPattern.get());
        walkList(n.catchBody);
        walkList(n.finallyBody);
    }
    void visit(const ThrowStmt& n) override { walk(n.value); }
    void visit(const Module& n) override { walkList(n.body); }

private:
    bool stopAtFirst_ = true;

    bool done() const { return found && stopAtFirst_; }

    template <typename T>
    void walk(const std::unique_ptr<T>& node) {
        if (node && !done()) node->accept(*this);
    }
    template <typename T>
    void walkList(const std::vector<std::unique_ptr<T>>& list) {
        for (const auto& n : list) walk(n);
    }
    void walkPattern(const BindingPattern* pattern) {
        if (!pattern) return;
        for (const auto& elem : pattern->elements) {
            walk(elem.keyExpr);
            walk(elem.target);
            walk(elem.defaultValue);
            walkPattern(elem.pattern.get());
        }
    }
};

}  // namespace

bool containsYield(const Node& node) {
    YieldScan scan;
    node.accept(scan);
    return scan.found;
}

bool containsYield(const std::vector<StmtPtr>& stmts) {
    YieldScan scan;
    for (const auto& s : stmts) {
        if (s) s->accept(scan);
        if (scan.found) return true;
    }
    return scan.found;
}

bool containsYield(const std::vector<const Stmt*>& stmts) {
    YieldScan scan;
    for (const auto* s : stmts) {
        if (s) s->accept(scan);
        if (scan.found) return true;
    }
    return scan.found;
}

YieldForms yieldFormsIn(const Node& node) {
    YieldScan scan(/*stopAtFirst=*/false);
    node.accept(scan);
    return scan.forms;
}

YieldForms yieldFormsIn(const std::vector<StmtPtr>& stmts) {
    YieldScan scan(/*stopAtFirst=*/false);
    for (const auto& s : stmts) {
        if (s) s->accept(scan);
    }
    return scan.forms;
}

YieldForms yieldFormsIn(const std::vector<const Stmt*>& stmts) {
    YieldScan scan(/*stopAtFirst=*/false);
    for (const auto* s : stmts) {
        if (s) s->accept(scan);
    }
    return scan.forms;
}

const char* yieldFormName(YieldForms forms) {
    const bool hasAw = hasAwait(forms);
    const bool hasY = (static_cast<uint8_t>(forms) & (static_cast<uint8_t>(YieldForms::Plain) | static_cast<uint8_t>(YieldForms::Delegating))) != 0;
    if (hasAw && hasY) return "a `yield` or an `await`";
    if (hasAw) return "an `await`";
    switch (forms) {
        case YieldForms::Delegating: return "a `yield*`";
        case YieldForms::Both: return "a `yield` or a `yield*`";
        // `None` cannot reach a refusal — nothing refuses a position with no
        // suspension in it — and answering for the plain form is the honest
        // reading of "there is a suspension here and it is not a delegation".
        default: return "a `yield`";
    }
}

}  // namespace bronze::ast
