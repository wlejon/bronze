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

// `X.p` where `X` is a module-scope object literal whose accessor `p` the
// whole-program proof in types/module_literal.h certified: the key the getter
// would have read, so the site reads that instead of calling the accessor.
//
// A different KEY at the same site, and not a different site. The receiver
// expression is untouched, evaluated once, in the same place, and the read that
// replaces the call is the read the getter's own body performed — so `_q`
// deleted, `_q` turned into an accessor, or `_q` answered off the prototype all
// still answer the way the call would have answered.
//
// Reads only. A write through `X.p` must run the setter, and does: nothing here
// touches the assignment path.
const std::string* devirtualizedKey(const types::InferenceResult* inference,
                                    const ast::Expr& receiver, const std::string& property) {
    if (inference == nullptr) return nullptr;
    const auto* id = dynamic_cast<const ast::Ident*>(&receiver);
    if (id == nullptr) return nullptr;
    return inference->moduleLiterals.backingKey(id->name, property);
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
    // `o.#x` reads no property and consults no inline cache: a private element
    // has no shape and no slot (lower_private.cpp).
    if (mem->isPrivate) return lowerPrivateRead(*mem, objBoxed, ilFn);

    const std::string* forwarded = devirtualizedKey(inference_, *mem->object, mem->property);
    uint32_t keyIdx = getKeyConstantIndex(forwarded != nullptr ? *forwarded : mem->property);
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
    inst.icFnRecv = functionBindingReceiver(*mem->object, keyIdx);
    stampStaticSlot(inst, *mem->object);
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
    std::optional<uint32_t> literalKey = literalIndexKey(*idxAccess.index);
    // `X["p"]` names the same property `X.p` does, so it takes the same
    // forwarding; a computed index names nothing this pass can read.
    if (const auto* keyLit = dynamic_cast<const ast::StringLit*>(idxAccess.index.get())) {
        if (const std::string* fwd = devirtualizedKey(inference_, *idxAccess.object, keyLit->value)) {
            literalKey = getKeyConstantIndex(*fwd);
        }
    }
    if (!literalKey) {
        // Computed index: a real elem.get on the index value.
        const bool native = provenArrayOrTypedArray(*idxAccess.object);
        recordElementOp(idxAccess.span.file, native, native ? "" : "computed dynamic index");
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
    inst.icFnRecv = functionBindingReceiver(*idxAccess.object, *literalKey);
    stampStaticSlot(inst, *idxAccess.object);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

}  // namespace bronze::lower
