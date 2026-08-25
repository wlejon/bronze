// Scopes, bindings and environment records: where a declaration lives, how a
// captured one is read and written, and how a closure value is produced over
// the environment innermost at its creation site.

// For `getenv`, which MSVC deprecates and every other toolchain does not. Same
// reasoning and same one-line define as lower_infer.cpp: the one seam here is
// read exactly once, at construction, from a single-threaded driver.
#define _CRT_SECURE_NO_WARNINGS

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/assigned.h"
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

bool Lowerer::declareVariable(const std::string& name, il::Type type, bool isConst, bool isLet,
                              bool isVar, bool isInitialized, il::ValueId valId, Span span) {
    auto it = activeVarMap_.find(name);
    if (it != activeVarMap_.end()) {
        auto& existing = varBindings_[it->second];
        if (existing.scopeDepth == currentScopeDepth_ && existing.isTdzHoisted && !isVar) {
            // The declaration that ends this binding's dead zone. Scope entry
            // already made the binding and gave it its environment slot; this
            // is the same binding reaching its initializer, not a second one.
            existing.isTdzHoisted = false;
            existing.type = type;
            existing.isConst = isConst;
            existing.isLet = isLet;
            existing.isInitialized = isInitialized;
            if (!existing.inEnv) existing.valueId = valId;
            return true;
        }
        if (isVar && existing.isVar) {
            if (valId != il::kNoValue && !existing.inEnv) {
                existing.valueId = valId;
                existing.type = type;
            }
            existing.isInitialized = existing.isInitialized || isInitialized;
            return true;
        }
        if (existing.scopeDepth == currentScopeDepth_ && !isVar) {
            diags_.error(span, "redeclaration of variable '" + name + "' in same scope");
            return false;
        }
    }
    VarBinding b;
    if (it != activeVarMap_.end()) b.shadowedBinding = it->second;
    b.name = name;
    b.type = type;
    b.isConst = isConst;
    b.isLet = isLet;
    b.isVar = isVar;
    b.isInitialized = isInitialized;
    b.declOrder = varDeclCounter_++;
    b.scopeDepth = isVar ? 0 : currentScopeDepth_;
    b.valueId = valId;

    // A captured declaration lives in its scope's environment. `var` is
    // function-scoped wherever it is written, so it belongs to the
    // function's environment, not the innermost block's.
    size_t ownerScope = SIZE_MAX;
    if (isVar) {
        if (functionEnvScope_ != SIZE_MAX &&
            envScopes_[functionEnvScope_].slotOf.contains(name)) {
            ownerScope = functionEnvScope_;
        }
    } else if (envScopes_.size() > functionEnvBase_ &&
               envScopes_.back().slotOf.contains(name)) {
        ownerScope = envScopes_.size() - 1;
    }
    if (ownerScope != SIZE_MAX) {
        b.inEnv = true;
        b.envScopeIndex = ownerScope;
        b.envSlot = envScopes_[ownerScope].slotOf.at(name);
        b.valueId = il::kNoValue;
    }

    size_t idx = varBindings_.size();
    varBindings_.push_back(b);
    activeVarMap_[name] = idx;
    return true;
}

// --- environment emission --------------------------------
il::ValueId Lowerer::emitConstUndefined(il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::ConstUndefined;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    emitInst(ilFn, inst);
    return res;
}

il::ValueId Lowerer::currentEnv(il::Function& ilFn) {
    if (!generator_) return currentEnvValue_;
    il::ValueId here = generator_->frameEnv;
    for (size_t i = generator_->frameScope; i + 1 < envScopes_.size(); ++i) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::EnvGet;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = {here};
        inst.envDepth = 0;
        inst.envIndex = envScopes_[i].childSlot;
        emitInst(ilFn, inst);
        here = res;
    }
    return here;
}

il::ValueId Lowerer::emitEnvCreate(uint32_t slotCount, il::Function& ilFn) {
    const il::ValueId enclosing = currentEnv(ilFn);
    il::ValueId parent = enclosing == il::kNoValue ? emitConstUndefined(ilFn) : enclosing;
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::EnvCreate;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {parent};
    inst.immI32 = static_cast<int32_t>(slotCount);
    emitInst(ilFn, inst);
    return res;
}

// The module scope's record, reached through the runtime rather than through a
// parameter. `main` publishes it once; anything that needs it loads it.
il::ValueId Lowerer::emitModuleEnvGet(il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::ModuleEnvGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    emitInst(ilFn, inst);
    return res;
}

void Lowerer::emitModuleEnvSet(il::ValueId env, il::Function& ilFn) {
    il::Instruction inst;
    inst.op = il::Op::ModuleEnvSet;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {env};
    emitInst(ilFn, inst);
}

bool Lowerer::envSlotIsLexical(uint32_t depth, uint32_t index) const {
    if (depth >= envScopes_.size()) return false;
    const EnvScopeInfo& scope = envScopes_[envScopes_.size() - 1 - depth];
    return index < scope.slotIsLexical.size() && scope.slotIsLexical[index];
}

// The A/B seam, in the house style of `BRONZE_NO_CLOSURE_EDGE`: `1` refuses to
// mark any slot definitely assigned, so every lexical binding keeps its marker
// AND its checked read and the two columns of a measurement come out of one
// binary. It is asked at the two places that SET the flag, never at the places
// that read it, because the marker store and the checked read have to agree:
// a slot with no marker whose reads are checked would answer `undefined` where
// the language says ReferenceError.
bool Lowerer::definiteInitDisabled() {
    static const bool disabled = [] {
        const char* env = std::getenv("BRONZE_NO_DEFINITE_INIT");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return disabled;
}

bool Lowerer::envSlotDefiniteInit(uint32_t depth, uint32_t index) const {
    if (depth >= envScopes_.size()) return false;
    const EnvScopeInfo& scope = envScopes_[envScopes_.size() - 1 - depth];
    return index < scope.slotIsDefiniteInit.size() && scope.slotIsDefiniteInit[index];
}

SlotImmutability Lowerer::envSlotImmutability(uint32_t depth, uint32_t index) const {
    if (depth >= envScopes_.size()) return SlotImmutability::Mutable;
    const EnvScopeInfo& scope = envScopes_[envScopes_.size() - 1 - depth];
    if (index >= scope.slotImmutable.size()) return SlotImmutability::Mutable;
    return scope.slotImmutable[index];
}

bool Lowerer::envSlotIsF64(uint32_t depth, uint32_t index) const {
    if (unboxedFieldsDisabled_ || depth >= envScopes_.size()) return false;
    const EnvScopeInfo& scope = envScopes_[envScopes_.size() - 1 - depth];
    return index < scope.slotIsF64.size() && scope.slotIsF64[index];
}

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
            }
        }
    }

    for (const auto& name : candidates) info.slotIsF64[info.slotOf.at(name)] = true;
}

// Every read of an environment slot passes through here, so this is the one
// place 9.1.1.1.6's check has to be decided. A lexical slot gets the checked
// form UNCONDITIONALLY — not only where lowering can see a read above the
// declaration. The dead zone is a property of a moment in evaluation, and a
// closure over the slot can be called at any moment, so "lowering has already
// passed the declaration" is a fact about the source text and not about the
// run. Eliding on it is exactly the bug the case in
// `cases/temporal_dead_zone.js` pins.
Lowerer::Value Lowerer::emitEnvGet(uint32_t depth, uint32_t index, il::Function& ilFn) {
    const bool lexical = envSlotIsLexical(depth, index) && !envSlotDefiniteInit(depth, index);
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = lexical ? il::Op::EnvGetTdz : il::Op::EnvGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {currentEnv(ilFn)};
    inst.envDepth = depth;
    inst.envIndex = index;
    if (lexical) {
        inst.keyIndex =
            getKeyConstantIndex(envScopes_[envScopes_.size() - 1 - depth].slotNames[index]);
    }
    emitInst(ilFn, inst);
    // A slot the proof above claimed: the load is the one it always was and the
    // value that comes back is a double by bitcast. Unboxed HERE and not only
    // in coercing positions, which is where the field seam draws the line —
    // because what a captured flag is used for is `!==` and `<` against another
    // one, and those are not coercing positions. The cost is the canonicalizing
    // box on the way back out of a slot read that ends up boxed anyway; the
    // buy is that a comparison of two slots is an fcmp instead of two dynamic
    // guards and a helper.
    //
    // After the instruction, never folded into it: the TDZ form throws, and the
    // marker is not a double.
    if (envSlotIsF64(depth, index)) return emitRawUnbox(Value{res, il::Type::Dynamic}, ilFn);
    return Value{res, il::Type::Dynamic};
}

void Lowerer::openLexicalBindings(size_t scopeIndex,
                                  const std::vector<std::string>& lexicalNames,
                                  const std::vector<std::string>& definiteNames,
                                  const std::vector<std::string>& constNames,
                                  il::Function& ilFn) {
    const uint32_t depth = envDepthOf(scopeIndex);
    for (const auto& name : lexicalNames) {
        auto slotIt = envScopes_[scopeIndex].slotOf.find(name);
        // No slot means nothing can observe the binding uninitialized: no
        // closure reaches it and no read in its own scope is written above it,
        // so it lives in SSA and its declaration is the first mention of it.
        if (slotIt == envScopes_[scopeIndex].slotOf.end()) continue;
        // Something at this depth already owns the name: a parameter, or a
        // second declaration of it. Both are the declaration path's error to
        // report, and putting the marker in the slot would answer them with a
        // dead zone the language does not give either one.
        auto bound = activeVarMap_.find(name);
        if (bound != activeVarMap_.end() &&
            varBindings_[bound->second].scopeDepth == currentScopeDepth_) {
            continue;
        }
        const uint32_t slot = slotIt->second;
        envScopes_[scopeIndex].slotIsLexical[slot] = true;

        // A definitely-assigned binding gets no marker and no check. The
        // binding stays lexical — the slot exists, the declaration still ends
        // a dead zone the language gives it, and every rule that turns on
        // `let`-ness is unchanged — but the record's slot holds `undefined`
        // until the initializer runs rather than the uninitialized singleton.
        // That is the deliberate failure mode: if this analysis were ever
        // wrong, a read of one answers `undefined` instead of throwing, and
        // never reads a marker's bits as a value.
        if (std::find(constNames.begin(), constNames.end(), name) != constNames.end()) {
            envScopes_[scopeIndex].slotImmutable[slot] = SlotImmutability::Throws;
        }
        if (!definiteInitDisabled() &&
            std::find(definiteNames.begin(), definiteNames.end(), name) != definiteNames.end()) {
            envScopes_[scopeIndex].slotIsDefiniteInit[slot] = true;
        } else {
            il::Instruction inst;
            inst.op = il::Op::EnvInitTdz;
            inst.type = il::Type::Void;
            inst.result = il::kNoValue;
            inst.operands = {currentEnv(ilFn)};
            inst.envDepth = depth;
            inst.envIndex = slot;
            emitInst(ilFn, inst);
        }

        if (!declareVariable(name, il::Type::Dynamic, /*isConst=*/false, /*isLet=*/true,
                             /*isVar=*/false, /*isInitialized=*/false, il::kNoValue, Span{})) {
            continue;
        }
        varBindings_[activeVarMap_.at(name)].isTdzHoisted = true;
    }
}

// `assigning` says this write comes from an assignment rather than from the
// declaration that ends the binding's dead zone. 6.2.5.6 PutValue reaches
// 9.1.1.1.5 SetMutableBinding, which throws a ReferenceError for an
// uninitialized binding exactly as a read does — so an assignment to a lexical
// slot is CHECKED first, by the same instruction a read uses. Its result is
// discarded: what is wanted from it is the throw.
void Lowerer::emitEnvSet(uint32_t depth, uint32_t index, Value val, il::Function& ilFn,
                         bool assigning) {
    // 9.1.1.1.5's two checks, in the order the step list runs them: step 5
    // raises a ReferenceError for a binding still in its dead zone, and step 7
    // an immutable one's TypeError. Both DROP the store, which is why this is
    // decided here and not at each of the three call sites that assign to a
    // name (assignment, update, destructuring) — a store that reached the slot
    // from any of them would let a program rename its own function.
    //
    // What decides step 7's throw is S, and S is the BINDING's: `const` is
    // created with CreateImmutableBinding(name, true) (14.3.1.1), so an
    // assignment to one is a TypeError in sloppy code as much as in strict.
    // Only the other immutable binding in the language — a named function
    // expression's own name, created with `false` at 15.2.5 — takes S from the
    // assigning code, and that one still stores nothing either way.
    const SlotImmutability immutable = envSlotImmutability(depth, index);
    if (assigning && envSlotIsLexical(depth, index) && !envSlotDefiniteInit(depth, index)) {
        emitEnvGet(depth, index, ilFn);
    }
    if (assigning && immutable != SlotImmutability::Mutable) {
        if (immutable == SlotImmutability::Throws || strictCode_) {
            emitImmutableAssign(envScopes_[envScopes_.size() - 1 - depth].slotNames[index],
                                ilFn);
        }
        return;
    }
    Value boxed = boxValueIfNeeded(val, ilFn);
    il::Instruction inst;
    inst.op = il::Op::EnvSet;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {currentEnv(ilFn), boxed.id};
    inst.envDepth = depth;
    inst.envIndex = index;
    emitInst(ilFn, inst);
}

// The strict half of the rule above, as an instruction. Its result is
// discarded: what is wanted from it is the throw, and the backend's exception
// test after it is what carries control to the handler.
void Lowerer::emitImmutableAssign(const std::string& name, il::Function& ilFn) {
    il::Instruction inst;
    inst.op = il::Op::ImmutableAssign;
    inst.type = il::Type::Dynamic;
    inst.result = ilFn.valueCount++;
    inst.keyIndex = getKeyConstantIndex(name);
    emitInst(ilFn, inst);
}

uint32_t Lowerer::envDepthOf(size_t scopeIndex) const {
    return static_cast<uint32_t>(envScopes_.size() - 1 - scopeIndex);
}

Lowerer::Value Lowerer::readBinding(const VarBinding& b, il::Function& ilFn) {
    if (!b.inEnv) return Value{b.valueId, b.type};
    return emitEnvGet(envDepthOf(b.envScopeIndex), b.envSlot, ilFn);
}

// 9.1.1.1.5 step 7 for the bindings `emitEnvSet` never sees. A `const` no
// closure reads gets no environment slot at all — its value is an SSA register
// — so the immutable arm above has nothing to fire on and an assignment to it
// was a RENAME: the store went through and the next read saw the new value.
//
// The TypeError is the same instruction the captured form raises, and the
// store is dropped the same way. Answering true is what tells the four
// assignment paths (assignment, compound assignment, update, destructuring)
// not to write.
bool Lowerer::refuseConstAssignment(const VarBinding& b, il::Function& ilFn) {
    if (!b.isConst) return false;
    emitImmutableAssign(b.name, ilFn);
    return true;
}

void Lowerer::writeBinding(VarBinding& b, Value val, il::Function& ilFn) {
    if (b.inEnv) {
        emitEnvSet(envDepthOf(b.envScopeIndex), b.envSlot, val, ilFn, /*assigning=*/true);
        b.isInitialized = true;
        return;
    }
    b.valueId = val.id;
    b.type = val.type;
    b.isInitialized = true;
}

// A free variable of the function being lowered, resolved against the
// environments of enclosing scopes.
bool Lowerer::findEnclosingEnvVar(const std::string& name, uint32_t& depth,
                                  uint32_t& index) const {
    for (size_t i = envScopes_.size(); i-- > 0;) {
        auto it = envScopes_[i].slotOf.find(name);
        if (it != envScopes_[i].slotOf.end()) {
            depth = envDepthOf(i);
            index = it->second;
            return true;
        }
    }
    return false;
}

// THE STATIC CALL PLAN. A `function f() {}` written in a scope is the one
// binding form whose value this compilation knows outright: the declaration IS
// the value, it is installed before the scope's first statement runs, and — if
// no assignment anywhere in the scope's lexical reach names `f` — nothing the
// program can do will ever put anything else in the slot. The environment that
// closure captured is this scope's own record, because a nested declaration is
// created over the record innermost where it is written, which is the record
// that holds its binding.
//
// Those two facts together are a DIRECT CALL EDGE with no guard: a caller
// anywhere below counts parent links to the record and calls the function.
// Nothing here is a guess about the running program — it is the same class of
// fact `llvm_env.cpp` calls the static plan, and, like the depth and index of
// every env access, it is established before any IL exists.
//
// The refusals, all of them name-based and all in the safe direction:
//   - a name assigned or declared anywhere in the subtree, nested closures and
//     class bodies included (`getDeeplyAssignedNames`);
//   - a name a parameter default writes, which is code of this scope the body
//     does not contain;
//   - a declaration that is not the plain form: a generator or an async
//     function is lowered into a frame plus a resume function, so the value in
//     the slot is not the body this would call.
// A refusal costs the edge and nothing else: the call takes the dynamic path
// it took before.
void Lowerer::planStableFunctionSlots(const std::vector<const ast::Stmt*>& stmts,
                                      const std::vector<ast::Param>* params,
                                      EnvScopeInfo& info) const {
    bool anyDeclaration = false;
    for (const auto* stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt);
        if (fnDecl != nullptr && !fnDecl->isGenerator && !fnDecl->isAsync &&
            info.slotOf.contains(fnDecl->name)) {
            anyDeclaration = true;
            break;
        }
    }
    if (!anyDeclaration) return;

    std::unordered_set<std::string> rebound = ast::getDeeplyAssignedNames(stmts);
    if (params != nullptr) {
        for (const auto& p : *params) {
            if (!p.defaultValue) continue;
            for (auto& name : ast::getDeeplyAssignedNames(*p.defaultValue)) {
                rebound.insert(std::move(name));
            }
        }
    }

    info.slotIsStableFn.assign(info.slotNames.size(), false);
    info.slotFnIndex.assign(info.slotNames.size(), kNoStableFn);
    for (const auto* stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt);
        if (fnDecl == nullptr || fnDecl->isGenerator || fnDecl->isAsync) continue;
        auto slot = info.slotOf.find(fnDecl->name);
        if (slot == info.slotOf.end()) continue;
        if (rebound.contains(fnDecl->name)) continue;
        // Two declarations of the same name in one list: the second wins and
        // the first is dead, so neither is a fact about what the slot holds.
        if (info.slotIsStableFn[slot->second]) {
            info.slotIsStableFn[slot->second] = false;
            continue;
        }
        info.slotIsStableFn[slot->second] = true;
    }
}

void Lowerer::planStableFunctionSlots(const std::vector<ast::StmtPtr>& stmts,
                                      const std::vector<ast::Param>* params,
                                      EnvScopeInfo& info) const {
    std::vector<const ast::Stmt*> raw;
    raw.reserve(stmts.size());
    for (const auto& s : stmts) raw.push_back(s.get());
    planStableFunctionSlots(raw, params, info);
}

bool Lowerer::closureParamProofDisabled() {
    static const bool disabled = [] {
        const char* env = std::getenv("BRONZE_NO_CLOSURE_PARAM_PROOF");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return disabled;
}

namespace {

// Every mention of one name under a function body, split into the two things a
// mention can be: the CALLEE of an ordinary call — whose argument list is then
// evidence about the parameters — and anything else at all, which is the
// function value leaving through a door this analysis cannot follow.
//
// Built on `ast::detail::IdentVisitor` and not beside it, because the walk that
// decides "is this every mention?" must be THE walk that finds mentions:
// a form this one forgot to descend into is not a missed optimization, it is an
// unsound proof. Two overrides, and both are removals:
//
//   - `Call`: a callee that is exactly the bare name is recorded as a site and
//     NOT as an escape; the callee of anything else, and every argument, walk
//     normally. `?.()` is a site too — it either calls with these arguments or
//     does not call at all. `f(...xs)` is refused outright, because a spread
//     breaks the one-argument-per-parameter correspondence the claim IS.
//   - `FunctionDecl`: the declaration's own name is a binding, not a mention.
//     The base class records it (it is a name an enclosing scope must put in a
//     record), which would make every function an escape of itself.
//
// A `VarDecl` of the name still counts as a mention, and so as an escape. That
// is deliberately blunt and in the safe direction: a redeclaration means the
// name does not denote one binding throughout, and modelling shadowing here
// would be a scope resolver, not a proof.
class CalleeOnlyScan final : public ast::detail::IdentVisitor {
public:
    explicit CalleeOnlyScan(std::string name) : name_(std::move(name)) {}

    std::vector<const ast::Call*> sites;
    bool escaped = false;
    bool spread = false;

    void visit(const ast::Call& c) override {
        const auto* id = dynamic_cast<const ast::Ident*>(c.callee.get());
        if (id != nullptr && id->name == name_) {
            for (const auto& arg : c.args) {
                if (dynamic_cast<const ast::SpreadElement*>(arg.get()) != nullptr) spread = true;
            }
            sites.push_back(&c);
        } else {
            c.callee->accept(*this);
        }
        for (const auto& arg : c.args) arg->accept(*this);
    }

    void visit(const ast::FunctionDecl& f) override {
        ast::detail::visitParamExprs(f.params, *this);
        for (const auto& s : f.body) {
            if (s) s->accept(*this);
        }
    }

    void visit(const ast::Ident& i) override {
        if (i.name == name_) escaped = true;
        ast::detail::IdentVisitor::visit(i);
    }

    void visit(const ast::VarDecl& v) override {
        if (!v.pattern && v.name == name_) escaped = true;
        ast::detail::IdentVisitor::visit(v);
    }

private:
    std::string name_;
};

}  // namespace

// THE PARAMETER PROOF FOR A CLOSURE, and the general form of what `param
// f(x): number` does in a manifest by hand.
//
// Stage E3 wrote down the gap: a class method's parameters are typed by joining
// what every call site passes (types/flow_expr.cpp, `contributeArgs`), a module
// function's by the same join over its enumerated callers (types/infer.cpp),
// and a nested `function f() {}` by nothing — it is reached through a function
// value, so inference has no `functionIndex` for it and no signature that can
// speak for its result or its arguments. `bench/pins/env-slot-kernel.pins`
// carries five `param` lines that exist only because of that hole.
//
// But a nested declaration's callers ARE enumerable, on exactly the terms the
// static call plan above already establishes for its VALUE. The declaration is
// the value, it is installed before the scope's first statement runs, and the
// scope's whole lexical reach is one subtree this compilation holds. So: walk
// that subtree once, and if every mention of the name is the callee of an
// ordinary call, those calls are ALL the calls, and the join over their
// arguments at position k is a fact about every value parameter k will ever
// hold — the same join, over the same lattice, that the two paths above run.
//
// This is a PROOF and not a pin. Every clause below is a refusal in the safe
// direction, and a refusal costs the f64 slot and nothing else:
//
//   - a plain declaration only: a generator or an async function's parameters
//     are bound by a resume edge, not by the call.
//   - no default, rest or pattern parameter, because the value bound is then
//     not the value passed — the same three positions `applySignaturePins`
//     refuses, and for the same reason: there is no `undefined` in an f64.
//   - the name is not rebound anywhere in the subtree (`getDeeplyAssignedNames`,
//     the stable-plan's test), and every mention of it is a callee.
//   - no spread at any site.
//   - every site supplies position k, and the argument there is a proven
//     Number. A SHORT call binds `undefined`, which is not a Number, so a
//     single one refuses the position.
//
// What it deliberately does NOT reach: a closure that escapes. `env_slot_kernel`
// ends `return render;`, so `render`'s own parameter stays unproven and stays
// pinned — the manifest says so. That is the honest boundary of this mechanism:
// it types the closures a factory calls, not the closure the factory hands out.
void Lowerer::planClosureParamNumbers(const std::vector<ast::Param>& params,
                                      const std::vector<ast::StmtPtr>& stmts) {
    if (inference_ == nullptr || closureParamProofDisabled()) return;

    bool anyDeclaration = false;
    for (const auto& stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt.get());
        if (fnDecl != nullptr && !fnDecl->isGenerator && !fnDecl->isAsync &&
            !fnDecl->params.empty()) {
            anyDeclaration = true;
            break;
        }
    }
    if (!anyDeclaration) return;

    std::unordered_set<std::string> rebound = ast::getDeeplyAssignedNames(stmts);
    for (const auto& p : params) {
        if (!p.defaultValue) continue;
        for (auto& name : ast::getDeeplyAssignedNames(*p.defaultValue)) {
            rebound.insert(std::move(name));
        }
    }

    for (const auto& stmt : stmts) {
        const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmt.get());
        if (fnDecl == nullptr || fnDecl->isGenerator || fnDecl->isAsync) continue;
        if (fnDecl->name.empty() || fnDecl->params.empty()) continue;
        if (rebound.contains(fnDecl->name)) continue;
        if (provenClosureParams_.contains(fnDecl)) continue;
        bool shapeFits = true;
        for (const auto& p : fnDecl->params) {
            if (p.defaultValue || p.isRest || p.pattern || p.name.empty()) shapeFits = false;
        }
        if (!shapeFits) continue;

        CalleeOnlyScan scan(fnDecl->name);
        for (const auto& s : stmts) {
            if (s) s->accept(scan);
        }
        for (const auto& p : params) {
            if (p.defaultValue) p.defaultValue->accept(scan);
            if (p.pattern) ast::detail::visitPatternExprs(p.pattern.get(), scan);
        }
        if (scan.escaped || scan.spread || scan.sites.empty()) continue;

        std::vector<bool> proven(fnDecl->params.size(), true);
        for (const ast::Call* site : scan.sites) {
            for (size_t k = 0; k < proven.size(); ++k) {
                if (k >= site->args.size() || !provenNumber(*site->args[k])) proven[k] = false;
            }
        }
        bool any = false;
        for (const bool p : proven) any = any || p;
        if (any) provenClosureParams_.emplace(fnDecl, std::move(proven));
    }
}

void Lowerer::applyProvenClosureParams(const ast::Node& site,
                                       const std::vector<ast::Param>& params,
                                       il::Function& fn) const {
    const auto* decl = dynamic_cast<const ast::FunctionDecl*>(&site);
    if (decl == nullptr) return;
    const auto it = provenClosureParams_.find(decl);
    if (it == provenClosureParams_.end()) return;
    const size_t base = fn.firstSourceParam();
    for (size_t i = 0; i < params.size() && i < it->second.size() && i + base < fn.params.size();
         ++i) {
        // Only ever Dynamic to F64. A `--pins` entry that already said the same
        // thing has run just above; the proof and the promise agree, and
        // neither can be reached by a slot the other typed differently.
        if (it->second[i] && fn.params[i + base].type == il::Type::Dynamic) {
            fn.params[i + base].type = il::Type::F64;
        }
    }
}

// The point at which a slot's function becomes known — and, as a side effect
// worth stating, the reason the edge graph is ACYCLIC.
//
// A site gets an edge to `F` only if this ran for `F` before the site's own
// body was lowered, and this runs only after `lowerClosure(F)` has returned.
// So every edge points at a function whose lowering finished strictly earlier,
// which is a strict partial order. A self-call is not an edge (the slot is
// still empty while the body is being lowered), a forward reference between two
// siblings gets an edge in one direction only, and a closure nested inside `F`
// cannot reach `F`'s own slot. That is what makes the backend's `alwaysinline`
// ask on these calls safe to make unconditionally.
void Lowerer::recordStableFunctionSlot(size_t scopeIndex, uint32_t slot, uint32_t fnIndex) {
    // The closure was created over `currentEnv`, which is the INNERMOST record;
    // the plan's claim is that this is also the record holding the binding, and
    // a caller reaches it by counting parent links to `scopeIndex`. Anything
    // else — a `var` promoted to the function record from inside a block, say —
    // would break that identity, so it is refused rather than assumed.
    if (envScopes_.empty() || scopeIndex != envScopes_.size() - 1) return;
    EnvScopeInfo& info = envScopes_[scopeIndex];
    if (slot >= info.slotIsStableFn.size()) return;
    if (!info.slotIsStableFn[slot]) return;
    info.slotFnIndex[slot] = fnIndex;
}

// The resolution a CALL of `name` performs, asked of the plan rather than of
// the value. It has to agree exactly with the one `lowerExpr` performs for the
// same identifier — a local binding first, then the environment chain
// innermost-out — because a different answer here is a call to a different
// function.
bool Lowerer::findStableFunctionCallee(const std::string& name, uint32_t& envHops,
                                       uint32_t& fnIndex) const {
    // The A/B seam, in the house style of `BRONZE_NO_DIRECT_METHOD`: `1` refuses
    // every edge and leaves the rest of the compiler alone, so the two columns
    // of a measurement come out of one binary and can be interleaved.
    static const bool disabled = [] {
        const char* env = std::getenv("BRONZE_NO_CLOSURE_EDGE");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    if (disabled) return false;
    // Never from inside a machine body. `currentEnv` there is a walk DOWN from
    // the frame emitted at the point of use, and an argument expression holding
    // a `yield` splits the block between that walk and the call — so the record
    // this edge would pass is a value whose definition may not dominate its use.
    // The plan already refuses a machine body's own record (lower.cpp,
    // `enterFunctionEnv`); this is the other side of the same refusal.
    if (generator_) return false;

    size_t scopeIndex = 0;
    uint32_t slot = 0;
    auto local = activeVarMap_.find(name);
    if (local != activeVarMap_.end()) {
        const VarBinding& b = varBindings_[local->second];
        // An uncaptured declaration lives in SSA, and its value is an ordinary
        // one this does not speak for.
        if (!b.inEnv) return false;
        scopeIndex = b.envScopeIndex;
        slot = b.envSlot;
    } else {
        bool found = false;
        for (size_t i = envScopes_.size(); i-- > 0;) {
            auto it = envScopes_[i].slotOf.find(name);
            if (it == envScopes_[i].slotOf.end()) continue;
            scopeIndex = i;
            slot = it->second;
            found = true;
            break;
        }
        if (!found) return false;
    }
    if (scopeIndex >= envScopes_.size()) return false;
    const EnvScopeInfo& info = envScopes_[scopeIndex];
    if (slot >= info.slotFnIndex.size() || slot >= info.slotIsStableFn.size()) return false;
    if (!info.slotIsStableFn[slot]) return false;
    if (info.slotFnIndex[slot] == kNoStableFn) return false;
    envHops = envDepthOf(scopeIndex);
    fnIndex = info.slotFnIndex[slot];
    return true;
}

void Lowerer::enterScope() {
    currentScopeDepth_++;
    scopeHasEnv_.push_back(false);
}

// Scope entry that first gives the scope an environment record if any
// of its own declarations are captured. For a loop body this runs once
// per iteration at runtime, which is exactly the per-iteration binding
// the language specifies.
void Lowerer::enterScope(const std::vector<ast::StmtPtr>& stmts, il::Function& ilFn,
                         const std::vector<std::string>& extraDeclarations,
                         const std::vector<std::string>& extraLexicalDeclarations) {
    currentScopeDepth_++;
    std::vector<std::string> slots;
    auto addSlot = [&](const std::string& name) {
        if (name.empty()) return;
        if (std::find(slots.begin(), slots.end(), name) != slots.end()) return;
        slots.push_back(name);
    };
    // for-of's loop variable is declared by the loop HEAD but belongs to the
    // body's scope, so it is not in the statement list and has to be named
    // here. Without it a closure over the loop variable would find no slot and
    // read SSA that the next iteration overwrites. A destructuring head names
    // several.
    for (const auto& name : extraDeclarations) {
        if (memoryNames_.contains(name)) addSlot(name);
    }
    // Unfiltered, unlike everything else here: a switch body's lexical binding
    // needs a slot BECAUSE the dead zone is what makes it well defined, not
    // because something captured it.
    for (const auto& name : extraLexicalDeclarations) addSlot(name);
    for (const auto& name : ast::getScopeDeclarations(stmts)) {
        if (memoryNames_.contains(name)) addSlot(name);
    }
    if (slots.empty()) {
        scopeHasEnv_.push_back(false);
        return;
    }
    EnvScopeInfo info;
    // One more slot, in a generator only, for the record of whatever scope opens
    // inside this one. The chain of them is the only way down from the frame,
    // and a suspension needs one — see `currentEnv`. Named, so a dump of the
    // record says what the extra word is.
    if (generator_) {
        info.childSlot = static_cast<uint32_t>(slots.size());
        slots.emplace_back(generatorEnvSlotName());
    }
    for (uint32_t i = 0; i < slots.size(); ++i) info.slotOf[slots[i]] = i;
    info.slotNames = slots;
    info.slotIsLexical.assign(slots.size(), false);
    info.slotIsDefiniteInit.assign(slots.size(), false);
    info.slotImmutable.assign(slots.size(), SlotImmutability::Mutable);
    // A block's own function declarations, on the same terms as a function
    // body's (lower.cpp, `enterFunctionEnv`) — and never inside a machine body,
    // for the reason stated there.
    if (!generator_) planStableFunctionSlots(stmts, /*params=*/nullptr, info);
    const il::ValueId parentRecord = generator_ ? currentEnv(ilFn) : il::kNoValue;
    const uint32_t parentChildSlot =
        generator_ ? envScopes_.back().childSlot : UINT32_MAX;
    info.envValue = emitEnvCreate(static_cast<uint32_t>(slots.size()), ilFn);
    if (generator_) {
        il::Instruction link;
        link.op = il::Op::EnvSet;
        link.type = il::Type::Void;
        link.result = il::kNoValue;
        link.operands = {parentRecord, info.envValue};
        link.envDepth = 0;
        link.envIndex = parentChildSlot;
        emitInst(ilFn, link);
    }
    envScopes_.push_back(std::move(info));
    savedEnvValues_.push_back(currentEnvValue_);
    currentEnvValue_ = envScopes_.back().envValue;
    scopeHasEnv_.push_back(true);

    std::vector<std::string> lexical = extraLexicalDeclarations;
    for (auto& name : ast::getLexicalDeclarations(stmts)) lexical.push_back(std::move(name));
    // The EXTRA names are not candidates: they come from a head this
    // statement list does not contain (a `for`-`of` binding, a catch
    // parameter), so nothing here can say when they are initialized.
    openLexicalBindings(envScopes_.size() - 1, lexical,
                        ast::getDefinitelyAssignedLexicalNames(stmts),
                        ast::getConstDeclarations(stmts), ilFn);
}

void Lowerer::pushSyntheticEnv(std::vector<std::string> slots, il::Function& ilFn) {
    currentScopeDepth_++;
    EnvScopeInfo info;
    // The generator's downward link, for the reason `enterScope` adds one: a
    // resume edge defines no SSA value, so the chain of child slots is the only
    // way from the frame to the record innermost at a suspension point.
    if (generator_) {
        info.childSlot = static_cast<uint32_t>(slots.size());
        slots.emplace_back(generatorEnvSlotName());
    }
    for (uint32_t i = 0; i < slots.size(); ++i) info.slotOf[slots[i]] = i;
    info.slotNames = slots;
    info.slotIsLexical.assign(slots.size(), false);
    info.slotIsDefiniteInit.assign(slots.size(), false);
    info.slotImmutable.assign(slots.size(), SlotImmutability::Mutable);
    const il::ValueId parentRecord = generator_ ? currentEnv(ilFn) : il::kNoValue;
    const uint32_t parentChildSlot = generator_ ? envScopes_.back().childSlot : UINT32_MAX;
    info.envValue = emitEnvCreate(static_cast<uint32_t>(slots.size()), ilFn);
    if (generator_) {
        il::Instruction link;
        link.op = il::Op::EnvSet;
        link.type = il::Type::Void;
        link.result = il::kNoValue;
        link.operands = {parentRecord, info.envValue};
        link.envDepth = 0;
        link.envIndex = parentChildSlot;
        emitInst(ilFn, link);
    }
    envScopes_.push_back(std::move(info));
    savedEnvValues_.push_back(currentEnvValue_);
    currentEnvValue_ = envScopes_.back().envValue;
    scopeHasEnv_.push_back(true);
}

void Lowerer::exitScope() {
    // Leaving a scope UNCOVERS what its declarations shadowed; it does not
    // delete the name. Erasing outright made an inner `let x` remove the
    // enclosing `x` for the rest of the function.
    std::vector<std::pair<std::string, size_t>> toRestore;
    for (const auto& entry : activeVarMap_) {
        const auto& binding = varBindings_[entry.second];
        if (binding.scopeDepth == currentScopeDepth_ && !binding.isVar) {
            toRestore.emplace_back(entry.first, binding.shadowedBinding);
        }
    }
    for (const auto& [name, shadowed] : toRestore) {
        if (shadowed == SIZE_MAX) {
            activeVarMap_.erase(name);
        } else {
            activeVarMap_[name] = shadowed;
        }
    }
    if (!scopeHasEnv_.empty()) {
        if (scopeHasEnv_.back()) {
            envScopes_.pop_back();
            currentEnvValue_ = savedEnvValues_.back();
            savedEnvValues_.pop_back();
        }
        scopeHasEnv_.pop_back();
    }
    currentScopeDepth_--;
}

// Shared by function expressions and nested function declarations: both produce
// a closure value over the environment that is innermost at the creation site.
std::optional<Lowerer::Value> Lowerer::lowerNamedEvaluation(const ast::Expr& expr,
                                                            const std::string& name,
                                                            il::Function& ilFn) {
    const auto* fn = dynamic_cast<const ast::FunctionExpr*>(&expr);
    // A function expression that wrote its OWN name keeps it: 15.2.5 binds that
    // name inside the body and 8.6.2 does not apply, so `const f = function g()
    // {}` has `f.name === "g"`.
    if (!fn || !fn->name.empty()) return lowerExpr(expr, ilFn);
    return lowerClosure(*fn, /*declaredName=*/"", name, fn->params, fn->returnType, fn->body,
                        fn->span, ilFn, fn->isArrow);
}

std::optional<Lowerer::Value> Lowerer::lowerClosure(const ast::Node& site,
                                                    const std::string& declaredName,
                                                    const std::optional<std::string>& jsName,
                                                    const std::vector<ast::Param>& params,
                                                    const std::string& returnTypeAnn,
                                                    const std::vector<ast::StmtPtr>& body,
                                                    Span span, il::Function& ilFn, bool isArrow,
                                                    bool bindsOwnName) {
    std::string fnName = declaredName;
    if (fnName.empty()) {
        fnName = "__anon_fn_" + std::to_string(ilModule_.functions.size());
    }
    il::Function newFn;
    newFn.name = fnName;
    // The key constant is allocated here even for the empty name, because "" is
    // a real answer — 10.2.9 gives an anonymous function expression exactly that
    // — and the runtime has to be able to tell it from "no name recorded".
    if (jsName) newFn.nameKeyIndex = getKeyConstantIndex(*jsName);
    newFn.returnType = il::Type::Dynamic;
    const size_t outerEnvDepth = envScopes_.size();
    const auto* siteFnExpr = dynamic_cast<const ast::FunctionExpr*>(&site);
    const auto* siteFnDecl = dynamic_cast<const ast::FunctionDecl*>(&site);
    const bool isGenerator = (siteFnExpr && siteFnExpr->isGenerator) ||
                             (siteFnDecl && siteFnDecl->isGenerator);
    const bool isAsync =
        (siteFnExpr && siteFnExpr->isAsync) || (siteFnDecl && siteFnDecl->isAsync);

    // 15.2.5 InstantiateOrdinaryFunctionExpression, the named branch: a
    // function expression that wrote its own name is created in a declarative
    // environment of its own holding ONE immutable binding of that name, and
    // that record — not the enclosing one — is the closure's scope. So the
    // recursive reference in `(function fact(n) { return n * fact(n - 1) })` is
    // an ordinary capture: the body resolves `fact` through the environment
    // chain like any other free name, one hop further out than its own record.
    //
    // Built only where the body (or a parameter default, which is code of this
    // function that the body does not contain) actually mentions the name. The
    // binding is unobservable otherwise, and paying a record per evaluation for
    // it would change the IL of every `function f() {}` in the program.
    il::ValueId nfeEnv = il::kNoValue;
    if (bindsOwnName && !isArrow && !declaredName.empty()) {
        auto referenced = ast::getReferencedNames(body);
        for (auto& name : ast::getParamReferencedNames(params)) {
            referenced.insert(std::move(name));
        }
        if (referenced.contains(declaredName)) {
            nfeEnv = emitEnvCreate(1, ilFn);
            EnvScopeInfo info;
            info.slotOf[declaredName] = 0;
            info.slotNames = {declaredName};
            info.slotIsLexical.assign(1, false);
            info.slotIsDefiniteInit.assign(1, false);
            // The whole point of the record: 15.2.5 step 3 is
            // CreateImmutableBinding, so an inner `fact = x` stores nothing.
            info.slotImmutable.assign(1, SlotImmutability::Silent);
            info.envValue = nfeEnv;
            envScopes_.push_back(std::move(info));
            currentEnvValue_ = nfeEnv;
        }
    }

    // A function only requires an environment parameter when it captures from an
    // enclosing non-module scope, is a generator/async function, has an NFE
    // binding, or is an arrow reading outer `this`/`arguments`. Top-level methods
    // and closures without outer lexical scopes have `needsEnv == false`, allowing
    // direct calls and IC latching.
    const size_t nonModuleScopeThreshold =
        (moduleEnvScope_ != std::numeric_limits<size_t>::max()) ? 1 : 0;
    bool needsEnv = true;
    if (!isGenerator && !isAsync && nfeEnv == il::kNoValue &&
        (!isArrow || (!ast::usesThis(params, body) && !ast::usesArguments(params, body))) &&
        outerEnvDepth <= nonModuleScopeThreshold) {
        needsEnv = false;
    }

    newFn.needsEnv = needsEnv;
    if (needsEnv) {
        newFn.params.push_back({"__env", il::Type::Dynamic});
    }
    // An arrow deliberately does NOT take a receiver parameter, however it is
    // called: its `this` is the enclosing function's, read from the
    // environment. Giving it one would be a second, contradictory answer to the
    // same question.
    if (!isArrow && ast::usesThis(params, body)) {
        newFn.needsThis = true;
        newFn.params.push_back({"__this", il::Type::Dynamic});
    }
    // An arrow has no `arguments` either, and for the same reason: it sees the
    // enclosing function's through the environment.
    if (!isArrow && ast::usesArguments(params, body)) {
        newFn.needsArguments = true;
        newFn.params.push_back({"__arguments", il::Type::Dynamic});
    }
    // A closure's parameters and return are always the uniform dynamic
    // convention, and an annotation cannot change that.
    //
    // A closure PARAMETER has no proof and cannot have one: a signature is
    // inferred by joining over every call site, which is sound only for a name
    // whose callers this compilation can enumerate, and a closure is reached
    // through a function value — signature specialization excludes it by
    // construction. So the proof handed to the check is `dynamic`, which is the
    // honest report of "nothing was observed here", and every parameter
    // annotation on a closure is discarded with a warning saying so.
    for (const auto& param : params) {
        newFn.params.push_back(
            {param.pattern ? "__pattern" + std::to_string(newFn.params.size()) : param.name,
             il::Type::Dynamic});
        if (!checkAnnotation(param.typeAnnotation, span, param.name, types::Type::dynamic())) {
            return std::nullopt;
        }
    }
    applyParamShape(params, newFn);
    // The RETURN is different in kind: what a body returns is a fact about
    // the body alone, and inference joins every `return` in it — so a return
    // annotation on a closure CAN agree with a proof, where a parameter
    // annotation cannot (that needs escape analysis). It buys nothing at the
    // IL level: the return type above stays dynamic, because that is the
    // calling convention and no annotation may widen it.
    //
    // Reported on `fnName`, which for an anonymous function expression is
    // the synthesized `__anon_fn_N` — deliberately, because that is the name
    // it has in the IL dump and the span already points at the source.
    if (!checkAnnotation(returnTypeAnn, span, fnName, provenClosureReturn(site))) {
        return std::nullopt;
    }
    newFn.valueCount = static_cast<uint32_t>(newFn.params.size());
    // `span` is the SITE's, and for a class that is the whole `class C {...}`
    // rather than the constructor's own text — which is exactly what
    // `Class.toString()` has to return, so no caller special-cases it.
    newFn.sourceFile = span.file;
    newFn.sourceBegin = span.begin;
    newFn.sourceEnd = span.end;

    size_t outerBlockIdx = currentBlockIdx_;
    auto outerVarBindings = varBindings_;
    auto outerActiveVarMap = activeVarMap_;
    // The `func.ref` memo is per IL FUNCTION: a `Value` in it names an
    // instruction result, and result ids are numbered within one function.
    // `lowerFunctionBody` clears it on the way in, which is only half of what
    // an entry/exit pair needs — without the restore, the enclosing function
    // came back holding the CALLEE's ids, so a later mention of a top-level
    // `function` in the caller read whatever instruction happened to have that
    // number. `(function () { h(g, 4); })(); h(g, 3);` called the anonymous
    // function twice and never called `h` again. Saved with the rest of the
    // per-function state, and restored below with it.
    auto outerFunctionRefMap = functionRefMap_;
    auto outerScopeDepth = currentScopeDepth_;
    auto outerVarDeclCounter = varDeclCounter_;
    auto outerJumpStack = jumpStack_;
    // Labels do not cross a function boundary: `break outer` inside a nested
    // function names nothing, and the outer label must not be visible to it.
    auto outerLabelStack = labelStack_;
    labelStack_.clear();
    auto outerScopeHasEnv = scopeHasEnv_;
    auto outerCaptured = capturedNames_;
    auto outerMemoryNames = memoryNames_;
    // A `return` inside a nested function runs THAT function's finallys and
    // none of the enclosing ones, exactly as `break outer` names nothing
    // across the same boundary.
    auto outerCleanupStack = cleanupStack_;
    cleanupStack_.clear();
    // A function written inside a generator body is not one: its `return` is an
    // ordinary return and it has no frame of the enclosing machine's.
    auto outerGenerator = std::move(generator_);
    generator_.reset();
    auto outerHandler = currentHandler_;
    currentHandler_ = il::kNoBlock;
    auto outerEnvValue = currentEnvValue_;
    auto outerThisValue = currentThisValue_;
    auto outerIsArrow = currentFunctionIsArrow_;
    currentFunctionIsArrow_ = isArrow;
    // Strictness comes from the FUNCTION NODE, not from the enclosing code:
    // the parser has already resolved inheritance (a function inside strict
    // code is strict) and a body's own `"use strict"` (a strict function
    // inside sloppy code). `site` is that node, which is why nothing here has
    // to be passed a flag.
    const bool outerStrict = strictCode_;
    if (siteFnExpr) {
        strictCode_ = siteFnExpr->strict;
    } else if (siteFnDecl) {
        strictCode_ = siteFnDecl->strict;
    }
    auto outerEnvBase = functionEnvBase_;
    auto outerEnvScope = functionEnvScope_;
    // `var` names are per FUNCTION, so the nested body's list must not outlive
    // it: leaving the callee's behind would make an enclosing free name look
    // like a `var` the callee declared.
    auto outerVarNames = functionVarNames_;
    // The typed-element binding scan asks about the body being lowered NOW;
    // lowerFunctionBody points it at the nested body, so the outer pointer
    // comes back with everything else here.
    const auto* outerBodyStmts = currentBodyStmts_;

    // A `FunctionDecl` site is always the ordinary form — there is no syntax
    // for a declaration that is a method or an arrow — so only the expression
    // carries a kind worth reading.
    newFn.fnFlags = functionObjectFlags(
        siteFnExpr ? siteFnExpr->kind : ast::FunctionKind::Normal, isGenerator, isAsync);
    // The `--pins` signature entries, before the body is lowered: the parameter
    // types are what the body's reads of them resolve against, and the return
    // type is part of the calling convention a recursive call already reads.
    if (!applySignaturePins(params, span, newFn)) return std::nullopt;
    // And the proof, which since stage E4 covers the case a pin used to be the
    // only answer for: a nested declaration whose every call site this
    // compilation can enumerate (`planClosureParamNumbers`). After the pins, so
    // that the manifest's error reporting still owns a position it cannot
    // honour; both only ever move a slot from Dynamic to F64, so the order
    // decides nothing else.
    applyProvenClosureParams(site, params, newFn);
    const bool bodyOk = lowerFunctionBody(params, body, newFn, isGenerator, isAsync);
    // The name's record is visible to the BODY and to nothing else — 15.2.5
    // creates it around the closure, not in the scope that wrote the
    // expression — so it leaves the stack the moment the body is lowered, and
    // before the balance check below counts what is left.
    if (nfeEnv != il::kNoValue) envScopes_.pop_back();

    generator_ = std::move(outerGenerator);
    strictCode_ = outerStrict;
    varBindings_ = outerVarBindings;
    activeVarMap_ = outerActiveVarMap;
    functionRefMap_ = outerFunctionRefMap;
    currentScopeDepth_ = outerScopeDepth;
    varDeclCounter_ = outerVarDeclCounter;
    jumpStack_ = outerJumpStack;
    labelStack_ = outerLabelStack;
    currentBlockIdx_ = outerBlockIdx;
    scopeHasEnv_ = outerScopeHasEnv;
    capturedNames_ = outerCaptured;
    memoryNames_ = outerMemoryNames;
    cleanupStack_ = outerCleanupStack;
    currentHandler_ = outerHandler;
    currentEnvValue_ = outerEnvValue;
    currentThisValue_ = outerThisValue;
    currentFunctionIsArrow_ = outerIsArrow;
    functionEnvBase_ = outerEnvBase;
    functionEnvScope_ = outerEnvScope;
    functionVarNames_ = outerVarNames;
    currentBodyStmts_ = outerBodyStmts;
    if (!bodyOk) {
        if (envScopes_.size() > outerEnvDepth) envScopes_.resize(outerEnvDepth);
        return std::nullopt;
    }
    if (envScopes_.size() != outerEnvDepth) {
        diags_.error(span, "internal: environment stack unbalanced after lowering " + fnName);
        return std::nullopt;
    }

    uint32_t createdFnIdx = static_cast<uint32_t>(ilModule_.functions.size());
    // NOT registered in `functionIndices_`. That map is the module's symbol
    // table — the names a reference anywhere in the module may resolve to
    // directly — and a nested function's name is not one of them: it is a
    // binding of the scope that wrote it, reached through `activeVarMap_` or
    // the environment chain like any other. Registering it put a scope-local
    // name into a module-wide table, and the two ways that was wrong are the
    // reason this line is a comment: an unrelated function's `f()` resolved to
    // a closure it cannot see, and a nested `function f` OVERWROTE a top-level
    // `f` of the same name, so every later call to the top-level one was
    // redirected to the inner one.
    ilModule_.functions.push_back(std::move(newFn));
    // Valid until the next closure is lowered, and read by exactly one caller:
    // `lowerClass`, immediately after this returns, to record which module
    // function a method NAME denotes (lowerer.h, `classMethods_`). A return
    // value would have been better and is not available — every caller of
    // `lowerClosure` wants the closure VALUE, and there is one of these.
    lastClosureFnIndex_ = createdFnIdx;

    // The closure captures the environment that is innermost right
    // here, at its creation site — or, for a named function expression, the
    // one-slot record built above, which chains to it. `currentEnv` cannot
    // answer that: the record left the scope stack with the body, because
    // nothing outside the closure may resolve a name through it.
    const il::ValueId enclosingEnv = nfeEnv != il::kNoValue ? nfeEnv : currentEnv(ilFn);
    il::ValueId envArg =
        enclosingEnv == il::kNoValue ? emitConstUndefined(ilFn) : enclosingEnv;
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::CreateFunction;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.calleeIndex = createdFnIdx;
    // The arity a CALL adapts to. A rest parameter is not one of them: it is
    // built from whatever is left over, so counting it would have short calls
    // padded with an `undefined` that the rest array then contained.
    // Zero for a closure that owns an `arguments` object, for the reason
    // `il::Function::adaptArity` records: padding would erase the difference
    // between `f(1)` and `f(1, undefined)`, which `arguments.length` sees.
    inst.immI32 = ilModule_.functions[createdFnIdx].needsArguments
                      ? 0
                      : static_cast<int32_t>(params.size()) -
                            (params.empty() || !params.back().isRest ? 0 : 1);
    inst.operands = {envArg};
    emitInst(ilFn, inst);
    // 15.2.5 step 5, InitializeBinding: the record holds the closure it is the
    // scope of, which is what closes the cycle the recursion walks. Written
    // through the record VALUE rather than through `emitEnvSet`, because the
    // scope that owns the slot is no longer on the stack a (depth, index) pair
    // is counted against.
    if (nfeEnv != il::kNoValue) {
        il::Instruction bind;
        bind.op = il::Op::EnvSet;
        bind.type = il::Type::Void;
        bind.result = il::kNoValue;
        bind.operands = {nfeEnv, res};
        bind.envDepth = 0;
        bind.envIndex = 0;
        emitInst(ilFn, bind);
    }
    return Value{res, il::Type::Dynamic};
}

}  // namespace bronze::lower
