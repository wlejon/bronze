// The async function: the GENERATOR MACHINE driven by the promise runtime.
//
// `async function f(a) { ... }` lowers exactly as `function* f(a) { ... }`
// does — one frame record, one resume function, resume blocks dispatched on a
// state index — with two substitutions and nothing else:
//
//   - the tail hands the resume closure to `create.async_machine` /
//     `async.start` (runtime/builtin_async.cpp) instead of
//     `create.generator_object`, and returns the PROMISE the driver made;
//   - a suspension point subscribes (`async.await machine, value`) before it
//     returns `{ value: undefined, done: false }` to the driver, instead of
//     yielding a value to a caller.
//
// The driver resumes the closure with the same (__mode, __sent) convention a
// generator object uses, which is what buys `try`/`catch`/`finally` across an
// await for free: a rejected awaited promise arrives as mode `throw`, the
// resume block re-raises it AT the await point, and the handler stamping and
// cleanup routing lower_try.cpp already does for yield does the rest. Only two
// of the three modes can arrive — nothing external can `return` into an async
// body (27.7.5.3 has no such step) — so the abrupt arm here is the throw arm.
//
// 27.7.5.1 step 9 is the tail's one semantic commitment: `async.start` runs
// the body synchronously up to the first await (or to completion), so code
// before the first await observes the caller's world unchanged.

#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

namespace {
// The frame slot holding the machine value. Dotted so no source binding can
// shadow it; distinct from the "gen.*" trio because a body is never both.
constexpr const char* kMachineSlot = "async.machine";
}  // namespace

const char* Lowerer::asyncMachineSlotName() { return kMachineSlot; }

std::optional<Lowerer::Value> Lowerer::lowerAwait(const ast::YieldExpr& await,
                                                  il::Function& ilFn) {
    if (!generator_ || !generator_->isAsync) {
        // Unreachable through the parser, which only builds an await-flagged
        // YieldExpr inside an async body. Named rather than assumed, for the
        // reason lowerYield names its twin.
        diags_.error(await.span, "internal: an `await` outside an async function body");
        return std::nullopt;
    }
    auto operand = lowerExpr(*await.argument, ilFn);
    if (!operand) return std::nullopt;
    Value awaited = boxValueIfNeeded(*operand, ilFn);

    GeneratorContext& gen = *generator_;
    const uint32_t depth = envDepthOf(gen.frameScope);
    const auto index = static_cast<double>(gen.resumeBlocks.size());

    // Order matters twice here. The state index is written BEFORE the
    // subscription because `async.await` of an already-settled promise
    // enqueues the resumption job immediately — the machine must already say
    // where to come back to. And the subscription happens BEFORE this
    // function returns its `done: false` because the driver does nothing
    // with a suspended result except trust that the await has subscribed.
    emitEnvSet(depth, gen.stateSlot, emitConstF64(index, ilFn), ilFn);

    Value machine = emitFrameSlotGet(gen.machineSlot, ilFn);
    il::Instruction subscribe;
    subscribe.op = il::Op::AsyncAwait;
    subscribe.type = il::Type::Void;
    subscribe.result = il::kNoValue;
    subscribe.operands = {machine.id, awaited.id};
    emitInst(ilFn, subscribe);

    emitGeneratorResult(Value{emitConstUndefined(ilFn), il::Type::Dynamic},
                        /*done=*/false, ilFn);

    const il::BlockId bResume = createBlock(ilFn);
    gen.resumeBlocks.push_back(bResume);
    setCurrentBlock(bResume);

    // Which of the two resumptions this is: fulfillment (mode `next`, the
    // value in `__sent`) or rejection (mode `throw`, the reason in `__sent`).
    // The test is emitted HERE, inside whatever protected region the await
    // was written in — that placement is the entire implementation of
    // `try { await p } catch (e) { ... }`.
    il::ValueId mode = ilFn.valueCount++;
    il::Instruction unbox;
    unbox.op = il::Op::Unbox;
    unbox.type = il::Type::F64;
    unbox.result = mode;
    unbox.operands = {gen.modeParam};
    emitInst(ilFn, unbox);

    il::BlockId bThrow = createBlock(ilFn);
    il::BlockId bNormal = createBlock(ilFn);
    il::ValueId isAbrupt = ilFn.valueCount++;
    il::Instruction cmpAbrupt;
    cmpAbrupt.op = il::Op::CmpGt;
    cmpAbrupt.type = il::Type::Bool;
    cmpAbrupt.result = isAbrupt;
    cmpAbrupt.operands = {mode, emitConstF64(0, ilFn).id};
    emitInst(ilFn, cmpAbrupt);
    il::Instruction branch;
    branch.op = il::Op::Branch;
    branch.type = il::Type::Void;
    branch.result = il::kNoValue;
    branch.operands = {isAbrupt};
    branch.target = il::BlockTarget{.block = bThrow, .args = {}};
    branch.elseTarget = il::BlockTarget{.block = bNormal, .args = {}};
    emitInst(ilFn, branch);

    // Rejection: raised at the await point, so it takes this block's handler
    // like any other throw written there.
    setCurrentBlock(bThrow);
    il::Instruction throwInst;
    throwInst.op = il::Op::Throw;
    throwInst.type = il::Type::Void;
    throwInst.result = il::kNoValue;
    throwInst.operands = {gen.sentParam};
    emitInst(ilFn, throwInst);

    setCurrentBlock(bNormal);
    // The value of the `await` is what the awaited promise fulfilled with.
    return Value{gen.sentParam, il::Type::Dynamic};
}

// The async function's own body: park the state, close the resume function
// over the frame, hand the closure to the driver, write the machine where the
// await sites will read it, start the machine, return its promise. The shape
// is lowerGeneratorTail's with the machine plumbing added — kept separate
// because the one shared middle (building the resume function) already lives
// in lowerResumeBody, and interleaving the two tails' differences would cost
// more than the dozen lines they share.
bool Lowerer::lowerAsyncTail(const std::vector<const ast::Stmt*>& stmts, il::Function& ilFn) {
    if (functionEnvScope_ == SIZE_MAX) {
        diags_.error(Span{}, "internal: an async function with no frame record");
        return false;
    }
    const size_t frameScope = functionEnvScope_;
    const uint32_t depth = envDepthOf(frameScope);
    emitEnvSet(depth, envScopes_[frameScope].slotOf.at(generatorStateSlotName()),
               emitConstF64(0, ilFn), ilFn);
    const il::ValueId frameRecord = currentEnvValue_;

    il::Function resumeFn;
    resumeFn.name = ilFn.name + ".resume";
    resumeFn.returnType = il::Type::Dynamic;
    resumeFn.needsEnv = true;
    resumeFn.params.push_back({"__env", il::Type::Dynamic});
    // The same (__mode, __sent) pair a generator's resume function takes,
    // delivered the same way (bronze_dynamic_call from the runtime driver).
    // Keeping the convention identical is deliberate: one calling protocol,
    // pinned once, for every machine body.
    resumeFn.params.push_back({"__mode", il::Type::Dynamic});
    resumeFn.params.push_back({"__sent", il::Type::Dynamic});
    resumeFn.requiredArgs = 2;
    resumeFn.valueCount = static_cast<uint32_t>(resumeFn.params.size());

    if (!lowerResumeBody(stmts, resumeFn, /*isAsync=*/true)) return false;

    const auto resumeIndex = static_cast<uint32_t>(ilModule_.functions.size());
    ilModule_.functions.push_back(std::move(resumeFn));

    il::ValueId closure = ilFn.valueCount++;
    il::Instruction create;
    create.op = il::Op::CreateFunction;
    create.type = il::Type::Dynamic;
    create.result = closure;
    create.calleeIndex = resumeIndex;
    create.immI32 = 2;
    create.operands = {frameRecord};
    emitInst(ilFn, create);

    il::ValueId machine = ilFn.valueCount++;
    il::Instruction makeMachine;
    makeMachine.op = il::Op::CreateAsyncMachine;
    makeMachine.type = il::Type::Dynamic;
    makeMachine.result = machine;
    makeMachine.operands = {closure};
    emitInst(ilFn, makeMachine);

    // Into the frame BEFORE the machine starts: `async.start` runs the body
    // synchronously to the first await, and that first await reads the
    // machine out of this slot.
    emitEnvSet(depth, envScopes_[frameScope].slotOf.at(kMachineSlot),
               Value{machine, il::Type::Dynamic}, ilFn);

    il::ValueId promise = ilFn.valueCount++;
    il::Instruction start;
    start.op = il::Op::AsyncStart;
    start.type = il::Type::Dynamic;
    start.result = promise;
    start.operands = {machine};
    emitInst(ilFn, start);

    il::Instruction ret;
    ret.op = il::Op::Ret;
    ret.type = il::Type::Dynamic;
    ret.result = il::kNoValue;
    ret.operands = {promise};
    emitInst(ilFn, ret);
    return true;
}

}  // namespace bronze::lower
