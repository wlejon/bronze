// Coroutine bodies: generators, async functions, async generators, and a top
// level that awaits.
//
// Such a function lowers as ONE body, statements in order, exactly as an
// ordinary function does, with every suspension an instruction in the middle
// of it:
//
//     %sent = coro.suspend yield %v      ; hand %v out, wait
//     %mode = coro.mode                  ; how the resumer asked to go on
//     br %mode == next -> continue, throw -> throw %sent, return -> return %sent
//
// Nothing here saves or restores anything across a suspension. The backend
// runs the body on a brass coroutine frame, and brass's coroutine lowering
// spills whatever SSA value is live across a suspend into that frame — so
// bindings stay in SSA, a `finally` pending across a `yield` is an ordinary
// value, and `throw()`/`return()` at a suspension are a throw and a return
// written at that point, reaching the enclosing `catch` and `finally` blocks
// through the same lowering every other throw and return takes.
//
// Once lowered, the function is split (splitCoroutineBody): its blocks move
// into a new function, the BODY, and the function its callers reach becomes a
// stub that starts the body on a fresh frame and returns what the runtime
// answers — the generator object, or the promise (runtime/coro.cpp).
//
// The runtime owns the protocol state (27.5.1.1's [[GeneratorState]], the
// async generator's request queue, promise settlement): which resumption to
// send, and what a completed or unstarted generator answers without entering
// the body at all.

#include <string>
#include <utility>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

Lowerer::Value Lowerer::emitCoroSuspend(uint32_t kind, Value value, il::Function& ilFn) {
    Value boxed = boxValueIfNeeded(value, ilFn);
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::CoroSuspend;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.immI32 = static_cast<int32_t>(kind);
    inst.operands = {boxed.id};
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

Lowerer::Value Lowerer::emitCoroMode(il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::CoroMode;
    inst.type = il::Type::I32;
    inst.result = res;
    emitInst(ilFn, inst);
    return Value{res, il::Type::I32};
}

il::ValueId Lowerer::emitModeIs(Value mode, uint32_t want, il::Function& ilFn) {
    il::ValueId k = ilFn.valueCount++;
    il::Instruction c;
    c.op = il::Op::ConstI32;
    c.type = il::Type::I32;
    c.result = k;
    c.immI32 = static_cast<int32_t>(want);
    emitInst(ilFn, c);
    il::ValueId hit = ilFn.valueCount++;
    il::Instruction cmp;
    cmp.op = il::Op::CmpEq;
    cmp.type = il::Type::Bool;
    cmp.result = hit;
    cmp.operands = {mode.id, k};
    emitInst(ilFn, cmp);
    return hit;
}

il::Instruction Lowerer::branchOn(il::ValueId cond, il::BlockId ifTrue, il::BlockId ifFalse) {
    il::Instruction branch;
    branch.op = il::Op::Branch;
    branch.type = il::Type::Void;
    branch.result = il::kNoValue;
    branch.operands = {cond};
    branch.target = il::BlockTarget{.block = ifTrue, .args = {}};
    branch.elseTarget = il::BlockTarget{.block = ifFalse, .args = {}};
    return branch;
}

// The three ways a resumption continues after a `yield` (27.5.3.2 for `next`,
// 27.5.3.3 for the two abrupt ones), branched on HERE, inside whatever
// protected region the yield was written in: that placement is what makes
// `gen.throw(e)` reach the `catch` around the yield and `gen.return(v)` run
// the `finally`. Leaves lowering in the `next` continuation.
bool Lowerer::emitResumeDispatch(Value sent, il::Function& ilFn) {
    const Value mode = emitCoroMode(ilFn);
    const il::BlockId bNormal = createBlock(ilFn);
    const il::BlockId bAbrupt = createBlock(ilFn);
    emitInst(ilFn, branchOn(emitModeIs(mode, BRONZE_ABI_RESUME_NEXT, ilFn), bNormal, bAbrupt));

    setCurrentBlock(bAbrupt);
    const il::BlockId bThrow = createBlock(ilFn);
    const il::BlockId bReturn = createBlock(ilFn);
    emitInst(ilFn, branchOn(emitModeIs(mode, BRONZE_ABI_RESUME_THROW, ilFn), bThrow, bReturn));

    // `gen.throw(e)`: the exception is raised AT the suspension point, so it
    // takes this block's handler like any other throw written there.
    setCurrentBlock(bThrow);
    il::Instruction throwInst;
    throwInst.op = il::Op::Throw;
    throwInst.type = il::Type::Void;
    throwInst.result = il::kNoValue;
    throwInst.operands = {sent.id};
    emitInst(ilFn, throwInst);

    // `gen.return(v)`: a return completion at the suspension point, which is
    // exactly what `return v;` written here would do.
    setCurrentBlock(bReturn);
    if (!emitCoroutineReturn(sent, ilFn)) return false;

    setCurrentBlock(bNormal);
    return true;
}

// A return completion of the body carrying `value`: every enclosing cleanup
// runs, then the body returns. In an async generator the value is awaited
// first (27.6.3.8 AsyncGeneratorYield for a `return` resumption, and 15.6.2's
// `return` statement), inside the protected region, so a rejection is thrown
// at the return point.
bool Lowerer::emitCoroutineReturn(Value value, il::Function& ilFn) {
    Value result = boxValueIfNeeded(value, ilFn);
    if (coro_->isAsyncGenerator()) {
        auto awaited = lowerAwaitValue(result, currentStmtSpan_, ilFn);
        if (!awaited) return false;
        result = *awaited;
    }
    if (!runCleanups(0, ilFn)) return false;
    if (currentBlockIsTerminated(ilFn)) return true;
    il::Instruction ret;
    ret.op = il::Op::Ret;
    ret.type = il::Type::Dynamic;
    ret.result = il::kNoValue;
    ret.operands = {result.id};
    emitInst(ilFn, ret);
    return true;
}

// `return;` and `return <expr>;` in a coroutine body. 14.15.3's rule about
// `finally` is unchanged — the expression is evaluated, then every enclosing
// cleanup runs, then the body completes.
bool Lowerer::lowerCoroutineReturn(const ast::ReturnStmt* retStmt, il::Function& ilFn) {
    Value value{il::kNoValue, il::Type::Void};
    if (retStmt->value) {
        auto lowered = lowerExpr(*retStmt->value, ilFn);
        if (!lowered) return false;
        value = *lowered;
    } else {
        value = Value{emitConstUndefined(ilFn), il::Type::Dynamic};
    }
    return emitCoroutineReturn(value, ilFn);
}

std::optional<Lowerer::Value> Lowerer::lowerYield(const ast::YieldExpr& yield,
                                                  il::Function& ilFn) {
    if (!coro_ || !coro_->isGenerator()) {
        // Unreachable through the parser, which only makes a `YieldExpr` inside
        // a generator body. Named rather than assumed, because the alternative
        // is a null dereference on a tree some later pass built.
        diags_.error(yield.span, "internal: a `yield` outside a generator body");
        return std::nullopt;
    }
    auto operand = lowerExpr(*yield.argument, ilFn);
    if (!operand) return std::nullopt;
    Value yielded = boxValueIfNeeded(*operand, ilFn);
    // 15.5.5: an async generator yields the AWAITED value.
    if (coro_->isAsyncGenerator()) {
        auto awaited = lowerAwaitValue(yielded, yield.span, ilFn);
        if (!awaited) return std::nullopt;
        yielded = *awaited;
    }
    const Value sent = emitCoroSuspend(BRONZE_ABI_SUSPEND_YIELD, yielded, ilFn);
    if (!emitResumeDispatch(sent, ilFn)) return std::nullopt;
    // The value of the `yield` is the argument of the `next(v)` that resumed
    // it (27.5.3.2 step 5).
    return sent;
}

// The function as lowered becomes the BODY, a new function that runs on a
// coroutine frame; `ilFn` itself — the index every caller and closure already
// names — becomes the stub
//
//     %r = coro.start <kind> @<name>.body(<every parameter>)
//     ret %r
//
// The body takes the parameters the function was called with, hidden ones
// included, so everything the prologue reads is where it was.
void Lowerer::splitCoroutineBody(il::Function& ilFn, int32_t kind) {
    il::Function body = ilFn;
    body.name = ilFn.name + ".body";
    body.coroKind = static_cast<int8_t>(kind);
    body.returnType = il::Type::Dynamic;
    body.isEntryPoint = false;
    body.isExported = false;
    body.returnPinned = false;
    const auto bodyIndex = static_cast<uint32_t>(ilModule_.functions.size());

    ilFn.blocks.clear();
    ilFn.blocks.push_back(il::Block{.id = 0});
    ilFn.valueCount = static_cast<uint32_t>(ilFn.params.size());
    ilFn.returnType = il::Type::Dynamic;
    currentBlockIdx_ = 0;

    il::Instruction start;
    start.op = il::Op::CoroStart;
    start.type = il::Type::Dynamic;
    start.result = ilFn.valueCount++;
    start.calleeIndex = bodyIndex;
    start.immI32 = kind;
    for (uint32_t i = 0; i < ilFn.params.size(); ++i) start.operands.push_back(i);
    ilFn.blocks[0].instructions.push_back(start);

    il::Instruction ret;
    ret.op = il::Op::Ret;
    ret.type = il::Type::Dynamic;
    ret.result = il::kNoValue;
    ret.operands = {start.result};
    ilFn.blocks[0].instructions.push_back(ret);

    ilModule_.functions.push_back(std::move(body));
}

}  // namespace bronze::lower
