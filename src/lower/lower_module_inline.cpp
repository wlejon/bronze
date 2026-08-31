// Running the body of a certified module-literal method at the call site
// instead of calling it.
//
// `X.m(a, b)` on a literal that types/module_literal.h certified is a call
// whose receiver and whose callee are both compile-time facts. What the site
// still pays for is the FRAME: a body that is a guarded early return does a
// handful of compares and returns, and the call around it costs more than the
// body does. So the site evaluates that body itself.
//
// The substitution is exact rather than approximate, and each part of it is
// exact for its own reason:
//
//   * the RECEIVER is the value the site read for the binding, read where the
//     site always read it. So `this` inside the body is the object the call
//     would have passed, and the dead-zone check that read carries is the one
//     the body runs under: a site that executes while `X` is still
//     uninitialised throws its ReferenceError before any of this happens, from
//     the same instruction as before.
//   * the ARGUMENTS are the site's own, evaluated left to right, exactly once,
//     before anything of the body runs — and every one of them is evaluated,
//     including the ones past the last parameter, which the call would also
//     have evaluated and dropped. A parameter the site did not supply is bound
//     to `undefined`, which is what the callee's frame would have held.
//   * the BODY is lowered here without asking inference anything about the
//     expressions it contains. The receiver is a value and not an AST node the
//     caller's frame can be interrogated about, and every claim a normal
//     member access carries — a monomorphic shape, a static slot, a direct
//     method edge — is a claim about the receiver expression as WRITTEN, which
//     at this site is `this` in a function this site is not inside. One such
//     claim answered from the caller's `this` is a miscompile, so none is
//     asked: reads and calls in a moved body are the plain dynamic ones.
//
// The one member read that is not plain is the `this.<key>` read, which the
// proof already established is an own DATA property of one object. That is a
// monomorphic site by construction, and it says so.
//
// A guard chain that answers no runs the method after all. That call is the
// call the site makes today — same receiver, same key, same argument values —
// so the slow path costs one evaluation of the guard conditions more than it
// used to, and the proof that this is free is the reason a condition may only
// mention own data properties, parameters and operators that run no user code.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

bool Lowerer::tryLowerModuleLiteralInline(const ast::Call& call, const ast::MemberAccess& mem,
                                          bool onSpine, il::Function& ilFn,
                                          std::optional<Value>& out) {
    if (inference_ == nullptr) return false;
    // `X.m?.()`, `X?.m()` and `X.#m()` each answer a question about whether the
    // call happens at all, which is not a question about the body.
    if (call.optional || mem.optional || mem.isPrivate) return false;
    // A spread's argument count is a run-time fact, and a parameter list bound
    // positionally needs a compile-time one.
    if (listHasSpread(call.args)) return false;

    const auto* recvIdent = dynamic_cast<const ast::Ident*>(mem.object.get());
    if (recvIdent == nullptr) return false;
    const types::ModuleLiteralInline* method =
        inference_->moduleLiterals.inlinableMethod(recvIdent->name, mem.property);
    if (method == nullptr) return false;
    for (const auto& active : inlineStack_) {
        if (active.first == recvIdent->name && active.second == mem.property) return false;
    }

    // The site's own receiver read, in the position and with the checks it has
    // today. Everything after it is the body.
    auto recvVal = lowerChainBase(*mem.object, ilFn, onSpine);
    if (!recvVal) {
        out = std::nullopt;
        return true;
    }
    Value receiver = boxValueIfNeeded(*recvVal, ilFn);

    std::vector<Value> args;
    args.reserve(call.args.size());
    for (const auto& argPtr : call.args) {
        auto argVal = lowerExpr(*argPtr, ilFn);
        if (!argVal) {
            out = std::nullopt;
            return true;
        }
        args.push_back(boxValueIfNeeded(*argVal, ilFn));
    }

    recordCall(call.span.file, true, "");
    out = emitInlinedMethod(recvIdent->name, mem.property, *method, receiver, args, ilFn);
    return true;
}

std::optional<Lowerer::Value> Lowerer::emitInlinedMethod(const std::string& binding,
                                                         const std::string& key,
                                                         const types::ModuleLiteralInline& method,
                                                         Value receiver,
                                                         const std::vector<Value>& args,
                                                         il::Function& ilFn) {
    // Counted here rather than at the site, so a body reached through another
    // body's tail is counted too: both frames are gone, and the report says so.
    ++inlinedLiteralSites_[binding + "." + key];

    InlineFrame frame;
    frame.binding = &binding;
    frame.receiver = receiver;
    for (size_t i = 0; i < method.params.size(); ++i) {
        frame.params[method.params[i]] =
            i < args.size() ? args[i] : Value{emitConstUndefined(ilFn), il::Type::Dynamic};
    }

    inlineStack_.emplace_back(binding, key);
    auto result = emitInlineGuard(method, 0, key, frame, args, ilFn);
    inlineStack_.pop_back();
    return result;
}

std::optional<Lowerer::Value> Lowerer::emitInlineGuard(const types::ModuleLiteralInline& method,
                                                       size_t guard, const std::string& key,
                                                       const InlineFrame& frame,
                                                       const std::vector<Value>& args,
                                                       il::Function& ilFn) {
    if (guard < method.guards.size()) {
        const auto& g = method.guards[guard];
        auto condVal = emitInlineExpr(*g.condition, frame, ilFn);
        if (!condVal) return std::nullopt;
        Value cond = lowerConditionFromVal(*condVal, ilFn);
        return emitInlineSelect(
            cond,
            [&]() -> std::optional<Value> {
                if (g.result == nullptr) {
                    return Value{emitConstUndefined(ilFn), il::Type::Dynamic};
                }
                return emitInlineExpr(*g.result, frame, ilFn);
            },
            [&]() { return emitInlineGuard(method, guard + 1, key, frame, args, ilFn); }, ilFn);
    }

    if (method.tail != nullptr) return emitInlineExpr(*method.tail, frame, ilFn);
    // No guard answered and the rest of the body is not expressible here, so
    // the method runs — as the call the site would have made, on the receiver
    // and the argument values it already holds.
    if (method.tailIsCall) return emitInlineMethodCall(frame.receiver, key, args, ilFn);
    return Value{emitConstUndefined(ilFn), il::Type::Dynamic};
}

std::optional<Lowerer::Value> Lowerer::emitInlineExpr(const ast::Expr& expr,
                                                      const InlineFrame& frame,
                                                      il::Function& ilFn) {
    if (const auto* id = dynamic_cast<const ast::Ident*>(&expr)) {
        const auto it = frame.params.find(id->name);
        if (it == frame.params.end()) {
            diags_.error(expr.span, "internal: an inlined body reached a free identifier");
            return std::nullopt;
        }
        return it->second;
    }
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(&expr)) {
        return emitInlinePropGet(frame.receiver, mem->property, ilFn);
    }
    if (const auto* un = dynamic_cast<const ast::Unary*>(&expr)) {
        auto val = emitInlineExpr(*un->operand, frame, ilFn);
        if (!val) return std::nullopt;
        if (un->op == ast::UnaryOp::Not) {
            return emitLogicalNot(lowerConditionFromVal(*val, ilFn), ilFn);
        }
        Value boxed = boxValueIfNeeded(*val, ilFn);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::TypeOf;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = {boxed.id};
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }
    if (const auto* bin = dynamic_cast<const ast::Binary*>(&expr)) {
        if (bin->op == ast::BinaryOp::LogicalAnd || bin->op == ast::BinaryOp::LogicalOr) {
            // `a && b` and `a || b` yield an OPERAND, not a boolean, so the
            // arm that skips `b` yields the `a` this block already holds.
            auto lhs = emitInlineExpr(*bin->lhs, frame, ilFn);
            if (!lhs) return std::nullopt;
            Value cond = lowerConditionFromVal(*lhs, ilFn);
            auto keep = [&]() -> std::optional<Value> { return *lhs; };
            auto rhs = [&]() { return emitInlineExpr(*bin->rhs, frame, ilFn); };
            if (bin->op == ast::BinaryOp::LogicalAnd) {
                return emitInlineSelect(cond, rhs, keep, ilFn);
            }
            return emitInlineSelect(cond, keep, rhs, ilFn);
        }
        auto lhs = emitInlineExpr(*bin->lhs, frame, ilFn);
        if (!lhs) return std::nullopt;
        auto rhs = emitInlineExpr(*bin->rhs, frame, ilFn);
        if (!rhs) return std::nullopt;
        return lowerEquality(bin->op, *lhs, *rhs, ilFn);
    }
    if (const auto* tern = dynamic_cast<const ast::Ternary*>(&expr)) {
        auto condVal = emitInlineExpr(*tern->condition, frame, ilFn);
        if (!condVal) return std::nullopt;
        Value cond = lowerConditionFromVal(*condVal, ilFn);
        return emitInlineSelect(
            cond, [&]() { return emitInlineExpr(*tern->thenExpr, frame, ilFn); },
            [&]() { return emitInlineExpr(*tern->elseExpr, frame, ilFn); }, ilFn);
    }
    if (const auto* call = dynamic_cast<const ast::Call*>(&expr)) {
        const auto* callee = dynamic_cast<const ast::MemberAccess*>(call->callee.get());
        if (callee == nullptr) {
            diags_.error(expr.span, "internal: an inlined body reached an unproven callee");
            return std::nullopt;
        }
        std::vector<Value> args;
        args.reserve(call->args.size());
        for (const auto& argPtr : call->args) {
            auto argVal = emitInlineExpr(*argPtr, frame, ilFn);
            if (!argVal) return std::nullopt;
            args.push_back(boxValueIfNeeded(*argVal, ilFn));
        }
        // A sibling this same proof describes, and not already on the stack:
        // its body moves here too, which is the only way BOTH frames of a
        // two-deep forwarding method disappear.
        const types::ModuleLiteralInline* nested =
            inference_->moduleLiterals.inlinableMethod(*frame.binding, callee->property);
        if (nested != nullptr) {
            bool active = false;
            for (const auto& on : inlineStack_) {
                if (on.first == *frame.binding && on.second == callee->property) active = true;
            }
            if (!active) {
                return emitInlinedMethod(*frame.binding, callee->property, *nested, frame.receiver,
                                         args, ilFn);
            }
        }
        return emitInlineMethodCall(frame.receiver, callee->property, args, ilFn);
    }
    // A literal, whose lowering asks nothing about a receiver.
    return lowerExpr(expr, ilFn);
}

Lowerer::Value Lowerer::emitInlinePropGet(Value receiver, const std::string& key,
                                          il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::PropGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {receiver.id};
    inst.keyIndex = getKeyConstantIndex(key);
    inst.icIndex = icSiteCounter_++;
    // The receiver is one object for the life of the program, so the site sees
    // one shape unless the program adds a property to the literal — which is
    // what a monomorphic cache is a claim about, and it is checked either way.
    inst.icMonomorphic = true;
    // Uncounted, deliberately: the census counts the sites the SOURCE wrote,
    // and this read is already counted where it was written — inside the
    // method, which is still compiled for every caller that does not inline it.
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

Lowerer::Value Lowerer::emitInlineMethodCall(Value receiver, const std::string& key,
                                             const std::vector<Value>& args,
                                             il::Function& ilFn) {
    std::vector<il::ValueId> operands;
    operands.reserve(args.size() + 1);
    operands.push_back(receiver.id);
    for (const auto& arg : args) operands.push_back(arg.id);

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::MethodCall;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = std::move(operands);
    inst.keyIndex = getKeyConstantIndex(key);
    inst.icIndex = icSiteCounter_++;
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

std::optional<Lowerer::Value> Lowerer::emitInlineSelect(
    Value cond, const std::function<std::optional<Value>()>& thenArm,
    const std::function<std::optional<Value>()>& elseArm, il::Function& ilFn) {
    il::BlockId bThen = createBlock(ilFn);
    il::BlockId bElse = createBlock(ilFn);
    il::BlockId bJoin = createBlock(ilFn);

    size_t entryBlockIdx = currentBlockIdx_;
    auto statePre = snapshotVarStates();

    setCurrentBlock(bThen);
    auto thenVal = thenArm();
    if (!thenVal) return std::nullopt;
    auto stateThen = snapshotVarStates();
    bool thenReaches = !currentBlockIsTerminated(ilFn);
    size_t thenEndBlockIdx = currentBlockIdx_;

    restoreVarStates(statePre);
    setCurrentBlock(bElse);
    auto elseVal = elseArm();
    if (!elseVal) return std::nullopt;
    auto stateElse = snapshotVarStates();
    bool elseReaches = !currentBlockIsTerminated(ilFn);
    size_t elseEndBlockIdx = currentBlockIdx_;

    il::Type joinType =
        thenVal->type == elseVal->type ? thenVal->type : il::Type::Dynamic;

    il::Instruction brInst;
    brInst.op = il::Op::Branch;
    brInst.type = il::Type::Void;
    brInst.result = il::kNoValue;
    brInst.operands = {cond.id};
    brInst.target = il::BlockTarget{.block = bThen, .args = {}};
    brInst.elseTarget = il::BlockTarget{.block = bElse, .args = {}};
    ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

    il::ValueId resParamId = ilFn.valueCount++;
    ilFn.blocks[bJoin].params.push_back({resParamId, joinType});
    ExprJoin join = makeExprJoin(stateThen, stateElse, bJoin, ilFn);

    auto emitArmEdge = [&](size_t endBlockIdx, Value v, const VarStateMap& state) {
        setCurrentBlock(endBlockIdx);
        if (joinType == il::Type::Dynamic) v = boxValueIfNeeded(v, ilFn);
        else if (v.type != joinType) v = unboxValueIfNeeded(v, joinType, ilFn);
        std::vector<il::ValueId> args{v.id};
        appendExprJoinArgs(args, join, state, ilFn);
        il::Instruction jmpInst;
        jmpInst.op = il::Op::Jump;
        jmpInst.type = il::Type::Void;
        jmpInst.result = il::kNoValue;
        jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
        emitInst(ilFn, jmpInst);
    };
    if (thenReaches) emitArmEdge(thenEndBlockIdx, *thenVal, stateThen);
    if (elseReaches) emitArmEdge(elseEndBlockIdx, *elseVal, stateElse);

    setCurrentBlock(bJoin);
    restoreVarStates(thenReaches || !elseReaches ? stateThen : stateElse);
    bindExprJoinParams(join);
    return Value{resParamId, joinType};
}

}  // namespace bronze::lower
