// Object and array literals, property and element access, `new`, and the call
// forms — the expressions that go through the runtime's shapes, prototypes and
// inline caches.

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

namespace {
// Whether a call passing `argCount` source arguments can be spelled as a
// direct call to `callee`, whose operand list is exactly its parameter list.
//
// ECMA-262 has no arity error, and neither does bronze: a call that passes
// fewer arguments binds the missing parameters to `undefined` (10.2.11 gives
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

std::optional<Lowerer::Value> Lowerer::lowerObjectLit(const ast::ObjectLit* objLit,
                                                      il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::CreateObject;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    emitInst(ilFn, inst);

    for (const auto& prop : objLit->props) {
        // `{ x = 1 }` parses only because `{ x = 1 } = o` might follow: it is
        // the cover grammar for an object PATTERN and means nothing on its
        // own (ECMA-262 13.2.5.1). Reaching lowering means no `=` followed.
        if (prop.coverInitialized) {
            diags_.error(prop.value->span,
                         "a shorthand property may not have an initializer outside a "
                         "destructuring pattern");
            return std::nullopt;
        }
        // `{...src }` copies src's own enumerable properties in here, in
        // enumeration order, so a later key overwrites an earlier one exactly
        // as a spelled-out property would.
        if (const auto* spread = dynamic_cast<const ast::SpreadElement*>(prop.value.get())) {
            auto srcOpt = lowerExpr(*spread->argument, ilFn);
            if (!srcOpt) return std::nullopt;
            emitContainerOp(il::Op::ObjectSpread, Value{res, il::Type::Dynamic}, *srcOpt, ilFn);
            continue;
        }

        // `get k() {}` / `set k(v) {}`. ENUMERABLE, unlike a class's: 13.2.5.5
        // defines an object literal's accessor with `enumerable: true`, so it
        // appears in `Object.keys` and its getter runs when the key is read
        // back.
        if (prop.accessor != ast::AccessorKind::None) {
            const auto* fn = dynamic_cast<const ast::FunctionExpr*>(prop.value.get());
            if (!fn) {
                diags_.error(prop.value->span, "internal: an accessor property with no function");
                return std::nullopt;
            }
            if (prop.computed()) {
                auto keyOpt = lowerExpr(*prop.keyExpr, ilFn);
                if (!keyOpt) return std::nullopt;
                auto keyBoxed = boxValueIfNeeded(*keyOpt, ilFn);
                if (!emitAccessorDefComputed(Value{res, il::Type::Dynamic}, keyBoxed,
                                             prop.accessor, *fn, /*enumerable=*/true, ilFn)) {
                    return std::nullopt;
                }
            } else {
                if (!emitAccessorDef(Value{res, il::Type::Dynamic}, prop.key, prop.accessor, *fn,
                                     /*enumerable=*/true, ilFn)) {
                    return std::nullopt;
                }
            }
            continue;
        }

        // A computed key is evaluated BEFORE its value, and the properties in
        // source order, so the two lowerings interleave exactly as ECMA-262
        // 13.2.5.5 evaluates them. Getting this backwards is observable the
        // moment either expression has an effect.
        std::optional<Value> keyBoxed;
        if (prop.keyExpr) {
            auto keyOpt = lowerExpr(*prop.keyExpr, ilFn);
            if (!keyOpt) return std::nullopt;
            keyBoxed = boxValueIfNeeded(*keyOpt, ilFn);
        }

        // 13.2.5.5 PropertyDefinitionEvaluation: an anonymous function value
        // takes the property's name. Only for a name the source WROTE — a
        // computed key is not known here, and 8.6.2 would take it from the
        // evaluated key rather than from anything this pass can see.
        //
        // A shorthand METHOD takes the key too, and needs saying separately:
        // its function already carries a name — the IL symbol the parser
        // synthesized — so NamedEvaluation, which fires only for an anonymous
        // one, would leave `{ m() {} }.m.name` reading "obj.0.m".
        std::optional<Value> valOpt;
        if (prop.computed()) {
            valOpt = lowerExpr(*prop.value, ilFn);
        } else if (prop.isMethod) {
            const auto& fn = static_cast<const ast::FunctionExpr&>(*prop.value);
            valOpt = lowerClosure(fn, fn.name, prop.key, fn.params, fn.returnType, fn.body,
                                  fn.span, ilFn, fn.isArrow);
        } else {
            valOpt = lowerNamedEvaluation(*prop.value, prop.key, ilFn);
        }
        if (!valOpt) return std::nullopt;
        auto valBoxed = boxValueIfNeeded(*valOpt, ilFn);

        // No strict flag on either spelling: a literal DEFINES a property
        // on an object it has just created (13.2.5.5 CreateDataProperty),
        // which cannot be refused and is not a reference at all — so there is
        // no strict-mode refusal that could fire here.
        if (keyBoxed) {
            il::Instruction setInst;
            setInst.op = il::Op::ElemSet;
            setInst.type = il::Type::Void;
            setInst.result = il::kNoValue;
            setInst.operands = {res, keyBoxed->id, valBoxed.id};
            setInst.immI32 = 0;
            emitInst(ilFn, setInst);
        } else {
            uint32_t keyIdx = getKeyConstantIndex(prop.key);
            uint32_t icIdx = icSiteCounter_++;

            il::Instruction setInst;
            setInst.op = il::Op::PropSet;
            setInst.type = il::Type::Void;
            setInst.result = il::kNoValue;
            setInst.operands = {res, valBoxed.id};
            setInst.keyIndex = keyIdx;
            setInst.icIndex = icIdx;
            emitInst(ilFn, setInst);
        }
    }
    // `import * as ns`: the getters are built exactly as any literal's, and the
    // exotic object is made from them here. A second instruction and not a
    // different creation op, because the conversion needs the properties to
    // already be there — a namespace's own-key order is 10.4.6.2's sort over
    // them, so there is nothing to sort until the last one is defined.
    if (objLit->isModuleNamespace) {
        il::ValueId ns = ilFn.valueCount++;
        il::Instruction wrap;
        wrap.op = il::Op::ModuleNamespace;
        wrap.type = il::Type::Dynamic;
        wrap.result = ns;
        wrap.operands = {res};
        emitInst(ilFn, wrap);
        return Value{ns, il::Type::Dynamic};
    }
    return Value{res, il::Type::Dynamic};
}

bool Lowerer::emitAccessorDef(Value target, const std::string& key, ast::AccessorKind kind,
                              const ast::FunctionExpr& fn, bool enumerable, il::Function& ilFn) {
    // 10.2.9's `prefix` argument: an accessor's `name` is "get x" / "set x",
    // which is what distinguishes the two halves of one property in a stack
    // trace and in `Object.getOwnPropertyDescriptor(o, 'x').get.name`.
    const std::string accessorName =
        (kind == ast::AccessorKind::Getter ? "get " : "set ") + key;
    auto fnVal = lowerClosure(fn, fn.name, accessorName, fn.params, fn.returnType, fn.body,
                              fn.span, ilFn);
    if (!fnVal) return false;
    const il::ValueId absent = emitConstUndefined(ilFn);
    const bool isGetter = kind == ast::AccessorKind::Getter;

    il::Instruction inst;
    inst.op = il::Op::AccessorDef;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    // Both halves are operands even though the source wrote one: the runtime
    // needs to know WHICH half this is, and a pair with an `undefined` half
    // is exactly how ECMA-262 6.1.7.1 describes a get-only property.
    inst.operands = {target.id, isGetter ? fnVal->id : absent, isGetter ? absent : fnVal->id};
    inst.keyIndex = getKeyConstantIndex(key);
    inst.immI32 = enumerable ? 1 : 0;
    emitInst(ilFn, inst);
    return true;
}

bool Lowerer::emitAccessorDefComputed(Value target, Value key, ast::AccessorKind kind,
                                      const ast::FunctionExpr& fn, bool enumerable,
                                      il::Function& ilFn) {
    auto fnVal = lowerClosure(fn, fn.name, std::nullopt, fn.params, fn.returnType, fn.body,
                              fn.span, ilFn);
    if (!fnVal) return false;
    const il::ValueId absent = emitConstUndefined(ilFn);
    const bool isGetter = kind == ast::AccessorKind::Getter;

    il::Instruction inst;
    inst.op = il::Op::AccessorDefComputed;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {target.id, key.id, isGetter ? fnVal->id : absent,
                     isGetter ? absent : fnVal->id};
    inst.immI32 = enumerable ? 1 : 0;
    emitInst(ilFn, inst);
    return true;
}

// `delete` never READS the property it names — 13.5.1 evaluates its operand
// to a Reference and then asks the object to remove it — so the operand is
// dispatched on here rather than lowered as a value. Lowering it first would
// call a getter the delete is about to remove.
std::optional<Lowerer::Value> Lowerer::lowerDeleteReference(const ast::Unary& del,
                                                            il::Function& ilFn) {
    const ast::Expr& operand = *del.operand;

    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(&operand)) {
        auto objVal = lowerChainBase(*mem->object, ilFn, /*onSpine=*/true);
        if (!objVal) return std::nullopt;
        auto objBoxed = boxValueIfNeeded(*objVal, ilFn);
        if (mem->optional) emitChainShortCircuit(objBoxed, ilFn);

        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::PropDelete;
        inst.type = il::Type::Bool;
        inst.result = res;
        inst.operands = {objBoxed.id};
        inst.keyIndex = getKeyConstantIndex(mem->property);
        // 13.5.1.2 step 5.b: a delete that answers false is a TypeError in
        // strict code and the boolean `false` in sloppy code.
        inst.immI32 = strictFlag();
        emitInst(ilFn, inst);
        return Value{res, il::Type::Bool};
    }

    if (const auto* idxAccess = dynamic_cast<const ast::IndexAccess*>(&operand)) {
        auto objVal = lowerChainBase(*idxAccess->object, ilFn, /*onSpine=*/true);
        if (!objVal) return std::nullopt;
        auto objBoxed = boxValueIfNeeded(*objVal, ilFn);
        if (idxAccess->optional) emitChainShortCircuit(objBoxed, ilFn);

        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.result = res;
        inst.type = il::Type::Bool;
        inst.immI32 = strictFlag();
        // A literal key folds onto the named form, exactly as the read path
        // folds `o["k"]` onto `o.k` — the same property, so the same op.
        if (const std::optional<uint32_t> literalKey = literalIndexKey(*idxAccess->index)) {
            inst.op = il::Op::PropDelete;
            inst.operands = {objBoxed.id};
            inst.keyIndex = *literalKey;
        } else {
            auto indexVal = lowerExpr(*idxAccess->index, ilFn);
            if (!indexVal) return std::nullopt;
            auto idxBoxed = boxValueIfNeeded(*indexVal, ilFn);
            inst.op = il::Op::ElemDelete;
            inst.operands = {objBoxed.id, idxBoxed.id};
        }
        emitInst(ilFn, inst);
        return Value{res, il::Type::Bool};
    }

    if (const auto* ident = dynamic_cast<const ast::Ident*>(&operand)) {
        // `delete x` names a BINDING, not a property. 13.5.1.1 makes it an
        // early SyntaxError in strict mode, and the sloppy-mode reading — a
        // no-op returning false for anything declared — is not one bronze can
        // even express, since it has no global object to delete from.
        diags_.error(del.span, "unsupported construct: `delete " + ident->name +
                                   "` deletes a binding, which is a SyntaxError in strict "
                                   "mode (delete removes a property: write `delete o." +
                                   ident->name + "`)");
        return std::nullopt;
    }
    if (dynamic_cast<const ast::SuperMember*>(&operand)) {
        diags_.error(del.span, "`delete super.x` is always a ReferenceError (ECMA-262 13.5.1.2)");
        return std::nullopt;
    }

    // Anything else is not a Reference at all: 13.5.1.2 step 3 evaluates the
    // operand for its effects and answers true. `delete f()` calls `f`.
    if (!lowerExpr(operand, ilFn)) return std::nullopt;
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::ConstBool;
    inst.type = il::Type::Bool;
    inst.result = res;
    inst.immI32 = 1;
    emitInst(ilFn, inst);
    return Value{res, il::Type::Bool};
}

std::optional<Lowerer::Value> Lowerer::lowerDelete(const ast::Unary& del, il::Function& ilFn) {
    // `delete a?.b` with a nullish `a` is `true`, not `undefined`: the chain
    // short-circuited, so there was no Reference, and 13.5.1.2's step 2 answers
    // true for that. The generic chain join produces `undefined` on its
    // short-circuit edges, so this is the one place that asks for the other
    // value.
    if (ast::containsOptionalLink(*del.operand)) {
        return lowerChainJoin([&] { return lowerDeleteReference(del, ilFn); }, ChainMiss::True,
                              ilFn);
    }
    return lowerDeleteReference(del, ilFn);
}

std::optional<Lowerer::Value> Lowerer::lowerArrayLit(const ast::ArrayLit* arrLit,
                                                     il::Function& ilFn) {
    // A spread makes the length a runtime fact, so the literal is built by
    // appending rather than by writing indices it cannot name.
    if (listHasSpread(arrLit->elements)) return lowerListToArray(arrLit->elements, ilFn);

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::CreateArray;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.immI32 = static_cast<int32_t>(arrLit->elements.size());
    emitInst(ilFn, inst);

    for (size_t i = 0; i < arrLit->elements.size(); ++i) {
        if (!arrLit->elements[i]) continue;  // Hole (elision) in array literal
        auto elemOpt = lowerExpr(*arrLit->elements[i], ilFn);
        if (!elemOpt) return std::nullopt;
        auto elemBoxed = boxValueIfNeeded(*elemOpt, ilFn);

        uint32_t keyIdx = getKeyConstantIndex(std::to_string(i));
        uint32_t icIdx = icSiteCounter_++;

        // Sloppy for the reason the object literal's writes are: an array
        // literal's elements are defined on a fresh array, never assigned.
        il::Instruction setInst;
        setInst.op = il::Op::PropSet;
        setInst.type = il::Type::Void;
        setInst.result = il::kNoValue;
        setInst.operands = {res, elemBoxed.id};
        setInst.keyIndex = keyIdx;
        setInst.icIndex = icIdx;
        emitInst(ilFn, setInst);
    }
    return Value{res, il::Type::Dynamic};
}

std::optional<Lowerer::Value> Lowerer::lowerNewExpr(const ast::NewExpr* newExpr,
                                                    il::Function& ilFn) {
    // Every callee is an ordinary value: the whole ceremony (prototype,
    // instance shape, receiver, result rule) lives in one runtime helper rather
    // than in codegen. "Is this a constructor" is therefore a check the helper
    // makes on the VALUE, which is what lets the callee be an arbitrary
    // expression at no cost to `new Foo()`: both reach the same `Op::Construct`
    // over one operand, and the only difference is which instructions produced
    // it.
    //
    // Lowered before the arguments, which is the order ECMA-262 13.3.5.1
    // evaluates them in and therefore the order their side effects run.
    auto calleeVal = lowerExpr(*newExpr->callee, ilFn);
    if (!calleeVal) return std::nullopt;

    const bool spreadArgs = listHasSpread(newExpr->args);
    std::vector<il::ValueId> operands;
    operands.push_back(boxValueIfNeeded(*calleeVal, ilFn).id);
    if (spreadArgs) {
        auto argsArr = lowerListToArray(newExpr->args, ilFn);
        if (!argsArr) return std::nullopt;
        operands.push_back(argsArr->id);
    } else {
        for (const auto& argPtr : newExpr->args) {
            auto argVal = lowerExpr(*argPtr, ilFn);
            if (!argVal) return std::nullopt;
            operands.push_back(boxValueIfNeeded(*argVal, ilFn).id);
        }
    }

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = spreadArgs ? il::Op::ConstructSpread : il::Op::Construct;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = std::move(operands);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

std::optional<Lowerer::Value> Lowerer::lowerMemberAccess(const ast::MemberAccess* mem,
                                                         il::Function& ilFn, bool onSpine) {
    auto objVal = lowerChainBase(*mem->object, ilFn, onSpine);
    if (!objVal) return std::nullopt;
    auto objBoxed = boxValueIfNeeded(*objVal, ilFn);
    if (mem->optional) emitChainShortCircuit(objBoxed, ilFn);

    uint32_t keyIdx = getKeyConstantIndex(mem->property);
    uint32_t icIdx = icSiteCounter_++;

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::PropGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {objBoxed.id};
    inst.keyIndex = keyIdx;
    inst.icIndex = icIdx;
    const bool mono = monomorphicPropSite(*mem->object);
    recordPropertyAccess(mem->span.file, mono, mono ? "" : propBailReason(*mem->object));
    inst.icMonomorphic = mono;
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

// `o["k"]` and `o[2]` are property reads whose key is known here, so they fold
// onto the same PropGet — and the same inline cache — that `o.k` uses.
//
// A NUMERIC literal folds only when it is a canonical array index. A property
// key is ToString(Number), and `std::to_string` of an integer is that string
// only for a non-negative integer in the array-index range: `a[1.5]` names the
// property "1.5", so folding it to "1" would read element 1 (`index_keys`).
// Everything else takes the elem path, where the index stays a value and the
// runtime applies the language's own rule to it.
std::optional<uint32_t> Lowerer::literalIndexKey(const ast::Expr& index) {
    if (const auto* strLit = dynamic_cast<const ast::StringLit*>(&index)) {
        return getKeyConstantIndex(strLit->value);
    }
    const auto* numLit = dynamic_cast<const ast::NumberLit*>(&index);
    if (numLit == nullptr) return std::nullopt;
    const double v = numLit->value;
    if (!(v >= 0.0) || v != std::floor(v) || v > 4294967294.0) return std::nullopt;
    return getKeyConstantIndex(std::to_string(static_cast<uint64_t>(v)));
}

std::optional<Lowerer::Value> Lowerer::lowerIndexAccess(const ast::IndexAccess* idxAccess,
                                                        il::Function& ilFn, bool onSpine) {
    auto objVal = lowerChainBase(*idxAccess->object, ilFn, onSpine);
    if (!objVal) return std::nullopt;
    auto objBoxed = boxValueIfNeeded(*objVal, ilFn);
    // Before the INDEX is evaluated: a skipped chain does not run the
    // expression inside its brackets either.
    if (idxAccess->optional) emitChainShortCircuit(objBoxed, ilFn);

    return emitIndexRead(*idxAccess, objBoxed, ilFn);
}

std::optional<Lowerer::Value> Lowerer::emitIndexRead(const ast::IndexAccess& idxAccess,
                                                    Value objBoxed, il::Function& ilFn) {
    const std::optional<uint32_t> literalKey = literalIndexKey(*idxAccess.index);
    if (!literalKey) {
        // Computed index: a real elem.get on the index value.
        recordElementOp(idxAccess.span.file, false, "computed dynamic index");
        auto indexVal = lowerExpr(*idxAccess.index, ilFn);
        if (!indexVal) return std::nullopt;
        auto idxBoxed = boxValueIfNeeded(*indexVal, ilFn);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::ElemGet;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = {objBoxed.id, idxBoxed.id};
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }

    uint32_t icIdx = icSiteCounter_++;
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::PropGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {objBoxed.id};
    inst.keyIndex = *literalKey;
    inst.icIndex = icIdx;
    const bool mono = monomorphicPropSite(*idxAccess.object);
    recordPropertyAccess(idxAccess.span.file, mono, mono ? "" : propBailReason(*idxAccess.object));
    inst.icMonomorphic = mono;
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
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
            // which only the uniform path's wrapper can see. The same exclusion
            // `needsEnv` carries.
            if (it != functionIndices_.end() && !ilModule_.functions[it->second].needsArguments &&
                directCallShapeFits(ilModule_.functions[it->second], call->args.size())) {
                recordCall(call->span.file, true, "");
                uint32_t calleeIdx = it->second;
                const auto& calleeFn = ilModule_.functions[calleeIdx];

                // Synthetic parameters are not source arguments; the arity the
                // program has to match is the source one. The operand list is
                // fixed, because the padding and the leftover array are both
                // built HERE, where the argument count is a compile-time fact —
                // and `directCallShapeFits` above has already established that
                // this call's count can be spelled that way.
                const size_t base = calleeFn.firstSourceParam();
                const size_t fixed = calleeFn.callerParamCount();

                std::vector<il::ValueId> argVals;
                // A plain `f()` has no receiver, so a direct call supplies
                // undefined for `__this`. `__env` cannot be supplied this way,
                // which is why the verifier forbids direct calls to closures
                // outright.
                if (calleeFn.needsThis) {
                    argVals.push_back(emitConstUndefined(ilFn));
                }
                for (size_t i = 0; i < fixed; ++i) {
                    // An omitted optional argument IS `undefined` — that is
                    // the value the callee's default tests for, so a short
                    // direct call and a short uniform one reach the same
                    // prologue with the same bits.
                    if (i >= call->args.size()) {
                        argVals.push_back(emitConstUndefined(ilFn));
                        continue;
                    }
                    auto argVal = lowerExpr(*call->args[i], ilFn);
                    if (!argVal) return std::nullopt;
                    const il::Type paramType = calleeFn.params[i + base].type;
                    if (paramType == il::Type::Dynamic) {
                        argVal = boxValueIfNeeded(*argVal, ilFn);
                    } else if (argVal->type == il::Type::Dynamic) {
                        argVal = unboxValueIfNeeded(*argVal, paramType, ilFn);
                    }
                    argVals.push_back(argVal->id);
                }
                if (calleeFn.hasRestParam) {
                    // Everything past the fixed parameters, as one fresh
                    // array — the value the rest parameter is bound to.
                    il::ValueId restArr = ilFn.valueCount++;
                    il::Instruction createInst;
                    createInst.op = il::Op::CreateArray;
                    createInst.type = il::Type::Dynamic;
                    createInst.result = restArr;
                    // Zero LENGTH, not zero capacity: `create.array n` makes
                    // an array of n undefined elements, so anything built by
                    // appending has to start empty.
                    createInst.immI32 = 0;
                    emitInst(ilFn, createInst);
                    for (size_t i = fixed; i < call->args.size(); ++i) {
                        auto elemVal = lowerExpr(*call->args[i], ilFn);
                        if (!elemVal) return std::nullopt;
                        emitContainerOp(il::Op::ArrayAppend, Value{restArr, il::Type::Dynamic},
                                        *elemVal, ilFn);
                    }
                    argVals.push_back(restArr);
                }

                il::Instruction inst;
                inst.op = il::Op::Call;
                inst.calleeIndex = calleeIdx;
                inst.operands = std::move(argVals);
                inst.type = calleeFn.returnType;

                il::ValueId res = il::kNoValue;
                if (calleeFn.returnType != il::Type::Void) {
                    res = ilFn.valueCount++;
                    inst.result = res;
                } else {
                    inst.result = il::kNoValue;
                }

                emitInst(ilFn, inst);
                if (calleeFn.returnType == il::Type::Void) {
                    // A function that returns no value still EVALUATES to
                    // one: `undefined`. The IL return type stays void so the
                    // call itself costs nothing, and the undefined is
                    // materialized here, where the only thing that can see it
                    // is the expression the call sits in. Handing back the
                    // void call's absent result instead let it escape into
                    // arbitrary expression contexts — `console.log(f())`
                    // reached the verifier as a box of value %kNoValue and
                    // failed to compile at all.
                    return Value{emitConstUndefined(ilFn), il::Type::Dynamic};
                }
                return Value{res, calleeFn.returnType};
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
        emitInst(ilFn, inst);
        calleeVal = Value{getRes, il::Type::Dynamic};
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
