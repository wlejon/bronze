// The generator state machine: a FRAME and a RESUME FUNCTION.
//
// `function* g(a) { ... }` becomes two IL functions. `g` itself never runs the
// body — 15.5.3 says calling a generator function evaluates none of it. It
// creates one environment record holding everything the body will need (its
// parameters, its receiver, every binding it declares, and the machine's own
// slots), closes the resume function over that record, and returns a
// generator object carrying the closure as its [[GeneratorContext]].
//
// `g.resume(__env, __mode, __sent)` is the body. Its entry block dispatches on
// a resume index held in the frame:
//
//     b0:  n = unbox.f64 env.get %__env, 0, <state>
//          br n == 0 -> bStart, n == 1 -> bResume1, ... else -> done
//
// and every `yield` compiles to "write the next index, return
// { value, done: false }" followed by a fresh block that the dispatch above will
// point at. That block asks what KIND of resumption this is: an ordinary
// `next(v)` continues with `__sent` as the yield's value, a `throw(e)` raises
// `__sent` from exactly this point (so an enclosing `catch` catches it), and a
// `return(v)` runs every enclosing `finally` and finishes the walk.
//
// Why a resume block can take no arguments — and why the frame has to hold
// everything — is the rule `il.h` already states for handler blocks: an edge
// from the entry block defines nothing, so nothing that lives in SSA can be
// read on the other side of it. Three things make every read satisfy that:
// `ast::getGeneratorFrameNames` puts every BINDING in the frame,
// `ast::liftYields` gives every intermediate a name so that it is one, and
// `Lowerer::currentEnv` re-derives the record innermost at each point by
// walking DOWN from the frame instead of carrying it in a value.
//
// `yield*` is the same machine with a loop at one suspension point, and it is
// enough of a protocol to live next door: lower_yield_star.cpp.
//
// The runtime owns [[GeneratorState]] (27.5.1.1) and therefore owns the two
// rules this file does not implement: resuming a generator that is already
// executing is a TypeError, and a completed one answers
// `{ value: undefined, done: true }` for ever without re-entering the body.

#include <string>
#include <utility>
#include <vector>

#include "ast/queries.h"
#include "lower/lowerer.h"

namespace bronze::lower {

namespace {
// The frame slots the machine owns. Dotted, for the reason every other
// synthesized name here is: a source identifier cannot contain a dot, so none
// of these can be shadowed by — or confused with — a binding a program wrote.
constexpr const char* kStateSlot = "gen.state";
constexpr const char* kEnvSlot = "gen.env";
// The third, present only in a body that delegates: see GeneratorContext's
// `iterSlot` for why one slot serves every `yield*` in a body.
constexpr const char* kIterSlot = "gen.iter";

// The resume index that means "the walk is over". Never dispatched to: the
// runtime latches [[GeneratorState]] the moment a result says `done`, so the
// body is not re-entered. Written anyway, so that the machine is consistent
// read on its own — the dispatch's final `else` answers `done` for it.
constexpr double kCompletedState = -1.0;
}  // namespace

const char* Lowerer::generatorStateSlotName() { return kStateSlot; }
const char* Lowerer::generatorEnvSlotName() { return kEnvSlot; }
const char* Lowerer::generatorIterSlotName() { return kIterSlot; }

Lowerer::Value Lowerer::emitConstF64(double value, il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::ConstF64;
    inst.type = il::Type::F64;
    inst.result = res;
    inst.immF64 = value;
    emitInst(ilFn, inst);
    return Value{res, il::Type::F64};
}

// `{ value: <v>, done: <d> }` — the IteratorResult 7.4.1 requires of every
// `next`, in that key order, because own keys print in creation order and
// `Object.keys` of one is pinned.
Lowerer::Value Lowerer::emitIterResult(Value value, bool done, il::Function& ilFn) {
    il::ValueId obj = ilFn.valueCount++;
    il::Instruction create;
    create.op = il::Op::CreateObject;
    create.type = il::Type::Dynamic;
    create.result = obj;
    emitInst(ilFn, create);

    Value boxed = boxValueIfNeeded(value, ilFn);
    il::Instruction setValue;
    setValue.op = il::Op::PropSet;
    setValue.type = il::Type::Void;
    setValue.result = il::kNoValue;
    setValue.operands = {obj, boxed.id};
    setValue.keyIndex = getKeyConstantIndex("value");
    setValue.icIndex = icSiteCounter_++;
    emitInst(ilFn, setValue);

    il::ValueId doneVal = ilFn.valueCount++;
    il::Instruction doneConst;
    doneConst.op = il::Op::ConstBool;
    doneConst.type = il::Type::Bool;
    doneConst.result = doneVal;
    doneConst.immI32 = done ? 1 : 0;
    emitInst(ilFn, doneConst);
    Value doneBoxed = boxValueIfNeeded(Value{doneVal, il::Type::Bool}, ilFn);

    il::Instruction setDone;
    setDone.op = il::Op::PropSet;
    setDone.type = il::Type::Void;
    setDone.result = il::kNoValue;
    setDone.operands = {obj, doneBoxed.id};
    setDone.keyIndex = getKeyConstantIndex("done");
    setDone.icIndex = icSiteCounter_++;
    emitInst(ilFn, setDone);

    return Value{obj, il::Type::Dynamic};
}

// One of the frame's own slots, read against the resume function's `__env`
// parameter. That parameter is the one environment value valid in EVERY block
// of the function, which is what the dispatch — the block no scope has been
// entered in yet — needs.
Lowerer::Value Lowerer::emitFrameSlotGet(uint32_t slot, il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::EnvGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {generator_->frameEnv};
    inst.envDepth = 0;
    inst.envIndex = slot;
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

void Lowerer::emitGeneratorResult(Value value, bool done, il::Function& ilFn) {
    Value result = emitIterResult(value, done, ilFn);
    il::Instruction ret;
    ret.op = il::Op::Ret;
    ret.type = il::Type::Dynamic;
    ret.result = il::kNoValue;
    ret.operands = {result.id};
    emitInst(ilFn, ret);
}

// The end of the walk, from wherever it is reached: a `return` in the body, the
// end of the body, `gen.return(v)` at a suspension, and the dispatch's
// unreachable final arm. The state index is parked at "completed" first so that
// the machine reads consistently on its own.
void Lowerer::emitGeneratorFinish(Value value, il::Function& ilFn) {
    emitEnvSet(envDepthOf(generator_->frameScope), generator_->stateSlot,
               emitConstF64(kCompletedState, ilFn), ilFn);
    emitGeneratorResult(value, /*done=*/true, ilFn);
}

// `return;` and `return <expr>;` inside a generator. 27.5.3.2: the value is the
// `value` of the final result rather than the function's return value, and
// 14.15.3's rule about `finally` is unchanged — the expression is evaluated,
// then every enclosing cleanup runs, then the walk ends.
bool Lowerer::lowerGeneratorReturn(const ast::ReturnStmt* retStmt, il::Function& ilFn) {
    Value value{il::kNoValue, il::Type::Void};
    if (retStmt->value) {
        auto lowered = lowerExpr(*retStmt->value, ilFn);
        if (!lowered) return false;
        value = boxValueIfNeeded(*lowered, ilFn);
    } else {
        value = Value{emitConstUndefined(ilFn), il::Type::Dynamic};
    }
    if (!runCleanups(0, ilFn)) return false;
    if (currentBlockIsTerminated(ilFn)) return true;
    emitGeneratorFinish(value, ilFn);
    return true;
}

std::optional<Lowerer::Value> Lowerer::lowerYield(const ast::YieldExpr& yield,
                                                  il::Function& ilFn) {
    if (!generator_) {
        // Unreachable through the parser, which only makes a `YieldExpr` inside
        // a generator body. Named rather than assumed, because the alternative
        // is a null dereference on a tree some later pass built.
        diags_.error(yield.span, "internal: a `yield` outside a generator body");
        return std::nullopt;
    }
    auto operand = lowerExpr(*yield.argument, ilFn);
    if (!operand) return std::nullopt;
    Value yielded = boxValueIfNeeded(*operand, ilFn);

    GeneratorContext& gen = *generator_;
    const uint32_t depth = envDepthOf(gen.frameScope);
    const auto index = static_cast<double>(gen.resumeBlocks.size());

    // Where to come back to. What to come back INTO needs no saving: every
    // record nested inside the frame is reachable from it through the child
    // links `enterScope` maintains, and `currentEnv` walks them afresh in
    // whatever block lowering is emitting into.
    emitEnvSet(depth, gen.stateSlot, emitConstF64(index, ilFn), ilFn);
    emitGeneratorResult(yielded, /*done=*/false, ilFn);

    const il::BlockId bResume = createBlock(ilFn);
    gen.resumeBlocks.push_back(bResume);
    setCurrentBlock(bResume);

    // Which of the three resumptions this is (27.5.3.2 for `next`, 27.5.3.3 for
    // the two abrupt ones). The test is emitted HERE, inside the protected
    // region the yield was written in, which is what makes `gen.throw(e)` reach
    // the `catch` around it and `gen.return(v)` run the `finally`.
    il::ValueId mode = ilFn.valueCount++;
    il::Instruction unbox;
    unbox.op = il::Op::Unbox;
    unbox.type = il::Type::F64;
    unbox.result = mode;
    unbox.operands = {gen.modeParam};
    emitInst(ilFn, unbox);

    il::BlockId bAbrupt = createBlock(ilFn);
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
    branch.target = il::BlockTarget{.block = bAbrupt, .args = {}};
    branch.elseTarget = il::BlockTarget{.block = bNormal, .args = {}};
    emitInst(ilFn, branch);

    setCurrentBlock(bAbrupt);
    il::BlockId bThrow = createBlock(ilFn);
    il::BlockId bReturn = createBlock(ilFn);
    il::ValueId isThrow = ilFn.valueCount++;
    il::Instruction cmpThrow;
    cmpThrow.op = il::Op::CmpEq;
    cmpThrow.type = il::Type::Bool;
    cmpThrow.result = isThrow;
    cmpThrow.operands = {mode, emitConstF64(GeneratorContext::kModeThrow, ilFn).id};
    emitInst(ilFn, cmpThrow);
    il::Instruction branchAbrupt;
    branchAbrupt.op = il::Op::Branch;
    branchAbrupt.type = il::Type::Void;
    branchAbrupt.result = il::kNoValue;
    branchAbrupt.operands = {isThrow};
    branchAbrupt.target = il::BlockTarget{.block = bThrow, .args = {}};
    branchAbrupt.elseTarget = il::BlockTarget{.block = bReturn, .args = {}};
    emitInst(ilFn, branchAbrupt);

    // `gen.throw(e)`: the exception is raised AT the suspension point, so it
    // takes this block's handler like any other throw written there.
    setCurrentBlock(bThrow);
    il::Instruction throwInst;
    throwInst.op = il::Op::Throw;
    throwInst.type = il::Type::Void;
    throwInst.result = il::kNoValue;
    throwInst.operands = {gen.sentParam};
    emitInst(ilFn, throwInst);

    // `gen.return(v)`: a return completion at the suspension point, which is
    // exactly what `return v;` written here would do.
    setCurrentBlock(bReturn);
    if (!runCleanups(0, ilFn)) return std::nullopt;
    if (!currentBlockIsTerminated(ilFn)) {
        emitGeneratorFinish(Value{gen.sentParam, il::Type::Dynamic}, ilFn);
    }

    setCurrentBlock(bNormal);
    // The value of the `yield` is the argument of the `next(v)` that resumed
    // it (27.5.3.2 step 5).
    return Value{gen.sentParam, il::Type::Dynamic};
}

// The entry block, built LAST: how many resume points there are is a fact about
// the whole body, and the blocks they name do not exist until it has been
// lowered. Block 0 is left empty until then, which costs nothing — an IL
// function's entry is its first block, not its first instruction.
void Lowerer::emitGeneratorDispatch(il::Function& ilFn) {
    GeneratorContext& gen = *generator_;
    setCurrentBlock(0);

    Value state = emitFrameSlotGet(gen.stateSlot, ilFn);
    il::ValueId index = ilFn.valueCount++;
    il::Instruction unbox;
    unbox.op = il::Op::Unbox;
    unbox.type = il::Type::F64;
    unbox.result = index;
    unbox.operands = {state.id};
    emitInst(ilFn, unbox);

    for (size_t i = 0; i < gen.resumeBlocks.size(); ++i) {
        il::BlockId bNext = createBlock(ilFn);
        il::ValueId hit = ilFn.valueCount++;
        il::Instruction cmp;
        cmp.op = il::Op::CmpEq;
        cmp.type = il::Type::Bool;
        cmp.result = hit;
        cmp.operands = {index, emitConstF64(static_cast<double>(i), ilFn).id};
        emitInst(ilFn, cmp);
        il::Instruction branch;
        branch.op = il::Op::Branch;
        branch.type = il::Type::Void;
        branch.result = il::kNoValue;
        branch.operands = {hit};
        branch.target = il::BlockTarget{.block = gen.resumeBlocks[i], .args = {}};
        branch.elseTarget = il::BlockTarget{.block = bNext, .args = {}};
        emitInst(ilFn, branch);
        setCurrentBlock(bNext);
    }
    // No index matched, which the runtime's [[GeneratorState]] latch makes
    // unreachable. Answering `done` rather than falling through keeps the
    // resume function total: every block ends in a terminator, and a machine
    // read on its own says what it does in every state.
    il::ValueId undef = emitConstUndefined(ilFn);
    emitGeneratorResult(Value{undef, il::Type::Dynamic}, /*done=*/true, ilFn);
}

// The resume function's body, lowered as a closure over the frame record: the
// same save-and-restore `lowerClosure` does, with one thing done differently.
// The frame scope is NOT re-created here — it belongs to the generator function
// that made the record — so its `envValue` is repointed at this function's
// `__env` parameter for the duration and put back afterwards.
bool Lowerer::lowerResumeBody(const std::vector<const ast::Stmt*>& stmts,
                              il::Function& resumeFn) {
    resumeFn.blocks.push_back(il::Block{.id = 0});

    const size_t outerBlockIdx = currentBlockIdx_;
    auto outerJumpStack = std::move(jumpStack_);
    auto outerLabelStack = std::move(labelStack_);
    auto outerCleanupStack = std::move(cleanupStack_);
    auto outerScopeHasEnv = std::move(scopeHasEnv_);
    jumpStack_.clear();
    labelStack_.clear();
    cleanupStack_.clear();
    scopeHasEnv_.clear();
    const il::BlockId outerHandler = currentHandler_;
    currentHandler_ = il::kNoBlock;
    const il::ValueId outerEnvValue = currentEnvValue_;
    const il::ValueId outerThisValue = currentThisValue_;
    const bool outerIsArrow = currentFunctionIsArrow_;
    const size_t outerEnvBase = functionEnvBase_;

    const size_t frameScope = functionEnvScope_;
    const il::ValueId outerFrameValue = envScopes_[frameScope].envValue;
    // Slot 0 of a `needsEnv` function is its environment parameter.
    const il::ValueId frameEnv = 0;
    envScopes_[frameScope].envValue = frameEnv;
    currentEnvValue_ = frameEnv;
    // The receiver comes out of the frame, exactly as an arrow's does: the
    // generator function copied `__this` into the record before returning, and
    // the body reads it from there on every resumption.
    currentThisValue_ = il::kNoValue;
    currentFunctionIsArrow_ = true;
    functionEnvBase_ = frameScope;

    GeneratorContext context;
    context.frameScope = frameScope;
    context.frameEnv = frameEnv;
    context.modeParam = 1;
    context.sentParam = 2;
    context.stateSlot = envScopes_[frameScope].slotOf.at(kStateSlot);
    if (auto it = envScopes_[frameScope].slotOf.find(kIterSlot);
        it != envScopes_[frameScope].slotOf.end()) {
        context.iterSlot = it->second;
    }
    auto outerGenerator = std::move(generator_);
    generator_ = std::move(context);

    const il::BlockId bStart = createBlock(resumeFn);
    generator_->resumeBlocks.push_back(bStart);
    setCurrentBlock(bStart);

    // The dead-zone markers go in the block that runs ONCE, on the first
    // `next()`. 15.5.3 instantiates the frame when the generator function is
    // called rather than when it is first resumed, and nothing can tell: no
    // code runs in between and the record is unreachable until the object it
    // hangs off is handed out.
    openLexicalBindings(frameScope, ast::getLexicalDeclarations(stmts), resumeFn);

    bool ok = lowerStmtList(stmts, resumeFn);
    if (ok && !currentBlockIsTerminated(resumeFn)) {
        // 15.5.3: falling off the end of a generator body is `return undefined`,
        // which is the final result's `value`.
        emitGeneratorFinish(Value{emitConstUndefined(resumeFn), il::Type::Dynamic}, resumeFn);
    }
    if (ok) emitGeneratorDispatch(resumeFn);

    generator_ = std::move(outerGenerator);
    envScopes_[frameScope].envValue = outerFrameValue;
    functionEnvBase_ = outerEnvBase;
    currentFunctionIsArrow_ = outerIsArrow;
    currentThisValue_ = outerThisValue;
    currentEnvValue_ = outerEnvValue;
    currentHandler_ = outerHandler;
    scopeHasEnv_ = std::move(outerScopeHasEnv);
    cleanupStack_ = std::move(outerCleanupStack);
    labelStack_ = std::move(outerLabelStack);
    jumpStack_ = std::move(outerJumpStack);
    currentBlockIdx_ = outerBlockIdx;
    return ok;
}

// The generator function's own body, which is three instructions: park the
// resume index at the start, close the resume function over the frame the
// prologue has already filled in, and hand back the object.
bool Lowerer::lowerGeneratorTail(const std::vector<const ast::Stmt*>& stmts,
                                 il::Function& ilFn) {
    if (functionEnvScope_ == SIZE_MAX) {
        diags_.error(Span{}, "internal: a generator with no frame record");
        return false;
    }
    const size_t frameScope = functionEnvScope_;
    const uint32_t depth = envDepthOf(frameScope);
    emitEnvSet(depth, envScopes_[frameScope].slotOf.at(kStateSlot), emitConstF64(0, ilFn), ilFn);
    const il::ValueId frameRecord = currentEnvValue_;

    il::Function resumeFn;
    resumeFn.name = ilFn.name + ".resume";
    resumeFn.returnType = il::Type::Dynamic;
    resumeFn.needsEnv = true;
    resumeFn.params.push_back({"__env", il::Type::Dynamic});
    // `__mode` and `__sent` are the two halves of a resumption: which of
    // `next`, `return` and `throw` asked, and the value it was asked with. They
    // are ordinary source-position parameters so that the uniform call
    // convention delivers them — the runtime reaches this function through
    // `bronze_dynamic_call` and nothing else.
    resumeFn.params.push_back({"__mode", il::Type::Dynamic});
    resumeFn.params.push_back({"__sent", il::Type::Dynamic});
    resumeFn.requiredArgs = 2;
    resumeFn.valueCount = static_cast<uint32_t>(resumeFn.params.size());

    if (!lowerResumeBody(stmts, resumeFn)) return false;

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

    il::ValueId genObj = ilFn.valueCount++;
    il::Instruction object;
    object.op = il::Op::CreateGeneratorObject;
    object.type = il::Type::Dynamic;
    object.result = genObj;
    object.operands = {closure};
    emitInst(ilFn, object);

    il::Instruction ret;
    ret.op = il::Op::Ret;
    ret.type = il::Type::Dynamic;
    ret.result = il::kNoValue;
    ret.operands = {genObj};
    emitInst(ilFn, ret);
    return true;
}

}  // namespace bronze::lower
