// The walk behind `planDerivedCtor` (derived_ctor_plan.h): a forward pass over
// the constructor in evaluation order, carrying what is known about the
// receiver's binding at each point.
//
// Three states. UNBOUND: no `super()` can have returned. BOUND: one has, on
// every path here. EITHER: the walk cannot say. A `this` site is tested unless
// the state is BOUND; a `super()` unless it is UNBOUND. An `if` joins its arms,
// an arm that ends in `return` or `throw` drops out of the join, and the
// short-circuit operators join the evaluated and the skipped operand.
//
// What the walk does not follow precisely it bounds: a loop, a `switch`, a
// `try` or a labeled statement that contains no `super()` keeps the state it
// was entered with all the way through (nothing in it can change it), and one
// that does contains EITHER throughout and leaves EITHER behind. An arrow is
// a function the constructor may call at any moment, so inside it the state
// is BOUND if it was bound where the arrow was made and EITHER otherwise —
// and once an arrow that calls `super()` exists, UNBOUND means EITHER from
// there on, since any call could be a call to it.

#include "lower/derived_ctor_plan.h"

#include "types/walk.h"

namespace bronze::lower {

namespace {

enum class Bind { Unbound, Bound, Either };

Bind join(Bind a, Bind b) { return a == b ? a : Bind::Either; }

// What an arrow made (or a region entered) in state `s` runs with.
Bind deferred(Bind s) { return s == Bind::Bound ? Bind::Bound : Bind::Either; }

// Whether a subtree calls `super()` — through an arrow too, since an arrow
// shares the constructor's binding; not through any other function.
class SuperCallCounter final : public types::Walker {
public:
    size_t count = 0;
    void visit(const ast::SuperCall& n) override {
        ++count;
        types::Walker::visit(n);
    }
    void visit(const ast::FunctionExpr& n) override {
        if (n.isArrow) types::Walker::visit(n);
    }
    void visit(const ast::FunctionDecl&) override {}
    void visit(const ast::ClassDecl& n) override {
        if (n.superClass) n.superClass->accept(*this);
        for (const auto& m : n.methods) {
            if (m.keyExpr) m.keyExpr->accept(*this);
        }
    }
    void visit(const ast::ClassExpr& n) override {
        if (n.superClass) n.superClass->accept(*this);
        for (const auto& m : n.methods) {
            if (m.keyExpr) m.keyExpr->accept(*this);
        }
    }
};

template <typename Node>
size_t superCallsIn(const Node& n) {
    SuperCallCounter c;
    n.accept(c);
    return c.count;
}

class Planner;

// Records every site of a subtree at one state, which is how a region the
// planner does not follow is handled: the state cannot change inside it (it
// calls no `super()`), or the planner has already widened it to EITHER.
class RegionRecorder final : public types::Walker {
public:
    RegionRecorder(Planner& p, Bind s, size_t arrowDepth) : p_(p), s_(s), depth_(arrowDepth) {}
    void visit(const ast::ThisExpr& n) override;
    void visit(const ast::SuperMember& n) override;
    void visit(const ast::SuperCall& n) override;
    void visit(const ast::ReturnStmt& n) override;
    void visit(const ast::FunctionExpr& n) override;
    void visit(const ast::FunctionDecl&) override {}
    void visit(const ast::ClassDecl& n) override {
        if (n.superClass) n.superClass->accept(*this);
        for (const auto& m : n.methods) {
            if (m.keyExpr) m.keyExpr->accept(*this);
        }
    }
    void visit(const ast::ClassExpr& n) override {
        if (n.superClass) n.superClass->accept(*this);
        for (const auto& m : n.methods) {
            if (m.keyExpr) m.keyExpr->accept(*this);
        }
    }

private:
    Planner& p_;
    Bind s_;
    size_t depth_;
};

struct Flow {
    Bind state;
    bool terminated = false;
};

class Planner {
public:
    explicit Planner(DerivedCtorPlan& plan) : plan_(plan) {}

    Bind effective(Bind s) const {
        return escaped_ && s == Bind::Unbound ? Bind::Either : s;
    }
    void recordThisSite(const void* node, Bind s) {
        if (effective(s) != Bind::Bound) plan_.checked.insert(node);
    }
    void recordSuperCall(const void* node, Bind s) {
        if (effective(s) != Bind::Unbound) plan_.checked.insert(node);
    }
    void recordReturn(const void* node, Bind s, size_t depth) {
        if (depth == 0 && effective(s) != Bind::Bound) plan_.checked.insert(node);
    }
    void noteArrowWithSuper() { escaped_ = true; }

    void params(const std::vector<ast::Param>& ps, Bind s, size_t depth) {
        RegionRecorder r(*this, s, depth);
        r.walkParams(ps);
    }

    Flow list(const std::vector<ast::StmtPtr>& stmts, Bind s, size_t depth) {
        Flow f{s, false};
        for (const auto& st : stmts) {
            if (!st) continue;
            // Code after a `return` or a `throw` in the same list never runs,
            // so nothing in it is tested.
            if (f.terminated) break;
            f = stmt(*st, f.state, depth);
        }
        return f;
    }

    Flow stmt(const ast::Stmt& st, Bind s, size_t depth) {
        if (const auto* es = dynamic_cast<const ast::ExprStmt*>(&st)) {
            return {expr(*es->expr, s, depth)};
        }
        if (const auto* vd = dynamic_cast<const ast::VarDecl*>(&st)) {
            Bind after = vd->init ? expr(*vd->init, s, depth) : s;
            if (vd->pattern) after = region(st, after, depth);
            return {after};
        }
        if (const auto* ret = dynamic_cast<const ast::ReturnStmt*>(&st)) {
            const Bind after = ret->value ? expr(*ret->value, s, depth) : s;
            recordReturn(ret, after, depth);
            return {after, true};
        }
        if (const auto* th = dynamic_cast<const ast::ThrowStmt*>(&st)) {
            if (th->value) expr(*th->value, s, depth);
            return {s, true};
        }
        if (const auto* blk = dynamic_cast<const ast::BlockStmt*>(&st)) {
            return list(blk->stmts, s, depth);
        }
        if (const auto* ifs = dynamic_cast<const ast::IfStmt*>(&st)) {
            const Bind c = expr(*ifs->condition, s, depth);
            const Flow t = list(ifs->thenBody, c, depth);
            const Flow e = list(ifs->elseBody, c, depth);
            if (t.terminated && e.terminated) return {join(t.state, e.state), true};
            if (t.terminated) return {e.state};
            if (e.terminated) return {t.state};
            return {join(t.state, e.state)};
        }
        // A nested function declaration binds a receiver of its own.
        if (dynamic_cast<const ast::FunctionDecl*>(&st)) return {s};
        return {region(st, s, depth)};
    }

    // A statement or expression followed only as far as its sites: see the
    // file comment.
    template <typename Node>
    Bind region(const Node& n, Bind s, size_t depth) {
        if (superCallsIn(n) == 0) {
            RegionRecorder r(*this, s, depth);
            n.accept(r);
            return s;
        }
        const Bind wide = deferred(s);
        RegionRecorder r(*this, wide, depth);
        n.accept(r);
        return wide;
    }

    Bind exprs(const std::vector<ast::ExprPtr>& es, Bind s, size_t depth) {
        for (const auto& e : es) {
            if (e) s = expr(*e, s, depth);
        }
        return s;
    }

    Bind expr(const ast::Expr& e, Bind s, size_t depth) {
        if (dynamic_cast<const ast::ThisExpr*>(&e)) {
            recordThisSite(&e, s);
            return s;
        }
        if (const auto* sc = dynamic_cast<const ast::SuperCall*>(&e)) {
            if (sc->baseExpr) s = expr(*sc->baseExpr, s, depth);
            s = exprs(sc->args, s, depth);
            recordSuperCall(sc, s);
            return Bind::Bound;
        }
        if (const auto* sm = dynamic_cast<const ast::SuperMember*>(&e)) {
            // 13.3.7.1: the receiver is read before the key is evaluated.
            recordThisSite(sm, s);
            if (sm->propertyExpr) s = expr(*sm->propertyExpr, s, depth);
            return s;
        }
        if (const auto* fn = dynamic_cast<const ast::FunctionExpr*>(&e)) {
            if (!fn->isArrow) return s;
            const bool callsSuper = superCallsIn(e) != 0;
            const Bind inner = deferred(effective(s));
            params(fn->params, inner, depth + 1);
            list(fn->body, inner, depth + 1);
            if (callsSuper) noteArrowWithSuper();
            return s;
        }
        if (const auto* bin = dynamic_cast<const ast::Binary*>(&e)) {
            const ast::BinaryOp op = bin->op;
            const bool shortCircuit = op == ast::BinaryOp::LogicalAnd ||
                                      op == ast::BinaryOp::LogicalOr ||
                                      op == ast::BinaryOp::NullishCoalescing ||
                                      op == ast::BinaryOp::LogicalAndAssign ||
                                      op == ast::BinaryOp::LogicalOrAssign ||
                                      op == ast::BinaryOp::NullishAssign;
            Bind left = s;
            // An assignment evaluates its target's object before its value; an
            // identifier target evaluates nothing.
            if (ast::isAssignOp(op)) {
                if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(bin->lhs.get())) {
                    if (mem->optional) return region(e, s, depth);
                    left = expr(*mem->object, s, depth);
                } else if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(bin->lhs.get())) {
                    if (ix->optional) return region(e, s, depth);
                    left = expr(*ix->object, s, depth);
                    left = expr(*ix->index, left, depth);
                } else if (const auto* sm = dynamic_cast<const ast::SuperMember*>(bin->lhs.get())) {
                    left = expr(*sm, s, depth);
                } else if (!dynamic_cast<const ast::Ident*>(bin->lhs.get())) {
                    left = expr(*bin->lhs, s, depth);
                }
            } else {
                left = expr(*bin->lhs, s, depth);
            }
            const Bind right = expr(*bin->rhs, left, depth);
            return shortCircuit ? join(left, right) : right;
        }
        if (const auto* tern = dynamic_cast<const ast::Ternary*>(&e)) {
            const Bind c = expr(*tern->condition, s, depth);
            return join(expr(*tern->thenExpr, c, depth), expr(*tern->elseExpr, c, depth));
        }
        if (const auto* un = dynamic_cast<const ast::Unary*>(&e)) {
            return expr(*un->operand, s, depth);
        }
        if (const auto* call = dynamic_cast<const ast::Call*>(&e)) {
            if (call->optional || ast::containsOptionalLink(e)) return region(e, s, depth);
            return exprs(call->args, expr(*call->callee, s, depth), depth);
        }
        if (const auto* nw = dynamic_cast<const ast::NewExpr*>(&e)) {
            return exprs(nw->args, expr(*nw->callee, s, depth), depth);
        }
        if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(&e)) {
            if (mem->optional || ast::containsOptionalLink(e)) return region(e, s, depth);
            return expr(*mem->object, s, depth);
        }
        if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(&e)) {
            if (ix->optional || ast::containsOptionalLink(e)) return region(e, s, depth);
            return expr(*ix->index, expr(*ix->object, s, depth), depth);
        }
        if (const auto* arr = dynamic_cast<const ast::ArrayLit*>(&e)) {
            return exprs(arr->elements, s, depth);
        }
        if (const auto* sp = dynamic_cast<const ast::SpreadElement*>(&e)) {
            return expr(*sp->argument, s, depth);
        }
        if (const auto* tl = dynamic_cast<const ast::TemplateLit*>(&e)) {
            return exprs(tl->exprs, s, depth);
        }
        if (const auto* ol = dynamic_cast<const ast::ObjectLit*>(&e)) {
            for (const auto& p : ol->props) {
                if (p.keyExpr) s = expr(*p.keyExpr, s, depth);
                // A method or accessor of the literal is a function with a
                // receiver of its own; a value is evaluated here.
                if (p.value && !p.isMethod && p.accessor == ast::AccessorKind::None) {
                    s = expr(*p.value, s, depth);
                }
            }
            return s;
        }
        return region(e, s, depth);
    }

private:
    DerivedCtorPlan& plan_;
    bool escaped_ = false;
};

void RegionRecorder::visit(const ast::ThisExpr& n) { p_.recordThisSite(&n, s_); }
void RegionRecorder::visit(const ast::SuperMember& n) {
    p_.recordThisSite(&n, s_);
    types::Walker::visit(n);
}
void RegionRecorder::visit(const ast::SuperCall& n) {
    types::Walker::visit(n);
    p_.recordSuperCall(&n, s_);
}
void RegionRecorder::visit(const ast::ReturnStmt& n) {
    types::Walker::visit(n);
    p_.recordReturn(&n, s_, depth_);
}
void RegionRecorder::visit(const ast::FunctionExpr& n) {
    if (!n.isArrow) return;
    const bool callsSuper = superCallsIn(n) != 0;
    RegionRecorder inner(p_, deferred(p_.effective(s_)), depth_ + 1);
    inner.walkParams(n.params);
    inner.walkList(n.body);
    if (callsSuper) p_.noteArrowWithSuper();
}

}  // namespace

DerivedCtorPlan planDerivedCtor(const std::vector<ast::Param>& params,
                                const std::vector<ast::StmtPtr>& body) {
    DerivedCtorPlan plan;
    for (const auto& st : body) {
        if (!st) continue;
        plan.superCalls += superCallsIn(*st);
        if (const auto* es = dynamic_cast<const ast::ExprStmt*>(st.get())) {
            if (dynamic_cast<const ast::SuperCall*>(es->expr.get())) {
                plan.topLevelSuperStatement = true;
            }
        }
    }
    size_t paramSuperCalls = 0;
    for (const auto& p : params) {
        if (p.defaultValue) paramSuperCalls += superCallsIn(*p.defaultValue);
    }
    plan.superCalls += paramSuperCalls;
    Planner planner(plan);
    // The parameters' defaults are evaluated before the body, with nothing
    // bound yet — unless one of them calls `super()`, which is legal and makes
    // every site from there on one the walk does not follow.
    const Bind entry = paramSuperCalls == 0 ? Bind::Unbound : Bind::Either;
    planner.params(params, entry, 0);
    const Flow end = planner.list(body, entry, 0);
    if (!end.terminated && planner.effective(end.state) != Bind::Bound) plan.endChecked = true;
    return plan;
}

}  // namespace bronze::lower
