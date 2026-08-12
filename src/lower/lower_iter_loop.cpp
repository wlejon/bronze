// The two loops that walk a container: `for-of` over an ITERATOR, and `for-in`
// over the key snapshot the runtime builds for it.
//
// They share one lowering, which is the point of snapshotting the keys: once
// `for-in`'s subject is an array of strings, the two statements differ in
// nothing but the value handed to the walk. Everything below — the four-block
// shape, the per-iteration binding, the close-on-abrupt-exit handler — is
// written once.

#include <string>
#include <vector>

#include "ast/assigned.h"
#include "lower/lowerer.h"

namespace bronze::lower {

// The same four-block shape as `for`, with two differences.
//
// The cursor is not a block parameter. The first for-of threaded an index
// through every block and every edge, because the walk WAS an index; the
// iteration record holds it now, so the loop carries nothing but its source
// bindings and `continue` has nothing extra to hand over.
//
// The head binding is per-iteration BY DEFINITION. It is declared inside the
// body's scope, which is entered once per iteration, so a closure over it
// captures that iteration's value. `for (let i = ...)` needs the same thing
// and does not get it (diagnoses it); here it is free,
// because there is no binding outside the body to share.
//
// The fifth block is the one this doc's chunk adds: a handler that closes the
// iterator when the body throws. It costs a block per loop in the IL and
// nothing at run time — the cell test after each call in the body is the one
// the unwind path already emits, pointed at this block instead of at the
// function's unwind block.
bool Lowerer::lowerIteratorLoop(const ast::Stmt& loopStmt, Value iterVal,
                                const std::string& headName,
                                const ast::BindingPattern* headPattern, bool isConst, bool isLet,
                                bool isVar, const std::vector<ast::StmtPtr>& body,
                                il::Function& ilFn) {
    const std::string label = takePendingLabel();

    il::ValueId recVal = ilFn.valueCount++;
    il::Instruction openInst;
    openInst.op = il::Op::IterOpen;
    openInst.type = il::Type::Dynamic;
    openInst.result = recVal;
    openInst.operands = {iterVal.id};
    emitInst(ilFn, openInst);

    const auto loopParams = collectLoopParams(loopStmt, ast::getAssignedNames(loopStmt));
    std::vector<std::string> loopVars;
    for (const auto& param : loopParams) loopVars.push_back(param.name);

    // Created BEFORE the handler moves, so that code after the loop, and the
    // close handler itself, both run under the ENCLOSING handler: an
    // exception raised by `iter.close` propagates outward rather than back
    // into the block that is already unwinding.
    const il::BlockId outerHandler = currentHandler_;
    il::BlockId bExit = createBlock(ilFn);
    il::BlockId bClose = createBlock(ilFn);

    currentHandler_ = bClose;
    il::BlockId bHeader = createBlock(ilFn);
    il::BlockId bBody = createBlock(ilFn);
    il::BlockId bUpdate = createBlock(ilFn);

    auto headerParamMap = addLoopBlockParams(loopParams, bHeader, ilFn);

    il::Instruction jmpEntry;
    jmpEntry.op = il::Op::Jump;
    jmpEntry.type = il::Type::Void;
    jmpEntry.result = il::kNoValue;
    jmpEntry.target = il::BlockTarget{.block = bHeader, .args = collectEdgeArgs(loopVars, bHeader, ilFn)};
    emitInst(ilFn, jmpEntry);

    setCurrentBlock(bHeader);
    bindLoopBlockParams(loopParams, headerParamMap);

    il::ValueId moreVal = ilFn.valueCount++;
    il::Instruction stepInst;
    stepInst.op = il::Op::IterStep;
    stepInst.type = il::Type::Bool;
    stepInst.result = moreVal;
    stepInst.operands = {recVal};
    emitInst(ilFn, stepInst);

    // The exit block carries the loop variables only: `break` reaches it from
    // the body and has nothing else to hand over.
    auto exitParamMap = addLoopBlockParams(loopParams, bExit, ilFn);
    std::vector<il::ValueId> headerExitArgs = collectEdgeArgs(loopVars, bExit, ilFn);

    il::Instruction brInst;
    brInst.op = il::Op::Branch;
    brInst.type = il::Type::Void;
    brInst.result = il::kNoValue;
    brInst.operands = {moreVal};
    brInst.target = il::BlockTarget{.block = bBody, .args = {}};
    brInst.elseTarget = il::BlockTarget{.block = bExit, .args = std::move(headerExitArgs)};
    emitInst(ilFn, brInst);

    auto updateParamMap = addLoopBlockParams(loopParams, bUpdate, ilFn);

    setCurrentBlock(bBody);
    il::ValueId elemVal = ilFn.valueCount++;
    il::Instruction valueInst;
    valueInst.op = il::Op::IterValue;
    valueInst.type = il::Type::Dynamic;
    valueInst.result = elemVal;
    valueInst.operands = {recVal};
    emitInst(ilFn, valueInst);

    // The jump target goes on FIRST and the cleanup second, so the cleanup's
    // recorded depth is inside this loop: a `break` here crosses it (and
    // closes the iterator) while a `continue` stops above it. That ordering
    // is the whole rule; see JumpTarget's two cleanup depths.
    JumpTarget ctx{JumpKind::Loop,  label,
                   bHeader,         bUpdate,
                   bExit,           loopVars,
                   cleanupStack_.size(), cleanupStack_.size()};
    jumpStack_.push_back(ctx);
    cleanupStack_.push_back(CleanupFrame{CleanupKind::IteratorClose, nullptr, recVal,
                                         jumpStack_.size(), outerHandler});
    jumpStack_.back().cleanupDepthInBody = cleanupStack_.size();

    // The head binding belongs to the body's scope, so it gets an environment
    // slot there when a closure captures it — one per iteration, which is the
    // whole of the language's rule for it. A destructuring head binds every
    // name its pattern spells, and all of them belong to this scope for the
    // same per-iteration reason.
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
    // this loop on either stack for a later `break` to find.
    if (bodyOk) bodyOk = lowerStmtList(bodyStmts, ilFn);
    exitScope();
    cleanupStack_.pop_back();
    jumpStack_.pop_back();
    if (!bodyOk) return false;

    if (!currentBlockIsTerminated(ilFn)) {
        il::Instruction toUpdate;
        toUpdate.op = il::Op::Jump;
        toUpdate.type = il::Type::Void;
        toUpdate.result = il::kNoValue;
        toUpdate.target =
            il::BlockTarget{.block = bUpdate, .args = collectEdgeArgs(loopVars, bUpdate, ilFn)};
        emitInst(ilFn, toUpdate);
    }

    setCurrentBlock(bUpdate);
    bindLoopBlockParams(loopParams, updateParamMap);
    il::Instruction backJmp;
    backJmp.op = il::Op::Jump;
    backJmp.type = il::Type::Void;
    backJmp.result = il::kNoValue;
    backJmp.target =
        il::BlockTarget{.block = bHeader, .args = collectEdgeArgs(loopVars, bHeader, ilFn)};
    emitInst(ilFn, backJmp);

    // The throw path (ECMA-262 7.4.9 with a throw completion). It takes the
    // pending value, closes the iterator with errors from `return` SUPPRESSED —
    // step 6 keeps the original completion — and re-raises. It reads no
    // binding, which is what lets it take no parameters even though it is
    // entered from an arbitrary point in the body.
    currentHandler_ = outerHandler;
    setCurrentBlock(bClose);
    il::ValueId pending = ilFn.valueCount++;
    il::Instruction take;
    take.op = il::Op::ExcTake;
    take.type = il::Type::Dynamic;
    take.result = pending;
    emitInst(ilFn, take);
    emitIterClose(recVal, /*suppress=*/true, ilFn);
    il::Instruction rethrow;
    rethrow.op = il::Op::Throw;
    rethrow.type = il::Type::Void;
    rethrow.result = il::kNoValue;
    rethrow.operands = {pending};
    emitInst(ilFn, rethrow);

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
    if (!lowerIteratorLoop(*forOf, iterVal, forOf->name, forOf->pattern.get(), forOf->isConst,
                           forOf->isLet, forOf->isVar, forOf->body, ilFn)) {
        return false;
    }
    exitScope();
    return true;
}

// The whole of for-in that is not for-of: the subject of the walk is not the
// object but the ARRAY OF KEYS the runtime builds from it, own and inherited,
// enumerable only, each key once.
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
    if (!lowerIteratorLoop(*forIn, Value{keysVal, il::Type::Dynamic}, forIn->name,
                           forIn->pattern.get(), forIn->isConst, forIn->isLet, forIn->isVar,
                           forIn->body, ilFn)) {
        return false;
    }
    exitScope();
    return true;
}

}  // namespace bronze::lower
