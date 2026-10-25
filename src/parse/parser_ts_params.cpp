// TypeScript parameter properties: `constructor(public x: number) {}`
// declares a parameter and assigns it to `this.x` when the constructor runs.
//
// Where the assignment goes is the whole question, because `this` does not
// exist in a derived class's constructor until `super()` returns: TypeScript
// writes the assignments immediately after the super call, wherever in the
// body that call is — after a statement that computes its arguments, inside
// an `if` arm, inside a `try`, or in the middle of an expression. A call
// that is a statement of its own is followed by one assignment statement per
// property; one nested in an expression becomes `(super(…), this.x = x,
// this)`, which still evaluates to what `super()` does (the new receiver).
// Every super call gets them, so each path through the constructor assigns
// once. A base class's constructor has no super call and takes them first.
//
// A `super()` inside an arrow function is the one placement left out: the
// assignments would have to run when the arrow is called, which is a rewrite
// of a different kind, so it is refused by name.

#include "parse/parser.h"
#include "parse/parser_ts_rewrite.h"

namespace bronze {

using namespace ast;
using namespace ts_rewrite;

namespace {

std::vector<ExprPtr> propertyAssignments(const std::vector<Param>& params) {
    std::vector<ExprPtr> out;
    for (const auto& p : params) {
        if (!p.isParameterProperty || p.name.empty()) continue;
        auto self = std::make_unique<ThisExpr>();
        self->span = p.span;
        out.push_back(makeBinary(BinaryOp::Assign, makeMember(std::move(self), p.name, p.span),
                                 makeIdent(p.name, p.span), p.span));
    }
    return out;
}

// The super calls nested inside expressions.
class NestedSuperCalls final : public ScopedWalk {
public:
    NestedSuperCalls(DiagnosticSink& diags, const std::vector<Param>& params)
        : ScopedWalk(diags), params_(params) {}
    size_t found = 0;

private:
    bool visitSlot(ExprPtr& p) override {
        auto* sc = dynamic_cast<SuperCall*>(p.get());
        if (!sc) return false;
        if (arrowDepth_ > 0) {
            fail(sc->span,
                 "unsupported construct: a super() call inside an arrow function in a "
                 "constructor with parameter properties; call super() in the constructor body");
            return true;
        }
        ++found;
        const Span span = sc->span;
        ExprPtr seq = std::move(p);
        for (auto& assign : propertyAssignments(params_)) {
            seq = makeBinary(BinaryOp::Comma, std::move(seq), std::move(assign), span);
        }
        auto self = std::make_unique<ThisExpr>();
        self->span = span;
        p = makeBinary(BinaryOp::Comma, std::move(seq), std::move(self), span);
        return true;
    }
    // A non-arrow function has a `super` of its own, if any; an arrow shares
    // the constructor's.
    bool enterFunction(const FunctionExpr*, bool isArrow) override {
        if (!isArrow) return false;
        ++arrowDepth_;
        return true;
    }
    void exitFunction(bool isArrow) override {
        if (isArrow) --arrowDepth_;
    }
    bool enterClassBody() override { return false; }

    const std::vector<Param>& params_;
    size_t arrowDepth_ = 0;
};

class SuperCallPlacer {
public:
    SuperCallPlacer(DiagnosticSink& diags, const std::vector<Param>& params)
        : params_(params), nested_(diags, params) {}

    bool run(std::vector<StmtPtr>& body) {
        place(body);
        if (!ok_) return false;
        if (found_ + nested_.found == 0) {
            insertAfter(body, 0, Span{});
        }
        return ok_;
    }

private:
    void expr(ExprPtr& e) {
        if (e && ok_) ok_ = nested_.walkExpr(e);
    }

    // The assignments as statements, at `at` in `list`. Returns how many.
    size_t insertAfter(std::vector<StmtPtr>& list, size_t at, Span span) {
        auto assigns = propertyAssignments(params_);
        for (size_t k = 0; k < assigns.size(); ++k) {
            const Span s = span.end ? span : assigns[k]->span;
            list.insert(list.begin() + static_cast<std::ptrdiff_t>(at + k),
                        makeExprStmt(std::move(assigns[k]), s));
        }
        return assigns.size();
    }

    void placeOne(StmtPtr& s) {
        std::vector<StmtPtr> one;
        one.push_back(std::move(s));
        place(one);
        if (one.size() == 1) {
            s = std::move(one[0]);
            return;
        }
        auto blk = std::make_unique<BlockStmt>();
        blk->span = one[0]->span;
        blk->stmts = std::move(one);
        s = std::move(blk);
    }

    void place(std::vector<StmtPtr>& list) {
        for (size_t i = 0; i < list.size() && ok_; ++i) {
            Stmt* s = list[i].get();
            if (auto* es = dynamic_cast<ExprStmt*>(s)) {
                if (dynamic_cast<SuperCall*>(es->expr.get())) {
                    ++found_;
                    i += insertAfter(list, i + 1, es->span);
                } else {
                    expr(es->expr);
                }
            } else if (auto* blk = dynamic_cast<BlockStmt*>(s)) {
                place(blk->stmts);
            } else if (auto* ifs = dynamic_cast<IfStmt*>(s)) {
                expr(ifs->condition);
                place(ifs->thenBody);
                place(ifs->elseBody);
            } else if (auto* wh = dynamic_cast<WhileStmt*>(s)) {
                expr(wh->condition);
                place(wh->body);
            } else if (auto* dw = dynamic_cast<DoWhileStmt*>(s)) {
                place(dw->body);
                expr(dw->condition);
            } else if (auto* fs = dynamic_cast<ForStmt*>(s)) {
                // The header is not a statement list anything can be inserted
                // into, so a super call there is an expression like any other.
                for (auto& init : fs->init) {
                    if (auto* ies = dynamic_cast<ExprStmt*>(init.get())) expr(ies->expr);
                    if (auto* ivd = dynamic_cast<VarDecl*>(init.get())) expr(ivd->init);
                }
                expr(fs->condition);
                expr(fs->update);
                place(fs->body);
            } else if (auto* fi = dynamic_cast<ForInStmt*>(s)) {
                expr(fi->object);
                place(fi->body);
            } else if (auto* fo = dynamic_cast<ForOfStmt*>(s)) {
                expr(fo->iterable);
                place(fo->body);
            } else if (auto* sw = dynamic_cast<SwitchStmt*>(s)) {
                expr(sw->discriminant);
                for (auto& c : sw->cases) {
                    expr(c.test);
                    place(c.body);
                }
            } else if (auto* lab = dynamic_cast<LabeledStmt*>(s)) {
                if (lab->body) placeOne(lab->body);
            } else if (auto* tr = dynamic_cast<TryStmt*>(s)) {
                place(tr->body);
                place(tr->catchBody);
                place(tr->finallyBody);
            } else if (auto* vd = dynamic_cast<VarDecl*>(s)) {
                expr(vd->init);
            } else if (auto* ret = dynamic_cast<ReturnStmt*>(s)) {
                expr(ret->value);
            } else if (auto* th = dynamic_cast<ThrowStmt*>(s)) {
                expr(th->value);
            }
            // A nested function or class declaration has a `super` of its
            // own, and nothing else holds an expression.
        }
    }

    const std::vector<Param>& params_;
    NestedSuperCalls nested_;
    size_t found_ = 0;
    bool ok_ = true;
};

}  // namespace

bool Parser::insertParameterProperties(FunctionExpr& ctor) {
    bool any = false;
    for (const auto& p : ctor.params) any = any || (p.isParameterProperty && !p.name.empty());
    if (!any) return true;
    return SuperCallPlacer(diags_, ctor.params).run(ctor.body);
}

}  // namespace bronze
