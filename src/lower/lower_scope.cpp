// Scopes, bindings and environment records: where a declaration lives, how a
// captured one is read and written, and how a scope's record is created and
// unwound.


#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "ast/queries.h"
#include "lower/lowerer.h"

namespace bronze::lower {

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
    // A slot the number proof claimed (lower_env_slot_number.cpp): the load is
    // the one it always was and the
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
    // The `--pins` barrier for `function <fn>.<binding>: number`, and it is
    // keyed on the SLOT rather than on the record: stage E3 merges a callee's
    // frame region into its caller's, so a "frame" holds the slots of several
    // logical scopes and a barrier that asked about the frame would let a
    // frameless callee's write through. The depth/index pair the scope plan
    // resolved is the logical slot, and it is what this asks about.
    //
    // Only the PINNED slots (lowerer_state.h, `slotIsPinned`). A slot the
    // fixpoint typed has had every write to it audited already.
    {
        const EnvScopeInfo& scope = envScopes_[envScopes_.size() - 1 - depth];
        if (index < scope.slotIsPinned.size() && scope.slotIsPinned[index]) {
            emitPinGuard(val, scope.slotPinText[index], il::PinBarrier::Number, ilFn);
        }
        // The census's env-slot observation, on the same slot key and for the
        // same reason: the record is not the unit, the logical slot is.
        if (index < scope.slotIsCensus.size() && scope.slotIsCensus[index]) {
            const std::string target = scope.slotCensusText[index];
            emitCensusRecord(val, target, il::CensusSite::EnvSlot, ilFn);
        }
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

}  // namespace bronze::lower
