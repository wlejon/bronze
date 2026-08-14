// Lifting `yield` out of expression position: the rewrite that turns a
// generator body into one where every suspension stands alone at a statement
// boundary. `yield_lift.h` states why the state machine needs that; this file is
// how each expression form gets there.
//
// Two rules cover almost everything:
//
//   - a list evaluated left to right (arguments, operands, elements, a call's
//     receiver) has every element up to the LAST one that can suspend pinned
//     into a temporary, because the resumption between two of them runs
//     arbitrary code and ECMA-262 already evaluated the earlier ones;
//   - an operand evaluated only on SOME paths (`&&`, `||`, `??`, `?:`) becomes
//     an `if` over a temporary, because there is no expression form that can
//     hold a suspension on one arm only.
//
// The rest is reference positions — a compound assignment reads its target
// before the right side runs — and the statement forms whose sub-expression
// re-runs (a loop condition), which have to keep the lifted statements inside
// the loop.

#include "ast/yield_lift.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bronze::ast {
namespace {

ExprPtr identExpr(const std::string& name, Span span) {
    auto n = std::make_unique<Ident>();
    n->span = span;
    n->name = name;
    return n;
}

ExprPtr undefinedExpr(Span span) {
    auto n = std::make_unique<UndefinedLit>();
    n->span = span;
    return n;
}

ExprPtr binaryExpr(BinaryOp op, ExprPtr lhs, ExprPtr rhs, Span span) {
    auto n = std::make_unique<Binary>();
    n->span = span;
    n->op = op;
    n->lhs = std::move(lhs);
    n->rhs = std::move(rhs);
    return n;
}

StmtPtr exprStmt(ExprPtr e, Span span) {
    auto n = std::make_unique<ExprStmt>();
    n->span = span;
    n->expr = std::move(e);
    return n;
}

// `let <name> = <init>;` — never `const`, because the two rewrites that need a
// temporary at all (`&&` and `?:`) declare it before the branch that fills it.
StmtPtr letDecl(const std::string& name, ExprPtr init, Span span) {
    auto n = std::make_unique<VarDecl>();
    n->span = span;
    n->name = name;
    n->init = std::move(init);
    return n;
}

class YieldLifter {
public:
    YieldLifter(std::string prefix, DiagnosticSink& diags)
        : prefix_(std::move(prefix)), diags_(diags) {}

    bool ok() const { return ok_; }

    void liftStmts(std::vector<StmtPtr>& list) {
        std::vector<StmtPtr> out;
        out.reserve(list.size());
        for (auto& s : list) liftStmt(s, out);
        list = std::move(out);
    }

private:
    std::string prefix_;
    DiagnosticSink& diags_;
    size_t nextTemp_ = 0;
    bool ok_ = true;

    // The refusal names the FORM it is refusing, taken from the subtree that
    // holds it: every position below admits `yield` and `yield*` alike, and a
    // message that guessed would send half its readers after the wrong rule.
    void refuse(Span span, YieldForms forms, const std::string& what) {
        diags_.error(span,
                     "unsupported construct: " + std::string(yieldFormName(forms)) + " " + what);
        ok_ = false;
    }

    std::string freshTemp() { return prefix_ + "t" + std::to_string(nextTemp_++); }

    // Is this expression's value unchanged by whatever code runs during a
    // suspension? A literal is; so is one of this pass's own temporaries, which
    // has no name a program could spell and is written exactly once. Everything
    // else — an identifier included — has to be pinned, because the caller
    // between two `next()` calls can rebind it.
    bool isStable(const Expr& e) const {
        if (dynamic_cast<const NumberLit*>(&e) || dynamic_cast<const StringLit*>(&e) ||
            dynamic_cast<const BoolLit*>(&e) || dynamic_cast<const NullLit*>(&e) ||
            dynamic_cast<const UndefinedLit*>(&e)) {
            return true;
        }
        const auto* ident = dynamic_cast<const Ident*>(&e);
        if (!ident) return false;
        // `console.log` is one Ident spelling a whole member path, folded by the
        // parser: it names no binding at all, so nothing can rebind it — and
        // pinning it would turn the name lowering dispatches on into a
        // temporary holding an unresolvable reference.
        if (consoleStreamOf(ident->name) != ConsoleStream::None) return true;
        return ident->name.rfind(prefix_, 0) == 0;
    }

    // `let tN = <e>;` ahead of the suspension, so that the value ECMA-262
    // computed BEFORE it survives one.
    ExprPtr pin(ExprPtr e, std::vector<StmtPtr>& pre) {
        if (!e || isStable(*e)) return e;
        return pinAlways(std::move(e), pre);
    }

    // The same, for a position that must end up an `Ident` whatever it held: the
    // base of a compound assignment's target, which is read once and written
    // once and so has to be REBUILT rather than cloned.
    ExprPtr pinAlways(ExprPtr e, std::vector<StmtPtr>& pre) {
        const Span span = e->span;
        const std::string name = freshTemp();
        pre.push_back(letDecl(name, std::move(e), span));
        return identExpr(name, span);
    }

    // Every slot in evaluation order. Each one up to the last that can suspend
    // is lifted and then pinned; the ones after it are untouched, because
    // nothing suspends between them and their use.
    void liftSlots(const std::vector<ExprPtr*>& slots, std::vector<StmtPtr>& pre) {
        size_t last = SIZE_MAX;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (*slots[i] && containsYield(**slots[i])) last = i;
        }
        if (last == SIZE_MAX) return;
        for (size_t i = 0; i <= last; ++i) {
            if (!*slots[i]) continue;
            *slots[i] = lift(std::move(*slots[i]), pre);
            if (i < last) *slots[i] = pin(std::move(*slots[i]), pre);
        }
    }

    void liftList(std::vector<ExprPtr>& list, std::vector<StmtPtr>& pre) {
        std::vector<ExprPtr*> slots;
        slots.reserve(list.size());
        for (auto& e : list) slots.push_back(&e);
        liftSlots(slots, pre);
    }

    // --- expressions ------------------------------------------------------
    ExprPtr lift(ExprPtr e, std::vector<StmtPtr>& pre) {
        if (!e || !ok_ || !containsYield(*e)) return e;

        if (auto* y = dynamic_cast<YieldExpr*>(e.get())) {
            y->argument = lift(std::move(y->argument), pre);
            const Span span = y->span;
            const std::string name = freshTemp();
            pre.push_back(letDecl(name, std::move(e), span));
            return identExpr(name, span);
        }
        if (auto* bin = dynamic_cast<Binary*>(e.get())) return liftBinary(std::move(e), *bin, pre);
        if (auto* tern = dynamic_cast<Ternary*>(e.get())) return liftTernary(std::move(e), *tern, pre);
        if (auto* un = dynamic_cast<Unary*>(e.get())) {
            switch (un->op) {
                case UnaryOp::Delete:
                case UnaryOp::PreInc:
                case UnaryOp::PreDec:
                case UnaryOp::PostInc:
                case UnaryOp::PostDec:
                    // These take a REFERENCE, not a value: there is no
                    // intermediate to name, and rewriting one would have to
                    // invent a target expression the source never wrote.
                    refuse(un->span, yieldFormsIn(*un),
                           "in the operand of `" + std::string(unaryOpName(un->op)) +
                               "`, which names a reference rather than a value");
                    return e;
                default:
                    break;
            }
            un->operand = lift(std::move(un->operand), pre);
            return e;
        }
        if (auto* mem = dynamic_cast<MemberAccess*>(e.get())) {
            if (mem->optional) return refuseOptional(std::move(e), mem->span, yieldFormsIn(*mem));
            mem->object = lift(std::move(mem->object), pre);
            return e;
        }
        if (auto* idx = dynamic_cast<IndexAccess*>(e.get())) {
            if (idx->optional) return refuseOptional(std::move(e), idx->span, yieldFormsIn(*idx));
            liftSlots({&idx->object, &idx->index}, pre);
            return e;
        }
        if (auto* call = dynamic_cast<Call*>(e.get())) return liftCall(std::move(e), *call, pre);
        if (auto* nw = dynamic_cast<NewExpr*>(e.get())) {
            std::vector<ExprPtr*> slots{&nw->callee};
            for (auto& a : nw->args) slots.push_back(&a);
            liftSlots(slots, pre);
            return e;
        }
        if (auto* sc = dynamic_cast<SuperCall*>(e.get())) {
            liftList(sc->args, pre);
            return e;
        }
        if (auto* arr = dynamic_cast<ArrayLit*>(e.get())) {
            liftList(arr->elements, pre);
            return e;
        }
        if (auto* obj = dynamic_cast<ObjectLit*>(e.get())) {
            std::vector<ExprPtr*> slots;
            for (auto& p : obj->props) {
                slots.push_back(&p.keyExpr);
                slots.push_back(&p.value);
            }
            liftSlots(slots, pre);
            return e;
        }
        if (auto* tpl = dynamic_cast<TemplateLit*>(e.get())) {
            liftList(tpl->exprs, pre);
            return e;
        }
        if (auto* tt = dynamic_cast<TaggedTemplate*>(e.get())) {
            std::vector<ExprPtr*> slots{&tt->tag};
            for (auto& exp : tt->templateLit->exprs) slots.push_back(&exp);
            liftSlots(slots, pre);
            return e;
        }
        if (auto* spread = dynamic_cast<SpreadElement*>(e.get())) {
            spread->argument = lift(std::move(spread->argument), pre);
            return e;
        }
        if (auto* da = dynamic_cast<DestructuringAssign*>(e.get())) {
            if (patternHasYield(da->pattern.get())) {
                refuse(da->span, patternForms(da->pattern.get()),
                       "in the default value of a destructuring pattern");
                return e;
            }
            da->value = lift(std::move(da->value), pre);
            return e;
        }
        if (auto* di = dynamic_cast<DynamicImportExpr*>(e.get())) {
            di->specifier = lift(std::move(di->specifier), pre);
            return e;
        }
        refuse(e->span, yieldFormsIn(*e), "in a position bronze cannot lift it out of");
        return e;
    }

    ExprPtr refuseOptional(ExprPtr e, Span span, YieldForms forms) {
        refuse(span, forms,
               "inside an optional chain (a `?.` link decides at run time whether the rest of "
               "the chain runs at all, and the suspension would sit on only one of those paths)");
        return e;
    }

    // Which forms a pattern's own expressions hold, for the two refusals that
    // are about a pattern rather than about an expression. Its own walk because
    // a `BindingPattern` is not a `Node` and so cannot be handed to
    // `yieldFormsIn` whole.
    YieldForms patternForms(const BindingPattern* pattern) const {
        YieldForms forms = YieldForms::None;
        if (!pattern) return forms;
        for (const auto& elem : pattern->elements) {
            if (elem.keyExpr) forms = forms | yieldFormsIn(*elem.keyExpr);
            if (elem.defaultValue) forms = forms | yieldFormsIn(*elem.defaultValue);
            forms = forms | patternForms(elem.pattern.get());
        }
        return forms;
    }

    bool patternHasYield(const BindingPattern* pattern) const {
        if (!pattern) return false;
        for (const auto& elem : pattern->elements) {
            if (elem.keyExpr && containsYield(*elem.keyExpr)) return true;
            if (elem.defaultValue && containsYield(*elem.defaultValue)) return true;
            if (patternHasYield(elem.pattern.get())) return true;
        }
        return false;
    }

    ExprPtr liftBinary(ExprPtr e, Binary& bin, std::vector<StmtPtr>& pre) {
        switch (bin.op) {
            case BinaryOp::LogicalAnd:
            case BinaryOp::LogicalOr:
            case BinaryOp::NullishCoalescing:
                if (containsYield(*bin.rhs)) return liftShortCircuit(bin, pre);
                bin.lhs = lift(std::move(bin.lhs), pre);
                return e;
            default:
                break;
        }
        if (isCompoundAssignOp(bin.op)) return liftCompoundAssign(std::move(e), bin, pre);
        if (bin.op == BinaryOp::Assign) {
            stabilizeTarget(bin.lhs, *bin.rhs, pre);
            if (!ok_) return e;
            bin.rhs = lift(std::move(bin.rhs), pre);
            return e;
        }
        liftSlots({&bin.lhs, &bin.rhs}, pre);
        return e;
    }

    // `a && (yield b)` cannot stay an expression: the suspension is on one path
    // and the value is the join of two. 13.13.1 evaluates the left side, tests
    // it, and evaluates the right ONLY if the test passed — which is an `if`.
    ExprPtr liftShortCircuit(Binary& bin, std::vector<StmtPtr>& pre) {
        const Span span = bin.span;
        ExprPtr left = lift(std::move(bin.lhs), pre);
        const std::string name = freshTemp();
        pre.push_back(letDecl(name, std::move(left), span));

        ExprPtr cond;
        if (bin.op == BinaryOp::LogicalAnd) {
            cond = identExpr(name, span);
        } else if (bin.op == BinaryOp::LogicalOr) {
            auto notted = std::make_unique<Unary>();
            notted->span = span;
            notted->op = UnaryOp::Not;
            notted->operand = identExpr(name, span);
            cond = std::move(notted);
        } else {
            // `??` fires on null and on undefined and on nothing else, which is
            // exactly what `is.nullish` means; spelled out here because this
            // pass produces source-level nodes and there is no such operator.
            auto nullLit = std::make_unique<NullLit>();
            nullLit->span = span;
            cond = binaryExpr(
                BinaryOp::LogicalOr,
                binaryExpr(BinaryOp::StrictEq, identExpr(name, span), std::move(nullLit), span),
                binaryExpr(BinaryOp::StrictEq, identExpr(name, span), undefinedExpr(span), span),
                span);
        }

        std::vector<StmtPtr> body;
        ExprPtr right = lift(std::move(bin.rhs), body);
        body.push_back(exprStmt(
            binaryExpr(BinaryOp::Assign, identExpr(name, span), std::move(right), span), span));

        auto ifStmt = std::make_unique<IfStmt>();
        ifStmt->span = span;
        ifStmt->condition = std::move(cond);
        ifStmt->thenBody = std::move(body);
        pre.push_back(std::move(ifStmt));
        return identExpr(name, span);
    }

    ExprPtr liftTernary(ExprPtr e, Ternary& tern, std::vector<StmtPtr>& pre) {
        const bool armSuspends =
            containsYield(*tern.thenExpr) || containsYield(*tern.elseExpr);
        tern.condition = lift(std::move(tern.condition), pre);
        if (!armSuspends) return e;

        const Span span = tern.span;
        const std::string name = freshTemp();
        pre.push_back(letDecl(name, undefinedExpr(span), span));

        auto arm = [&](ExprPtr value) {
            std::vector<StmtPtr> out;
            ExprPtr lifted = lift(std::move(value), out);
            out.push_back(exprStmt(
                binaryExpr(BinaryOp::Assign, identExpr(name, span), std::move(lifted), span),
                span));
            return out;
        };
        auto ifStmt = std::make_unique<IfStmt>();
        ifStmt->span = span;
        ifStmt->condition = std::move(tern.condition);
        ifStmt->thenBody = arm(std::move(tern.thenExpr));
        ifStmt->elseBody = arm(std::move(tern.elseExpr));
        pre.push_back(std::move(ifStmt));
        return identExpr(name, span);
    }

    // The base of an assignment target, pinned so that the suspension on the
    // right cannot change WHICH object is written. The target itself stays a
    // reference expression; only what it hangs off becomes a temporary.
    void stabilizeTarget(ExprPtr& target, const Expr& subject, std::vector<StmtPtr>& pre) {
        if (auto* mem = dynamic_cast<MemberAccess*>(target.get())) {
            if (mem->optional) {
                refuseOptional(nullptr, mem->span, yieldFormsIn(subject));
                return;
            }
            mem->object = pinAlways(lift(std::move(mem->object), pre), pre);
            return;
        }
        if (auto* idx = dynamic_cast<IndexAccess*>(target.get())) {
            if (idx->optional) {
                refuseOptional(nullptr, idx->span, yieldFormsIn(subject));
                return;
            }
            idx->object = pinAlways(lift(std::move(idx->object), pre), pre);
            idx->index = pinAlways(lift(std::move(idx->index), pre), pre);
            return;
        }
        if (dynamic_cast<Ident*>(target.get())) return;
        refuse(target->span, yieldFormsIn(subject),
               "on the right of an assignment to this target form");
    }

    // `x += yield v` reads `x` BEFORE the right side runs (13.15.2 step 1), so
    // the read has to happen — and be pinned — ahead of the suspension. The
    // target is stabilized first, which leaves it in one of the three forms this
    // can rebuild: `t`, `t.k` and `t[i]`.
    ExprPtr liftCompoundAssign(ExprPtr e, Binary& bin, std::vector<StmtPtr>& pre) {
        const YieldForms rhsForms = yieldFormsIn(*bin.rhs);
        stabilizeTarget(bin.lhs, *bin.rhs, pre);
        if (!ok_) return e;
        const Span span = bin.span;
        ExprPtr readBack = rebuildTarget(*bin.lhs);
        if (!readBack) {
            refuse(span, rhsForms, "on the right of a compound assignment to this target form");
            return e;
        }
        ExprPtr old = pinAlways(std::move(readBack), pre);
        ExprPtr rhs = lift(std::move(bin.rhs), pre);
        return binaryExpr(
            BinaryOp::Assign, std::move(bin.lhs),
            binaryExpr(compoundAssignBase(bin.op), std::move(old), std::move(rhs), span), span);
    }

    // A second expression denoting the same reference, for the READ half of a
    // compound assignment. Only ever called on a target `stabilizeTarget` has
    // already reduced to identifiers, so there is nothing to copy but names.
    ExprPtr rebuildTarget(const Expr& target) const {
        if (const auto* ident = dynamic_cast<const Ident*>(&target)) {
            return identExpr(ident->name, ident->span);
        }
        if (const auto* mem = dynamic_cast<const MemberAccess*>(&target)) {
            const auto* base = dynamic_cast<const Ident*>(mem->object.get());
            if (!base) return nullptr;
            auto out = std::make_unique<MemberAccess>();
            out->span = mem->span;
            out->object = identExpr(base->name, base->span);
            out->property = mem->property;
            return out;
        }
        if (const auto* idx = dynamic_cast<const IndexAccess*>(&target)) {
            const auto* base = dynamic_cast<const Ident*>(idx->object.get());
            const auto* key = dynamic_cast<const Ident*>(idx->index.get());
            if (!base || !key) return nullptr;
            auto out = std::make_unique<IndexAccess>();
            out->span = idx->span;
            out->object = identExpr(base->name, base->span);
            out->index = identExpr(key->name, key->span);
            return out;
        }
        return nullptr;
    }

    // A call evaluates its callee — and, for a method call, the RECEIVER the
    // callee was read off — before its arguments. Pinning the receiver's object
    // rather than the callee keeps the call a member expression, which is the
    // only way `this` inside the method stays the object the source named.
    ExprPtr liftCall(ExprPtr e, Call& call, std::vector<StmtPtr>& pre) {
        if (call.optional) return refuseOptional(std::move(e), call.span, yieldFormsIn(call));
        bool argSuspends = false;
        for (const auto& a : call.args) {
            if (a && containsYield(*a)) argSuspends = true;
        }
        if (auto* mem = dynamic_cast<MemberAccess*>(call.callee.get())) {
            if (mem->optional) return refuseOptional(std::move(e), mem->span, yieldFormsIn(call));
            mem->object = lift(std::move(mem->object), pre);
            if (argSuspends) mem->object = pin(std::move(mem->object), pre);
        } else if (auto* idx = dynamic_cast<IndexAccess*>(call.callee.get())) {
            if (idx->optional) return refuseOptional(std::move(e), idx->span, yieldFormsIn(call));
            liftSlots({&idx->object, &idx->index}, pre);
            if (argSuspends) {
                idx->object = pin(std::move(idx->object), pre);
                idx->index = pin(std::move(idx->index), pre);
            }
        } else {
            call.callee = lift(std::move(call.callee), pre);
            if (argSuspends) call.callee = pin(std::move(call.callee), pre);
        }
        liftList(call.args, pre);
        return e;
    }

    // --- statements -------------------------------------------------------
    void liftStmt(StmtPtr& s, std::vector<StmtPtr>& out) {
        if (!s || !ok_) {
            if (s) out.push_back(std::move(s));
            return;
        }
        if (auto* es = dynamic_cast<ExprStmt*>(s.get())) {
            // `yield v;` on its own needs no temporary at all: nothing reads the
            // value, so the suspension IS the statement.
            if (auto* y = dynamic_cast<YieldExpr*>(es->expr.get())) {
                y->argument = lift(std::move(y->argument), out);
                out.push_back(std::move(s));
                return;
            }
            es->expr = lift(std::move(es->expr), out);
            out.push_back(std::move(s));
            return;
        }
        if (auto* vd = dynamic_cast<VarDecl*>(s.get())) {
            if (vd->pattern && patternHasYield(vd->pattern.get())) {
                refuse(vd->span, patternForms(vd->pattern.get()),
                       "in the default value of a destructuring declaration");
            }
            vd->init = lift(std::move(vd->init), out);
            out.push_back(std::move(s));
            return;
        }
        if (auto* ret = dynamic_cast<ReturnStmt*>(s.get())) {
            ret->value = lift(std::move(ret->value), out);
            out.push_back(std::move(s));
            return;
        }
        if (auto* th = dynamic_cast<ThrowStmt*>(s.get())) {
            th->value = lift(std::move(th->value), out);
            out.push_back(std::move(s));
            return;
        }
        if (auto* ifs = dynamic_cast<IfStmt*>(s.get())) {
            ifs->condition = lift(std::move(ifs->condition), out);
            liftStmts(ifs->thenBody);
            liftStmts(ifs->elseBody);
            out.push_back(std::move(s));
            return;
        }
        if (auto* block = dynamic_cast<BlockStmt*>(s.get())) {
            liftStmts(block->stmts);
            out.push_back(std::move(s));
            return;
        }
        if (auto* lbl = dynamic_cast<LabeledStmt*>(s.get())) {
            // The label has to stay in front of the loop it fronts, so the
            // labelled statement is rewritten IN PLACE rather than preceded by
            // anything: a lifted statement between the two would break the
            // `break lbl` it exists for.
            std::vector<StmtPtr> inner;
            liftStmt(lbl->body, inner);
            if (inner.size() != 1) {
                refuse(lbl->span, yieldFormsIn(*lbl), "in the head of a labelled statement");
                return;
            }
            lbl->body = std::move(inner.front());
            out.push_back(std::move(s));
            return;
        }
        if (auto* wh = dynamic_cast<WhileStmt*>(s.get())) {
            liftWhile(*wh);
            out.push_back(std::move(s));
            return;
        }
        if (auto* dw = dynamic_cast<DoWhileStmt*>(s.get())) {
            if (containsYield(*dw->condition)) {
                // `continue` in a do-while jumps to the CONDITION, and the
                // rewrite that gives the condition a statement of its own would
                // have to put it where `continue` does not reach.
                refuse(dw->condition->span, yieldFormsIn(*dw->condition),
                       "in the condition of a `do`/`while` (a `continue` in the body jumps "
                       "straight to that condition, and the lifted statements have nowhere to "
                       "sit that a `continue` reaches)");
            }
            liftStmts(dw->body);
            out.push_back(std::move(s));
            return;
        }
        if (auto* fs = dynamic_cast<ForStmt*>(s.get())) {
            liftFor(*fs, out);
            out.push_back(std::move(s));
            return;
        }
        // The head of either form runs ONCE, so its lifted statements belong
        // ahead of the loop. The BODY needs nothing from this pass beyond the
        // ordinary lifting: the one thing a suspension in it would strand is
        // the iteration record the loop is stepping, which has no name to lift
        // it under — so lowering keeps that record in a frame slot of its own
        // (Lowerer::loopIterSlotName) instead of in SSA.
        if (auto* fo = dynamic_cast<ForOfStmt*>(s.get())) {
            fo->iterable = lift(std::move(fo->iterable), out);
            liftStmts(fo->body);
            out.push_back(std::move(s));
            return;
        }
        if (auto* fi = dynamic_cast<ForInStmt*>(s.get())) {
            fi->object = lift(std::move(fi->object), out);
            liftStmts(fi->body);
            out.push_back(std::move(s));
            return;
        }
        if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
            sw->discriminant = lift(std::move(sw->discriminant), out);
            for (auto& c : sw->cases) {
                if (c.test && containsYield(*c.test)) {
                    // A clause's test is evaluated in the selection sequence
                    // (14.12.4), between the tests before and after it, and a
                    // statement lifted out of it would run whether or not the
                    // selection ever reached that clause.
                    refuse(c.span, yieldFormsIn(*c.test), "in the test of a `case` clause");
                }
                liftStmts(c.body);
            }
            out.push_back(std::move(s));
            return;
        }
        if (auto* tr = dynamic_cast<TryStmt*>(s.get())) {
            liftStmts(tr->body);
            liftStmts(tr->catchBody);
            if (containsYield(tr->finallyBody)) {
                // A `finally` body is lowered once per path that leaves the
                // protected region, so a suspension inside one would be several
                // suspension points wearing one source position — and the
                // resumption would have to choose between them.
                refuse(tr->span, yieldFormsIn(tr->finallyBody), "inside a `finally` block");
            }
            liftStmts(tr->finallyBody);
            out.push_back(std::move(s));
            return;
        }
        // Everything else — `break`, `continue`, a nested declaration — either
        // holds no expression of this generator's or opens a scope of its own.
        out.push_back(std::move(s));
    }

    // `while (yield c)` — the condition re-runs every iteration, so its lifted
    // statements have to run every iteration too. `while (true)` with an
    // explicit exit test is the one rewrite that keeps both `break` and
    // `continue` meaning what they meant: an unlabelled `break` still leaves
    // this loop, and a `continue` still jumps to the top, where the condition
    // now is.
    void liftWhile(WhileStmt& wh) {
        if (!containsYield(*wh.condition)) {
            liftStmts(wh.body);
            return;
        }
        const Span span = wh.condition->span;
        std::vector<StmtPtr> head;
        ExprPtr cond = lift(std::move(wh.condition), head);

        auto notted = std::make_unique<Unary>();
        notted->span = span;
        notted->op = UnaryOp::Not;
        notted->operand = std::move(cond);
        auto exit = std::make_unique<IfStmt>();
        exit->span = span;
        exit->condition = std::move(notted);
        auto brk = std::make_unique<BreakStmt>();
        brk->span = span;
        exit->thenBody.push_back(std::move(brk));
        head.push_back(std::move(exit));

        liftStmts(wh.body);
        for (auto& s : wh.body) head.push_back(std::move(s));
        wh.body = std::move(head);

        auto always = std::make_unique<BoolLit>();
        always->span = span;
        always->value = true;
        wh.condition = std::move(always);
    }

    void liftFor(ForStmt& fs, std::vector<StmtPtr>& out) {
        // The head runs once, so its lifted statements belong ahead of the loop.
        // The bindings it declares stay in the head, where the loop's scope is.
        for (auto& init : fs.init) {
            if (auto* vd = dynamic_cast<VarDecl*>(init.get())) {
                vd->init = lift(std::move(vd->init), out);
            } else if (auto* es = dynamic_cast<ExprStmt*>(init.get())) {
                es->expr = lift(std::move(es->expr), out);
            }
        }
        if (fs.update && containsYield(*fs.update)) {
            // The update runs on the back edge AND on every `continue`, and
            // there is no statement position that both of those reach.
            refuse(fs.update->span, yieldFormsIn(*fs.update),
                   "in the update clause of a `for` (a `continue` in the body runs that clause, "
                   "and the lifted statements have nowhere to sit that a `continue` reaches)");
        }
        if (fs.condition && containsYield(*fs.condition)) {
            const Span span = fs.condition->span;
            std::vector<StmtPtr> head;
            ExprPtr cond = lift(std::move(fs.condition), head);
            auto notted = std::make_unique<Unary>();
            notted->span = span;
            notted->op = UnaryOp::Not;
            notted->operand = std::move(cond);
            auto exit = std::make_unique<IfStmt>();
            exit->span = span;
            exit->condition = std::move(notted);
            auto brk = std::make_unique<BreakStmt>();
            brk->span = span;
            exit->thenBody.push_back(std::move(brk));
            head.push_back(std::move(exit));

            liftStmts(fs.body);
            for (auto& s : fs.body) head.push_back(std::move(s));
            fs.body = std::move(head);
            fs.condition = nullptr;
            return;
        }
        liftStmts(fs.body);
    }
};

}  // namespace

bool liftYields(std::vector<StmtPtr>& body, const std::string& tempPrefix,
                DiagnosticSink& diags) {
    YieldLifter lifter(tempPrefix, diags);
    lifter.liftStmts(body);
    return lifter.ok();
}

}  // namespace bronze::ast
