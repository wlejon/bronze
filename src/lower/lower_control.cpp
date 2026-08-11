// Control-flow statements and the block-argument SSA joins they build:
// if/else, the three loop forms, break and continue (docs/0005).

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "lower/assigned_set.h"
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
    const auto loopParams = collectLoopParams(*whileStmt, getAssignedVariables(*whileStmt));
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
    loopStack_.push_back(LoopContext{bHeader, bHeader, bExit, loopVars});
    enterScope(whileStmt->body, ilFn);
    std::vector<const ast::Stmt*> bodyStmts;
    for (const auto& s : whileStmt->body) bodyStmts.push_back(s.get());
    if (!lowerStmtList(bodyStmts, ilFn)) return false;
    exitScope();
    loopStack_.pop_back();

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
    const auto loopParams = collectLoopParams(*doWhileStmt, getAssignedVariables(*doWhileStmt));
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

    loopStack_.push_back(LoopContext{bHeader, bCond, bExit, loopVars});
    enterScope(doWhileStmt->body, ilFn);
    std::vector<const ast::Stmt*> bodyStmts;
    for (const auto& s : doWhileStmt->body) bodyStmts.push_back(s.get());
    if (!lowerStmtList(bodyStmts, ilFn)) return false;
    exitScope();
    loopStack_.pop_back();

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
    // A `for (let i = ...)` header binding is copied per iteration in
    // JS, so a closure over it must capture a fresh binding each time
    // round. That needs the environment threaded across the back
    // edge, which the block-scope rule does not give for free
    // (docs/0007 decision 2) — diagnose it rather than silently
    // sharing one binding.
    if (const auto* initDecl = dynamic_cast<const ast::VarDecl*>(forStmt->init.get())) {
        if (capturedNames_.contains(initDecl->name)) {
            diags_.error(forStmt->span,
                         "unsupported construct: closure capturing the for-loop binding '" +
                             initDecl->name +
                             "' (per-iteration binding semantics); use a `let` declared "
                             "inside the loop body");
            return false;
        }
    }
    enterScope();
    if (forStmt->init) {
        if (!lowerStmt(*forStmt->init, ilFn)) return false;
    }

    const auto loopParams = collectLoopParams(*forStmt, getAssignedVariables(*forStmt));
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
    loopStack_.push_back(LoopContext{bHeader, bUpdate, bExit, loopVars});
    enterScope(forStmt->body, ilFn);
    std::vector<const ast::Stmt*> bodyStmts;
    for (const auto& s : forStmt->body) bodyStmts.push_back(s.get());
    if (!lowerStmtList(bodyStmts, ilFn)) return false;
    exitScope();
    loopStack_.pop_back();

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

bool Lowerer::lowerBreakStmt(const ast::BreakStmt* breakStmt, il::Function& ilFn) {
    if (!breakStmt->label.empty()) {
        diags_.error(breakStmt->span, "unsupported construct: labeled break/continue");
        return false;
    }
    if (loopStack_.empty()) {
        diags_.error(breakStmt->span, "break statement outside of loop");
        return false;
    }
    const auto& loopCtx = loopStack_.back();
    il::Instruction jmpInst;
    jmpInst.op = il::Op::Jump;
    jmpInst.type = il::Type::Void;
    jmpInst.result = il::kNoValue;
    jmpInst.target = il::BlockTarget{
        .block = loopCtx.exitBlock,
        .args = collectEdgeArgs(loopCtx.loopVars, loopCtx.exitBlock, ilFn)};
    emitInst(ilFn, jmpInst);

    // Create dead block for any unreachable trailing instructions in the same scope
    il::BlockId deadBlock = createBlock(ilFn);
    setCurrentBlock(deadBlock);
    return true;
}

bool Lowerer::lowerContinueStmt(const ast::ContinueStmt* continueStmt, il::Function& ilFn) {
    if (!continueStmt->label.empty()) {
        diags_.error(continueStmt->span, "unsupported construct: labeled break/continue");
        return false;
    }
    if (loopStack_.empty()) {
        diags_.error(continueStmt->span, "continue statement outside of loop");
        return false;
    }
    const auto& loopCtx = loopStack_.back();
    il::Instruction jmpInst;
    jmpInst.op = il::Op::Jump;
    jmpInst.type = il::Type::Void;
    jmpInst.result = il::kNoValue;
    jmpInst.target = il::BlockTarget{
        .block = loopCtx.updateBlock,
        .args = collectEdgeArgs(loopCtx.loopVars, loopCtx.updateBlock, ilFn)};
    emitInst(ilFn, jmpInst);

    il::BlockId deadBlock = createBlock(ilFn);
    setCurrentBlock(deadBlock);
    return true;
}

}  // namespace bronze::lower
