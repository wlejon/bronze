// Binding patterns, parameter defaults and spread.
//
// One seam, because the three are one mechanism seen from three sides: a
// default is a branch on `undefined`, a rest element is "everything left" of
// a walk, and a spread is that same walk feeding a container. Splitting them
// would put the `undefined`-and-only-`undefined` rule in three places.

#include <optional>
#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

namespace {
// The immediate `pattern.check` carries, matching the runtime's kinds.
constexpr int32_t kPatternKindArray = 0;
constexpr int32_t kPatternKindObject = 1;
}  // namespace

// `current === undefined ? <default> : current`, as a real branch.
//
// Two rules are load-bearing and both are visible in this shape. The test is
// STRICT equality with `undefined`, so `f(1, null)` binds `null` and does not
// fire the default — the single most commonly botched rule here. And the
// default is lowered inside its own block, so its side effects happen on
// exactly the calls that omitted the argument and on no others; a select over
// two already-evaluated operands would run them every time.
std::optional<Lowerer::Value> Lowerer::emitDefaultIfUndefined(Value current,
                                                              const ast::Expr& defaultExpr,
                                                              const std::string& bindingName,
                                                              il::Function& ilFn) {
    Value cur = boxValueIfNeeded(current, ilFn);

    il::ValueId undefId = emitConstUndefined(ilFn);
    il::ValueId isUndef = ilFn.valueCount++;
    il::Instruction eqInst;
    eqInst.op = il::Op::StrictEq;
    eqInst.type = il::Type::Bool;
    eqInst.result = isUndef;
    eqInst.operands = {cur.id, undefId};
    emitInst(ilFn, eqInst);

    il::BlockId bDefault = createBlock(ilFn);
    il::BlockId bJoin = createBlock(ilFn);
    const size_t entryBlockIdx = currentBlockIdx_;
    auto statePre = snapshotVarStates();

    setCurrentBlock(bDefault);
    // 8.6.2 through 14.3.3.3 / 15.1.3: the initializer of a SingleNameBinding
    // is a NamedEvaluation position, so an anonymous function here takes the
    // binding's name rather than "".
    auto defOpt = bindingName.empty() ? lowerExpr(defaultExpr, ilFn)
                                      : lowerNamedEvaluation(defaultExpr, bindingName, ilFn);
    if (!defOpt) return std::nullopt;
    auto stateDefault = snapshotVarStates();
    const bool defaultReaches = !currentBlockIsTerminated(ilFn);
    const size_t defaultEndBlockIdx = currentBlockIdx_;

    // The result is always dynamic: the two arms are an argument the caller
    // passed and whatever the default computed, and nothing proves they share
    // an unboxed type.
    il::ValueId resParamId = ilFn.valueCount++;
    ilFn.blocks[bJoin].params.push_back({resParamId, il::Type::Dynamic});
    ExprJoin join = makeExprJoin(statePre, stateDefault, bJoin, ilFn);

    // The skip edge's coercions feed the entry block's branch, so they have
    // to be emitted there rather than in the default's block.
    setCurrentBlock(entryBlockIdx);
    std::vector<il::ValueId> skipArgs{cur.id};
    appendExprJoinArgs(skipArgs, join, statePre, ilFn);

    il::Instruction brInst;
    brInst.op = il::Op::Branch;
    brInst.type = il::Type::Void;
    brInst.result = il::kNoValue;
    brInst.operands = {isUndef};
    brInst.target = il::BlockTarget{.block = bDefault, .args = {}};
    brInst.elseTarget = il::BlockTarget{.block = bJoin, .args = std::move(skipArgs)};
    ilFn.blocks[entryBlockIdx].instructions.push_back(brInst);

    if (defaultReaches) {
        setCurrentBlock(defaultEndBlockIdx);
        Value defBoxed = boxValueIfNeeded(*defOpt, ilFn);
        std::vector<il::ValueId> args{defBoxed.id};
        appendExprJoinArgs(args, join, stateDefault, ilFn);
        il::Instruction jmpInst;
        jmpInst.op = il::Op::Jump;
        jmpInst.type = il::Type::Void;
        jmpInst.result = il::kNoValue;
        jmpInst.target = il::BlockTarget{.block = bJoin, .args = std::move(args)};
        emitInst(ilFn, jmpInst);
    }

    setCurrentBlock(bJoin);
    restoreVarStates(statePre);
    bindExprJoinParams(join);
    return Value{resParamId, il::Type::Dynamic};
}

// The source, checked once before any element is read. It is what lets every
// read below assume a walkable value, and what lets the diagnostic name the
// construct that asked rather than for-of.
Lowerer::Value Lowerer::emitPatternCheck(Value source, bool isObject, il::Function& ilFn) {
    Value boxed = boxValueIfNeeded(source, ilFn);
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::PatternCheck;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {boxed.id};
    inst.immI32 = isObject ? kPatternKindObject : kPatternKindArray;
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

bool Lowerer::bindPatternName(const std::string& name, Value value, const PatternTarget& target,
                              Span span, il::Function& ilFn) {
    Value boxed = boxValueIfNeeded(value, ilFn);
    if (target.declare) {
        if (!declareVariable(name, boxed.type, target.isConst, target.isLet, target.isVar,
                             /*isInitialized=*/true, boxed.id, span)) {
            return false;
        }
        VarBinding& bound = varBindings_[activeVarMap_[name]];
        if (bound.inEnv) {
            emitEnvSet(envDepthOf(bound.envScopeIndex), bound.envSlot, boxed, ilFn);
        }
        return true;
    }
    // An assignment target resolves exactly as `x = v` does: a binding of this
    // function, or a slot in an enclosing scope's environment record. Two
    // resolutions of the same name by different means is how the update path
    // and the assignment path once disagreed.
    auto it = activeVarMap_.find(name);
    if (it != activeVarMap_.end()) {
        writeBinding(varBindings_[it->second], boxed, ilFn);
        return true;
    }
    uint32_t depth = 0;
    uint32_t index = 0;
    if (currentEnvValue_ != il::kNoValue && findEnclosingEnvVar(name, depth, index)) {
        emitEnvSet(depth, index, boxed, ilFn, /*assigning=*/true);
        return true;
    }
    if (!resolvesName(name)) {
        // A destructuring ASSIGNMENT target that names nothing. The source has
        // already been evaluated and destructured, which is the order 13.15.2
        // gives: every PutValue happens after, and this one throws.
        emitReferenceError(name, span, ilFn);
        return true;
    }
    diags_.error(span, "cannot assign to '" + name + "'");
    return false;
}

// The reference half of `({ k: o.a } = src)`, evaluated where the spec puts it:
// before the element this target will receive is read. Nothing is stored yet —
// `storePatternRef` does that once the value (and its default, if any) exists.
std::optional<Lowerer::PatternRef> Lowerer::evalPatternRef(const ast::Expr& target,
                                                           il::Function& ilFn) {
    PatternRef ref;
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(&target)) {
        auto objVal = lowerExpr(*mem->object, ilFn);
        if (!objVal) return std::nullopt;
        ref.object = boxValueIfNeeded(*objVal, ilFn);
        ref.keyIndex = getKeyConstantIndex(mem->property);
        ref.hasKeyIndex = true;
        return ref;
    }
    const auto* idx = dynamic_cast<const ast::IndexAccess*>(&target);
    if (!idx) {
        diags_.error(target.span, "internal: a destructuring target that is not a reference");
        return std::nullopt;
    }
    auto objVal = lowerExpr(*idx->object, ilFn);
    if (!objVal) return std::nullopt;
    ref.object = boxValueIfNeeded(*objVal, ilFn);
    // `o["a"] = v` is the same write as `o.a = v`, and taking the constant-key
    // path here is what lets it share the inline cache rather than falling to
    // the generic element helper.
    if (const std::optional<uint32_t> literalKey = literalIndexKey(*idx->index)) {
        ref.keyIndex = *literalKey;
        ref.hasKeyIndex = true;
        return ref;
    }
    auto indexVal = lowerExpr(*idx->index, ilFn);
    if (!indexVal) return std::nullopt;
    ref.index = boxValueIfNeeded(*indexVal, ilFn);
    return ref;
}

void Lowerer::storePatternRef(const PatternRef& ref, Value value, il::Function& ilFn) {
    Value boxed = boxValueIfNeeded(value, ilFn);
    il::Instruction inst;
    if (ref.hasKeyIndex) {
        inst.op = il::Op::PropSet;
        inst.operands = {ref.object.id, boxed.id};
        inst.keyIndex = ref.keyIndex;
        inst.icIndex = icSiteCounter_++;
    } else {
        inst.op = il::Op::ElemSet;
        inst.operands = {ref.object.id, ref.index.id, boxed.id};
    }
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    // Same rule as an ordinary assignment (13.15.2 PutValue step 6.d): whether
    // a refused Set throws is decided by the strictness of the code that wrote
    // the pattern, not by anything about the pattern.
    inst.immI32 = strictFlag();
    emitInst(ilFn, inst);
}

bool Lowerer::lowerPattern(const ast::BindingPattern& pattern, Value source,
                           const PatternTarget& target, il::Function& ilFn) {
    Value checked = emitPatternCheck(source, pattern.isObject, ilFn);
    return pattern.isObject ? lowerObjectPattern(pattern, checked, target, ilFn)
                            : lowerArrayPattern(pattern, checked, target, ilFn);
}

// `[a, b = 1, [c],...rest]` — 8.6.2 ArrayBindingPattern, which is defined over
// an ITERATOR and not over indices. That is what makes `const [first] = mySet`
// work, and it is why the reads below are a straight-line chain of `iter.step`
// / `iter.value` rather than a cursor threaded through an advance: the record
// holds the cursor, and a string's code-point step lives inside it where it
// belongs.
//
// `iter.step`'s boolean result is deliberately unused: an exhausted record
// answers `undefined`, which is exactly what an element past the end of the
// source must see so that its default can fire.
bool Lowerer::lowerArrayPattern(const ast::BindingPattern& pattern, Value source,
                                const PatternTarget& target, il::Function& ilFn) {
    il::ValueId recId = ilFn.valueCount++;
    il::Instruction openInst;
    openInst.op = il::Op::IterOpen;
    openInst.type = il::Type::Dynamic;
    openInst.result = recId;
    openInst.operands = {source.id};
    emitInst(ilFn, openInst);

    bool sawRest = false;
    for (size_t i = 0; i < pattern.elements.size(); ++i) {
        const auto& elem = pattern.elements[i];

        // 13.15.5.5 step 1: the target's reference is evaluated before the
        // iterator is stepped for it.
        std::optional<PatternRef> ref;
        if (elem.target) {
            ref = evalPatternRef(*elem.target, ilFn);
            if (!ref) return false;
        }

        il::ValueId readId = ilFn.valueCount++;
        if (elem.isRest) {
            // Everything the cursor has left, as one fresh array.
            il::Instruction restInst;
            restInst.op = il::Op::IterRest;
            restInst.type = il::Type::Dynamic;
            restInst.result = readId;
            restInst.operands = {recId};
            emitInst(ilFn, restInst);
            sawRest = true;
        } else {
            il::ValueId stepId = ilFn.valueCount++;
            il::Instruction stepInst;
            stepInst.op = il::Op::IterStep;
            stepInst.type = il::Type::Bool;
            stepInst.result = stepId;
            stepInst.operands = {recId};
            emitInst(ilFn, stepInst);

            il::Instruction readInst;
            readInst.op = il::Op::IterValue;
            readInst.type = il::Type::Dynamic;
            readInst.result = readId;
            readInst.operands = {recId};
            emitInst(ilFn, readInst);
        }

        Value value{readId, il::Type::Dynamic};
        if (elem.defaultValue) {
            auto withDefault = emitDefaultIfUndefined(
                value, *elem.defaultValue, elem.pattern ? std::string{} : elem.name, ilFn);
            if (!withDefault) return false;
            value = *withDefault;
        }
        if (elem.pattern) {
            if (!lowerPattern(*elem.pattern, value, target, ilFn)) return false;
        } else if (ref) {
            storePatternRef(*ref, value, ilFn);
        } else if (!elem.name.empty()) {
            if (!bindPatternName(elem.name, value, target, elem.span, ilFn)) return false;
        }
        if (elem.isRest) break;
    }
    // 8.6.2 step 5: a pattern that stopped before the iterator was exhausted
    // closes it. A rest element drained it, so there is nothing left to close
    // — and every fast kind treats this as a no-op, so an array destructuring
    // pays one call and no user code.
    if (!sawRest) emitIterClose(recId, /*suppress=*/false, ilFn);
    return true;
}

// `{ x, y: renamed, z = 5, [k]: c,...others }`. Read by key, so each element is
// an ordinary property read — the same PropGet a `.x` would emit, computed keys
// taking the element path exactly as they do in an object literal.
bool Lowerer::lowerObjectPattern(const ast::BindingPattern& pattern, Value source,
                                 const PatternTarget& target, il::Function& ilFn) {
    bool hasRest = false;
    for (const auto& elem : pattern.elements) hasRest = hasRest || elem.isRest;

    // The keys a `...rest` must NOT copy. An array rather than a compile-time
    // list because a computed key is not known until the pattern runs, and
    // the two kinds of key have to end up in one place.
    Value excluded{il::kNoValue, il::Type::Dynamic};
    if (hasRest) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::CreateArray;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.immI32 = 0;
        emitInst(ilFn, inst);
        excluded = Value{res, il::Type::Dynamic};
    }

    for (const auto& elem : pattern.elements) {
        if (elem.isRest) {
            std::optional<PatternRef> ref;
            if (elem.target) {
                ref = evalPatternRef(*elem.target, ilFn);
                if (!ref) return false;
            }

            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::ObjectRest;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.operands = {source.id, excluded.id};
            emitInst(ilFn, inst);

            Value restVal{res, il::Type::Dynamic};
            if (elem.pattern) {
                if (!lowerPattern(*elem.pattern, restVal, target, ilFn)) return false;
            } else if (ref) {
                storePatternRef(*ref, restVal, ilFn);
            } else if (!elem.name.empty()) {
                if (!bindPatternName(elem.name, restVal, target, elem.span, ilFn)) {
                    return false;
                }
            }
            break;
        }

        il::ValueId readId = ilFn.valueCount++;
        il::Instruction readInst;
        if (elem.keyExpr) {
            recordElementOp(elem.span.file, false, "computed dynamic index");
            auto keyOpt = lowerExpr(*elem.keyExpr, ilFn);
            if (!keyOpt) return false;
            Value keyBoxed = boxValueIfNeeded(*keyOpt, ilFn);
            readInst.op = il::Op::ElemGet;
            readInst.operands = {source.id, keyBoxed.id};
            if (hasRest) emitContainerOp(il::Op::ArrayAppend, excluded, keyBoxed, ilFn);
        } else {
            recordPropertyAccess(elem.span.file, false, "destructuring read");
            readInst.op = il::Op::PropGet;
            readInst.operands = {source.id};
            readInst.keyIndex = getKeyConstantIndex(elem.key);
            readInst.icIndex = icSiteCounter_++;
            readInst.icMonomorphic = false;
            if (hasRest) {
                il::ValueId keyStr = ilFn.valueCount++;
                il::Instruction keyInst;
                keyInst.op = il::Op::Box;
                keyInst.type = il::Type::Dynamic;
                keyInst.boxType = il::Type::Str;
                keyInst.result = keyStr;
                keyInst.keyIndex = getKeyConstantIndex(elem.key);
                emitInst(ilFn, keyInst);
                emitContainerOp(il::Op::ArrayAppend, excluded,
                                Value{keyStr, il::Type::Dynamic}, ilFn);
            }
        }
        // 13.15.5.6 KeyedDestructuringAssignmentEvaluation step 1: the target's
        // reference is evaluated after the computed key above and before the
        // GetV below, so it is lowered between the two rather than at either.
        std::optional<PatternRef> ref;
        if (elem.target) {
            ref = evalPatternRef(*elem.target, ilFn);
            if (!ref) return false;
        }

        readInst.type = il::Type::Dynamic;
        readInst.result = readId;
        emitInst(ilFn, readInst);

        Value value{readId, il::Type::Dynamic};
        if (elem.defaultValue) {
            auto withDefault = emitDefaultIfUndefined(
                value, *elem.defaultValue, elem.pattern ? std::string{} : elem.name, ilFn);
            if (!withDefault) return false;
            value = *withDefault;
        }
        if (elem.pattern) {
            if (!lowerPattern(*elem.pattern, value, target, ilFn)) return false;
        } else if (ref) {
            storePatternRef(*ref, value, ilFn);
        } else if (!bindPatternName(elem.name, value, target, elem.span, ilFn)) {
            return false;
        }
    }
    return true;
}

// `[a, b] = [b, a]`. The whole right side is evaluated BEFORE any target is
// written, which is what makes the swap a swap; the pattern walk below reads
// only the value it was handed.
std::optional<Lowerer::Value> Lowerer::lowerDestructuringAssign(
    const ast::DestructuringAssign* node, il::Function& ilFn) {
    auto valueOpt = lowerExpr(*node->value, ilFn);
    if (!valueOpt) return std::nullopt;
    Value boxed = boxValueIfNeeded(*valueOpt, ilFn);

    PatternTarget target;
    target.declare = false;
    if (!lowerPattern(*node->pattern, boxed, target, ilFn)) return std::nullopt;
    // An assignment expression evaluates to the value assigned, which for a
    // destructuring assignment is the whole right-hand side.
    return boxed;
}

void Lowerer::emitContainerOp(il::Op op, Value container, Value value, il::Function& ilFn) {
    il::Instruction inst;
    inst.op = op;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {container.id, boxValueIfNeeded(value, ilFn).id};
    emitInst(ilFn, inst);
}

void Lowerer::applyParamShape(const std::vector<ast::Param>& params, il::Function& fn) {
    fn.hasRestParam = !params.empty() && params.back().isRest;
    // Everything from the first optional parameter on may be omitted, and a
    // parameter after a defaulted one is optional too: the caller has no way
    // to skip the default and supply the later one.
    uint32_t required = 0;
    for (const auto& param : params) {
        if (param.defaultValue || param.isRest) break;
        required++;
    }
    fn.requiredArgs = required;
}

bool Lowerer::listHasSpread(const std::vector<ast::ExprPtr>& list) {
    for (const auto& e : list) {
        if (dynamic_cast<const ast::SpreadElement*>(e.get()) != nullptr) return true;
    }
    return false;
}

// The elements of a list, spreads expanded, as one array. This is what a call
// with a spread argument passes and what an array literal with one builds:
// the length is not known where the code is generated, so the container is
// grown rather than indexed.
std::optional<Lowerer::Value> Lowerer::lowerListToArray(const std::vector<ast::ExprPtr>& list,
                                                        il::Function& ilFn) {
    il::ValueId arr = ilFn.valueCount++;
    il::Instruction createInst;
    createInst.op = il::Op::CreateArray;
    createInst.type = il::Type::Dynamic;
    createInst.result = arr;
    createInst.immI32 = 0;
    emitInst(ilFn, createInst);
    Value container{arr, il::Type::Dynamic};

    for (const auto& elemPtr : list) {
        if (!elemPtr) {
            il::ValueId undef = emitConstUndefined(ilFn);
            emitContainerOp(il::Op::ArrayAppend, container, Value{undef, il::Type::Dynamic}, ilFn);
            continue;
        }
        const auto* spread = dynamic_cast<const ast::SpreadElement*>(elemPtr.get());
        const ast::Expr& source = spread ? *spread->argument : *elemPtr;
        auto valOpt = lowerExpr(source, ilFn);
        if (!valOpt) return std::nullopt;
        emitContainerOp(spread ? il::Op::ArraySpread : il::Op::ArrayAppend, container, *valOpt,
                        ilFn);
    }
    return container;
}

// One parameter list, bound left to right into the function's own scope.
//
// The order is the semantics: `function later(a, b = a * 2)` needs `a` bound
// before `b`'s default runs, so a default is a piece of code evaluated in the
// parameter scope at CALL time and not a constant stored at definition.
bool Lowerer::lowerParamBindings(const std::vector<ast::Param>& params, uint32_t paramBase,
                                 il::Function& ilFn) {
    for (uint32_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        const il::ValueId argId = i + paramBase;
        Value value{argId, ilFn.params[argId].type};

        // A rest parameter never takes a default and never omits: it arrives as
        // an array the calling convention built.
        if (param.defaultValue) {
            auto withDefault = emitDefaultIfUndefined(
                value, *param.defaultValue, param.pattern ? std::string{} : param.name, ilFn);
            if (!withDefault) return false;
            value = *withDefault;
        }

        if (param.pattern) {
            PatternTarget target;
            target.declare = true;
            target.isLet = false;  // a parameter binding is neither let nor var
            if (!lowerPattern(*param.pattern, value, target, ilFn)) return false;
            continue;
        }

        if (!declareVariable(param.name, value.type, /*isConst=*/false, /*isLet=*/false,
                             /*isVar=*/false, /*isInitialized=*/true, value.id, param.span)) {
            return false;
        }
        // A captured parameter arrives in a register; copy it into its
        // environment slot so closures see the same binding.
        VarBinding& bound = varBindings_[activeVarMap_[param.name]];
        if (bound.inEnv) {
            emitEnvSet(envDepthOf(bound.envScopeIndex), bound.envSlot, value, ilFn);
        }
    }
    return true;
}

}  // namespace bronze::lower
