// `switch`, as selection followed by fallthrough.
//
// The shape is two chains, not a tree. One chain of TEST blocks runs the case
// expressions in source order, each branching to its own body block or on to
// the next test; one chain of BODY blocks sits in source order with a
// fall-through edge from each to the next. A `break` jumps out of the second
// chain to the exit block.
//
// Everything the statement is comes out of that arrangement rather than out of
// special cases:
//
//   - the discriminant is evaluated once, before the first test, because it is
//     lowered before the chain;
//   - a case expression below the selected one never runs, because the test
//     chain's true edge leaves it;
//   - `default` is reached from the END of the test chain wherever it is
//     written, and falls through to whatever physically follows it, because it
//     is an ordinary link in the body chain;
//   - fallthrough is the default because the body chain's edges are there
//     unless a `break` replaces them.

#include <string>
#include <vector>

#include "ast/assigned.h"
#include "ast/queries.h"
#include "lower/lowerer.h"

namespace bronze::lower {



bool Lowerer::lowerSwitchStmt(const ast::SwitchStmt* sw, il::Function& ilFn) {
    const std::string label = takePendingLabel();

    // The CaseBlock's own lexical bindings, in source order across every
    // clause: 14.12.2 makes the whole body ONE declarative environment, so
    // these belong to the switch and not to the clause that spells them.
    std::vector<std::string> caseLexicals;
    for (const auto& clause : sw->cases) {
        for (auto& name : ast::getLexicalDeclarations(clause.body)) {
            caseLexicals.push_back(std::move(name));
        }
    }

    // ECMA-262 14.12.4 evaluates the discriminant ONCE, before any case.
    auto discOpt = lowerExpr(*sw->discriminant, ilFn);
    if (!discOpt) return false;
    const Value disc = boxValueIfNeeded(*discOpt, ilFn);

    // Every block this statement builds takes the same parameter list: one
    // per variable the switch assigns anywhere. The body blocks need it
    // because they are reached both from a test and from the clause above,
    // and the exit block because a `break` can leave from any of them.
    const auto params = collectLoopParams(*sw, ast::getAssignedNames(*sw));
    std::vector<std::string> vars;
    for (const auto& p : params) vars.push_back(p.name);

    const size_t clauseCount = sw->cases.size();
    std::vector<il::BlockId> bodyBlocks(clauseCount, il::kNoBlock);
    std::vector<std::unordered_map<std::string, il::ValueId>> bodyParamMaps(clauseCount);
    for (size_t i = 0; i < clauseCount; ++i) {
        bodyBlocks[i] = createBlock(ilFn);
        bodyParamMaps[i] = addLoopBlockParams(params, bodyBlocks[i], ilFn);
    }
    il::BlockId bExit = createBlock(ilFn);
    auto exitParamMap = addLoopBlockParams(params, bExit, ilFn);

    size_t defaultClause = clauseCount;
    for (size_t i = 0; i < clauseCount; ++i) {
        if (!sw->cases[i].test) defaultClause = i;
    }

    // One scope for the whole body, opened before the tests so that its record
    // — and the uninitialized marker in every lexical slot of it — exists
    // however the chain of tests ends up entering it.
    enterScope(std::vector<ast::StmtPtr>{}, ilFn, {}, caseLexicals);

    // Function declarations in any case clause hoist to the CaseBlock scope
    // (ECMA-262 14.12.4 / Annex B.3.2 BlockDeclarationInstantiation).
    for (const auto& clause : sw->cases) {
        for (const auto& s : clause.body) {
            const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(s.get());
            if (!fnDecl) continue;
            auto closure = lowerClosure(*fnDecl, fnDecl->name, fnDecl->name, fnDecl->params,
                                        fnDecl->returnType, fnDecl->body, fnDecl->span, ilFn);
            if (!closure) return false;
            if (!declareVariable(fnDecl->name, il::Type::Dynamic, /*isConst=*/false,
                                 /*isLet=*/true, /*isVar=*/false, /*isInitialized=*/true,
                                 closure->id, fnDecl->span)) {
                return false;
            }
            VarBinding& b = varBindings_[activeVarMap_[fnDecl->name]];
            if (b.inEnv) {
                emitEnvSet(envDepthOf(b.envScopeIndex), b.envSlot, *closure, ilFn);
            }
        }
    }

    auto snapshotState = [&]() { return snapshotVarStates(); };
    const VarStateMap stateBeforeTests = snapshotState();

    // --- the test chain -----------------------------------------------------
    // Each test block has exactly ONE predecessor (the previous test's false
    // edge, or the statement before the switch), so no test block takes
    // parameters: there is nothing to join.
    for (size_t i = 0; i < clauseCount; ++i) {
        if (!sw->cases[i].test) continue;  // the default is not a test
        auto testOpt = lowerExpr(*sw->cases[i].test, ilFn);
        if (!testOpt) return false;
        const Value testVal = boxValueIfNeeded(*testOpt, ilFn);

        // IsStrictlyEqual, so `switch (1)` does not select `case "1"` and no
        // case selects NaN. The same op `===` lowers to, because it is the
        // same question.
        il::ValueId eqVal = ilFn.valueCount++;
        il::Instruction eqInst;
        eqInst.op = il::Op::StrictEq;
        eqInst.type = il::Type::Bool;
        eqInst.result = eqVal;
        eqInst.operands = {disc.id, testVal.id};
        emitInst(ilFn, eqInst);

        il::BlockId bNextTest = createBlock(ilFn);
        il::Instruction brInst;
        brInst.op = il::Op::Branch;
        brInst.type = il::Type::Void;
        brInst.result = il::kNoValue;
        brInst.operands = {eqVal};
        brInst.target = il::BlockTarget{.block = bodyBlocks[i],
                                        .args = collectEdgeArgs(vars, bodyBlocks[i], ilFn)};
        brInst.elseTarget = il::BlockTarget{.block = bNextTest, .args = {}};
        emitInst(ilFn, brInst);
        setCurrentBlock(bNextTest);
    }

    // No case matched. The default is selected here however early it was
    // written, which is what makes "default in the middle" a real shape and
    // not a curiosity.
    const il::BlockId afterTests =
        defaultClause < clauseCount ? bodyBlocks[defaultClause] : bExit;
    il::Instruction toDefault;
    toDefault.op = il::Op::Jump;
    toDefault.type = il::Type::Void;
    toDefault.result = il::kNoValue;
    toDefault.target =
        il::BlockTarget{.block = afterTests, .args = collectEdgeArgs(vars, afterTests, ilFn)};
    emitInst(ilFn, toDefault);

    // --- the body chain -----------------------------------------------------
    jumpStack_.push_back(
        JumpTarget{JumpKind::Switch, label, il::kNoBlock, il::kNoBlock, bExit, vars,
                   cleanupStack_.size(), cleanupStack_.size()});
    for (size_t i = 0; i < clauseCount; ++i) {
        setCurrentBlock(bodyBlocks[i]);
        // Every binding this block can see arrives as a parameter or was
        // already live before the switch; restoring the pre-test state and
        // then binding the parameters is what makes the two edges into this
        // block agree about every name.
        restoreVarStates(stateBeforeTests);
        bindLoopBlockParams(params, bodyParamMaps[i]);

        std::vector<const ast::Stmt*> stmts;
        for (const auto& s : sw->cases[i].body) {
            if (!dynamic_cast<const ast::FunctionDecl*>(s.get())) {
                stmts.push_back(s.get());
            }
        }
        if (!lowerStmtList(stmts, ilFn)) {
            jumpStack_.pop_back();
            return false;
        }
        if (currentBlockIsTerminated(ilFn)) continue;
        // Fallthrough: no `break`, so control runs into whatever clause is
        // written next, or leaves the switch if this was the last one.
        const il::BlockId next = (i + 1 < clauseCount) ? bodyBlocks[i + 1] : bExit;
        il::Instruction fall;
        fall.op = il::Op::Jump;
        fall.type = il::Type::Void;
        fall.result = il::kNoValue;
        fall.target = il::BlockTarget{.block = next, .args = collectEdgeArgs(vars, next, ilFn)};
        emitInst(ilFn, fall);
    }
    jumpStack_.pop_back();
    exitScope();

    setCurrentBlock(bExit);
    restoreVarStates(stateBeforeTests);
    bindLoopBlockParams(params, exitParamMap);
    return true;
}

}  // namespace bronze::lower
