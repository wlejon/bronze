// `try` / `catch` / `finally` and `throw` (docs/0020).
//
// Three ideas, in the order they depend on each other.
//
// The handler is a property of the BLOCK (decision 3): `createBlock` stamps
// `currentHandler_` onto everything made inside a `try`, the backend derives
// its cell tests from that, and nothing here emits one. So a `try` body needs
// a block of its own — lowering cannot simply carry on in the block the
// statement was reached in, or the first `throw` in the body would take the
// ENCLOSING handler's edge.
//
// A handler block takes no parameters, because it is entered from an
// arbitrary point in the protected region (decision 4). Every binding a
// handler could disagree about is in an environment record before lowering
// gets here, which is why the joins below are all parameterless.
//
// `finally` is duplicated per exit path (decision 5). There is one copy for
// normal completion, one for the exception path, and one in front of each
// `return` / `break` / `continue` that crosses it. Nothing dispatches on a
// completion record, so `try { return 1 } finally { return 2 }` falls out:
// the inline copy's own `ret` terminates the block, and the outer one is
// unreachable code that docs/0014's statement-list rule already drops.

#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

namespace {

il::Instruction jumpTo(il::BlockId target) {
    il::Instruction inst;
    inst.op = il::Op::Jump;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.target = il::BlockTarget{.block = target, .args = {}};
    return inst;
}

}  // namespace

bool Lowerer::lowerThrowStmt(const ast::ThrowStmt* throwStmt, il::Function& ilFn) {
    if (!throwStmt->value) {
        diags_.error(throwStmt->span, "internal: a throw with no expression");
        return false;
    }
    auto val = lowerExpr(*throwStmt->value, ilFn);
    if (!val) return false;
    // Any value is throwable — `throw "negative"` is legal JS and is what the
    // promoted oracle case does — so the operand is boxed like any other
    // dynamic value rather than checked for Error-ness.
    Value boxed = boxValueIfNeeded(*val, ilFn);

    il::Instruction inst;
    inst.op = il::Op::Throw;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {boxed.id};
    emitInst(ilFn, inst);

    // Statements after a `throw` in the same list are unreachable and have
    // nowhere to go; a dead block gives them a home the verifier accepts.
    // Same shape as `emitJumpToTarget`'s.
    setCurrentBlock(createBlock(ilFn));
    return true;
}

size_t Lowerer::cleanupDepthForJump(size_t targetIndex) const {
    // A cleanup pushed when `jumpStack_` had size S is nested inside the
    // targets at indices 0..S-1, so a jump to index i crosses it exactly when
    // S > i. The stack is ordered, so the first crossing entry is the lowest
    // one that has to run.
    size_t depth = cleanupStack_.size();
    while (depth > 0 && cleanupStack_[depth - 1].jumpDepth > targetIndex) --depth;
    return depth;
}

void Lowerer::emitIterClose(il::ValueId record, bool suppress, il::Function& ilFn) {
    il::Instruction inst;
    inst.op = il::Op::IterClose;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {record};
    inst.immI32 = suppress ? 1 : 0;
    emitInst(ilFn, inst);
}

void Lowerer::openBlockUnderHandler(il::BlockId handler, il::Function& ilFn) {
    currentHandler_ = handler;
    const il::BlockId block = createBlock(ilFn);
    emitInst(ilFn, jumpTo(block));
    setCurrentBlock(block);
}

bool Lowerer::lowerFinallyBody(const ast::TryStmt& stmt, il::Function& ilFn) {
    enterScope(stmt.finallyBody, ilFn);
    std::vector<const ast::Stmt*> stmts;
    for (const auto& s : stmt.finallyBody) stmts.push_back(s.get());
    const bool ok = lowerStmtList(stmts, ilFn);
    exitScope();
    return ok;
}

bool Lowerer::runCleanups(size_t downTo, il::Function& ilFn) {
    for (size_t i = cleanupStack_.size(); i-- > downTo;) {
        // A completion from inside a `finally` overrides the one that was on
        // its way out, and the way that happens here is that the block is
        // already terminated: there is nothing left to emit the pending jump
        // or return into.
        if (currentBlockIsTerminated(ilFn)) break;
        const CleanupFrame frame = cleanupStack_[i];
        if (frame.kind == CleanupKind::IteratorClose) {
            // No block of its own and no handler dance: closing an iterator
            // is one call, and an error its `return` method raises on THIS
            // path (a `break` or a `return`, not a throw) propagates like any
            // other — 7.4.9 discards one only when a throw is already in
            // flight, which is the handler's copy, not this one.
            emitIterClose(frame.iterRecord, /*suppress=*/false, ilFn);
            continue;
        }
        // Truncated below this entry while its body runs, so a `break` inside
        // a `finally` does not run that same `finally` again on its way out.
        auto saved = cleanupStack_;
        cleanupStack_.resize(i);
        const il::BlockId savedHandler = currentHandler_;
        openBlockUnderHandler(frame.outerHandler, ilFn);
        const bool ok = lowerFinallyBody(*frame.stmt, ilFn);
        currentHandler_ = savedHandler;
        cleanupStack_ = std::move(saved);
        if (!ok) return false;
    }
    return true;
}

bool Lowerer::lowerTryStmt(const ast::TryStmt* tryStmt, il::Function& ilFn) {
    if (!tryStmt->hasFinally) return lowerTryCatch(tryStmt, ilFn);

    // 14.15.3 defines the three-part form as the two-part one wrapped in a
    // finally: the finally covers the catch clause too, so a `throw` from
    // inside `catch` still runs it.
    const il::BlockId bHandler = createBlock(ilFn);
    const il::BlockId bJoin = createBlock(ilFn);
    // Both were created BEFORE currentHandler_ moves, so an exception thrown
    // by the finally body itself propagates outward rather than back into
    // this handler.
    const il::BlockId outerHandler = currentHandler_;

    const auto stateBefore = snapshotVarStates();

    currentHandler_ = bHandler;
    const il::BlockId bProtected = createBlock(ilFn);
    emitInst(ilFn, jumpTo(bProtected));
    setCurrentBlock(bProtected);

    cleanupStack_.push_back(CleanupFrame{CleanupKind::Finally, tryStmt, il::kNoValue,
                                        jumpStack_.size(), outerHandler});
    const bool protectedOk =
        tryStmt->hasCatch ? lowerTryCatch(tryStmt, ilFn) : lowerTryBlock(tryStmt, ilFn);
    cleanupStack_.pop_back();
    currentHandler_ = outerHandler;
    if (!protectedOk) return false;

    // Normal completion: the finally runs, then control leaves the statement.
    // In a block of its own, under the OUTER handler — the block lowering is
    // in here was made inside the protected region and still names this try's
    // handler, which would send an exception raised by the finally straight
    // back into the finally.
    if (!currentBlockIsTerminated(ilFn)) {
        openBlockUnderHandler(outerHandler, ilFn);
        if (!lowerFinallyBody(*tryStmt, ilFn)) return false;
        if (!currentBlockIsTerminated(ilFn)) emitInst(ilFn, jumpTo(bJoin));
    }

    // The exception path. `exc.take` clears the cell, so the finally body runs
    // with nothing pending — which is what lets a `return` inside it discard
    // the exception simply by terminating the block before the re-raise.
    restoreVarStates(stateBefore);
    setCurrentBlock(bHandler);
    il::ValueId pending = ilFn.valueCount++;
    il::Instruction take;
    take.op = il::Op::ExcTake;
    take.type = il::Type::Dynamic;
    take.result = pending;
    emitInst(ilFn, take);

    if (!lowerFinallyBody(*tryStmt, ilFn)) return false;
    if (!currentBlockIsTerminated(ilFn)) {
        il::Instruction rethrow;
        rethrow.op = il::Op::Throw;
        rethrow.type = il::Type::Void;
        rethrow.result = il::kNoValue;
        rethrow.operands = {pending};
        emitInst(ilFn, rethrow);
    }

    restoreVarStates(stateBefore);
    setCurrentBlock(bJoin);
    return true;
}

bool Lowerer::lowerTryBlock(const ast::TryStmt* tryStmt, il::Function& ilFn) {
    enterScope(tryStmt->body, ilFn);
    std::vector<const ast::Stmt*> stmts;
    for (const auto& s : tryStmt->body) stmts.push_back(s.get());
    const bool ok = lowerStmtList(stmts, ilFn);
    exitScope();
    return ok;
}

bool Lowerer::lowerTryCatch(const ast::TryStmt* tryStmt, il::Function& ilFn) {
    const il::BlockId bHandler = createBlock(ilFn);
    const il::BlockId bJoin = createBlock(ilFn);
    const il::BlockId outerHandler = currentHandler_;

    const auto stateBefore = snapshotVarStates();

    currentHandler_ = bHandler;
    const il::BlockId bProtected = createBlock(ilFn);
    emitInst(ilFn, jumpTo(bProtected));
    setCurrentBlock(bProtected);
    const bool bodyOk = lowerTryBlock(tryStmt, ilFn);
    currentHandler_ = outerHandler;
    if (!bodyOk) return false;

    if (!currentBlockIsTerminated(ilFn)) emitInst(ilFn, jumpTo(bJoin));

    restoreVarStates(stateBefore);
    setCurrentBlock(bHandler);

    il::ValueId caught = ilFn.valueCount++;
    il::Instruction take;
    take.op = il::Op::ExcTake;
    take.type = il::Type::Dynamic;
    take.result = caught;
    emitInst(ilFn, take);

    // 14.15.2 gives the catch parameter its own declarative environment
    // around the block, so the parameter's names belong to the scope the body
    // is lowered in — the same shape a for-of head's binding has, and the
    // reason `enterScope` takes extra declarations at all.
    std::vector<std::string> bound;
    if (tryStmt->hasCatchParam) {
        if (tryStmt->catchPattern) {
            bound = ast::patternBoundNames(*tryStmt->catchPattern);
        } else {
            bound.push_back(tryStmt->catchName);
        }
    }
    enterScope(tryStmt->catchBody, ilFn, bound);

    bool bindOk = true;
    if (tryStmt->hasCatchParam) {
        Value thrown{caught, il::Type::Dynamic};
        if (tryStmt->catchPattern) {
            // A CatchParameter is a BindingPattern like any other, so
            // docs/0017's destructuring applies unchanged — including a
            // default and a rest element inside it.
            PatternTarget target{.declare = true, .isConst = false, .isLet = true, .isVar = false};
            bindOk = lowerPattern(*tryStmt->catchPattern, thrown, target, ilFn);
        } else {
            bindOk = declareVariable(tryStmt->catchName, il::Type::Dynamic, /*isConst=*/false,
                                     /*isLet=*/true, /*isVar=*/false, /*isInitialized=*/true,
                                     caught, tryStmt->span);
            if (bindOk) {
                VarBinding& b = varBindings_[activeVarMap_[tryStmt->catchName]];
                if (b.inEnv) emitEnvSet(envDepthOf(b.envScopeIndex), b.envSlot, thrown, ilFn);
            }
        }
    }

    bool catchOk = bindOk;
    if (catchOk) {
        std::vector<const ast::Stmt*> catchStmts;
        for (const auto& s : tryStmt->catchBody) catchStmts.push_back(s.get());
        catchOk = lowerStmtList(catchStmts, ilFn);
    }
    exitScope();
    if (!catchOk) return false;

    if (!currentBlockIsTerminated(ilFn)) emitInst(ilFn, jumpTo(bJoin));

    restoreVarStates(stateBefore);
    setCurrentBlock(bJoin);
    return true;
}

}  // namespace bronze::lower
