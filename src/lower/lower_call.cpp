#include <cmath>
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
        auto argVal = lowerExpr(*call->args[i], ilFn);
        if (!argVal) return std::nullopt;
        const il::Type paramType = calleeFn.params[i + base].type;
        // The `--pins` barrier for `param <owner>(<p>): number`, at the CALL
        // SITE, because a pinned parameter has no boxed store to put one on:
        // the value arrives in an f64 register and is written to an f64 slot.
        // This is the enumerated set stage E4's parameter proof joins over —
        // the direct sites plus the boxed wrapper — and the wrapper's half is
        // in llvm_backend.cpp. Emitted before the conversion so the check
        // REPLACES the ToNumber it was quietly relying on rather than joining
        // it: the pin says the caller passes a Number, not that the callee
        // will coerce whatever arrives.
        if (calleeFn.params[i + base].pinned) {
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
        const auto* baseIdent = dynamic_cast<const ast::Ident*>(mem->object.get());
        // `Object.keys(x)` keeps its own instruction — every other member of
        // `Object` now resolves through the namespace object like a member of
        // `Math` does, so this is a fast path rather than the only path, and it
        // falls through to the general call lowering for anything it does not
        // recognize.
        if (baseIdent && baseIdent->name == "Object" && mem->property == "keys" &&
            call->args.size() == 1 &&
            !dynamic_cast<const ast::SpreadElement*>(call->args[0].get()) &&
            activeVarMap_.find("Object") == activeVarMap_.end()) {
            auto argVal = lowerExpr(*call->args[0], ilFn);
            if (!argVal) return std::nullopt;
            auto argBoxed = boxValueIfNeeded(*argVal, ilFn);

            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::ObjectKeys;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.operands = {argBoxed.id};
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        }
        // `Object.defineProperties(o, { ... })` with descriptors the compiler
        // can read: a run of defines instead of seven allocations and
        // thirty-six field reads (lower_define_props.cpp). Nullopt is "not a
        // call it takes", and the general lowering below is what it falls to.
        if (auto defined = lowerDefinePropertiesLiteral(call, *mem, ilFn)) return defined;

        // A method of a certified module-scope object literal, whose body the
        // site runs instead of calling (lower_module_inline.cpp). Asked before
        // the receiver is lowered, because the inline lowers it itself — once,
        // where the call would have.
        std::optional<Value> inlined;
        if (tryLowerModuleLiteralInline(*call, *mem, onSpine, ilFn, inlined)) return inlined;
    }

    // `Math.sqrt(x)` and its bit-exact siblings on the PROVEN pristine
    // builtin, with one machine-number argument: one f64 instruction — no
    // global read, no property load, no callee guards, no boxing. The proof
    // (`pristineMathCalls`) is what licenses skipping the evaluation of
    // `Math.<fn>` itself; the function list is `il::MathUnaryFn`'s, so the
    // dynamic path the other mode takes cannot differ in any output bit.
    if (const auto* mathMem = dynamic_cast<const ast::MemberAccess*>(call->callee.get());
        mathMem != nullptr && !mathMem->optional && !call->optional &&
        pristineMathCall(*call) && call->args.size() == 1 &&
        !dynamic_cast<const ast::SpreadElement*>(call->args[0].get())) {
        std::optional<il::MathUnaryFn> mathFn;
        if (mathMem->property == "sqrt") mathFn = il::MathUnaryFn::Sqrt;
        else if (mathMem->property == "abs") mathFn = il::MathUnaryFn::Abs;
        else if (mathMem->property == "floor") mathFn = il::MathUnaryFn::Floor;
        else if (mathMem->property == "ceil") mathFn = il::MathUnaryFn::Ceil;
        else if (mathMem->property == "trunc") mathFn = il::MathUnaryFn::Trunc;
        // `definitelyNumericOperand`, not just `provenNumber`: the argument
        // this exists for is the FFT/N-body shape — a const chain built from
        // typed-element reads, which inference types dynamic but whose value,
        // when one exists, can only be a number (a mixed BigInt pair throws
        // before the call). That is exactly the licence the method's own
        // ToNumber needs: a number in, unchanged, out.
        if (mathFn.has_value() && definitelyNumericOperand(*call->args[0], 8)) {
            auto argVal = lowerCoercingOperand(*call->args[0], ilFn);
            if (!argVal) return std::nullopt;
            // Definitely numeric, so this unbox is exact.
            Value argF64 = unboxValueIfNeeded(*argVal, il::Type::F64, ilFn);
            recordCall(call->span.file, true, "");
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::MathUnary;
            inst.type = il::Type::F64;
            inst.result = res;
            inst.operands = {argF64.id};
            inst.immI32 = static_cast<int32_t>(*mathFn);
            emitInst(ilFn, inst);
            return Value{res, il::Type::F64};
        }
    }

    // A spread argument means the argument count is not known here, and a
    // direct call's operand list is exactly its parameter list. So a spread
    // call always takes the uniform path, where the argument vector is a real
    // array the runtime unpacks.
    const bool spreadArgs = listHasSpread(call->args);

    // An OPTIONAL call never takes the direct path: the point of `f?.()` is
    // that the callee may be nullish, and a direct call names a module
    // function that cannot be.
    if (const auto* calleeIdent = dynamic_cast<const ast::Ident*>(call->callee.get());
        calleeIdent && !spreadArgs && !call->optional) {
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
                recordCall(call->span.file, true, "");
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
                recordCall(call->span.file, true, "");
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
        recordCall(call->span.file, false, "callee is super member");
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
            recordCall(call->span.file, false, "callee is a private member");
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
            const bool mono = monomorphicPropSite(*mem->object);
            recordPropertyAccess(mem->span.file, mono, mono ? "" : propBailReason(*mem->object));
            recordCall(call->span.file, false, "callee is method / property access");
            inst.icMonomorphic = mono;
            inst.icFnRecv = functionBindingReceiver(*mem->object, keyIdx);
            stampStaticSlot(inst, *mem->object);
            emitInst(ilFn, inst);
            calleeVal = Value{getRes, il::Type::Dynamic};
        } else {
            // Direct method call IC path:
            uint32_t keyIdx = getKeyConstantIndex(mem->property);
            uint32_t icIdx = icSiteCounter_++;
            const bool mono = monomorphicPropSite(*mem->object);
            recordCall(call->span.file, true, "");

            il::ValueId callRes = ilFn.valueCount++;
            il::Instruction inst;
            inst.keyIndex = keyIdx;
            inst.icIndex = icIdx;
            inst.icMonomorphic = mono;
            inst.icFnRecv = functionBindingReceiver(*mem->object, keyIdx);
            inst.type = il::Type::Dynamic;
            inst.result = callRes;

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
                // Only the fixed-operand form can name a callee: a spread's
                // argument count is a runtime fact and a direct edge's operand
                // list is its parameter list.
                recordMethodCallSite(inst, *mem->object, mem->property);
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
        recordCall(call->span.file, false, "callee is computed / index access");
        auto objVal = lowerChainBase(*idx->object, ilFn, onSpine);
        if (!objVal) return std::nullopt;
        thisArgVal = boxValueIfNeeded(*objVal, ilFn);
        if (idx->optional) emitChainShortCircuit(thisArgVal, ilFn);
        auto fnVal = emitIndexRead(*idx, thisArgVal, ilFn);
        if (!fnVal) return std::nullopt;
        calleeVal = boxValueIfNeeded(*fnVal, ilFn);
    } else if (const auto* calleeIdent = dynamic_cast<const ast::Ident*>(call->callee.get())) {
        if (spreadArgs) {
            recordCall(call->span.file, false, "call has spread argument");
        } else if (call->optional) {
            recordCall(call->span.file, false, "optional call (?.)");
        } else {
            uint32_t shadowDepth = 0;
            uint32_t shadowIndex = 0;
            auto envIt = activeVarMap_.find(calleeIdent->name);
            if (envIt != activeVarMap_.end() ||
                findEnclosingEnvVar(calleeIdent->name, shadowDepth, shadowIndex)) {
                recordCall(call->span.file, false, "callee is local or captured variable");
            } else {
                auto it = functionIndices_.find(calleeIdent->name);
                if (it == functionIndices_.end()) {
                    recordCall(call->span.file, false, "callee not a module function");
                } else if (ilModule_.functions[it->second].needsArguments) {
                    recordCall(call->span.file, false, "callee uses arguments object");
                } else if (!directCallShapeFits(ilModule_.functions[it->second], call->args.size())) {
                    recordCall(call->span.file, false, "callee arity mismatch");
                } else {
                    recordCall(call->span.file, false, "callee is dynamic");
                }
            }
        }
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
        recordCall(call->span.file, false, "callee is an expression");
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
    emitInst(ilFn, inst);
    return Value{callRes, il::Type::Dynamic};
}

}  // namespace bronze::lower
