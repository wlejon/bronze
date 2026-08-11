// The two loops that walk a container: `for-of` over the iterable itself
// (docs/0012 decision 2), and `for-in` over the key snapshot the runtime
// builds for it (docs/0018 decision 1).
//
// They share one lowering, which is the point of snapshotting the keys: once
// `for-in`'s subject is an array of strings, the two statements differ in
// nothing but the value handed to the walk. Everything below — the four-block
// shape, the threaded index, the per-iteration binding — is written once.

#include <string>
#include <vector>

#include "ast/assigned.h"
#include "lower/lowerer.h"

namespace bronze::lower {

// The same four-block shape as `for`, with two differences:
//
//   - the index is not a source binding, so it is threaded as an EXTRA block
//     parameter appended after the loop variables — appended in every block
//     and on every edge, so the positional match holds. `continue` jumps to
//     the update block too, which is why the jump target carries the value to
//     pass;
//   - the head binding is per-iteration BY DEFINITION. It is declared inside
//     the body's scope, which is entered once per iteration, so a closure over
//     it captures that iteration's value. `for (let i = ...)` needs the same
//     thing and does not get it (docs/0007 decision 2 diagnoses it); here it
//     is free, because there is no binding outside the body to share.
bool Lowerer::lowerIndexWalkLoop(const ast::Stmt& loopStmt, Value iterVal,
                                 const std::string& headName,
                                 const ast::BindingPattern* headPattern, bool isConst, bool isLet,
                                 bool isVar, const std::vector<ast::StmtPtr>& body,
                                 il::Function& ilFn) {
    const std::string label = takePendingLabel();

    il::ValueId zero = ilFn.valueCount++;
    il::Instruction zeroInst;
    zeroInst.op = il::Op::ConstF64;
    zeroInst.type = il::Type::F64;
    zeroInst.result = zero;
    zeroInst.immF64 = 0.0;
    emitInst(ilFn, zeroInst);

    const auto loopParams = collectLoopParams(loopStmt, ast::getAssignedNames(loopStmt));
    std::vector<std::string> loopVars;
    for (const auto& param : loopParams) loopVars.push_back(param.name);

    il::BlockId bHeader = createBlock(ilFn);
    il::BlockId bBody = createBlock(ilFn);
    il::BlockId bUpdate = createBlock(ilFn);
    il::BlockId bExit = createBlock(ilFn);

    // The index parameter is added to each block right after that block's
    // loop-variable parameters, and the matching argument is appended to
    // every edge into it.
    auto addIndexParam = [&](il::BlockId block) {
        il::ValueId id = ilFn.valueCount++;
        ilFn.blocks[block].params.push_back({id, il::Type::F64});
        return id;
    };
    auto edgeArgsWithIndex = [&](il::BlockId target, il::ValueId index) {
        auto args = collectEdgeArgs(loopVars, target, ilFn);
        args.push_back(index);
        return args;
    };

    auto headerParamMap = addLoopBlockParams(loopParams, bHeader, ilFn);
    il::ValueId headerIndex = addIndexParam(bHeader);

    il::Instruction jmpEntry;
    jmpEntry.op = il::Op::Jump;
    jmpEntry.type = il::Type::Void;
    jmpEntry.result = il::kNoValue;
    jmpEntry.target = il::BlockTarget{.block = bHeader, .args = edgeArgsWithIndex(bHeader, zero)};
    emitInst(ilFn, jmpEntry);

    setCurrentBlock(bHeader);
    bindLoopBlockParams(loopParams, headerParamMap);

    // The length is read every iteration rather than once: an array iterator
    // compares against the CURRENT length, so a for-of body that pushes sees
    // what it pushed and one that pops stops early. A for-in walks a snapshot
    // array nothing can reach, so for it this is the same number each time.
    il::ValueId lenVal = ilFn.valueCount++;
    il::Instruction lenInst;
    lenInst.op = il::Op::IterLength;
    lenInst.type = il::Type::F64;
    lenInst.result = lenVal;
    lenInst.operands = {iterVal.id};
    emitInst(ilFn, lenInst);

    il::ValueId condVal = ilFn.valueCount++;
    il::Instruction cmpInst;
    cmpInst.op = il::Op::CmpLt;
    cmpInst.type = il::Type::Bool;
    cmpInst.result = condVal;
    cmpInst.operands = {headerIndex, lenVal};
    emitInst(ilFn, cmpInst);

    // The exit block carries the loop variables only: `break` reaches it from
    // the body and has no index to hand over.
    auto exitParamMap = addLoopBlockParams(loopParams, bExit, ilFn);
    std::vector<il::ValueId> headerExitArgs = collectEdgeArgs(loopVars, bExit, ilFn);

    il::Instruction brInst;
    brInst.op = il::Op::Branch;
    brInst.type = il::Type::Void;
    brInst.result = il::kNoValue;
    brInst.operands = {condVal};
    brInst.target = il::BlockTarget{.block = bBody, .args = {}};
    brInst.elseTarget = il::BlockTarget{.block = bExit, .args = std::move(headerExitArgs)};
    emitInst(ilFn, brInst);

    auto updateParamMap = addLoopBlockParams(loopParams, bUpdate, ilFn);
    il::ValueId updateIndex = addIndexParam(bUpdate);

    setCurrentBlock(bBody);
    il::ValueId elemVal = ilFn.valueCount++;
    il::Instruction atInst;
    atInst.op = il::Op::IterAt;
    atInst.type = il::Type::Dynamic;
    atInst.result = elemVal;
    atInst.operands = {iterVal.id, headerIndex};
    emitInst(ilFn, atInst);

    JumpTarget ctx{JumpKind::Loop, label, bHeader, bUpdate, bExit, loopVars};
    ctx.updateExtraArg = headerIndex;
    jumpStack_.push_back(ctx);
    // The head binding belongs to the body's scope, so it gets an environment
    // slot there when a closure captures it — one per iteration, which is the
    // whole of the language's rule for it. A destructuring head binds every
    // name its pattern spells, and all of them belong to this scope for the
    // same per-iteration reason (docs/0017 decision 6).
    const std::vector<std::string> headNames =
        headPattern ? ast::patternBoundNames(*headPattern) : std::vector<std::string>{headName};
    enterScope(body, ilFn, headNames);
    bool bodyOk = true;
    if (headPattern) {
        PatternTarget target{
            .declare = true, .isConst = isConst, .isLet = isLet, .isVar = isVar};
        bodyOk = lowerPattern(*headPattern, Value{elemVal, il::Type::Dynamic}, target, ilFn);
    } else {
        bodyOk = declareVariable(headName, il::Type::Dynamic, isConst, isLet, isVar,
                                 /*isInitialized=*/true, elemVal, loopStmt.span);
        if (bodyOk) {
            writeBinding(varBindings_[activeVarMap_[headName]],
                         Value{elemVal, il::Type::Dynamic}, ilFn);
        }
    }

    std::vector<const ast::Stmt*> bodyStmts;
    for (const auto& s : body) bodyStmts.push_back(s.get());
    // Unwound on both paths, so that a failure inside the body cannot leave
    // this loop on the jump stack for a later `break` to find.
    if (bodyOk) bodyOk = lowerStmtList(bodyStmts, ilFn);
    exitScope();
    jumpStack_.pop_back();
    if (!bodyOk) return false;

    if (!currentBlockIsTerminated(ilFn)) {
        il::Instruction toUpdate;
        toUpdate.op = il::Op::Jump;
        toUpdate.type = il::Type::Void;
        toUpdate.result = il::kNoValue;
        toUpdate.target =
            il::BlockTarget{.block = bUpdate, .args = edgeArgsWithIndex(bUpdate, headerIndex)};
        emitInst(ilFn, toUpdate);
    }

    setCurrentBlock(bUpdate);
    bindLoopBlockParams(loopParams, updateParamMap);
    il::ValueId nextIndex = ilFn.valueCount++;
    il::Instruction advInst;
    advInst.op = il::Op::IterAdvance;
    advInst.type = il::Type::F64;
    advInst.result = nextIndex;
    advInst.operands = {iterVal.id, updateIndex};
    emitInst(ilFn, advInst);

    il::Instruction backJmp;
    backJmp.op = il::Op::Jump;
    backJmp.type = il::Type::Void;
    backJmp.result = il::kNoValue;
    backJmp.target =
        il::BlockTarget{.block = bHeader, .args = edgeArgsWithIndex(bHeader, nextIndex)};
    emitInst(ilFn, backJmp);

    setCurrentBlock(bExit);
    bindLoopBlockParams(loopParams, exitParamMap);
    return true;
}

bool Lowerer::lowerForOfStmt(const ast::ForOfStmt* forOf, il::Function& ilFn) {
    const std::string label = takePendingLabel();
    enterScope();
    auto iterOpt = lowerExpr(*forOf->iterable, ilFn);
    if (!iterOpt) return false;
    const Value iterVal = boxValueIfNeeded(*iterOpt, ilFn);
    pendingLabel_ = label;
    if (!lowerIndexWalkLoop(*forOf, iterVal, forOf->name, forOf->pattern.get(), forOf->isConst,
                            forOf->isLet, forOf->isVar, forOf->body, ilFn)) {
        return false;
    }
    exitScope();
    return true;
}

// The whole of for-in that is not for-of: the subject of the walk is not the
// object but the ARRAY OF KEYS the runtime builds from it, own and inherited,
// enumerable only, each key once (docs/0018 decision 1).
//
// That one instruction is also where `for (const k in null)` becomes a no-op
// rather than an error: ECMA-262 14.7.5.5 returns an empty completion for
// null and undefined, so the runtime hands back an empty array and the loop
// runs zero times without lowering needing to know it might.
bool Lowerer::lowerForInStmt(const ast::ForInStmt* forIn, il::Function& ilFn) {
    const std::string label = takePendingLabel();
    enterScope();
    auto objOpt = lowerExpr(*forIn->object, ilFn);
    if (!objOpt) return false;
    const Value objVal = boxValueIfNeeded(*objOpt, ilFn);

    il::ValueId keysVal = ilFn.valueCount++;
    il::Instruction keysInst;
    keysInst.op = il::Op::ForInKeys;
    keysInst.type = il::Type::Dynamic;
    keysInst.result = keysVal;
    keysInst.operands = {objVal.id};
    emitInst(ilFn, keysInst);

    pendingLabel_ = label;
    if (!lowerIndexWalkLoop(*forIn, Value{keysVal, il::Type::Dynamic}, forIn->name,
                            forIn->pattern.get(), forIn->isConst, forIn->isLet, forIn->isVar,
                            forIn->body, ilFn)) {
        return false;
    }
    exitScope();
    return true;
}

}  // namespace bronze::lower
