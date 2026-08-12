// Control-flow statements and the block-argument SSA joins they build:
// if/else, the three loop forms, break and continue (docs/0005).

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/assigned.h"
#include "ast/queries.h"
#include "lower/lowerer.h"

namespace bronze::lower {

// Current values of the given variables, coerced (in the current block)
// to the target block's parameter types, in parameter order.
std::vector<il::ValueId> Lowerer::collectEdgeArgs(const std::vector<std::string>& vars,
                                                 il::BlockId target, il::Function& ilFn) {
    std::vector<il::ValueId> args;
    for (size_t i = 0; i < vars.size(); ++i) {
        const auto& b = varBindings_[activeVarMap_[vars[i]]];
        args.push_back(coerceToType(Value{b.valueId, b.type},
                                    ilFn.blocks[target].params[i].type, ilFn).id);
    }
    return args;
}

// The parameters a loop's header, exit, and update/condition blocks all
// take: one per variable assigned anywhere in the loop, in declaration
// order (docs/0005 decision 2), typed by what inference proved holds at the
// loop's merges.
//
// The type comes from the analysis and never from the value live at loop
// entry. The entry value's type describes one edge; a block parameter has
// to describe all of them, and the back edge — which has not been lowered
// yet, and which is exactly where a binding's type changes — is one of
// them. Reading the entry value here is what compiled
// `let x = 1; while (c) { x = "s"; }` into an unbox of a string as a
// double. With no inference every parameter is dynamic, which is the sound
// answer for "the back edge could carry anything".
std::vector<Lowerer::LoopParam> Lowerer::collectLoopParams(
    const ast::Stmt& loopStmt, const std::unordered_set<std::string>& assigned) {
    std::vector<LoopParam> params;
    for (const auto& name : getActiveVarsInDeclOrder()) {
        if (!assigned.contains(name)) continue;
        // Becoming a loop parameter means being read on the entry edge, and
        // an annotated `let x: number;` has no value to read there —
        // undefined has no typed form (see lowerVarDecl). Diagnosed here by
        // name, like every other read before initialization (docs/0005),
        // rather than left to emit an edge argument that does not exist.
        const VarBinding& b = varBindings_[activeVarMap_[name]];
        if (!b.isInitialized) {
            diags_.error(loopStmt.span, std::string("use of '") + (b.isConst ? "const" : "let") +
                                            "' binding '" + name +
                                            "' before initialization (it is carried into the "
                                            "loop on the entry edge)");
            continue;
        }
        params.push_back(LoopParam{name, mergeParamType(loopStmt, name)});
    }
    return params;
}

// Add one block parameter per loop variable, fresh value ids from the
// function-wide counter, and report the ids in the same order.
std::unordered_map<std::string, il::ValueId> Lowerer::addLoopBlockParams(
    const std::vector<LoopParam>& loopParams, il::BlockId block, il::Function& ilFn) {
    std::unordered_map<std::string, il::ValueId> paramOf;
    for (const auto& param : loopParams) {
        il::ValueId pId = ilFn.valueCount++;
        ilFn.blocks[block].params.push_back({pId, param.type});
        paramOf[param.name] = pId;
    }
    return paramOf;
}

// Point each loop variable at the parameter the block just entered defines.
void Lowerer::bindLoopBlockParams(const std::vector<LoopParam>& loopParams,
                                  const std::unordered_map<std::string, il::ValueId>& paramOf) {
    for (const auto& param : loopParams) {
        auto& b = varBindings_[activeVarMap_[param.name]];
        b.valueId = paramOf.at(param.name);
        b.type = param.type;
    }
}

// Env-backed variables are memory, not SSA, so they never become join
// or loop-header parameters. This is the single funnel every join uses.
std::vector<std::string> Lowerer::getActiveVarsInDeclOrder() const {
    std::vector<const VarBinding*> active;
    for (const auto& entry : activeVarMap_) {
        if (varBindings_[entry.second].inEnv) continue;
        active.push_back(&varBindings_[entry.second]);
    }
    std::sort(active.begin(), active.end(), [](const VarBinding* a, const VarBinding* b) {
        return a->declOrder < b->declOrder;
    });
    std::vector<std::string> names;
    for (const auto* b : active) {
        names.push_back(b->name);
    }
    return names;
}

bool Lowerer::lowerIfStmt(const ast::IfStmt* ifStmt, il::Function& ilFn) {
    Value condVal = lowerCondition(*ifStmt->condition, ilFn);
    if (condVal.id == il::kNoValue) return false;

    il::BlockId bThen = createBlock(ilFn);
    il::BlockId bElse = createBlock(ilFn);
    il::BlockId bJoin = createBlock(ilFn);

    auto envBefore = activeVarMap_;
    size_t entryBlockIdx = currentBlockIdx_;

    // Assignments rebind varBindings_ entries in place, so each arm
    // must start from a value snapshot of the pre-if state, and the
    // join must compare snapshots, not the (shared) live slots.
    // (Snapshots here range over envBefore, not the live map, because
    // arm bodies enter and exit scopes.)
    auto snapshot = [&]() {
        VarStateMap snap;
        for (const auto& [name, idx] : envBefore) {
            snap[name] = VarState{varBindings_[idx].valueId, varBindings_[idx].type};
        }
        return snap;
    };
    auto restore = [&](const std::unordered_map<std::string, VarState>& snap) {
        for (const auto& [name, idx] : envBefore) {
            varBindings_[idx].valueId = snap.at(name).valueId;
            varBindings_[idx].type = snap.at(name).type;
        }
    };
    auto stateBefore = snapshot();

    // Then branch
    setCurrentBlock(bThen);
    enterScope(ifStmt->thenBody, ilFn);
    std::vector<const ast::Stmt*> thenStmts;
    for (const auto& s : ifStmt->thenBody) thenStmts.push_back(s.get());
    if (!lowerStmtList(thenStmts, ilFn)) return false;
    exitScope();
    auto stateThenEnd = snapshot();
    bool thenReaches = !currentBlockIsTerminated(ilFn);
    size_t thenEndBlockIdx = currentBlockIdx_;

    // Else branch, from the pre-if state
    activeVarMap_ = envBefore;
    restore(stateBefore);
    setCurrentBlock(bElse);
    enterScope(ifStmt->elseBody, ilFn);
    std::vector<const ast::Stmt*> elseStmts;
    for (const auto& s : ifStmt->elseBody) elseStmts.push_back(s.get());
    if (!lowerStmtList(elseStmts, ilFn)) return false;
    exitScope();
    auto stateElseEnd = snapshot();
    bool elseReaches = !currentBlockIsTerminated(ilFn);
    size_t elseEndBlockIdx = currentBlockIdx_;

    // Emit branch instruction in entry block
    il::Instruction brInst;
    brInst.op = il::Op::Branch;
    brInst.type = il::Type::Void;
    brInst.result = il::kNoValue;
    brInst.operands = {condVal.id};
    brInst.target = il::BlockTarget{.block = bThen, .args = {}};
    brInst.elseTarget = il::BlockTarget{.block = bElse, .args = {}};
    ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

    activeVarMap_ = envBefore;

    if (!thenReaches && !elseReaches) {
        // Both arms terminated; bJoin stays as the (unreachable)
        // continuation so later statements have a home.
        restore(stateBefore);
        setCurrentBlock(bJoin);
        return true;
    }

    if (thenReaches != elseReaches) {
        // Single live edge: no join parameters, the reachable arm's
        // values flow through directly.
        const auto& liveState = thenReaches ? stateThenEnd : stateElseEnd;
        setCurrentBlock(thenReaches ? thenEndBlockIdx : elseEndBlockIdx);
        il::Instruction jmpInst;
        jmpInst.op = il::Op::Jump;
        jmpInst.type = il::Type::Void;
        jmpInst.result = il::kNoValue;
        jmpInst.target = il::BlockTarget{.block = bJoin, .args = {}};
        emitInst(ilFn, jmpInst);
        restore(liveState);
        setCurrentBlock(bJoin);
        return true;
    }

    // Both arms reach: join parameters for every variable whose value
    // differs between the arms; param type unifies the arm types
    // (boxing to dynamic on disagreement).
    std::vector<std::string> activeNames = getActiveVarsInDeclOrder();
    std::vector<std::string> joinVars;
    for (const auto& name : activeNames) {
        if (stateThenEnd.at(name).valueId != stateElseEnd.at(name).valueId) {
            joinVars.push_back(name);
        }
    }

    std::unordered_map<std::string, il::ValueId> joinParamMap;
    std::unordered_map<std::string, il::Type> joinParamType;
    for (const auto& name : joinVars) {
        il::ValueId pId = ilFn.valueCount++;
        il::Type tThen = stateThenEnd.at(name).type;
        il::Type tElse = stateElseEnd.at(name).type;
        // Both edges into this join are already lowered, so when they agree
        // their common type is itself the proof that the parameter can have
        // it — no inference needed, which is what keeps `--no-infer` from
        // boxing an if/else that never changes a binding's type. When they
        // disagree only a proof can license anything but `Dynamic`, and the
        // proof is the analysis's type at this merge.
        il::Type pType = (tThen == tElse) ? tThen : mergeParamType(*ifStmt, name);
        ilFn.blocks[bJoin].params.push_back({pId, pType});
        joinParamMap[name] = pId;
        joinParamType[name] = pType;
    }

    auto emitJoinEdge = [&](size_t endBlockIdx,
                            const std::unordered_map<std::string, VarState>& state) {
        setCurrentBlock(endBlockIdx);
        std::vector<il::ValueId> args;
        for (const auto& name : joinVars) {
            Value v{state.at(name).valueId, state.at(name).type};
            args.push_back(coerceToType(v, joinParamType[name], ilFn).id);
        }
        il::Instruction jmpInst;
        jmpInst.op = il::Op::Jump;
        jmpInst.type = il::Type::Void;
        jmpInst.result = il::kNoValue;
        jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
        emitInst(ilFn, jmpInst);
    };
    emitJoinEdge(thenEndBlockIdx, stateThenEnd);
    emitJoinEdge(elseEndBlockIdx, stateElseEnd);

    setCurrentBlock(bJoin);
    restore(stateThenEnd);
    for (const auto& name : joinVars) {
        varBindings_[activeVarMap_[name]].valueId = joinParamMap[name];
        varBindings_[activeVarMap_[name]].type = joinParamType[name];
    }
    return true;
}

bool Lowerer::lowerWhileStmt(const ast::WhileStmt* whileStmt, il::Function& ilFn) {
    const std::string label = takePendingLabel();
    const auto loopParams = collectLoopParams(*whileStmt, ast::getAssignedNames(*whileStmt));
    std::vector<std::string> loopVars;
    for (const auto& param : loopParams) loopVars.push_back(param.name);

    il::BlockId bHeader = createBlock(ilFn);
    il::BlockId bBody = createBlock(ilFn);
    il::BlockId bExit = createBlock(ilFn);

    // Header params, then the entry edge into them: the entry values are
    // coerced to the parameter types like every other edge's.
    auto headerParamMap = addLoopBlockParams(loopParams, bHeader, ilFn);

    il::Instruction jmpEntry;
    jmpEntry.op = il::Op::Jump;
    jmpEntry.type = il::Type::Void;
    jmpEntry.result = il::kNoValue;
    jmpEntry.target =
        il::BlockTarget{.block = bHeader, .args = collectEdgeArgs(loopVars, bHeader, ilFn)};
    emitInst(ilFn, jmpEntry);

    setCurrentBlock(bHeader);
    bindLoopBlockParams(loopParams, headerParamMap);

    Value condVal = lowerCondition(*whileStmt->condition, ilFn);

    // Exit params, and the header's own exit edge into them.
    auto exitParamMap = addLoopBlockParams(loopParams, bExit, ilFn);
    std::vector<il::ValueId> headerExitArgs = collectEdgeArgs(loopVars, bExit, ilFn);

    il::Instruction brInst;
    brInst.op = il::Op::Branch;
    brInst.type = il::Type::Void;
    brInst.result = il::kNoValue;
    brInst.operands = {condVal.id};
    brInst.target = il::BlockTarget{.block = bBody, .args = {}};
    brInst.elseTarget = il::BlockTarget{.block = bExit, .args = std::move(headerExitArgs)};
    emitInst(ilFn, brInst);

    // Body
    setCurrentBlock(bBody);
    jumpStack_.push_back(JumpTarget{JumpKind::Loop, label, bHeader, bHeader, bExit, loopVars,
                                    cleanupStack_.size(), cleanupStack_.size()});
    enterScope(whileStmt->body, ilFn);
    std::vector<const ast::Stmt*> bodyStmts;
    for (const auto& s : whileStmt->body) bodyStmts.push_back(s.get());
    // The scope and the jump target are unwound on BOTH paths: leaving an
    // entry on the jump stack after a failed body would let a later `break`
    // resolve to a loop that is no longer being lowered (see the note in
    // lowerClosure).
    const bool bodyOk = lowerStmtList(bodyStmts, ilFn);
    exitScope();
    jumpStack_.pop_back();
    if (!bodyOk) return false;

    if (!currentBlockIsTerminated(ilFn)) {
        il::Instruction backJmp;
        backJmp.op = il::Op::Jump;
        backJmp.type = il::Type::Void;
        backJmp.result = il::kNoValue;
        backJmp.target = il::BlockTarget{
            .block = bHeader,
            .args = collectEdgeArgs(loopVars, bHeader, ilFn)};
        emitInst(ilFn, backJmp);
    }

    setCurrentBlock(bExit);
    bindLoopBlockParams(loopParams, exitParamMap);
    return true;
}

bool Lowerer::lowerDoWhileStmt(const ast::DoWhileStmt* doWhileStmt, il::Function& ilFn) {
    const std::string label = takePendingLabel();
    const auto loopParams = collectLoopParams(*doWhileStmt, ast::getAssignedNames(*doWhileStmt));
    std::vector<std::string> loopVars;
    for (const auto& param : loopParams) loopVars.push_back(param.name);

    il::BlockId bHeader = createBlock(ilFn);
    il::BlockId bCond = createBlock(ilFn);
    il::BlockId bExit = createBlock(ilFn);

    // Header params, then the entry edge into them.
    auto headerParamMap = addLoopBlockParams(loopParams, bHeader, ilFn);

    il::Instruction jmpEntry;
    jmpEntry.op = il::Op::Jump;
    jmpEntry.type = il::Type::Void;
    jmpEntry.result = il::kNoValue;
    jmpEntry.target =
        il::BlockTarget{.block = bHeader, .args = collectEdgeArgs(loopVars, bHeader, ilFn)};
    emitInst(ilFn, jmpEntry);

    setCurrentBlock(bHeader);
    bindLoopBlockParams(loopParams, headerParamMap);

    auto exitParamMap = addLoopBlockParams(loopParams, bExit, ilFn);
    // The condition block joins the body fall-through and continue
    // edges, so it takes the loop variables as parameters.
    auto condParamMap = addLoopBlockParams(loopParams, bCond, ilFn);

    jumpStack_.push_back(JumpTarget{JumpKind::Loop, label, bHeader, bCond, bExit, loopVars,
                                    cleanupStack_.size(), cleanupStack_.size()});
    enterScope(doWhileStmt->body, ilFn);
    std::vector<const ast::Stmt*> bodyStmts;
    for (const auto& s : doWhileStmt->body) bodyStmts.push_back(s.get());
    // The scope and the jump target are unwound on BOTH paths: leaving an
    // entry on the jump stack after a failed body would let a later `break`
    // resolve to a loop that is no longer being lowered (see the note in
    // lowerClosure).
    const bool bodyOk = lowerStmtList(bodyStmts, ilFn);
    exitScope();
    jumpStack_.pop_back();
    if (!bodyOk) return false;

    if (!currentBlockIsTerminated(ilFn)) {
        il::Instruction toCond;
        toCond.op = il::Op::Jump;
        toCond.type = il::Type::Void;
        toCond.result = il::kNoValue;
        toCond.target = il::BlockTarget{
            .block = bCond, .args = collectEdgeArgs(loopVars, bCond, ilFn)};
        emitInst(ilFn, toCond);
    }

    setCurrentBlock(bCond);
    bindLoopBlockParams(loopParams, condParamMap);
    Value condVal = lowerCondition(*doWhileStmt->condition, ilFn);

    std::vector<il::ValueId> condBackArgs = collectEdgeArgs(loopVars, bHeader, ilFn);
    std::vector<il::ValueId> condExitArgs = collectEdgeArgs(loopVars, bExit, ilFn);

    il::Instruction brInst;
    brInst.op = il::Op::Branch;
    brInst.type = il::Type::Void;
    brInst.result = il::kNoValue;
    brInst.operands = {condVal.id};
    brInst.target = il::BlockTarget{.block = bHeader, .args = std::move(condBackArgs)};
    brInst.elseTarget = il::BlockTarget{.block = bExit, .args = std::move(condExitArgs)};
    emitInst(ilFn, brInst);

    setCurrentBlock(bExit);
    bindLoopBlockParams(loopParams, exitParamMap);
    return true;
}

bool Lowerer::lowerForStmt(const ast::ForStmt* forStmt, il::Function& ilFn) {
    const std::string label = takePendingLabel();
    // A `for (let i = ...)` header binding is copied per iteration
    // (14.7.4.9), so a closure over it must capture a fresh binding each time
    // round. That needs the environment threaded across the back edge, which
    // the block-scope rule does not give for free (docs/0007 decision 2) —
    // diagnose it rather than silently sharing one binding.
    //
    // The test is whether a closure UNDER THIS LOOP reaches this binding, not
    // whether the enclosing function captures the name anywhere: `for (let i…)`
    // beside an unrelated `arr.map((i) => …)` shares nothing but a spelling,
    // and rejecting it rejects a large fraction of ordinary JavaScript
    // (docs/0028 decision 3). `var` is exempt outright — it declares ONE
    // binding for the whole loop, so a closure over it is already correct.
    for (const auto& initStmt : forStmt->init) {
        const auto* initDecl = dynamic_cast<const ast::VarDecl*>(initStmt.get());
        if (initDecl && !initDecl->isVar &&
            ast::closureCapturesLoopBinding(*forStmt, initDecl->name)) {
            diags_.error(forStmt->span,
                         "unsupported construct: closure capturing the for-loop binding '" +
                             initDecl->name +
                             "' (per-iteration binding semantics); use a `let` declared "
                             "inside the loop body");
            return false;
        }
    }
    // The header's own scope, and it needs an environment record of its own
    // whenever one of its bindings is env-backed. Entered with `init` so that
    // `getScopeDeclarations` sees the header's declarations: without them the
    // innermost record at the declaration is the enclosing function's, and a
    // `let i` here would be given the slot that scope's OWN `i` already
    // occupies — a shadowing declaration aliasing what it shadows.
    enterScope(forStmt->init, ilFn);
    for (const auto& initStmt : forStmt->init) {
        if (!lowerStmt(*initStmt, ilFn)) return false;
    }

    const auto loopParams = collectLoopParams(*forStmt, ast::getAssignedNames(*forStmt));
    std::vector<std::string> loopVars;
    for (const auto& param : loopParams) loopVars.push_back(param.name);

    il::BlockId bHeader = createBlock(ilFn);
    il::BlockId bBody = createBlock(ilFn);
    il::BlockId bUpdate = createBlock(ilFn);
    il::BlockId bExit = createBlock(ilFn);

    // Header params, then the entry edge into them.
    auto headerParamMap = addLoopBlockParams(loopParams, bHeader, ilFn);

    il::Instruction jmpEntry;
    jmpEntry.op = il::Op::Jump;
    jmpEntry.type = il::Type::Void;
    jmpEntry.result = il::kNoValue;
    jmpEntry.target =
        il::BlockTarget{.block = bHeader, .args = collectEdgeArgs(loopVars, bHeader, ilFn)};
    emitInst(ilFn, jmpEntry);

    setCurrentBlock(bHeader);
    bindLoopBlockParams(loopParams, headerParamMap);

    auto exitParamMap = addLoopBlockParams(loopParams, bExit, ilFn);
    std::vector<il::ValueId> headerExitArgs = collectEdgeArgs(loopVars, bExit, ilFn);

    if (forStmt->condition) {
        Value condVal = lowerCondition(*forStmt->condition, ilFn);
        il::Instruction brInst;
        brInst.op = il::Op::Branch;
        brInst.type = il::Type::Void;
        brInst.result = il::kNoValue;
        brInst.operands = {condVal.id};
        brInst.target = il::BlockTarget{.block = bBody, .args = {}};
        brInst.elseTarget = il::BlockTarget{.block = bExit, .args = std::move(headerExitArgs)};
        emitInst(ilFn, brInst);
    } else {
        il::Instruction jmpBody;
        jmpBody.op = il::Op::Jump;
        jmpBody.type = il::Type::Void;
        jmpBody.result = il::kNoValue;
        jmpBody.target = il::BlockTarget{.block = bBody, .args = {}};
        emitInst(ilFn, jmpBody);
    }

    // The update block joins the body fall-through and continue
    // edges, so it takes the loop variables as parameters.
    auto updateParamMap = addLoopBlockParams(loopParams, bUpdate, ilFn);

    // Body
    setCurrentBlock(bBody);
    jumpStack_.push_back(JumpTarget{JumpKind::Loop, label, bHeader, bUpdate, bExit, loopVars,
                                    cleanupStack_.size(), cleanupStack_.size()});
    enterScope(forStmt->body, ilFn);
    std::vector<const ast::Stmt*> bodyStmts;
    for (const auto& s : forStmt->body) bodyStmts.push_back(s.get());
    // The scope and the jump target are unwound on BOTH paths: leaving an
    // entry on the jump stack after a failed body would let a later `break`
    // resolve to a loop that is no longer being lowered (see the note in
    // lowerClosure).
    const bool bodyOk = lowerStmtList(bodyStmts, ilFn);
    exitScope();
    jumpStack_.pop_back();
    if (!bodyOk) return false;

    if (!currentBlockIsTerminated(ilFn)) {
        il::Instruction toUpdate;
        toUpdate.op = il::Op::Jump;
        toUpdate.type = il::Type::Void;
        toUpdate.result = il::kNoValue;
        toUpdate.target = il::BlockTarget{
            .block = bUpdate, .args = collectEdgeArgs(loopVars, bUpdate, ilFn)};
        emitInst(ilFn, toUpdate);
    }

    // Update
    setCurrentBlock(bUpdate);
    bindLoopBlockParams(loopParams, updateParamMap);
    if (forStmt->update) {
        if (!lowerExpr(*forStmt->update, ilFn)) return false;
    }
    il::Instruction backJmp;
    backJmp.op = il::Op::Jump;
    backJmp.type = il::Type::Void;
    backJmp.result = il::kNoValue;
    backJmp.target = il::BlockTarget{
        .block = bHeader, .args = collectEdgeArgs(loopVars, bHeader, ilFn)};
    emitInst(ilFn, backJmp);

    setCurrentBlock(bExit);
    bindLoopBlockParams(loopParams, exitParamMap);
    exitScope();
    return true;
}

// A jump out of a `try` runs every `finally` it crosses, innermost first,
// before it lands (docs/0020 decision 5). The target is re-read from the
// index afterwards rather than held as a pointer: lowering a finally body
// pushes and pops jump targets of its own, and `jumpStack_` can reallocate.
//
// If one of those finallys completes abruptly — a `return` or a `throw` of
// its own — the current block is already terminated and the jump is simply
// not emitted, which is how the override of 14.15.3 happens without a rule.
bool Lowerer::emitJumpCrossingFinallys(size_t targetIndex, bool toExit,
                                       il::Function& ilFn) {
    const size_t downTo = toExit ? jumpStack_[targetIndex].cleanupDepthAtEntry
                                 : jumpStack_[targetIndex].cleanupDepthInBody;
    if (!runCleanups(downTo, ilFn)) return false;
    if (currentBlockIsTerminated(ilFn)) return true;
    const JumpTarget& target = jumpStack_[targetIndex];
    emitJumpToTarget(target, toExit ? target.exitBlock : target.updateBlock, {}, ilFn);
    return true;
}

bool Lowerer::lowerBreakStmt(const ast::BreakStmt* breakStmt, il::Function& ilFn) {
    const JumpTarget* target =
        findJumpTarget(breakStmt->label, /*forContinue=*/false, breakStmt->span);
    if (!target) return false;
    return emitJumpCrossingFinallys(static_cast<size_t>(target - jumpStack_.data()),
                                    /*toExit=*/true, ilFn);
}

bool Lowerer::lowerContinueStmt(const ast::ContinueStmt* continueStmt, il::Function& ilFn) {
    const JumpTarget* target =
        findJumpTarget(continueStmt->label, /*forContinue=*/true, continueStmt->span);
    if (!target) return false;
    return emitJumpCrossingFinallys(static_cast<size_t>(target - jumpStack_.data()),
                                    /*toExit=*/false, ilFn);
}

}  // namespace bronze::lower
