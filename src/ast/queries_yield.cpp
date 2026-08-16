// What a SUSPENSION needs to know: where one is, and what has to survive it.
//
// One unit because both questions share a boundary rule and it is the only
// interesting thing about either walk — a nested function's body is not
// descended into. A `yield` written inside a nested generator is that
// generator's suspension point, and a non-generator function cannot hold one at
// all (the parser makes `yield` an ordinary identifier there), so a walk that
// descended would attribute an inner suspension to the outer body and lift
// bindings into a frame nothing reads.

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

namespace {

// Every name a statement BINDS, at any depth below it, stopping at nested
// functions. Not `getScopeDeclarations` — that answers for one list, and this
// has to reach into every block, loop head and catch clause under the body,
// because a generator's frame holds all of them at once.
void collectFrameNames(const Stmt& s, std::unordered_set<std::string>& out);

void collectFrameNames(const std::vector<StmtPtr>& stmts, std::unordered_set<std::string>& out) {
    for (const auto& s : stmts) {
        if (s) collectFrameNames(*s, out);
    }
}

void addPatternOrName(const std::string& name, const BindingPattern* pattern,
                      std::unordered_set<std::string>& out) {
    if (pattern) {
        for (auto& bound : patternBoundNames(*pattern)) out.insert(std::move(bound));
    } else if (!name.empty()) {
        out.insert(name);
    }
}

void collectFrameNames(const Stmt& s, std::unordered_set<std::string>& out) {
    if (const auto* v = dynamic_cast<const VarDecl*>(&s)) {
        std::vector<std::string> names;
        detail::appendDeclaredNames(*v, names);
        for (auto& n : names) out.insert(std::move(n));
        return;
    }
    // A nested function or class declares a NAME in this scope, and that name is
    // a binding of the frame like any other. Its body is not descended into: it
    // has a frame of its own.
    if (const auto* f = dynamic_cast<const FunctionDecl*>(&s)) {
        out.insert(f->name);
        return;
    }
    if (const auto* c = dynamic_cast<const ClassDecl*>(&s)) {
        out.insert(c->name);
        return;
    }
    if (const auto* b = dynamic_cast<const BlockStmt*>(&s)) {
        collectFrameNames(b->stmts, out);
        return;
    }
    if (const auto* i = dynamic_cast<const IfStmt*>(&s)) {
        collectFrameNames(i->thenBody, out);
        collectFrameNames(i->elseBody, out);
        return;
    }
    if (const auto* w = dynamic_cast<const WhileStmt*>(&s)) {
        collectFrameNames(w->body, out);
        return;
    }
    if (const auto* d = dynamic_cast<const DoWhileStmt*>(&s)) {
        collectFrameNames(d->body, out);
        return;
    }
    if (const auto* f = dynamic_cast<const ForStmt*>(&s)) {
        collectFrameNames(f->init, out);
        collectFrameNames(f->body, out);
        return;
    }
    if (const auto* fo = dynamic_cast<const ForOfStmt*>(&s)) {
        addPatternOrName(fo->name, fo->pattern.get(), out);
        collectFrameNames(fo->body, out);
        return;
    }
    if (const auto* fi = dynamic_cast<const ForInStmt*>(&s)) {
        addPatternOrName(fi->name, fi->pattern.get(), out);
        collectFrameNames(fi->body, out);
        return;
    }
    if (const auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
        for (const auto& c : sw->cases) collectFrameNames(c.body, out);
        return;
    }
    if (const auto* lb = dynamic_cast<const LabeledStmt*>(&s)) {
        if (lb->body) collectFrameNames(*lb->body, out);
        return;
    }
    if (const auto* tr = dynamic_cast<const TryStmt*>(&s)) {
        collectFrameNames(tr->body, out);
        if (tr->hasCatchParam) addPatternOrName(tr->catchName, tr->catchPattern.get(), out);
        collectFrameNames(tr->catchBody, out);
        collectFrameNames(tr->finallyBody, out);
        return;
    }
}

}  // namespace

std::unordered_set<std::string> getGeneratorFrameNames(const std::vector<StmtPtr>& stmts) {
    std::unordered_set<std::string> out;
    collectFrameNames(stmts, out);
    return out;
}

std::unordered_set<std::string> getGeneratorFrameNames(const std::vector<const Stmt*>& stmts) {
    std::unordered_set<std::string> out;
    for (const auto* s : stmts) {
        if (s) collectFrameNames(*s, out);
    }
    return out;
}

namespace {

// The same walk shape as collectFrameNames — every statement form that can
// hold another, nested functions excluded — answering a depth instead of a set.
uint32_t iterationDepth(const Stmt& s);

uint32_t iterationDepth(const std::vector<StmtPtr>& stmts) {
    uint32_t deepest = 0;
    for (const auto& s : stmts) {
        if (s) deepest = std::max(deepest, iterationDepth(*s));
    }
    return deepest;
}

// A loop of this kind claims a slot only when its BODY can suspend: a for-of
// whose body runs straight through never leaves the record in a place the
// resume dispatch has to find it again, and pays nothing.
uint32_t iterationLoopDepth(const std::vector<StmtPtr>& body) {
    const uint32_t inner = iterationDepth(body);
    return containsYield(body) ? inner + 1 : inner;
}

uint32_t iterationDepth(const Stmt& s) {
    if (const auto* b = dynamic_cast<const BlockStmt*>(&s)) return iterationDepth(b->stmts);
    if (const auto* i = dynamic_cast<const IfStmt*>(&s)) {
        return std::max(iterationDepth(i->thenBody), iterationDepth(i->elseBody));
    }
    if (const auto* w = dynamic_cast<const WhileStmt*>(&s)) return iterationDepth(w->body);
    if (const auto* d = dynamic_cast<const DoWhileStmt*>(&s)) return iterationDepth(d->body);
    if (const auto* f = dynamic_cast<const ForStmt*>(&s)) return iterationDepth(f->body);
    if (const auto* fo = dynamic_cast<const ForOfStmt*>(&s)) {
        return fo->isAwait ? iterationDepth(fo->body) + 1 : iterationLoopDepth(fo->body);
    }
    if (const auto* fi = dynamic_cast<const ForInStmt*>(&s)) return iterationLoopDepth(fi->body);
    if (const auto* sw = dynamic_cast<const SwitchStmt*>(&s)) {
        uint32_t deepest = 0;
        for (const auto& c : sw->cases) deepest = std::max(deepest, iterationDepth(c.body));
        return deepest;
    }
    if (const auto* lb = dynamic_cast<const LabeledStmt*>(&s)) {
        return lb->body ? iterationDepth(*lb->body) : 0;
    }
    if (const auto* tr = dynamic_cast<const TryStmt*>(&s)) {
        return std::max({iterationDepth(tr->body), iterationDepth(tr->catchBody),
                         iterationDepth(tr->finallyBody)});
    }
    return 0;
}

}  // namespace

uint32_t maxSuspendingIterationDepth(const std::vector<StmtPtr>& stmts) {
    return iterationDepth(stmts);
}

uint32_t maxSuspendingIterationDepth(const std::vector<const Stmt*>& stmts) {
    uint32_t deepest = 0;
    for (const auto* s : stmts) {
        if (s) deepest = std::max(deepest, iterationDepth(*s));
    }
    return deepest;
}

}  // namespace bronze::ast
