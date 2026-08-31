// The env-slot NUMBER PROOF: which of a function's environment slots hold a
// Number at every read, and the one walk of the body that decides it.

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/queries.h"
#include "ast/query_walk.h"
#include "lower/lowerer.h"

namespace bronze::lower {

namespace {

// Everything the env-slot number proof has to know about how a function's body
// treats a set of candidate names, in ONE walk of the subtree.
//
// The two halves answer different questions and both are needed. `disqualified`
// is about the BINDING: a name a nested scope declares again, destructures,
// binds in a `catch` or a `for-of` head is a name whose slot this analysis
// cannot claim to have seen every write to, and the honest answer there is to
// drop it rather than to model the shadowing. `writes` is about the VALUES: the
// expression stored by every simple assignment and by the declaration itself.
//
// `++` and `--` appear in neither: they write ToNumeric of the slot's own
// contents, which is a Number exactly when the slot already held one, so the
// assumption under test proves them and there is no expression to record.
class EnvSlotWriteScan final : public ast::detail::IdentVisitor {
public:
    EnvSlotWriteScan(const std::unordered_set<std::string>& candidates,
                     const std::unordered_set<const ast::VarDecl*>& ownDecls)
        : candidates_(candidates), ownDecls_(ownDecls) {}

    std::unordered_set<std::string> disqualified;
    std::unordered_map<std::string, std::vector<const ast::Expr*>> writes;

    void visit(const ast::VarDecl& v) override {
        // Every declaration OTHER than the owner's own is a second binding of
        // the name as far as this pass is concerned — the walk descends into
        // nested functions and blocks, and a name that means two bindings is
        // one this analysis has not seen every write to. The owner's own
        // declarations arrive in `ownDecls_`, and their initializers are
        // proved by the caller alongside the assignments.
        if (ownDecls_.count(&v) != 0) {
            ast::detail::IdentVisitor::visit(v);
            return;
        }
        if (v.pattern) {
            for (const auto& n : ast::patternBoundNames(*v.pattern)) drop(n);
        } else {
            drop(v.name);
        }
        ast::detail::IdentVisitor::visit(v);
    }
    void visit(const ast::FunctionDecl& f) override {
        drop(f.name);
        dropParams(f.params);
        ast::detail::IdentVisitor::visit(f);
    }
    void visit(const ast::FunctionExpr& f) override {
        drop(f.name);
        dropParams(f.params);
        ast::detail::IdentVisitor::visit(f);
    }
    void visit(const ast::ClassDecl& c) override {
        drop(c.name);
        ast::detail::IdentVisitor::visit(c);
    }
    void visit(const ast::ClassExpr& c) override {
        drop(c.name);
        ast::detail::IdentVisitor::visit(c);
    }
    void visit(const ast::TryStmt& t) override {
        drop(t.catchName);
        if (t.catchPattern) {
            for (const auto& n : ast::patternBoundNames(*t.catchPattern)) drop(n);
        }
        ast::detail::IdentVisitor::visit(t);
    }
    void visit(const ast::ForInStmt& f) override {
        // The head both BINDS and WRITES, with a value that is a property key
        // (a string) or whatever the iterable yields. Neither is provable here.
        drop(f.name);
        if (f.pattern) {
            for (const auto& n : ast::patternBoundNames(*f.pattern)) drop(n);
        }
        ast::detail::IdentVisitor::visit(f);
    }
    void visit(const ast::ForOfStmt& f) override {
        drop(f.name);
        if (f.pattern) {
            for (const auto& n : ast::patternBoundNames(*f.pattern)) drop(n);
        }
        ast::detail::IdentVisitor::visit(f);
    }
    void visit(const ast::DestructuringAssign& d) override {
        // `[a, b] = ...` writes names this pass cannot follow to a value.
        if (d.pattern) {
            for (const auto& n : ast::patternBoundNames(*d.pattern)) drop(n);
        }
        ast::detail::IdentVisitor::visit(d);
    }
    void visit(const ast::Binary& b) override {
        if (ast::isAssignOp(b.op)) {
            if (const auto* target = dynamic_cast<const ast::Ident*>(b.lhs.get());
                target != nullptr && candidates_.count(target->name) != 0) {
                // Every assignment form, compound included, stores something
                // built from the slot's own contents and the RHS. The slot's
                // half is a Number by the assumption under test, so the RHS is
                // the whole of what has to be proved.
                writes[target->name].push_back(b.rhs.get());
            }
        }
        ast::detail::IdentVisitor::visit(b);
    }
    void visit(const ast::Unary& u) override {
        // `++`/`--` write ToNumeric of the slot, which the assumption already
        // says is a Number. Nothing to record and nothing to refuse.
        ast::detail::IdentVisitor::visit(u);
    }

private:
    void drop(const std::string& name) {
        if (!name.empty() && candidates_.count(name) != 0) disqualified.insert(name);
    }
    void dropParams(const std::vector<ast::Param>& params) {
        for (const auto& p : params) {
            if (p.pattern) {
                for (const auto& n : ast::patternBoundNames(*p.pattern)) drop(n);
            } else {
                drop(p.name);
            }
        }
    }

    const std::unordered_set<std::string>& candidates_;
    const std::unordered_set<const ast::VarDecl*>& ownDecls_;
};

}  // namespace

// Which of a function's environment slots hold a Number at every read.
//
// The wall this exists for: a factory closure's hot state is captured
// variables, not fields — `WebGLState`'s seven flags, `WebGLUniforms`'s cache —
// and a captured binding is a boxed slot, so every `if (blending !==
// currentBlending)` is a dynamic compare over two values whose types nothing
// knows. The field path has had a proof for years; the env path had none.
//
// Nothing about the RECORD changes. A canonical double is a Value by NaN-box
// construction, the collector already walks past one as a non-pointer, and the
// store still goes through the canonicalizing box — a raw f64 out of an SSE
// divide can be the negative quiet NaN whose bits are `Tag::Symbol`, and a slot
// holding that is a heap scan reading a pointer that is not there. What changes
// is that the READ may stop testing.
//
// The proof, and why each clause is load-bearing:
//
//   - The binding is a `let` or `const` declared DIRECTLY in this function
//     body's statement list, with an initializer. Direct-and-lexical is what
//     puts it in `getLexicalDeclarations`, which is what gives the slot its
//     dead-zone marker (`openLexicalBindings`), which is what makes every read
//     before the declaration a checked one that throws. A `var` would answer
//     `undefined` there instead, and an unboxed read of `undefined` is the
//     miscompile this whole file is written to avoid.
//   - Nothing anywhere under this function re-binds the name: no second
//     declaration, no parameter of a nested function, no `catch` parameter, no
//     `for-of` head, no destructuring target. Env layouts are static, so "every
//     write is visible" is decidable — but only for a name that means one
//     binding throughout, and modelling the shadowing instead would be a
//     scope resolver, not a proof.
//   - Every written value is a Number. This is a GREATEST FIXPOINT and not a
//     single pass, because the shape the case is made of is self-referential:
//     `stateChanges = stateChanges + 1` is provable only while `stateChanges`
//     is itself assumed numeric, and a one-pass rule would refuse every counter
//     in the program. Start with every candidate in, drop the ones whose writes
//     do not hold under the current set, repeat until nothing drops. The
//     descending iteration is what makes the result sound: a name survives only
//     if its writes hold under the set that also survived.
//
// The PIN case (types/pins.h, `function <fn>.<binding>: number`) skips the last
// clause and no other. The binding structure still has to be exactly the one
// above, because that half is a fact about the program text this pass can read
// — a pin is a promise about VALUES, and letting it also promise that a name
// means one binding would be a promise nobody could keep. That is the same line
// the field pins hold: identity claims are checked, primitive claims are the
// promise.
void Lowerer::planEnvSlotNumberTypes(const std::vector<ast::Param>& params,
                                     const std::vector<const ast::Stmt*>& body,
                                     const std::string& functionName,
                                     EnvScopeInfo& info) const {
    if (inference_ == nullptr || unboxedFieldsDisabled_) return;

    // The candidates: this body's own top-level `let`/`const` with an
    // initializer, that also got a slot in this record.
    std::unordered_set<std::string> candidates;
    std::unordered_map<std::string, const ast::Expr*> declInit;
    std::unordered_set<const ast::VarDecl*> ownDecls;
    const std::unordered_set<std::string> lexical = [&] {
        const auto names = ast::getLexicalDeclarations(body);
        return std::unordered_set<std::string>(names.begin(), names.end());
    }();
    for (const ast::Stmt* s : body) {
        const auto* vd = dynamic_cast<const ast::VarDecl*>(s);
        if (vd == nullptr || vd->pattern || vd->name.empty() || vd->isVar) continue;
        if (vd->init == nullptr || lexical.count(vd->name) == 0) continue;
        if (!info.slotOf.contains(vd->name)) continue;
        candidates.insert(vd->name);
        declInit[vd->name] = vd->init.get();
        ownDecls.insert(vd);
    }
    // A parameter of THIS function with the same name is the binding the body's
    // mentions resolve to; the declaration below it would be a redeclaration
    // error, but the record is laid out before that is diagnosed.
    for (const auto& p : params) {
        if (p.pattern) {
            for (const auto& n : ast::patternBoundNames(*p.pattern)) candidates.erase(n);
        } else {
            candidates.erase(p.name);
        }
    }
    if (candidates.empty()) return;

    EnvSlotWriteScan scan(candidates, ownDecls);
    for (const ast::Stmt* s : body) s->accept(scan);
    for (const auto& name : scan.disqualified) candidates.erase(name);
    if (candidates.empty()) return;
    // What is left has the binding STRUCTURE the claim needs, whatever its
    // writes turn out to hold. Kept apart from `candidates` because the pin arm
    // below may only re-admit a name the fixpoint dropped — never one a
    // parameter shadows or a nested scope re-declares.
    const std::unordered_set<std::string> structural = candidates;

    // Is `e` a Number given that every name in `live` holds one? The `live`
    // reference is read at call time, which is what makes this a fixpoint step
    // rather than a snapshot.
    const std::unordered_set<std::string>* live = &candidates;
    std::function<bool(const ast::Expr&, int)> numericUnder = [&](const ast::Expr& e,
                                                                 int depth) -> bool {
        if (depth <= 0) return false;
        if (provenNumber(e)) return true;
        if (dynamic_cast<const ast::NumberLit*>(&e) != nullptr) return true;
        if (const auto* id = dynamic_cast<const ast::Ident*>(&e)) {
            return live->count(id->name) != 0;
        }
        if (const auto* un = dynamic_cast<const ast::Unary*>(&e)) {
            switch (un->op) {
                // ToNumber, unconditionally: 13.5.4 throws for a BigInt rather
                // than producing one.
                case ast::UnaryOp::Posate:
                    return true;
                // ToNumeric, so a BigInt operand yields a BigInt — which the
                // operand's own proof rules out.
                case ast::UnaryOp::Negate:
                case ast::UnaryOp::BitNot:
                case ast::UnaryOp::PreInc:
                case ast::UnaryOp::PreDec:
                case ast::UnaryOp::PostInc:
                case ast::UnaryOp::PostDec:
                    return numericUnder(*un->operand, depth - 1);
                default:
                    return false;
            }
        }
        if (const auto* bin = dynamic_cast<const ast::Binary*>(&e)) {
            if (ast::isAssignOp(bin->op)) return false;
            // `+` needs both sides (either one being a string concatenates);
            // the rest of the arithmetic and bitwise family needs both sides
            // too, because one BigInt operand is either a BigInt result or a
            // TypeError and neither is this claim.
            if (bin->op == ast::BinaryOp::Add || alwaysCoercingBinary(bin->op)) {
                return numericUnder(*bin->lhs, depth - 1) && numericUnder(*bin->rhs, depth - 1);
            }
            return false;
        }
        if (const auto* tern = dynamic_cast<const ast::Ternary*>(&e)) {
            return numericUnder(*tern->thenExpr, depth - 1) &&
                   numericUnder(*tern->elseExpr, depth - 1);
        }
        return false;
    };

    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = candidates.begin(); it != candidates.end();) {
            bool ok = numericUnder(*declInit.at(*it), 8);
            if (ok) {
                const auto w = scan.writes.find(*it);
                if (w != scan.writes.end()) {
                    for (const ast::Expr* rhs : w->second) {
                        if (!numericUnder(*rhs, 8)) {
                            ok = false;
                            break;
                        }
                    }
                }
            }
            if (ok) {
                ++it;
                continue;
            }
            it = candidates.erase(it);
            changed = true;
        }
    }

    // The pin arm, added after the fixpoint rather than into it: a pinned slot
    // is a promise about that slot and must not become evidence for another
    // one's proof. `stateChanges = stateChanges + currentBlending` stays refused
    // whether or not `currentBlending` is pinned.
    if (pins_ != nullptr && !functionName.empty()) {
        for (const auto& name : structural) {
            if (candidates.count(name) == 0 && pins_->envSlotPinned(functionName, name)) {
                candidates.insert(name);
                // The write barrier's licence, and ONLY on this arm. What the
                // fixpoint above admitted it also audited — every write to it
                // is a numeric expression this compilation read — so a barrier
                // there would be a check on a proof. What arrives here is the
                // opposite: a slot whose writes the fixpoint could not type,
                // admitted because the manifest promised. The promise is what
                // gets held (types/pins.h, stage B1).
                if (types::pinBarriersEnabled()) {
                    const uint32_t slot = info.slotOf.at(name);
                    if (info.slotIsPinned.size() < info.slotNames.size()) {
                        info.slotIsPinned.assign(info.slotNames.size(), false);
                        info.slotPinText.assign(info.slotNames.size(), std::string());
                    }
                    // The LAST dotted component, because that is the spelling
                    // the manifest uses (the linker's `mod1.` prefix is not in
                    // the file), and the message exists to be grepped for in
                    // the file.
                    const auto dot = functionName.rfind('.');
                    const std::string owner = dot == std::string::npos
                                                  ? functionName
                                                  : functionName.substr(dot + 1);
                    info.slotIsPinned[slot] = true;
                    info.slotPinText[slot] = "function " + owner + "." + name + ": number";
                }
            }
        }
    }

    // THE CENSUS ARM (`--census`, src/runtime/pin_census.h), and it is the
    // complement of everything above: a name with the binding STRUCTURE an
    // env-slot pin needs that neither the fixpoint proved nor a manifest
    // promised. That is precisely the set the five
    // `function WebGLState.<slot>: number` lines of
    // bench/pins/env-slot-kernel.pins were written by hand for, and the fact
    // that the proof runs first is what keeps the census from proposing
    // `stateChanges` and `frames`, which it proves for free.
    if (censusEnabled() && !functionName.empty()) {
        const std::string owner = [&] {
            const auto dot = functionName.rfind('.');
            return dot == std::string::npos ? functionName : functionName.substr(dot + 1);
        }();
        for (const auto& name : structural) {
            if (candidates.count(name) != 0) continue;
            const uint32_t slot = info.slotOf.at(name);
            if (info.slotIsCensus.size() < info.slotNames.size()) {
                info.slotIsCensus.assign(info.slotNames.size(), false);
                info.slotCensusText.assign(info.slotNames.size(), std::string());
            }
            info.slotIsCensus[slot] = true;
            info.slotCensusText[slot] = "function " + owner + "." + name;
        }
    }

    for (const auto& name : candidates) info.slotIsF64[info.slotOf.at(name)] = true;
}

}  // namespace bronze::lower
