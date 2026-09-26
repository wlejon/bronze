// `yield*`: the delegation protocol, ECMA-262 27.5.3.7 (15.5.5).
//
// A plain `yield` is one suspension with three resumptions. A `yield*` is a
// LOOP over them: every resumption of the outer generator, however it arrived,
// is forwarded to the inner iterator; whatever the inner iterator produces is
// yielded onward; and the delegation ends only when an inner result says done
// — at which point that result's `value` is the value of the whole `yield*`.
//
//     %rec = iter.open <operand>              (async: async.iter.open)
//     jump bHead(next, undefined)
//     bHead(%mode: i32, %recv):
//                %res = iter.delegate %rec, %mode, %recv
//                br %mode == return -> bRet, else -> bResult
//     bRet:      br is.nullish %res -> bPass, else -> bResult
//     bResult:   (async: %res = await %res)
//                br truthy %res.done -> bDone, else -> bSuspend
//     bSuspend:  %sent = coro.suspend delegate %res   (async: yield %res.value)
//                jump bHead(coro.mode, %sent)
//     bDone:     %v = %res.value; br %mode == return -> bFinish, else -> bAfter
//     bFinish:   return %v           (5.c.viii: the inner return reported done)
//     bPass:     return %recv        (5.c.iii: no inner `return` method)
//     bAfter:    the value of `yield* ...` is %v
//
// The received completion — which of `next`, `return` and `throw` resumed us,
// and with what — is a pair of block parameters: it is produced ON each entry
// to the loop, by whichever edge got there. The mode is read here only to tell
// the two ENDINGS apart: a delegation does not raise a `throw` resumption at
// its own site — it hands it to the inner iterator, which may catch it — and it
// does not end on a `return` resumption either while the inner `return`
// reports not done. Both belong to `iter.delegate`'s answer.
//
// A sync generator hands the inner result object out BY IDENTITY (27.5.3.8
// GeneratorYield), which is what the DELEGATE suspension tells the runtime; an
// async generator yields the inner result's value (AsyncGeneratorYield).

#include <optional>

#include "lower/lowerer.h"

namespace bronze::lower {

std::optional<Lowerer::Value> Lowerer::lowerYieldStar(const ast::YieldExpr& yield,
                                                      il::Function& ilFn) {
    if (!coro_ || !coro_->isGenerator()) {
        diags_.error(yield.span, "internal: a `yield*` outside a generator body");
        return std::nullopt;
    }
    const bool isAsync = coro_->isAsyncGenerator();

    // 27.5.3.7 step 2: GetIterator on the operand, once, before any resumption
    // is forwarded.
    auto operand = lowerExpr(*yield.argument, ilFn);
    if (!operand) return std::nullopt;
    Value source = boxValueIfNeeded(*operand, ilFn);

    il::ValueId record = ilFn.valueCount++;
    il::Instruction openInst;
    openInst.op = isAsync ? il::Op::AsyncIterOpen : il::Op::IterOpen;
    openInst.type = il::Type::Dynamic;
    openInst.result = record;
    openInst.operands = {source.id};
    emitInst(ilFn, openInst);

    // Made before anything is emitted into them, so every branch below can
    // name its target. `createBlock` stamps each with the handler in force
    // here, which is what makes an exception out of the delegation reach a
    // `try` the `yield*` was written inside.
    const il::BlockId bHead = createBlock(ilFn);
    const il::BlockId bReturnPath = createBlock(ilFn);
    const il::BlockId bResult = createBlock(ilFn);
    const il::BlockId bSuspend = createBlock(ilFn);
    const il::BlockId bDone = createBlock(ilFn);
    const il::BlockId bFinishReturn = createBlock(ilFn);
    const il::BlockId bPassThrough = createBlock(ilFn);
    const il::BlockId bAfter = createBlock(ilFn);

    const il::ValueId modeParam = ilFn.valueCount++;
    const il::ValueId recvParam = ilFn.valueCount++;
    ilFn.blocks[bHead].params.push_back({modeParam, il::Type::I32});
    ilFn.blocks[bHead].params.push_back({recvParam, il::Type::Dynamic});

    // 27.5.3.7 step 4: the loop is entered with a NORMAL completion carrying
    // undefined.
    il::ValueId firstMode = ilFn.valueCount++;
    il::Instruction modeConst;
    modeConst.op = il::Op::ConstI32;
    modeConst.type = il::Type::I32;
    modeConst.result = firstMode;
    modeConst.immI32 = BRONZE_ABI_RESUME_NEXT;
    emitInst(ilFn, modeConst);
    const il::ValueId firstRecv = emitConstUndefined(ilFn);
    il::Instruction enter;
    enter.op = il::Op::Jump;
    enter.type = il::Type::Void;
    enter.result = il::kNoValue;
    enter.target = il::BlockTarget{.block = bHead, .args = {firstMode, firstRecv}};
    emitInst(ilFn, enter);

    // --- bHead: forward this resumption ------------------------------------
    setCurrentBlock(bHead);
    const Value mode{modeParam, il::Type::I32};
    const Value boxedMode = boxValueIfNeeded(mode, ilFn);
    il::ValueId stepped = ilFn.valueCount++;
    il::Instruction step;
    step.op = il::Op::IterDelegate;
    step.type = il::Type::Dynamic;
    step.result = stepped;
    step.operands = {record, boxedMode.id, recvParam};
    emitInst(ilFn, step);
    const il::ValueId isReturn = emitModeIs(mode, BRONZE_ABI_RESUME_RETURN, ilFn);
    emitInst(ilFn, branchOn(isReturn, bReturnPath, bResult));

    // --- bReturnPath: did the inner iterator have a `return` at all? --------
    setCurrentBlock(bReturnPath);
    il::ValueId noReturnMethod = ilFn.valueCount++;
    il::Instruction nullish;
    nullish.op = il::Op::IsNullish;
    nullish.type = il::Type::Bool;
    nullish.result = noReturnMethod;
    nullish.operands = {stepped};
    emitInst(ilFn, nullish);
    emitInst(ilFn, branchOn(noReturnMethod, bPassThrough, bResult));

    // --- bResult: 7.4.4 IteratorComplete ------------------------------------
    setCurrentBlock(bResult);
    Value result{stepped, il::Type::Dynamic};
    if (isAsync) {
        // An async iterator's methods answer promises of their results.
        auto awaited = lowerAwaitValue(result, yield.span, ilFn);
        if (!awaited) return std::nullopt;
        result = *awaited;
    }
    il::ValueId doneProp = ilFn.valueCount++;
    il::Instruction readDone;
    readDone.op = il::Op::PropGet;
    readDone.type = il::Type::Dynamic;
    readDone.result = doneProp;
    readDone.operands = {result.id};
    readDone.keyIndex = getKeyConstantIndex("done");
    readDone.icIndex = icSiteCounter_++;
    emitInst(ilFn, readDone);
    Value done = lowerConditionFromVal(Value{doneProp, il::Type::Dynamic}, ilFn);
    emitInst(ilFn, branchOn(done.id, bDone, bSuspend));

    const auto readValue = [&](il::Function& fn) {
        il::ValueId valueProp = fn.valueCount++;
        il::Instruction read;
        read.op = il::Op::PropGet;
        read.type = il::Type::Dynamic;
        read.result = valueProp;
        read.operands = {result.id};
        read.keyIndex = getKeyConstantIndex("value");
        read.icIndex = icSiteCounter_++;
        emitInst(fn, read);
        return Value{valueProp, il::Type::Dynamic};
    };

    // --- bSuspend: yield onward, then back to the top with the resumption ---
    setCurrentBlock(bSuspend);
    const Value sent =
        isAsync ? emitCoroSuspend(BRONZE_ABI_SUSPEND_YIELD, readValue(ilFn), ilFn)
                : emitCoroSuspend(BRONZE_ABI_SUSPEND_DELEGATE, result, ilFn);
    const Value resumedMode = emitCoroMode(ilFn);
    il::Instruction again;
    again.op = il::Op::Jump;
    again.type = il::Type::Void;
    again.result = il::kNoValue;
    again.target = il::BlockTarget{.block = bHead, .args = {resumedMode.id, sent.id}};
    emitInst(ilFn, again);

    // --- bDone: 7.4.5 IteratorValue, then which ending this is -------------
    setCurrentBlock(bDone);
    const Value finalValue = readValue(ilFn);
    emitInst(ilFn, branchOn(isReturn, bFinishReturn, bAfter));

    // 5.c.viii: a `return` resumption whose inner `return` reported done ends
    // the OUTER body too, carrying the inner result's value.
    setCurrentBlock(bFinishReturn);
    if (!emitCoroutineReturn(finalValue, ilFn)) return std::nullopt;

    // 5.c.iii: no `return` method on the inner iterator, so the return
    // completion passes straight through with the value it was given.
    setCurrentBlock(bPassThrough);
    if (!emitCoroutineReturn(Value{recvParam, il::Type::Dynamic}, ilFn)) return std::nullopt;

    // 5.a.v: the delegation finished normally.
    setCurrentBlock(bAfter);
    return finalValue;
}

}  // namespace bronze::lower
