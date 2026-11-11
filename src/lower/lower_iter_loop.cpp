// The two loops that walk a container: `for-of` over an ITERATOR, and `for-in`
// over the key snapshot the runtime builds for it.
//
// They share one lowering, which is the point of snapshotting the keys: once
// `for-in`'s subject is an array of strings, the two statements differ in
// nothing but the value handed to the walk. Everything below — the four-block
// shape, the per-iteration binding, the close-on-abrupt-exit handler — is
// written once.

#include <optional>
#include <string>
#include <vector>

#include "ast/assigned.h"
#include "ast/queries.h"
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
// captures that iteration's value — free here, because there is no binding
// outside the body to share. `for (let i = ...)` has one, and so has to build
// the copies explicitly (14.7.4.9, see lowerForStmt).
//
// The fifth block is the one this doc's chunk adds: a handler that closes the
// iterator when the body throws. It costs a block per loop in the IL and
// nothing on the path that does not throw: the body's calls unwind to this
// block instead of out of the function.
bool Lowerer::lowerIteratorLoop(const ast::Stmt& loopStmt, Value iterVal,
                                const std::string& headName,
                                const ast::BindingPattern* headPattern, bool isConst, bool isLet,
                                const std::vector<ast::StmtPtr>& body,
                                il::Function& ilFn, bool isAwait) {
    const std::string label = takePendingLabel();

    // AN ARRAY IS WALKED BY INDEX. A plain array (`is.plain_array`: no
    // named-property box, so its `[Symbol.iterator]` is %Array.prototype.values%
    // and that iterator's `next` the intrinsic, both frozen) is stepped as the
    // array iterator steps it — `index < length` asked again each time, then
    // Get(array, index) — with the index a value of the loop instead of a
    // field of an iteration record: no record is opened, the step is a compare
    // and the read an inline element load. The question is asked ONCE, where
    // GetIterator would have read the method; a box the body gives the array
    // later changes nothing, as it would not for an iterator already made.
    // There is nothing to close: an array iterator has no `return`.
    // Everything else opens a record, in the same loop: each of the step and
    // the value is a branch on the one answer.
    const bool indexed = !isAwait;
    il::ValueId isArrVal = il::kNoValue;
    il::ValueId recVal = il::kNoValue;
    const auto jumpTo = [&](il::BlockId block, std::vector<il::ValueId> args) {
        il::Instruction j;
        j.op = il::Op::Jump;
        j.type = il::Type::Void;
        j.result = il::kNoValue;
        j.target = il::BlockTarget{.block = block, .args = std::move(args)};
        emitInst(ilFn, j);
    };
    const auto branchOnKind = [&](il::BlockId onArray, il::BlockId onRecord) {
        il::Instruction br;
        br.op = il::Op::Branch;
        br.type = il::Type::Void;
        br.result = il::kNoValue;
        br.operands = {isArrVal};
        br.target = il::BlockTarget{.block = onArray, .args = {}};
        br.elseTarget = il::BlockTarget{.block = onRecord, .args = {}};
        emitInst(ilFn, br);
    };
    if (indexed) {
        isArrVal = ilFn.valueCount++;
        il::Instruction isArr;
        isArr.op = il::Op::IsPlainArray;
        isArr.type = il::Type::Bool;
        isArr.result = isArrVal;
        isArr.operands = {iterVal.id};
        emitInst(ilFn, isArr);
        const il::BlockId bArray = createBlock(ilFn);
        const il::BlockId bOpen = createBlock(ilFn);
        const il::BlockId bJoin = createBlock(ilFn);
        branchOnKind(bArray, bOpen);
        setCurrentBlock(bArray);
        jumpTo(bJoin, {emitConstUndefined(ilFn)});
        setCurrentBlock(bOpen);
        const il::ValueId opened = ilFn.valueCount++;
        il::Instruction openInst;
        openInst.op = il::Op::IterOpen;
        openInst.type = il::Type::Dynamic;
        openInst.result = opened;
        openInst.operands = {iterVal.id};
        emitInst(ilFn, openInst);
        jumpTo(bJoin, {opened});
        // The record, or `undefined` on the array's side, where nothing reads it.
        recVal = ilFn.valueCount++;
        ilFn.blocks[bJoin].params.push_back({recVal, il::Type::Dynamic});
        setCurrentBlock(bJoin);
    } else {
        recVal = ilFn.valueCount++;
        il::Instruction openInst;
        openInst.op = il::Op::AsyncIterOpen;
        openInst.type = il::Type::Dynamic;
        openInst.result = recVal;
        openInst.operands = {iterVal.id};
        emitInst(ilFn, openInst);
    }

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
    // The array walk's index: the header's last parameter, after the source
    // bindings (collectEdgeArgs pairs those by position).
    il::ValueId indexVal = il::kNoValue;
    const auto f64Const = [&](double v) {
        const il::ValueId id = ilFn.valueCount++;
        il::Instruction c;
        c.op = il::Op::ConstF64;
        c.type = il::Type::F64;
        c.result = id;
        c.immF64 = v;
        emitInst(ilFn, c);
        return id;
    };
    {
        std::vector<il::ValueId> entryArgs = collectEdgeArgs(loopVars, bHeader, ilFn);
        if (indexed) {
            indexVal = ilFn.valueCount++;
            ilFn.blocks[bHeader].params.push_back({indexVal, il::Type::F64});
            entryArgs.push_back(f64Const(0.0));
        }
        jumpTo(bHeader, std::move(entryArgs));
    }

    setCurrentBlock(bHeader);
    bindLoopBlockParams(loopParams, headerParamMap);

    il::ValueId moreVal = il::kNoValue;
    Value asyncIterResult{il::kNoValue, il::Type::Dynamic};
    if (isAwait) {
        il::ValueId nextPromiseVal = ilFn.valueCount++;
        il::Instruction nextInst;
        nextInst.op = il::Op::AsyncIterNext;
        nextInst.type = il::Type::Dynamic;
        nextInst.result = nextPromiseVal;
        nextInst.operands = {recVal};
        emitInst(ilFn, nextInst);

        auto awaitResult = lowerAwaitValue(Value{nextPromiseVal, il::Type::Dynamic},
                                           loopStmt.span, ilFn);
        if (!awaitResult) return false;
        asyncIterResult = *awaitResult;

        const uint32_t doneIdx = getKeyConstantIndex("done");
        il::ValueId doneVal = ilFn.valueCount++;
        il::Instruction getDone;
        getDone.op = il::Op::PropGet;
        getDone.type = il::Type::Dynamic;
        getDone.result = doneVal;
        getDone.keyIndex = doneIdx;
        getDone.icIndex = icSiteCounter_++;
        getDone.operands = {asyncIterResult.id};
        emitInst(ilFn, getDone);

        Value doneBool = lowerConditionFromVal(Value{doneVal, il::Type::Dynamic}, ilFn);
        Value notDone = emitLogicalNot(doneBool, ilFn);
        moreVal = notDone.id;
    } else {
        const il::BlockId bArrayStep = createBlock(ilFn);
        const il::BlockId bRecordStep = createBlock(ilFn);
        const il::BlockId bTest = createBlock(ilFn);
        branchOnKind(bArrayStep, bRecordStep);

        setCurrentBlock(bArrayStep);
        const il::ValueId lenVal = ilFn.valueCount++;
        il::Instruction lenInst;
        lenInst.op = il::Op::PropGet;
        lenInst.type = il::Type::Dynamic;
        lenInst.result = lenVal;
        lenInst.keyIndex = getKeyConstantIndex("length");
        lenInst.icIndex = icSiteCounter_++;
        lenInst.immI32 = il::kPropGetArrayLength;
        lenInst.operands = {iterVal.id};
        emitInst(ilFn, lenInst);
        const Value lenNum =
            unboxValueIfNeeded(Value{lenVal, il::Type::Dynamic}, il::Type::F64, ilFn);
        const il::ValueId inRange = ilFn.valueCount++;
        il::Instruction lt;
        lt.op = il::Op::CmpLt;
        lt.type = il::Type::Bool;
        lt.result = inRange;
        lt.operands = {indexVal, lenNum.id};
        emitInst(ilFn, lt);
        jumpTo(bTest, {inRange});

        setCurrentBlock(bRecordStep);
        const il::ValueId stepped = ilFn.valueCount++;
        il::Instruction stepInst;
        stepInst.op = il::Op::IterStep;
        stepInst.type = il::Type::Bool;
        stepInst.result = stepped;
        stepInst.operands = {recVal};
        emitInst(ilFn, stepInst);
        jumpTo(bTest, {stepped});

        moreVal = ilFn.valueCount++;
        ilFn.blocks[bTest].params.push_back({moreVal, il::Type::Bool});
        setCurrentBlock(bTest);
    }

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
    il::ValueId elemVal;
    if (isAwait) {
        const uint32_t valIdx = getKeyConstantIndex("value");
        elemVal = ilFn.valueCount++;
        il::Instruction getVal;
        getVal.op = il::Op::PropGet;
        getVal.type = il::Type::Dynamic;
        getVal.result = elemVal;
        getVal.keyIndex = valIdx;
        getVal.icIndex = icSiteCounter_++;
        getVal.operands = {asyncIterResult.id};
        emitInst(ilFn, getVal);
    } else {
        const il::BlockId bArrayValue = createBlock(ilFn);
        const il::BlockId bRecordValue = createBlock(ilFn);
        const il::BlockId bValue = createBlock(ilFn);
        branchOnKind(bArrayValue, bRecordValue);

        setCurrentBlock(bArrayValue);
        const Value indexBoxed = boxValueIfNeeded(Value{indexVal, il::Type::F64}, ilFn);
        const il::ValueId readVal = ilFn.valueCount++;
        il::Instruction getInst;
        getInst.op = il::Op::ElemGet;
        getInst.type = il::Type::Dynamic;
        getInst.result = readVal;
        getInst.operands = {iterVal.id, indexBoxed.id};
        getInst.immI32 = il::kElemFlagArrayReceiver | il::kElemFlagProvenIntegralIndex;
        emitInst(ilFn, getInst);
        jumpTo(bValue, {readVal});

        setCurrentBlock(bRecordValue);
        const il::ValueId stepValue = ilFn.valueCount++;
        il::Instruction valueInst;
        valueInst.op = il::Op::IterValue;
        valueInst.type = il::Type::Dynamic;
        valueInst.result = stepValue;
        valueInst.operands = {recVal};
        emitInst(ilFn, valueInst);
        jumpTo(bValue, {stepValue});

        elemVal = ilFn.valueCount++;
        ilFn.blocks[bValue].params.push_back({elemVal, il::Type::Dynamic});
        setCurrentBlock(bValue);
    }

    // The jump target goes on FIRST and the cleanup second, so the cleanup's
    // recorded depth is inside this loop: a `break` here crosses it (and
    // closes the iterator) while a `continue` stops above it. That ordering
    // is the whole rule; see JumpTarget's two cleanup depths.
    JumpTarget ctx{JumpKind::Loop,  label,
                   bHeader,         bUpdate,
                   bExit,           loopVars,
                   cleanupStack_.size(), cleanupStack_.size()};
    jumpStack_.push_back(ctx);
    jumpStack_.back().scopeDepth = currentScopeDepth_;
    cleanupStack_.push_back(CleanupFrame{
        isAwait ? CleanupKind::AsyncIteratorClose : CleanupKind::IteratorClose, nullptr, recVal,
        jumpStack_.size(), outerHandler, isArrVal});
    jumpStack_.back().cleanupDepthInBody = cleanupStack_.size();

    // A `let`/`const` head binding belongs to the body's scope, so it gets an
    // environment slot there when a closure captures it — one per iteration,
    // which is the whole of the language's rule for it. A `var` head is the
    // function's binding (hoisted with the function's other vars), so like a
    // head naming existing bindings it introduces nothing in this scope and
    // assigns the one binding every iteration and the code after the loop share.
    const bool isDecl = isConst || isLet;
    const std::vector<std::string> headNames =
        isDecl ? (headPattern ? ast::patternBoundNames(*headPattern)
                              : std::vector<std::string>{headName})
               : std::vector<std::string>{};
    enterScope(body, ilFn, headNames);
    bool bodyOk = true;
    const PatternTarget target{
        .declare = isDecl, .isConst = isConst, .isLet = isLet, .isVar = false};
    if (headPattern) {
        bodyOk = lowerPattern(*headPattern, Value{elemVal, il::Type::Dynamic}, target, ilFn);
    } else {
        bodyOk = bindPatternName(headName, Value{elemVal, il::Type::Dynamic}, target,
                                 loopStmt.span, ilFn);
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
    {
        std::vector<il::ValueId> backArgs = collectEdgeArgs(loopVars, bHeader, ilFn);
        if (indexed) {
            const il::ValueId one = f64Const(1.0);
            const il::ValueId next = ilFn.valueCount++;
            il::Instruction inc;
            inc.op = il::Op::Add;
            inc.type = il::Type::F64;
            inc.result = next;
            inc.operands = {indexVal, one};
            emitInst(ilFn, inc);
            backArgs.push_back(next);
        }
        jumpTo(bHeader, std::move(backArgs));
    }

    // The throw path (ECMA-262 7.4.9 with a throw completion). It takes the
    // thrown value, closes the iterator with errors from `return` SUPPRESSED —
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
    emitIterClose(recVal, /*suppress=*/true, ilFn, isAwait, isArrVal);
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
                           forOf->isLet, forOf->body, ilFn, forOf->isAwait)) {
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
                           forIn->pattern.get(), forIn->isConst, forIn->isLet,
                           forIn->body, ilFn)) {
        return false;
    }
    exitScope();
    return true;
}

}  // namespace bronze::lower
