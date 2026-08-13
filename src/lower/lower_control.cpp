// Control-flow statements and the block-argument SSA joins they build: if/else,
// the three loop forms, break and continue.

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

// The parameters a loop's header, exit, and update/condition blocks all take:
// one per variable assigned anywhere in the loop, in declaration order, typed
// by what inference proved holds at the loop's merges.
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
        // Becoming a loop parameter means being read on the entry edge, and an
        // annotated `let x: number;` has no value to read there — undefined has
        // no typed form (see lowerVarDecl). Diagnosed here by name, like every
        // other read before initialization, rather than left to emit an edge
        // argument that does not exist.
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

il::ValueId Lowerer::addEnvBlockParam(il::BlockId block, il::Function& ilFn) {
    il::ValueId pId = ilFn.valueCount++;
    ilFn.blocks[block].params.push_back({pId, il::Type::Dynamic});
    return pId;
}

bool Lowerer::forNeedsPerIterationEnv(const ast::ForStmt& forStmt) const {
    // No record for the head scope means no head binding is env-backed, and a
    // binding that lives in SSA is already one value per iteration — the back
    // edge's block argument IS the copy.
    if (scopeHasEnv_.empty() || !scopeHasEnv_.back()) return false;
    // The record's slots are exactly `getScopeDeclarations(init)` filtered by
    // what needs memory, and that query drops `var` — so every slot here is a
    // LexicalDeclaration's, which is 14.7.4.9's `perIterationBindings` and the
    // reason the whole record can be copied rather than a subset of it.
    for (const auto& name : envScopes_.back().slotNames) {
        if (ast::closureCapturesLoopBinding(forStmt, name)) return true;
    }
    return false;
}

il::ValueId Lowerer::emitPerIterationEnv(il::ValueId source, uint32_t slotCount,
                                         il::ValueId parent, il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction create;
    create.op = il::Op::EnvCreate;
    create.type = il::Type::Dynamic;
    create.result = res;
    create.operands = {parent};
    create.immI32 = static_cast<int32_t>(slotCount);
    emitInst(ilFn, create);

    for (uint32_t slot = 0; slot < slotCount; ++slot) {
        // The UNCHECKED read, though the slot holds a lexical binding and
        // every read written in the source gets 9.1.1.1.6's check. Step 2.d.ii
        // reads with a throw on uninitialized, and it cannot fire here: the
        // head is one LexicalDeclaration evaluated to completion above the
        // first copy, so every binding it declares is initialized — `let i;`
        // to undefined — before any copy is made, and each later copy reads
        // the copy before it.
        il::ValueId cur = ilFn.valueCount++;
        il::Instruction get;
        get.op = il::Op::EnvGet;
        get.type = il::Type::Dynamic;
        get.result = cur;
        get.operands = {source};
        get.envDepth = 0;
        get.envIndex = slot;
        emitInst(ilFn, get);

        il::Instruction set;
        set.op = il::Op::EnvSet;
        set.type = il::Type::Void;
        set.result = il::kNoValue;
        set.operands = {res, cur};
        set.envDepth = 0;
        set.envIndex = slot;
        emitInst(ilFn, set);
    }
    return res;
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

    // ECMA-262 14.7.4.9: a `for` whose head is a LexicalDeclaration gets a
    // FRESH copy of the record that head ran in before each iteration, with
    // the previous copy's values carried in. So the loop's environment is not
    // one record threaded round the back edge but a chain of siblings, and the
    // record the head itself ran in is never any iteration's — which is the
    // whole of why `for (let i = 0, f = () => i; …)` has `f` see 0 forever
    // while the body's closures see 0, 1, 2.
    //
    // That makes the environment a loop-carried value like any other, so it
    // becomes a block parameter of the header and of the update block, and
    // `collectEdgeArgs`'s vector grows by one on every edge into them. The
    // body needs no parameter: it is dominated by the header, so the header's
    // is the one it reads and the one every closure in it captures.
    const bool perIteration = forNeedsPerIterationEnv(*forStmt);
    if (perIteration && generator_ && ast::containsYield(*forStmt)) {
        // 14.7.4.9's copy is threaded through the loop's blocks as one more
        // block argument, and a resume edge into the body hands over no
        // arguments at all — so the copy a resumed iteration would read is a
        // value nothing defined on the path it arrived by. Refused by name
        // rather than approximated, because the approximation is one record
        // shared by every iteration and a closure reading the wrong one.
        diags_.error(forStmt->span,
                     "unsupported construct: a `yield` in a `for` loop whose head binding a "
                     "closure written inside the loop captures (ECMA-262 14.7.4.9 copies that "
                     "binding once per iteration, and bronze carries the copy in a block "
                     "argument that a resumption cannot supply)");
        return false;
    }
    const uint32_t headSlots =
        perIteration ? static_cast<uint32_t>(envScopes_.back().slotNames.size()) : 0;
    il::ValueId headEnvParent = il::kNoValue;
    il::ValueId entryEnv = il::kNoValue;
    if (perIteration) {
        // Hoisted above the loop so that both copy sites — the entry one here
        // and the one in the update block — name a value that dominates them.
        headEnvParent = savedEnvValues_.back() == il::kNoValue ? emitConstUndefined(ilFn)
                                                               : savedEnvValues_.back();
        // ForBodyEvaluation step 2: the first copy is made before the first
        // test, so no iteration ever reads or writes the head's own record.
        entryEnv = emitPerIterationEnv(currentEnvValue_, headSlots, headEnvParent, ilFn);
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
    const il::ValueId headerEnvParam =
        perIteration ? addEnvBlockParam(bHeader, ilFn) : il::kNoValue;

    std::vector<il::ValueId> entryArgs = collectEdgeArgs(loopVars, bHeader, ilFn);
    if (perIteration) entryArgs.push_back(entryEnv);
    il::Instruction jmpEntry;
    jmpEntry.op = il::Op::Jump;
    jmpEntry.type = il::Type::Void;
    jmpEntry.result = il::kNoValue;
    jmpEntry.target = il::BlockTarget{.block = bHeader, .args = std::move(entryArgs)};
    emitInst(ilFn, jmpEntry);

    setCurrentBlock(bHeader);
    bindLoopBlockParams(loopParams, headerParamMap);
    // The condition is 14.7.4.8 step 3.a, which runs in THIS iteration's
    // environment — so a closure created in it captures the same record the
    // body's do.
    if (perIteration) currentEnvValue_ = headerEnvParam;

    auto exitParamMap = addLoopBlockParams(loopParams, bExit, ilFn);

    if (forStmt->condition) {
        Value condVal = lowerCondition(*forStmt->condition, ilFn);
        // AFTER the condition, never before it. 14.7.4.8 step 3.a evaluates the
        // test expression, and that expression may assign — `for (;(seen = i),
        // i < 2;)` writes `seen` on the false evaluation too. The exit edge
        // owes the exit block the values as of the BRANCH, so the args are
        // collected where the branch is emitted; snapshotting them at the top
        // of the header is how the final, loop-ending write got dropped.
        std::vector<il::ValueId> headerExitArgs = collectEdgeArgs(loopVars, bExit, ilFn);
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
    const il::ValueId updateEnvParam =
        perIteration ? addEnvBlockParam(bUpdate, ilFn) : il::kNoValue;

    // Body
    setCurrentBlock(bBody);
    JumpTarget forTarget{JumpKind::Loop, label, bHeader, bUpdate, bExit, loopVars,
                         cleanupStack_.size(), cleanupStack_.size()};
    forTarget.perIterationEnv = headerEnvParam;
    jumpStack_.push_back(forTarget);
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
        std::vector<il::ValueId> updateArgs = collectEdgeArgs(loopVars, bUpdate, ilFn);
        if (perIteration) updateArgs.push_back(headerEnvParam);
        il::Instruction toUpdate;
        toUpdate.op = il::Op::Jump;
        toUpdate.type = il::Type::Void;
        toUpdate.result = il::kNoValue;
        toUpdate.target = il::BlockTarget{.block = bUpdate, .args = std::move(updateArgs)};
        emitInst(ilFn, toUpdate);
    }

    // Update
    setCurrentBlock(bUpdate);
    bindLoopBlockParams(loopParams, updateParamMap);
    il::ValueId nextEnv = il::kNoValue;
    if (perIteration) {
        // Step 3.e before step 3.f: the copy is made and only THEN does the
        // increment run, so `i++` writes the next iteration's binding rather
        // than the one the body just closed over. Getting these two the other
        // way round is not a reordering — it is the difference between an
        // arrow seeing 0 and seeing 1.
        nextEnv = emitPerIterationEnv(updateEnvParam, headSlots, headEnvParent, ilFn);
        currentEnvValue_ = nextEnv;
    }
    if (forStmt->update) {
        if (!lowerExpr(*forStmt->update, ilFn)) return false;
    }
    // The copy by name rather than `currentEnvValue_`: what the back edge owes
    // the header is the record this iteration made, whatever the update
    // expression did to the innermost one on its way past.
    std::vector<il::ValueId> backArgs = collectEdgeArgs(loopVars, bHeader, ilFn);
    if (perIteration) backArgs.push_back(nextEnv);
    il::Instruction backJmp;
    backJmp.op = il::Op::Jump;
    backJmp.type = il::Type::Void;
    backJmp.result = il::kNoValue;
    backJmp.target = il::BlockTarget{.block = bHeader, .args = std::move(backArgs)};
    emitInst(ilFn, backJmp);

    setCurrentBlock(bExit);
    bindLoopBlockParams(loopParams, exitParamMap);
    exitScope();
    return true;
}

// A jump out of a `try` runs every `finally` it crosses, innermost first,
// before it lands. The target is re-read from the index afterwards rather than
// held as a pointer: lowering a finally body pushes and pops jump targets of
// its own, and `jumpStack_` can reallocate.
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
