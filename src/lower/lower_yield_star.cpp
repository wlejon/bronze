// `yield*`: the delegation protocol, ECMA-262 27.5.3.7.
//
// A plain `yield` is one suspension with three resumptions, and lower_generator
// builds it as one block that asks which of the three it got. A `yield*` is a
// LOOP over that question. Every resumption of the outer generator, however it
// arrived, is forwarded to an inner iterator; whatever the inner iterator
// produces is yielded onward; and the delegation ends only when an inner result
// says done — at which point that result's `value` is the value of the whole
// `yield*` expression. So one syntactic site is a cycle, not a step.
//
// The shape it lowers to, with `%rec` the iteration record in the frame:
//
//     ...        %rec = iter.open <operand>; env.set frame.gen.iter, %rec
//                jump bHead(<mode 0>, undefined)
//     bHead(%mode, %recv):
//                %res = iter.delegate env.get frame.gen.iter, %mode, %recv
//                br %mode == return -> bRet, else -> bDone?
//     bRet:      br is.nullish %res  -> bPass, else -> bDone?
//     bDone?:    br truthy %res.done -> bDone, else -> bSuspend
//     bSuspend:  env.set frame.gen.state, <index>; ret %res
//     bResume:   jump bHead(__mode, __sent)
//     bDone:     %v = %res.value; br %mode == return -> bFinish, else -> bAfter
//     bFinish:   <cleanups>; the walk ends with %v
//     bPass:     <cleanups>; the walk ends with %recv
//     bAfter:    the value of `yield* ...` is %v
//
// Two things live across the suspension and they live in two different places,
// for one reason each.
//
// The ITERATION RECORD is in the frame's environment record. It has to be: the
// only edge into `bResume` comes from the resume dispatch in the entry block,
// and `il.h`'s rule for that edge is that it defines no SSA value at all. A
// record held in SSA would be a value `bHead` could not read on the second time
// round.
//
// The RECEIVED COMPLETION — which of `next`, `return` and `throw` resumed us,
// and with what — is a pair of BLOCK PARAMETERS, and needs no frame slot. It
// does not cross the suspension: it is produced ON each entry to the loop, by
// whichever edge got there. The entry edge produces "normal, undefined"
// (27.5.3.7 step 4); the resume edge produces the resume function's own `__mode`
// and `__sent`, which are ordinary parameters valid in every block. Putting
// them in the frame would be storing a value that is already in hand.
//
// What is NOT here is the three-way `br` on the mode that `lowerYield` emits.
// A delegation does not raise a `throw` resumption at its own site — it hands it
// to the inner iterator, which may catch it — and it does not end the walk on a
// `return` resumption either, because 5.c.x keeps yielding when the inner
// `return` reports not done. Both decisions belong to `iter.delegate`'s answer
// rather than to the resumption's kind alone, which is why the mode is read
// here only to tell the two ENDINGS apart.

#include <optional>

#include "lower/lowerer.h"

namespace bronze::lower {

std::optional<Lowerer::Value> Lowerer::lowerYieldStar(const ast::YieldExpr& yield,
                                                      il::Function& ilFn) {
    if (!generator_) {
        diags_.error(yield.span, "internal: a `yield*` outside a generator body");
        return std::nullopt;
    }
    GeneratorContext& gen = *generator_;
    if (gen.iterSlot == UINT32_MAX) {
        // The frame is laid out before the body is lowered, from the same query
        // that decides this. Reaching here means the two disagreed.
        diags_.error(yield.span, "internal: a `yield*` in a frame with no delegation slot");
        return std::nullopt;
    }

    // 27.5.3.7 step 2: GetIterator on the operand, once, before any resumption
    // is forwarded. `iter.open` is bronze's GetIterator — including the cursor
    // kinds, which is what lets `yield* [1, 2]` work in a runtime that has no
    // `Array.prototype[Symbol.iterator]` to call.
    auto operand = lowerExpr(*yield.argument, ilFn);
    if (!operand) return std::nullopt;
    Value source = boxValueIfNeeded(*operand, ilFn);

    il::ValueId recVal = ilFn.valueCount++;
    il::Instruction openInst;
    openInst.op = il::Op::IterOpen;
    openInst.type = il::Type::Dynamic;
    openInst.result = recVal;
    openInst.operands = {source.id};
    emitInst(ilFn, openInst);

    const uint32_t frameDepth = envDepthOf(gen.frameScope);
    emitEnvSet(frameDepth, gen.iterSlot, Value{recVal, il::Type::Dynamic}, ilFn);

    // The blocks, all made before anything is emitted into them, so that every
    // branch below can name its target. `createBlock` stamps each with the
    // handler in force here, which is what makes an exception out of the
    // delegation — the inner iterator's, or the TypeError 5.b.iii raises —
    // reach a `try` the `yield*` was written inside.
    const il::BlockId bHead = createBlock(ilFn);
    const il::BlockId bReturnPath = createBlock(ilFn);
    const il::BlockId bDoneTest = createBlock(ilFn);
    const il::BlockId bSuspend = createBlock(ilFn);
    const il::BlockId bResume = createBlock(ilFn);
    const il::BlockId bDone = createBlock(ilFn);
    const il::BlockId bFinishReturn = createBlock(ilFn);
    const il::BlockId bPassThrough = createBlock(ilFn);
    const il::BlockId bAfter = createBlock(ilFn);

    const il::ValueId modeParam = ilFn.valueCount++;
    const il::ValueId recvParam = ilFn.valueCount++;
    ilFn.blocks[bHead].params.push_back({modeParam, il::Type::Dynamic});
    ilFn.blocks[bHead].params.push_back({recvParam, il::Type::Dynamic});

    // 27.5.3.7 step 4: the loop is entered with a NORMAL completion carrying
    // undefined, which is what makes the first thing a delegation does a call
    // to the inner `next` with no useful argument.
    Value firstMode = boxValueIfNeeded(emitConstF64(GeneratorContext::kModeNext, ilFn), ilFn);
    const il::ValueId firstRecv = emitConstUndefined(ilFn);
    il::Instruction enter;
    enter.op = il::Op::Jump;
    enter.type = il::Type::Void;
    enter.result = il::kNoValue;
    enter.target = il::BlockTarget{.block = bHead, .args = {firstMode.id, firstRecv}};
    emitInst(ilFn, enter);

    // --- bHead: forward this resumption ------------------------------------
    setCurrentBlock(bHead);
    // Read through the resume function's `__env` parameter rather than through
    // the scope chain: this block is a join, and one of its predecessors is the
    // resume block, where no scope has been entered.
    Value record = emitFrameSlotGet(gen.iterSlot, ilFn);

    il::ValueId result = ilFn.valueCount++;
    il::Instruction step;
    step.op = il::Op::IterDelegate;
    step.type = il::Type::Dynamic;
    step.result = result;
    step.operands = {record.id, modeParam, recvParam};
    emitInst(ilFn, step);

    il::ValueId mode = ilFn.valueCount++;
    il::Instruction unbox;
    unbox.op = il::Op::Unbox;
    unbox.type = il::Type::F64;
    unbox.result = mode;
    unbox.operands = {modeParam};
    emitInst(ilFn, unbox);

    il::ValueId isReturn = ilFn.valueCount++;
    il::Instruction cmpReturn;
    cmpReturn.op = il::Op::CmpEq;
    cmpReturn.type = il::Type::Bool;
    cmpReturn.result = isReturn;
    cmpReturn.operands = {mode, emitConstF64(GeneratorContext::kModeReturn, ilFn).id};
    emitInst(ilFn, cmpReturn);

    il::Instruction branchReturn;
    branchReturn.op = il::Op::Branch;
    branchReturn.type = il::Type::Void;
    branchReturn.result = il::kNoValue;
    branchReturn.operands = {isReturn};
    branchReturn.target = il::BlockTarget{.block = bReturnPath, .args = {}};
    branchReturn.elseTarget = il::BlockTarget{.block = bDoneTest, .args = {}};
    emitInst(ilFn, branchReturn);

    // --- bReturnPath: did the inner iterator have a `return` at all? --------
    setCurrentBlock(bReturnPath);
    il::ValueId noReturnMethod = ilFn.valueCount++;
    il::Instruction nullish;
    nullish.op = il::Op::IsNullish;
    nullish.type = il::Type::Bool;
    nullish.result = noReturnMethod;
    nullish.operands = {result};
    emitInst(ilFn, nullish);

    il::Instruction branchPass;
    branchPass.op = il::Op::Branch;
    branchPass.type = il::Type::Void;
    branchPass.result = il::kNoValue;
    branchPass.operands = {noReturnMethod};
    branchPass.target = il::BlockTarget{.block = bPassThrough, .args = {}};
    branchPass.elseTarget = il::BlockTarget{.block = bDoneTest, .args = {}};
    emitInst(ilFn, branchPass);

    // --- bDoneTest: 7.4.4 IteratorComplete ---------------------------------
    setCurrentBlock(bDoneTest);
    il::ValueId doneProp = ilFn.valueCount++;
    il::Instruction readDone;
    readDone.op = il::Op::PropGet;
    readDone.type = il::Type::Dynamic;
    readDone.result = doneProp;
    readDone.operands = {result};
    readDone.keyIndex = getKeyConstantIndex("done");
    readDone.icIndex = icSiteCounter_++;
    emitInst(ilFn, readDone);
    Value done = lowerConditionFromVal(Value{doneProp, il::Type::Dynamic}, ilFn);

    il::Instruction branchDone;
    branchDone.op = il::Op::Branch;
    branchDone.type = il::Type::Void;
    branchDone.result = il::kNoValue;
    branchDone.operands = {done.id};
    branchDone.target = il::BlockTarget{.block = bDone, .args = {}};
    branchDone.elseTarget = il::BlockTarget{.block = bSuspend, .args = {}};
    emitInst(ilFn, branchDone);

    // --- bSuspend: yield the inner result ONWARD ---------------------------
    setCurrentBlock(bSuspend);
    const auto index = static_cast<double>(gen.resumeBlocks.size());
    gen.resumeBlocks.push_back(bResume);
    emitEnvSet(frameDepth, gen.stateSlot, emitConstF64(index, ilFn), ilFn);
    // The inner iterator's result object, by IDENTITY: 27.5.3.7 hands
    // `innerResult` to GeneratorYield, and 27.5.3.8 returns exactly the object
    // it was given. Building a fresh `{ value, done }` here would make
    // `outer.next()` and `inner.next()` disagree about object identity for a
    // value the delegation only passed along. `value` is deliberately not read:
    // 7.4.5 IteratorValue is step 5.a.v, reached only when `done` was true.
    il::Instruction ret;
    ret.op = il::Op::Ret;
    ret.type = il::Type::Dynamic;
    ret.result = il::kNoValue;
    ret.operands = {result};
    emitInst(ilFn, ret);

    // --- bResume: back to the top of the same loop -------------------------
    setCurrentBlock(bResume);
    il::Instruction again;
    again.op = il::Op::Jump;
    again.type = il::Type::Void;
    again.result = il::kNoValue;
    again.target = il::BlockTarget{.block = bHead, .args = {gen.modeParam, gen.sentParam}};
    emitInst(ilFn, again);

    // --- bDone: 7.4.5 IteratorValue, then which ending this is -------------
    setCurrentBlock(bDone);
    il::ValueId valueProp = ilFn.valueCount++;
    il::Instruction readValue;
    readValue.op = il::Op::PropGet;
    readValue.type = il::Type::Dynamic;
    readValue.result = valueProp;
    readValue.operands = {result};
    readValue.keyIndex = getKeyConstantIndex("value");
    readValue.icIndex = icSiteCounter_++;
    emitInst(ilFn, readValue);

    il::Instruction branchEnding;
    branchEnding.op = il::Op::Branch;
    branchEnding.type = il::Type::Void;
    branchEnding.result = il::kNoValue;
    branchEnding.operands = {isReturn};
    branchEnding.target = il::BlockTarget{.block = bFinishReturn, .args = {}};
    branchEnding.elseTarget = il::BlockTarget{.block = bAfter, .args = {}};
    emitInst(ilFn, branchEnding);

    // 5.c.viii: a `return` resumption whose inner `return` reported done ends
    // the OUTER walk too, carrying the inner result's value — not the value the
    // caller passed to `gen.return(v)`.
    setCurrentBlock(bFinishReturn);
    if (!runCleanups(0, ilFn)) return std::nullopt;
    if (!currentBlockIsTerminated(ilFn)) {
        emitGeneratorFinish(Value{valueProp, il::Type::Dynamic}, ilFn);
    }

    // 5.c.iii: no `return` method on the inner iterator, so the delegation has
    // nothing to say about the return completion and it passes straight
    // through, still carrying the value `gen.return(v)` was given.
    setCurrentBlock(bPassThrough);
    if (!runCleanups(0, ilFn)) return std::nullopt;
    if (!currentBlockIsTerminated(ilFn)) {
        emitGeneratorFinish(Value{recvParam, il::Type::Dynamic}, ilFn);
    }

    // 5.a.v: the delegation finished normally, and the inner result's `value`
    // is the value of the `yield*` expression.
    setCurrentBlock(bAfter);
    return Value{valueProp, il::Type::Dynamic};
}

}  // namespace bronze::lower
