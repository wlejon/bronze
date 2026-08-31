// The two halves of `EnvScopeInfo::slotIsStableFn`: the pass that marks a
// scope's unrebindable function declarations, the point at which one of those
// slots learns which IL function it holds, and the resolution a call of such a
// name performs against the mark.

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/assigned.h"
#include "lower/lowerer.h"

namespace bronze::lower {

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

}  // namespace bronze::lower
