// `await`: a suspension of the coroutine body (lower_generator.cpp) that hands
// the awaited value to the runtime driver instead of to a caller.
//
// The driver (runtime/coro.cpp) subscribes the resumption — PromiseResolve,
// then PerformPromiseThen (27.7.5.3 Await) — and resumes the body with mode
// `next` and the fulfillment value, or mode `throw` and the rejection reason.
// The throw arm is emitted HERE, inside whatever protected region the await
// was written in, which is the entire implementation of
// `try { await p } catch (e) { ... }`. Nothing resumes an await with `return`.

#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

std::optional<Lowerer::Value> Lowerer::lowerAwait(const ast::YieldExpr& await,
                                                  il::Function& ilFn) {
    auto operand = lowerExpr(*await.argument, ilFn);
    if (!operand) return std::nullopt;
    Value awaited = boxValueIfNeeded(*operand, ilFn);
    return lowerAwaitValue(awaited, await.span, ilFn);
}

std::optional<Lowerer::Value> Lowerer::lowerAwaitValue(Value awaited, Span span,
                                                       il::Function& ilFn) {
    if (!coro_ || !coro_->isAsync()) {
        diags_.error(span, "internal: an `await` outside an async function body");
        return std::nullopt;
    }
    const Value sent = emitCoroSuspend(BRONZE_ABI_SUSPEND_AWAIT, awaited, ilFn);
    const Value mode = emitCoroMode(ilFn);
    const il::BlockId bThrow = createBlock(ilFn);
    const il::BlockId bNormal = createBlock(ilFn);
    emitInst(ilFn, branchOn(emitModeIs(mode, BRONZE_ABI_RESUME_THROW, ilFn), bThrow, bNormal));

    // Rejection: raised at the await point, so it takes this block's handler
    // like any other throw written there.
    setCurrentBlock(bThrow);
    il::Instruction throwInst;
    throwInst.op = il::Op::Throw;
    throwInst.type = il::Type::Void;
    throwInst.result = il::kNoValue;
    throwInst.operands = {sent.id};
    emitInst(ilFn, throwInst);

    setCurrentBlock(bNormal);
    // The value of the `await` is what the awaited promise fulfilled with.
    return sent;
}

}  // namespace bronze::lower
