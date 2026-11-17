#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

namespace {

// A direct call must provide an operand for every parameter the callee entry
// takes. A call that passes fewer arguments pads them with `undefined` (JS gives
// every parameter that treatment, which is why a default tests for exactly
// that value), and one that passes more evaluates the extras and drops them.
// So this is not a question about the PROGRAM — it is a question about which
// of bronze's two call shapes can express it. What the direct shape cannot do
// is invent a value for a parameter inference proved a native type for: there
// is no `undefined` in an `f64`. Those calls, and the ones with arguments the
// fixed operand list has nowhere to put, take the uniform path instead, where
// the argument vector is a real array the runtime unpacks.
bool directCallShapeFits(const il::Function& callee, size_t argCount) {
    const size_t base = callee.firstSourceParam();
    const size_t fixed = callee.callerParamCount();
    if (!callee.hasRestParam && argCount > fixed) return false;
    for (size_t i = argCount; i < fixed; ++i) {
        if (callee.params[i + base].type != il::Type::Dynamic) return false;
    }
    return true;
}

}  // namespace

std::optional<Lowerer::Value> Lowerer::lowerDirectCall(const ast::Call* call, uint32_t calleeIdx,
                                                       il::ValueId envBase, uint32_t envHops,
                                                       il::Function& ilFn) {
    const auto& calleeFn = ilModule_.functions[calleeIdx];

    // Synthetic parameters are not source arguments; the arity the program has
    // to match is the source one. The operand list is fixed, because the
    // padding and the leftover array are both built HERE, where the argument
    // count is a compile-time fact — and `directCallShapeFits` has already
    // established that this call's count can be spelled that way.
    const size_t base = calleeFn.firstSourceParam();
    const size_t fixed = calleeFn.callerParamCount();

    std::vector<il::ValueId> argVals;
    // `__env` is the record the callee closed over. A call site cannot read it
    // off the closure value without loading the closure, so it does not: it
    // hands over its OWN record and the number of parent links to that one,
    // which the scope plan counted.
    if (calleeFn.needsEnv) {
        if (envBase == il::kNoValue) {
            diags_.error(call->span, "internal: direct call to a closure with no environment");
            return std::nullopt;
        }
        argVals.push_back(envBase);
    }
    // A plain `f()` has no receiver, so a direct call supplies undefined for
    // `__this`.
    if (calleeFn.needsThis) {
        argVals.push_back(emitConstUndefined(ilFn));
    }
    for (size_t i = 0; i < fixed; ++i) {
        // An omitted optional argument IS `undefined` — that is the value the
        // callee's default tests for, so a short direct call and a short
        // uniform one reach the same prologue with the same bits.
        if (i >= call->args.size()) {
            argVals.push_back(emitConstUndefined(ilFn));
            continue;
        }
        const il::Type paramType = calleeFn.params[i + base].type;
        const bool isPinned = calleeFn.params[i + base].pinned;
        auto argVal = lowerExpr(*call->args[i], ilFn);
        if (!argVal) return std::nullopt;
        // The `--pins` barrier for `param <owner>(<p>): number`, at the CALL
        // SITE, because a pinned parameter has no boxed store to put one on:
        // the value arrives in an f64 register and is written to an f64 slot.
        // This is the enumerated set stage E4's parameter proof joins over —
        // the direct sites plus the boxed wrapper — and the wrapper's half is
        // in llvm_backend.cpp. Emitted before the conversion so the check
        // REPLACES the ToNumber it was quietly relying on rather than joining
        // it: the pin says the caller passes a Number, not that the callee
        // will coerce whatever arrives.
        if (isPinned) {
            emitPinGuard(*argVal, keyStrings_[calleeFn.params[i + base].pinKeyIndex],
                         il::PinBarrier::Number, ilFn);
        }
        if (paramType == il::Type::Dynamic) {
            argVal = boxValueIfNeeded(*argVal, ilFn);
        } else if (argVal->type == il::Type::Dynamic) {
            argVal = unboxValueIfNeeded(*argVal, paramType, ilFn);
        }
        argVals.push_back(argVal->id);
    }
    if (calleeFn.hasRestParam) {
        // Everything past the fixed parameters, as one fresh array — the value
        // the rest parameter is bound to.
        il::ValueId restArr = ilFn.valueCount++;
        il::Instruction createInst;
        createInst.op = il::Op::CreateArray;
        createInst.type = il::Type::Dynamic;
        createInst.result = restArr;
        // Zero LENGTH, not zero capacity: `create.array n` makes an array of n
        // undefined elements, so anything built by appending has to start empty.
        createInst.immI32 = 0;
        emitInst(ilFn, createInst);
        for (size_t i = fixed; i < call->args.size(); ++i) {
            auto elemVal = lowerExpr(*call->args[i], ilFn);
            if (!elemVal) return std::nullopt;
            emitContainerOp(il::Op::ArrayAppend, Value{restArr, il::Type::Dynamic}, *elemVal,
                            ilFn);
        }
        argVals.push_back(restArr);
    }

    il::Instruction inst;
    inst.op = il::Op::Call;
    inst.calleeIndex = calleeIdx;
    inst.operands = std::move(argVals);
    inst.type = calleeFn.returnType;
    if (calleeFn.needsEnv) inst.callEnvHops = envHops;
    inst.span = call->callee ? call->callee->span : call->span;

    il::ValueId res = il::kNoValue;
    if (calleeFn.returnType != il::Type::Void) {
        res = ilFn.valueCount++;
        inst.result = res;
    } else {
        inst.result = il::kNoValue;
    }

    emitInst(ilFn, inst);
    if (calleeFn.returnType == il::Type::Void) {
        // A function that returns no value still EVALUATES to one: `undefined`.
        // The IL return type stays void so the call itself costs nothing, and
        // the undefined is materialized here, where the only thing that can see
        // it is the expression the call sits in. Handing back the void call's
        // absent result instead let it escape into arbitrary expression
        // contexts — `console.log(f())` reached the verifier as a box of value
        // %kNoValue and failed to compile at all.
        return Value{emitConstUndefined(ilFn), il::Type::Dynamic};
    }
    return Value{res, calleeFn.returnType};
}

std::optional<Lowerer::Value> Lowerer::lowerCall(const ast::Call* call, il::Function& ilFn,
                                                 bool onSpine) {
    // A rest pattern a bundler lowered to its helper's call (ast.h
    // `Call::isObjectRest`): the copy the pattern itself makes, over the
    // source and the excluded keys in the order the call evaluated them.
    if (call->isObjectRest) {
        auto src = lowerExpr(*call->args[0], ilFn);
        if (!src) return std::nullopt;
        auto excluded = lowerExpr(*call->args[1], ilFn);
        if (!excluded) return std::nullopt;
        il::Instruction inst;
        inst.op = il::Op::ObjectRest;
        inst.type = il::Type::Dynamic;
        inst.result = ilFn.valueCount++;
        inst.operands = {src->id, excluded->id};
        emitInst(ilFn, inst);
        return Value{inst.result, il::Type::Dynamic};
    }
    if (selfHost_) {
        std::optional<Value> intrinsic;
        if (lowerIntrinsicCall(call, ilFn, intrinsic)) return intrinsic;
    }
    if (nativeManifest_) {
        if (auto nativeVal = tryLowerNativeCall(call, ilFn)) {
            return nativeVal;
        }
    }

    // Which console method this is, if any. The parser folded the whole
    // member expression into one `Ident`, so the name is the only thing to
    // ask, and `consoleStreamOf` is the one table that answers.
    auto consoleStream = ast::ConsoleStream::None;
    std::string consoleName;
    if (const auto* calleeIdent = dynamic_cast<const ast::Ident*>(call->callee.get())) {
        consoleStream = ast::consoleStreamOf(calleeIdent->name);
        consoleName = calleeIdent->name;
        // A syntactically direct `eval(...)` of the GLOBAL eval. The call
        // compiles — `eval` is a provided global whose body defers to the
        // host (builtin_function.cpp) — but 19.2.1's direct form evaluates in
        // the caller's scope, and an AOT frame has no environment record to
        // hand over, so what runs has the indirect (global-environment)
        // semantics. Source that reads the caller's locals diverges silently;
        // that is exactly the class of thing bronze warns about rather than
        // hides. Once per module, like warnUnresolved — the set is shared
        // because a resolved `eval` can never reach warnUnresolved. A local
        // binding named `eval` shadows the global and is not the construct.
        if (calleeIdent->name == "eval") {
            uint32_t depth = 0;
            uint32_t index = 0;
            const bool shadowed =
                activeVarMap_.contains("eval") || functionIndices_.contains("eval") ||
                (currentEnvValue_ != il::kNoValue && findEnclosingEnvVar("eval", depth, index));
            if (!shadowed && warnedUnresolved_.insert("eval").second) {
                diags_.warning(calleeIdent->span,
                               "direct `eval` runs with indirect semantics: the source "
                               "evaluates in the global environment, not the caller's scope "
                               "(an AOT frame has no environment to reify; the call itself is "
                               "answered by the host's eval hook, or a TypeError without one)");
            }
        }
    } else if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
        if (const auto* baseIdent = dynamic_cast<const ast::Ident*>(mem->object.get())) {
            if (baseIdent->name == "console" && !hostGlobals_.contains("console")) {
                consoleStream = ast::consoleStreamOf("console." + mem->property);
                consoleName = "console." + mem->property;
                if (consoleStream == ast::ConsoleStream::None) {
                    warnUnresolved(consoleName, call->span);
                    return emitReferenceError(consoleName, call->span, ilFn);
                }
            }
        }
    }

    if (consoleStream != ast::ConsoleStream::None) {
        bool hasSpread = false;
        for (const auto& argPtr : call->args) {
            if (dynamic_cast<const ast::SpreadElement*>(argPtr.get())) {
                hasSpread = true;
                break;
            }
        }
        if (hasSpread) {
            auto argsArr = lowerListToArray(call->args, ilFn);
            if (!argsArr) return std::nullopt;
            il::Instruction inst;
            inst.op = consoleStream == ast::ConsoleStream::Err ? il::Op::PrintSpreadErr
                                                               : il::Op::PrintSpread;
            inst.type = il::Type::Void;
            inst.result = il::kNoValue;
            inst.operands = {argsArr->id};
            emitInst(ilFn, inst);
            return Value{emitConstUndefined(ilFn), il::Type::Dynamic};
        }
        // Any number of arguments, including none: node formats each one as it
        // would a lone argument and joins them with a single space. The joining
        // is the runtime's, so there is one inspect formatter and not two.
        std::vector<il::ValueId> args;
        args.reserve(call->args.size());
        for (const auto& argPtr : call->args) {
            auto argVal = lowerExpr(*argPtr, ilFn);
            if (!argVal) return std::nullopt;
            args.push_back(boxValueIfNeeded(*argVal, ilFn).id);
        }
        il::Instruction inst;
        // The ONLY difference between the two: `warn` and `error` write to
        // stderr, which keeps a library's chatter out of the bytes the oracle
        // pins.
        inst.op = consoleStream == ast::ConsoleStream::Err ? il::Op::PrintErr : il::Op::Print;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = std::move(args);
        emitInst(ilFn, inst);
        // `console.log(...)` evaluates to undefined, like any call that returns
        // nothing.
        return Value{emitConstUndefined(ilFn), il::Type::Dynamic};
    }

    // `Object` is recognized here rather than looked up: bronze has no global
    // object for it to live on.
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
        // `Object.keys(x)` is an ordinary call on %Object%. It once had an
        // instruction of its own that skipped reading `Object.keys`, which is
        // exactly what made `Object.keys = f` invisible to every call site;
        // the method-call cache reaches the same native once the site is warm.
        //
        // `Object.defineProperties(o, { ... })` with descriptors the compiler
        // can read: a run of defines instead of seven allocations and
        // thirty-six field reads (lower_define_props.cpp). Nullopt is "not a
        // call it takes", and the general lowering below is what it falls to.
        if (auto defined = lowerDefinePropertiesLiteral(call, *mem, ilFn)) return defined;
    }

    // A spread argument means the argument count is not known here, and a
    // direct call's operand list is exactly its parameter list. So a spread
    // call always takes the uniform path, where the argument vector is a real
    // array the runtime unpacks — and so does a call with more arguments than
    // the fixed-operand helpers take (`argsTakeArrayPath`). Only the spread
    // rules the DIRECT path out, though: a direct call's operands are the
    // callee's parameters however many there are.
    const bool hasSpread = listHasSpread(call->args);
    const bool spreadArgs = argsTakeArrayPath(call->args);

    // An OPTIONAL call never takes the direct path: the point of `f?.()` is
    // that the callee may be nullish, and a direct call names a module
    // function that cannot be.
    if (const auto* calleeIdent = dynamic_cast<const ast::Ident*>(call->callee.get());
        calleeIdent && !hasSpread && !call->optional) {
        // A local binding shadows a module-level function, and so does an
        // enclosing scope's environment slot: a nested function declaration
        // registers in functionIndices_ under its source name, but every
        // reference to it — including a sibling closure's — has to go through
        // the environment chain, not a direct call.
        uint32_t shadowDepth = 0;
        uint32_t shadowIndex = 0;
        auto envIt = activeVarMap_.find(calleeIdent->name);
        if (envIt == activeVarMap_.end() &&
            !findEnclosingEnvVar(calleeIdent->name, shadowDepth, shadowIndex)) {
            auto it = functionIndices_.find(calleeIdent->name);
            // A callee that needs an `arguments` object is not a direct-call
            // target: the object is built from the caller's REAL argument list,
            // which only the uniform path's wrapper can see.
            if (it != functionIndices_.end() && !ilModule_.functions[it->second].needsArguments &&
                !ilModule_.functions[it->second].needsEnv &&
                directCallShapeFits(ilModule_.functions[it->second], call->args.size())) {
                return lowerDirectCall(call, it->second, il::kNoValue, 0, ilFn);
            }
        } else {
            // A SIBLING CLOSURE, reached through the environment chain. The
            // scope plan may already know exactly which function is in that
            // slot and that nothing can ever put another one there
            // (lower_scope.cpp, `planStableFunctionSlots`) — and then the
            // environment the callee captured is the record holding the slot,
            // which is this caller's own record `envHops` parent links up. So
            // the call needs neither the closure value nor a guard over it.
            uint32_t envHops = 0;
            uint32_t stableFn = 0;
            if (currentEnvValue_ != il::kNoValue &&
                findStableFunctionCallee(calleeIdent->name, envHops, stableFn) &&
                !ilModule_.functions[stableFn].needsArguments &&
                directCallShapeFits(ilModule_.functions[stableFn], call->args.size())) {
                return lowerDirectCall(call, stableFn, currentEnv(ilFn), envHops, ilFn);
            }
        }
    }

    Value calleeVal;
    Value thisArgVal;
    // `super.m(...)` is the one call whose callee and receiver come from
    // different objects: the function is found on the PARENT prototype, and it
    // runs on the current receiver.
    if (const auto* superMem = dynamic_cast<const ast::SuperMember*>(call->callee.get())) {
        auto fnVal = lowerSuperMember(superMem, ilFn);
        if (!fnVal) return std::nullopt;
        auto thisVal = lowerThisValue(call->span, ilFn);
        if (!thisVal) return std::nullopt;
        calleeVal = boxValueIfNeeded(*fnVal, ilFn);
        thisArgVal = boxValueIfNeeded(*thisVal, ilFn);
    } else if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
        // The callee is itself a chain link, so its base continues the spine
        // and its own `?.` short-circuits the whole chain — and the RECEIVER
        // is still `mem->object`, which is what keeps `o.m?.()` calling `m`
        // on `o` rather than on undefined.
        auto objVal = lowerChainBase(*mem->object, ilFn, onSpine);
        if (!objVal) return std::nullopt;
        thisArgVal = boxValueIfNeeded(*objVal, ilFn);
        if (mem->optional) emitChainShortCircuit(thisArgVal, ilFn);

        // `o.#m(...)` — the same rule 13.3.6.1 gives `o.m(...)`: the base is
        // evaluated once and is the receiver. What differs is only where the
        // callee is found.
        if (mem->isPrivate) {
            auto fnVal = lowerPrivateRead(*mem, thisArgVal, ilFn);
            if (!fnVal) return std::nullopt;
            calleeVal = boxValueIfNeeded(*fnVal, ilFn);
        } else if (call->optional) {
            uint32_t keyIdx = getKeyConstantIndex(mem->property);
            uint32_t icIdx = icSiteCounter_++;

            il::ValueId getRes = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::PropGet;
            inst.type = il::Type::Dynamic;
            inst.result = getRes;
            inst.operands = {thisArgVal.id};
            inst.keyIndex = keyIdx;
            inst.icIndex = icIdx;
            Span propSpan = {
                static_cast<uint32_t>(mem->span.end >= mem->property.length() ? mem->span.end - mem->property.length() : mem->span.begin),
                mem->span.end,
                mem->span.file
            };
            inst.span = propSpan;
            emitInst(ilFn, inst);
            calleeVal = Value{getRes, il::Type::Dynamic};
        } else {
            // Direct method call IC path:
            uint32_t keyIdx = getKeyConstantIndex(mem->property);
            uint32_t icIdx = icSiteCounter_++;

            il::ValueId callRes = ilFn.valueCount++;
            il::Instruction inst;
            inst.keyIndex = keyIdx;
            inst.icIndex = icIdx;
            Span propSpan = {
                static_cast<uint32_t>(mem->span.end >= mem->property.length() ? mem->span.end - mem->property.length() : mem->span.begin),
                mem->span.end,
                mem->span.file
            };
            inst.span = propSpan;
            inst.type = il::Type::Dynamic;
            inst.result = callRes;

            // `f.apply(t, arguments)` in a counted function: the values
            // the parameters arrived with, when the wrapper passed the count.
            if (!spreadArgs && mem->property == "apply" && call->args.size() == 2 &&
                ilFn.params.size() - ilFn.firstSourceParam() <= 16) {
                if (const std::optional<Value> args = countedArguments(*call->args[1], ilFn)) {
                    return lowerForwardedApply(thisArgVal, *call->args[0], *args, inst, ilFn);
                }
            }
            if (spreadArgs) {
                auto argsArr = lowerListToArray(call->args, ilFn);
                if (!argsArr) return std::nullopt;
                inst.op = il::Op::MethodCallSpread;
                inst.operands = {thisArgVal.id, argsArr->id};
            } else {
                std::vector<il::ValueId> argVals;
                argVals.push_back(thisArgVal.id);
                for (const auto& argPtr : call->args) {
                    auto argVal = lowerExpr(*argPtr, ilFn);
                    if (!argVal) return std::nullopt;
                    auto argBoxed = boxValueIfNeeded(*argVal, ilFn);
                    argVals.push_back(argBoxed.id);
                }
                inst.op = il::Op::MethodCall;
                inst.operands = std::move(argVals);
            }
            emitInst(ilFn, inst);
            return Value{callRes, il::Type::Dynamic};
        }
    } else if (const auto* idx = dynamic_cast<const ast::IndexAccess*>(call->callee.get())) {
        // `o[k](...)` — the same rule as `o.m(...)`: 13.3.6.1 evaluates the
        // MemberExpression once and passes its BASE as the this value. Read
        // through `emitIndexRead` so the base is lowered once; reaching for
        // `lowerIndexAccess` here would evaluate `o` twice, and passing no
        // receiver at all (which is what this branch used to fall through to)
        // made `v[Symbol.iterator]()` run with `this` undefined.
        auto objVal = lowerChainBase(*idx->object, ilFn, onSpine);
        if (!objVal) return std::nullopt;
        thisArgVal = boxValueIfNeeded(*objVal, ilFn);
        if (idx->optional) emitChainShortCircuit(thisArgVal, ilFn);
        auto fnVal = emitIndexRead(*idx, thisArgVal, ilFn);
        if (!fnVal) return std::nullopt;
        calleeVal = boxValueIfNeeded(*fnVal, ilFn);
    } else if (dynamic_cast<const ast::Ident*>(call->callee.get()) != nullptr) {
        auto cVal = lowerChainBase(*call->callee, ilFn, onSpine);
        if (!cVal) return std::nullopt;
        calleeVal = boxValueIfNeeded(*cVal, ilFn);

        il::ValueId undefRes = ilFn.valueCount++;
        il::Instruction undefInst;
        undefInst.op = il::Op::ConstUndefined;
        undefInst.type = il::Type::Dynamic;
        undefInst.result = undefRes;
        emitInst(ilFn, undefInst);
        thisArgVal = Value{undefRes, il::Type::Dynamic};
    } else {
        auto cVal = lowerChainBase(*call->callee, ilFn, onSpine);
        if (!cVal) return std::nullopt;
        calleeVal = boxValueIfNeeded(*cVal, ilFn);

        // No receiver: `f()` is a call with no base, so 13.3.6.1 passes
        // `undefined` as the this value. bronze has no global object to
        // substitute for it in sloppy mode, so `undefined` is its one answer in
        // every mode — which is also the strict one. It used to pass a boxed
        // ZERO here, so `const g = o.m; g()` ran with `this === 0` while the
        // direct-call path in the same program ran with `this` undefined.
        il::ValueId undefRes = ilFn.valueCount++;
        il::Instruction undefInst;
        undefInst.op = il::Op::ConstUndefined;
        undefInst.type = il::Type::Dynamic;
        undefInst.result = undefRes;
        emitInst(ilFn, undefInst);
        thisArgVal = Value{undefRes, il::Type::Dynamic};
    }

    // `f?.()` — the CALLEE is what may be nullish here, and the check comes
    // after the receiver has been read but before a single argument is
    // evaluated, because the arguments are part of the chain too.
    if (call->optional) emitChainShortCircuit(calleeVal, ilFn);

    std::vector<il::ValueId> dynOperands;
    dynOperands.push_back(calleeVal.id);
    dynOperands.push_back(thisArgVal.id);

    if (spreadArgs) {
        // One operand, an array: the runtime reads its length and passes its
        // elements as the argument vector, which is the only place the count
        // is known.
        auto argsArr = lowerListToArray(call->args, ilFn);
        if (!argsArr) return std::nullopt;
        dynOperands.push_back(argsArr->id);
    } else {
        for (const auto& argPtr : call->args) {
            auto argVal = lowerExpr(*argPtr, ilFn);
            if (!argVal) return std::nullopt;
            auto argBoxed = boxValueIfNeeded(*argVal, ilFn);
            dynOperands.push_back(argBoxed.id);
        }
    }

    il::ValueId callRes = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = spreadArgs ? il::Op::DynamicCallSpread : il::Op::DynamicCall;
    inst.type = il::Type::Dynamic;
    inst.result = callRes;
    inst.operands = std::move(dynOperands);
    inst.span = call->callee ? call->callee->span : call->span;
    emitInst(ilFn, inst);
    return Value{callRes, il::Type::Dynamic};
}

// `recv.apply(this, arguments)` in a counted function (il.h
// `argumentsIsCount`): where `arguments` is the count, an `apply.args` of the
// values the parameters arrived with, and where it is the list, the method
// call. `site` is the method call's instruction as far as it was made (its
// key, site, span and result).
std::optional<Lowerer::Value> Lowerer::lowerForwardedApply(Value recv, const ast::Expr& thisExpr, Value args,
                                                          const il::Instruction& site, il::Function& ilFn) {
    auto thisVal = lowerExpr(thisExpr, ilFn);
    if (!thisVal) return std::nullopt;
    const Value thisBoxed = boxValueIfNeeded(*thisVal, ilFn);
    const il::BlockId bCount = createBlock(ilFn);
    const il::BlockId bList = createBlock(ilFn);
    const il::BlockId bJoin = createBlock(ilFn);
    const il::ValueId count = ilFn.valueCount++;
    ilFn.blocks[bCount].params.push_back({count, il::Type::Dynamic});
    ilFn.blocks[bJoin].params.push_back({site.result, il::Type::Dynamic});
    emitArgumentsIsCountBranch(args, bCount, bList, ilFn);

    const auto emitCall = [&](il::Op op, uint32_t icIndex, std::vector<il::ValueId> operands) {
        il::Instruction inst;
        inst.op = op;
        inst.type = il::Type::Dynamic;
        inst.result = ilFn.valueCount++;
        inst.operands = std::move(operands);
        inst.keyIndex = site.keyIndex;
        inst.icIndex = icIndex;
        inst.span = site.span;
        emitInst(ilFn, inst);
        il::Instruction jmp;
        jmp.op = il::Op::Jump;
        jmp.type = il::Type::Void;
        jmp.target = il::BlockTarget{.block = bJoin, .args = {inst.result}};
        emitInst(ilFn, jmp);
    };

    setCurrentBlock(bCount);
    std::vector<il::ValueId> operands{recv.id, thisBoxed.id, count};
    for (size_t p = ilFn.firstSourceParam(); p < ilFn.params.size(); ++p) {
        operands.push_back(boxValueIfNeeded(Value{static_cast<il::ValueId>(p), ilFn.params[p].type}, ilFn).id);
    }
    emitCall(il::Op::ApplyArgs, site.icIndex, std::move(operands));

    setCurrentBlock(bList);
    emitCall(il::Op::MethodCall, icSiteCounter_++, {recv.id, thisBoxed.id, args.id});

    setCurrentBlock(bJoin);
    return Value{site.result, il::Type::Dynamic};
}

}  // namespace bronze::lower
