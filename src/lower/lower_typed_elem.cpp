// Proven typed-array element access — the native path for `v[i]` where
// inference proved `v` a Float64Array or Float32Array view and `i` a number.
//
// The get is `elem.get.typed`: an unboxed f64 whose value is ToNumber of the
// language's read. That is EXACT for every in-range element and exact for
// every invalid index too — negative, fractional, NaN, past the view, over a
// detached buffer — because the language answers `undefined` there and
// ToNumber(undefined) is NaN. What an f64 cannot carry is `undefined` itself,
// so the op may only feed positions whose consumer performs that coercion
// anyway. This file is the single authority on which positions those are:
//
//   - an operand of an operator whose every branch is ToNumeric — the
//     arithmetic family except `+`, the bitwise/shift family, and the
//     relational compares (13.10 reaches its string branch only when BOTH
//     sides are strings, and this side never is);
//   - an operand of `+` whose OTHER side is definitely numeric, so the
//     string-concatenation branch (where "undefined" and "NaN" differ) is
//     unreachable;
//   - the value of a store into a proven view (23.2.5 coerces with ToNumber);
//   - the read half of a compound assignment or update on a proven view,
//     under the same rules;
//   - the initialiser of an uncaptured binding EVERY use of which is one of
//     the positions above (typedElemBindingUsesCoerce).
//
// The set is `elem.set.typed`, the full 23.2.5 store: a valid index stores
// with the kind's narrowing, an invalid one is a silent no-op — so it needs
// no coercion argument, only an f64 value, which is why a dynamic value falls
// back to the ordinary `elem.set` rather than being coerced here (ToNumber of
// an object can run user code; the helper owns that).

#define _CRT_SECURE_NO_WARNINGS
#include <cstdlib>
#include <optional>
#include <string>

#include "ast/queries.h"
#include "ast/query_walk.h"
#include "lower/lowerer.h"

namespace bronze::lower {

namespace {

// Operators whose EVERY operand goes through ToNumeric on every branch.
// `+` is absent (string branch), equality is absent (undefined and NaN
// compare differently), `in`/`instanceof` are absent (no coercion at all).
bool alwaysCoercingBinary(ast::BinaryOp op) {
    switch (op) {
        case ast::BinaryOp::Sub:
        case ast::BinaryOp::Mul:
        case ast::BinaryOp::Div:
        case ast::BinaryOp::Mod:
        case ast::BinaryOp::Exp:
        case ast::BinaryOp::BitAnd:
        case ast::BinaryOp::BitOr:
        case ast::BinaryOp::BitXor:
        case ast::BinaryOp::Shl:
        case ast::BinaryOp::Shr:
        case ast::BinaryOp::UShr:
        case ast::BinaryOp::Less:
        case ast::BinaryOp::Greater:
        case ast::BinaryOp::LessEqual:
        case ast::BinaryOp::GreaterEqual:
            return true;
        default:
            return false;
    }
}

// The compound-assignment operators whose combine step coerces the READ half
// on every branch. PlusAssign is handled separately (its combine has the
// string branch), the logical assigns never coerce the read at all.
bool alwaysCoercingCompound(ast::BinaryOp op) {
    switch (op) {
        case ast::BinaryOp::MinusAssign:
        case ast::BinaryOp::StarAssign:
        case ast::BinaryOp::SlashAssign:
        case ast::BinaryOp::PercentAssign:
        case ast::BinaryOp::ExpAssign:
        case ast::BinaryOp::AmpAssign:
        case ast::BinaryOp::PipeAssign:
        case ast::BinaryOp::CaretAssign:
        case ast::BinaryOp::ShlAssign:
        case ast::BinaryOp::ShrAssign:
        case ast::BinaryOp::UShrAssign:
            return true;
        default:
            return false;
    }
}

// Finds the declarations of one name directly visible in a function body —
// the resolver behind definitelyNumericOperand's one-step look through a
// const. Counts EVERY declaration form that could bind the name (so a count
// above one means "ambiguous, prove nothing") and keeps the VarDecl if that
// is what it found.
class DeclFinder final : public ast::detail::IdentVisitor {
public:
    explicit DeclFinder(const std::string& target) : target_(target) {}

    int count = 0;
    const ast::VarDecl* decl = nullptr;

    void visit(const ast::VarDecl& v) override {
        if (v.pattern) {
            for (const auto& n : ast::patternBoundNames(*v.pattern)) {
                if (n == target_) ++count;
            }
        } else if (v.name == target_) {
            ++count;
            decl = &v;
        }
        ast::detail::IdentVisitor::visit(v);
    }
    void visit(const ast::FunctionDecl& f) override {
        if (f.name == target_) ++count;
        ast::detail::IdentVisitor::visit(f);
    }
    void visit(const ast::ClassDecl& c) override {
        if (c.name == target_) ++count;
        ast::detail::IdentVisitor::visit(c);
    }

private:
    const std::string& target_;
};

}  // namespace

bool Lowerer::typedElemSeamDisabled() {
    return std::getenv("BRONZE_NO_TYPED_ELEM") != nullptr ||
           std::getenv("BRONZE_NO_ELEM_FAST_PATH") != nullptr;
}

bool Lowerer::provenArrayOrTypedArray(const ast::Expr& e) const {
    if (typedElemDisabled_ || inference_ == nullptr) return false;
    const types::Type recv = inferredType(e);
    return recv.is(types::TypeKind::TypedArray) || recv.is(types::TypeKind::Array);
}

std::optional<uint32_t> Lowerer::typedElemAccessKind(const ast::Expr& e) const {
    if (typedElemDisabled_ || inference_ == nullptr) return std::nullopt;
    const auto* ia = dynamic_cast<const ast::IndexAccess*>(&e);
    // An optional link (`a?.[i]`) short-circuits on nullish and never reads,
    // which is control flow this op does not model.
    if (ia == nullptr || ia->optional) return std::nullopt;
    const types::Type recv = inferredType(*ia->object);
    // The pin ceiling probe (BRONZE_UNSOUND_PINS): a proven plain-Array
    // receiver with a proven-number index takes the raw dense form — no
    // guards at all. See il::kElemKindPlainArrayF64.
    if (recv.is(types::TypeKind::Array)) {
        static const bool unsoundPins = std::getenv("BRONZE_UNSOUND_PINS") != nullptr;
        if (!unsoundPins || !provenNumber(*ia->index)) return std::nullopt;
        return static_cast<uint32_t>(il::kElemKindPlainArrayF64);
    }
    if (!recv.is(types::TypeKind::TypedArray)) return std::nullopt;
    const uint32_t raw = recv.typedArrayElemRaw();
    if (raw == types::kNoTypedArrayElem) return std::nullopt;
    // The index must be a PROVEN number: a boxed index of any other type goes
    // through ToPropertyKey, which can run user code, and the op has no edge
    // for that. A proven number's canonical-index rule is checked inline.
    if (!provenNumber(*ia->index)) return std::nullopt;
    return raw;
}

// Is `e` guaranteed to hold a NUMBER when it holds anything — i.e. can the
// `+` beside it never take the string branch and never see a both-BigInt
// pair? Yes when inference proved Number; yes for an always-coercing binary
// with a definitely-numeric side (the mixed BigInt pair throws before the
// `+` runs); yes for a proven typed-element read. An `Ident` is resolved one
// step through the current function's single-declaration consts, because the
// FFT-shaped code this exists for names its butterfly temporaries.
bool Lowerer::definitelyNumericOperand(const ast::Expr& e, int depth) const {
    if (depth <= 0) return false;
    if (provenNumber(e)) return true;
    if (typedElemAccessKind(e)) return true;
    if (const auto* bin = dynamic_cast<const ast::Binary*>(&e)) {
        if (alwaysCoercingBinary(bin->op)) {
            return definitelyNumericOperand(*bin->lhs, depth - 1) ||
                   definitelyNumericOperand(*bin->rhs, depth - 1);
        }
        // `+` alone among the operators needs BOTH sides: one numeric side
        // only rules out the pair being BigInts, while the other side could
        // still be a string and choose concatenation. Two definitely-numeric
        // sides leave 13.15.3 no branch but the numeric one.
        if (bin->op == ast::BinaryOp::Add) {
            return definitelyNumericOperand(*bin->lhs, depth - 1) &&
                   definitelyNumericOperand(*bin->rhs, depth - 1);
        }
        return false;
    }
    if (const auto* ident = dynamic_cast<const ast::Ident*>(&e)) {
        if (currentBodyStmts_ == nullptr) return false;
        // One declaration, a const, in this function, initialiser itself
        // definitely numeric. Anything ambiguous — a second declaration
        // anywhere in the subtree, a pattern, a `let` — proves nothing.
        DeclFinder finder(ident->name);
        for (const auto& s : *currentBodyStmts_) s->accept(finder);
        if (finder.count != 1 || finder.decl == nullptr || !finder.decl->isConst ||
            finder.decl->init == nullptr) {
            return false;
        }
        return definitelyNumericOperand(*finder.decl->init, depth - 1);
    }
    return false;
}

// May the READ half of `v[i] op= rhs` on a proven view go native? Yes for
// the operators whose combine is ToNumeric on every branch, and for `+=`
// exactly when the RHS is definitely numeric (no concatenation branch left).
// The logical assigns answer no: they can yield the read value itself.
bool Lowerer::typedElemCompoundAdmissible(ast::BinaryOp op, const ast::Expr& rhs) const {
    if (alwaysCoercingCompound(op)) return true;
    if (op == ast::BinaryOp::PlusAssign) return definitelyNumericOperand(rhs, 8);
    return false;
}

bool Lowerer::binaryCoercesOperand(ast::BinaryOp op, const ast::Expr& other) const {
    if (alwaysCoercingBinary(op) || alwaysCoercingCompound(op)) return true;
    if (op == ast::BinaryOp::Add || op == ast::BinaryOp::PlusAssign) return definitelyNumericOperand(other, 8);
    return false;
}

std::optional<Lowerer::Value> Lowerer::lowerCoercingOperand(const ast::Expr& e,
                                                            il::Function& ilFn) {
    if (const auto kind = typedElemAccessKind(e)) {
        return lowerTypedElemRead(static_cast<const ast::IndexAccess&>(e), *kind, ilFn);
    }
    auto val = lowerExpr(e, ilFn);
    // An AUDITED field read, in a context that coerces it: the property load is
    // the one it always was, and the value that comes back is turned into a
    // double by a bitcast rather than by a tag test, a branch and a phi.
    //
    // Doing it HERE and not at the read is what keeps the boxed uses free.
    // `console.log(p.x)` wants the Value, and an eager unbox would have to be
    // re-boxed through the canonicalizing select. This runs only where the
    // operator is going to convert anyway — and it is what turns `v.x * m.x +
    // v.y * m.y` from five dynamic operators, each with its own number test,
    // NaN canonicalization and phi, into three fmuls and two fadds in
    // registers.
    if (val && val->type == il::Type::Dynamic && provenFieldRead(e)) {
        return emitRawUnbox(*val, ilFn);
    }
    return val;
}

std::optional<Lowerer::Value> Lowerer::lowerTypedElemRead(const ast::IndexAccess& idx,
                                                          uint32_t elemKind,
                                                          il::Function& ilFn) {
    auto objVal = lowerExpr(*idx.object, ilFn);
    if (!objVal) return std::nullopt;
    Value objBoxed = boxValueIfNeeded(*objVal, ilFn);
    auto idxVal = lowerExpr(*idx.index, ilFn);
    if (!idxVal) return std::nullopt;
    // Proven number, so this unbox is exact — same licence lowerVarDecl uses.
    Value idxF64 = unboxValueIfNeeded(*idxVal, il::Type::F64, ilFn);

    recordElementOp(idx.span.file, true, "");
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::ElemGetTyped;
    inst.type = il::Type::F64;
    inst.result = res;
    inst.operands = {objBoxed.id, idxF64.id};
    inst.immI32 = static_cast<int32_t>(elemKind);
    emitInst(ilFn, inst);
    return Value{res, il::Type::F64};
}

void Lowerer::emitTypedElemSet(Value objBoxed, Value idxF64, Value valF64, uint32_t elemKind,
                               il::Function& ilFn) {
    il::Instruction inst;
    inst.op = il::Op::ElemSetTyped;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {objBoxed.id, idxF64.id, valF64.id};
    inst.immI32 = static_cast<int32_t>(elemKind);
    emitInst(ilFn, inst);
}

// The assignment forms on a proven view: `v[i] = x`, `v[i] op= x`. Reference
// evaluation order is the contract lowerAssignment keeps — object, index,
// (compound: read), rhs — and this keeps it identically. Returns the
// assignment expression's value.
//
// The compound read goes native only where its combine coerces it:
// the always-numeric operators, or `+=` whose RHS is definitely numeric.
// `+=` with anything else keeps the whole dynamic path — its combine can
// CONCATENATE the old value, and "undefined" and "NaN" concatenate
// differently. A logical assign never reaches here (its read is tested and
// possibly RETURNED raw, not coerced).
std::optional<Lowerer::Value> Lowerer::lowerTypedElemAssign(const ast::Binary* bin,
                                                            const ast::IndexAccess& idxAccess,
                                                            uint32_t elemKind,
                                                            il::Function& ilFn) {
    const bool compound = bin->op != ast::BinaryOp::Assign;

    auto objVal = lowerExpr(*idxAccess.object, ilFn);
    if (!objVal) return std::nullopt;
    Value objBoxed = boxValueIfNeeded(*objVal, ilFn);
    auto idxVal = lowerExpr(*idxAccess.index, ilFn);
    if (!idxVal) return std::nullopt;
    Value idxF64 = unboxValueIfNeeded(*idxVal, il::Type::F64, ilFn);

    std::optional<Value> curVal;
    if (compound) {
        recordElementOp(idxAccess.span.file, true, "");
        il::ValueId cur = ilFn.valueCount++;
        il::Instruction getInst;
        getInst.op = il::Op::ElemGetTyped;
        getInst.type = il::Type::F64;
        getInst.result = cur;
        getInst.operands = {objBoxed.id, idxF64.id};
        getInst.immI32 = static_cast<int32_t>(elemKind);
        emitInst(ilFn, getInst);
        curVal = Value{cur, il::Type::F64};
    }

    auto rhsVal = lowerExpr(*bin->rhs, ilFn);
    if (!rhsVal) return std::nullopt;
    // `+=` reaches this function only when its RHS is definitely numeric
    // (the caller's admissibility gate), which is exactly the proof that lets
    // the combine skip the concatenation branch even where inference alone
    // could not.
    const bool combineNumeric =
        provenNumber(*bin) || (bin->op == ast::BinaryOp::PlusAssign &&
                               definitelyNumericOperand(*bin->rhs, 8));
    Value stored = curVal ? emitCompoundCombine(*curVal, *rhsVal, bin->op,
                                                combineNumeric, ilFn)
                          : *rhsVal;

    if (stored.type == il::Type::F64 || stored.type == il::Type::I32 ||
        stored.type == il::Type::Bool) {
        Value storedF64 = unboxValueIfNeeded(stored, il::Type::F64, ilFn);
        recordElementOp(idxAccess.span.file, true, "");
        emitTypedElemSet(objBoxed, idxF64, storedF64, elemKind, ilFn);
        return stored;
    }

    // The value is boxed — a dynamic combine result, or a dynamic RHS whose
    // ToNumber may run user code. The ordinary elem.set owns that; the read
    // above (if any) was still native, which is sound because its NaN feeds
    // ONLY the coercing combine that produced `stored`.
    recordElementOp(idxAccess.span.file, false, "typed store value is dynamic");
    Value idxBoxed = boxValueIfNeeded(idxF64, ilFn);
    Value storedBoxed = boxValueIfNeeded(stored, ilFn);
    il::Instruction setInst;
    setInst.op = il::Op::ElemSet;
    setInst.type = il::Type::Void;
    setInst.result = il::kNoValue;
    setInst.operands = {objBoxed.id, idxBoxed.id, storedBoxed.id};
    setInst.immI32 = strictFlag();
    emitInst(ilFn, setInst);
    return storedBoxed;
}

namespace {

// The three questions the scan asks back of the Lowerer, carried as
// functions because the scan class cannot see the Lowerer's private half.
struct ScanHooks {
    std::function<bool(const ast::Expr&)> definitelyNumeric;
    std::function<bool(const ast::Expr&)> isTypedElemTarget;
    std::function<bool(const ast::Binary&)> plusAssignReadCoerces;
};

// The binding-use scan behind typedElemBindingUsesCoerce. Derives from
// IdentVisitor so the traversal IS the capture walk's — a node kind that walk
// visits, this walk visits. Every reference to a binding is an `Ident` node,
// so it suffices to prove no `Ident` of the name appears outside a coercing
// position; the non-Ident mention forms (a class extends clause, `super`
// bases, a rebinding pattern, a redeclaration) disqualify outright. Shadowed
// same-name bindings only ADD scanned occurrences, so every conclusion here
// is over-approximate in the sound direction: bail, never admit.
class CoercingUseScan final : public ast::detail::IdentVisitor {
public:
    CoercingUseScan(const ScanHooks& hooks, const std::string& target,
                    const ast::VarDecl* self)
        : hooks_(hooks), target_(target), self_(self) {}

    bool allCoercing() const { return !disqualified_; }

    void visit(const ast::Ident& i) override {
        if (i.name == target_) disqualified_ = true;
    }

    void visit(const ast::Binary& b) override {
        if (disqualified_) return;
        const bool lhsIsTarget = isTargetIdent(b.lhs.get());
        const bool rhsIsTarget = isTargetIdent(b.rhs.get());

        // Writes to the binding — plain or compound — end the proof: the
        // binding stops being the single initialiser read this is about.
        if (ast::isAssignOp(b.op) && lhsIsTarget) {
            disqualified_ = true;
            return;
        }

        bool lhsSanctioned = false;
        bool rhsSanctioned = false;
        if (alwaysCoercingBinary(b.op)) {
            lhsSanctioned = lhsIsTarget;
            rhsSanctioned = rhsIsTarget;
        } else if (b.op == ast::BinaryOp::Add) {
            lhsSanctioned = lhsIsTarget && hooks_.definitelyNumeric(*b.rhs);
            rhsSanctioned = rhsIsTarget && hooks_.definitelyNumeric(*b.lhs);
        } else if (b.op == ast::BinaryOp::Assign || alwaysCoercingCompound(b.op) ||
                   (b.op == ast::BinaryOp::PlusAssign && hooks_.plusAssignReadCoerces(b))) {
            // The RHS of a store into a proven view is coerced by the store
            // itself; a compound's combine coerces it on the operator rule.
            if (rhsIsTarget) {
                if (b.op == ast::BinaryOp::Assign) {
                    rhsSanctioned = hooks_.isTypedElemTarget(*b.lhs);
                } else {
                    rhsSanctioned = true;
                }
            }
        }

        if (lhsSanctioned) {
            if (!rhsIsTarget || rhsSanctioned) {
                if (!rhsSanctioned) b.rhs->accept(*this);
            } else {
                disqualified_ = true;
            }
            return;
        }
        if (rhsSanctioned) {
            if (lhsIsTarget) {
                disqualified_ = true;
                return;
            }
            b.lhs->accept(*this);
            return;
        }
        ast::detail::IdentVisitor::visit(b);
    }

    // The candidate's own declaration is the one mention that is neither a
    // use nor a shadow; any OTHER declaration of the name — a `var`
    // redeclaration writes the same binding, the rest shadow — bails.
    void visit(const ast::VarDecl& v) override {
        if (&v == self_) {
            if (v.init) v.init->accept(*this);
            return;
        }
        if (declaresTarget(v)) disqualified_ = true;
        ast::detail::IdentVisitor::visit(v);
    }
    void visit(const ast::FunctionDecl& f) override {
        if (f.name == target_) disqualified_ = true;
        ast::detail::IdentVisitor::visit(f);
    }
    void visit(const ast::ClassDecl& c) override {
        if (c.name == target_ || c.superName == target_) disqualified_ = true;
        ast::detail::IdentVisitor::visit(c);
    }
    void visit(const ast::ClassExpr& c) override {
        if (c.superName == target_) disqualified_ = true;
        ast::detail::IdentVisitor::visit(c);
    }
    void visit(const ast::SuperCall& s) override {
        if (s.baseName == target_) disqualified_ = true;
        ast::detail::IdentVisitor::visit(s);
    }
    void visit(const ast::SuperMember& m) override {
        if (m.baseName == target_) disqualified_ = true;
        ast::detail::IdentVisitor::visit(m);
    }
    void visit(const ast::DestructuringAssign& d) override {
        for (const auto& n : ast::patternBoundNames(*d.pattern)) {
            if (n == target_) disqualified_ = true;
        }
        ast::detail::IdentVisitor::visit(d);
    }
    void visit(const ast::ForInStmt& f) override {
        if (f.pattern != nullptr) {
            for (const auto& n : ast::patternBoundNames(*f.pattern)) {
                if (n == target_) disqualified_ = true;
            }
        }
        ast::detail::IdentVisitor::visit(f);
    }
    void visit(const ast::ForOfStmt& f) override {
        if (f.pattern != nullptr) {
            for (const auto& n : ast::patternBoundNames(*f.pattern)) {
                if (n == target_) disqualified_ = true;
            }
        }
        ast::detail::IdentVisitor::visit(f);
    }

private:
    bool isTargetIdent(const ast::Expr* e) const {
        const auto* id = dynamic_cast<const ast::Ident*>(e);
        return id != nullptr && id->name == target_;
    }
    bool declaresTarget(const ast::VarDecl& v) const {
        if (v.pattern) {
            for (const auto& n : ast::patternBoundNames(*v.pattern)) {
                if (n == target_) return true;
            }
            return false;
        }
        return v.name == target_;
    }

    const ScanHooks& hooks_;
    const std::string& target_;
    const ast::VarDecl* self_;
    bool disqualified_ = false;
};

}  // namespace

bool Lowerer::typedElemBindingUsesCoerce(const std::string& name,
                                         const ast::VarDecl* self) const {
    if (currentBodyStmts_ == nullptr) return false;
    ScanHooks hooks{
        [this](const ast::Expr& e) { return definitelyNumericOperand(e, 8); },
        [this](const ast::Expr& lhs) {
            if (const auto* ia = dynamic_cast<const ast::IndexAccess*>(&lhs)) {
                return typedElemAccessKind(*ia).has_value();
            }
            return false;
        },
        [this](const ast::Binary& b) {
            const auto* ia = dynamic_cast<const ast::IndexAccess*>(b.lhs.get());
            return ia != nullptr && typedElemAccessKind(*ia).has_value() &&
                   definitelyNumericOperand(*b.rhs, 8);
        }};
    CoercingUseScan scan(hooks, name, self);
    for (const auto& s : *currentBodyStmts_) s->accept(scan);
    return scan.allCoercing();
}

}  // namespace bronze::lower
