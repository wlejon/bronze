// Expression lowering: the dispatcher, literals, identifier resolution and
// the unary operators. The forms with their own seams delegate to
// lower_expr_binary.cpp (the binary operator families),
// lower_expr_assign.cpp (everything that writes through a reference),
// lower_expr_cond.cpp (short-circuit joins) and lower_object.cpp (objects,
// property access, new, calls).

#include <limits>
#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

std::optional<Lowerer::Value> Lowerer::lowerExpr(const ast::Expr& expr, il::Function& ilFn) {
    // Is this expression the BASE of a link in a chain already being lowered?
    // The flag is consumed here and re-set only for the base position of the
    // three link forms, so a `?.` written anywhere else — inside an argument,
    // inside an index — starts a chain of its own rather than short-circuiting
    // the enclosing one.
    const bool onSpine = spinePos_;
    spinePos_ = false;
    if (!onSpine && ast::containsOptionalLink(expr)) {
        return lowerOptionalChain(expr, ilFn);
    }

    if (const auto* numLit = dynamic_cast<const ast::NumberLit*>(&expr)) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::ConstF64;
        inst.type = il::Type::F64;
        inst.result = res;
        inst.immF64 = numLit->value;
        emitInst(ilFn, inst);
        return Value{res, il::Type::F64};
    }

    // A BigInt literal is a BOXED value and always has been one — there is no
    // unboxed BigInt in the IL, the way there is an unboxed f64. That is what
    // guarantees a typed numeric path can never meet one: the only type it can
    // have is Dynamic, so every operator over it takes the dynamic route.
    if (const auto* bigLit = dynamic_cast<const ast::BigIntLit*>(&expr)) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::ConstBigInt;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.keyIndex = getKeyConstantIndex(bigLit->digits);
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }

    if (const auto* boolLit = dynamic_cast<const ast::BoolLit*>(&expr)) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::ConstBool;
        inst.type = il::Type::Bool;
        inst.result = res;
        inst.immI32 = boolLit->value ? 1 : 0;
        emitInst(ilFn, inst);
        return Value{res, il::Type::Bool};
    }

    if (dynamic_cast<const ast::NullLit*>(&expr)) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::ConstNull;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }

    if (dynamic_cast<const ast::UndefinedLit*>(&expr)) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::ConstUndefined;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }

    if (dynamic_cast<const ast::ThisExpr*>(&expr)) {
        return lowerThisValue(expr.span, ilFn);
    }

    if (const auto* superCall = dynamic_cast<const ast::SuperCall*>(&expr)) {
        return lowerSuperCall(superCall, ilFn);
    }

    if (const auto* superMem = dynamic_cast<const ast::SuperMember*>(&expr)) {
        return lowerSuperMember(superMem, ilFn);
    }

    if (const auto* strLit = dynamic_cast<const ast::StringLit*>(&expr)) {
        uint32_t keyIdx = getKeyConstantIndex(strLit->value);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::Box;
        inst.type = il::Type::Dynamic;
        inst.boxType = il::Type::Str;
        inst.result = res;
        inst.keyIndex = keyIdx;
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }

    // A regular expression literal is `new RegExp(pattern, flags)`, spelled
    // out. It is a CONSTRUCTION and not a constant because 22.2.4.1 evaluates
    // the literal to a fresh object every time it is reached — two evaluations
    // of the same literal in a loop must not share a `lastIndex` — and it goes
    // through the provided-global path rather than through a `new` in the
    // source so that a program's own binding named `RegExp` cannot change what
    // a literal means (22.2.4.1 reads the intrinsic, not the binding).
    if (const auto* re = dynamic_cast<const ast::RegExpLit*>(&expr)) {
        auto emitStr = [&](const std::string& text) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::Box;
            inst.type = il::Type::Dynamic;
            inst.boxType = il::Type::Str;
            inst.result = res;
            inst.keyIndex = getKeyConstantIndex(text);
            emitInst(ilFn, inst);
            return res;
        };
        il::ValueId ctor = ilFn.valueCount++;
        il::Instruction ctorInst;
        ctorInst.op = il::Op::GlobalGet;
        ctorInst.type = il::Type::Dynamic;
        ctorInst.result = ctor;
        ctorInst.keyIndex = getKeyConstantIndex("RegExp");
        emitInst(ilFn, ctorInst);

        const il::ValueId patternId = emitStr(re->pattern);
        const il::ValueId flagsId = emitStr(re->flags);

        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::Construct;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = {ctor, patternId, flagsId};
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }

    // A template is left-to-right concatenation starting from its first
    // piece. `add.dynamic` is exactly the operation the language specifies —
    // ToString of whatever the substitution produced, because the left side
    // is already a string — so no separate ToString op is needed.
    //
    // The leading piece is emitted even when it is empty, and that is not a
    // missed optimization: `${a}${b}` without it would be `a + b`, which for
    // two numbers is addition rather than concatenation. Every LATER empty
    // piece is skipped, because by then the accumulator is a string and the
    // operation cannot change meaning.
    if (const auto* tmpl = dynamic_cast<const ast::TemplateLit*>(&expr)) {
        auto emitStr = [&](const std::string& text) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::Box;
            inst.type = il::Type::Dynamic;
            inst.boxType = il::Type::Str;
            inst.result = res;
            inst.keyIndex = getKeyConstantIndex(text);
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        };
        auto emitConcat = [&](Value lhs, Value rhs) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::Add;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.operands = {lhs.id, rhs.id};
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        };
        // 13.2.8.6 spells a substitution as ToString, which is NOT the `+`
        // below with a string on the left. Both reach ToPrimitive for an
        // object operand and they ask it for different hints — ToString asks
        // for "string" and 13.15.3 asks for none — so `${o}` and `'' + o`
        // disagree for an object that defines both `toString` and `valueOf`,
        // and the `+` chain alone would have answered `valueOf`'s.
        auto emitToString = [&](Value v) {
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::ToStr;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.operands = {v.id};
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        };

        Value acc = emitStr(tmpl->quasis.empty() ? std::string() : tmpl->quasis[0]);
        for (size_t i = 0; i < tmpl->exprs.size(); ++i) {
            auto sub = lowerExpr(*tmpl->exprs[i], ilFn);
            if (!sub) return std::nullopt;
            acc = emitConcat(acc, emitToString(boxValueIfNeeded(*sub, ilFn)));
            if (i + 1 < tmpl->quasis.size() && !tmpl->quasis[i + 1].empty()) {
                acc = emitConcat(acc, emitStr(tmpl->quasis[i + 1]));
            }
        }
        return acc;
    }

    if (const auto* tagged = dynamic_cast<const ast::TaggedTemplate*>(&expr)) {
        const auto& tpl = *tagged->templateLit;
        il::ValueId cookedArr = ilFn.valueCount++;
        il::Instruction createCooked;
        createCooked.op = il::Op::CreateArray;
        createCooked.type = il::Type::Dynamic;
        createCooked.result = cookedArr;
        createCooked.immI32 = static_cast<int32_t>(tpl.quasis.size());
        emitInst(ilFn, createCooked);

        for (size_t i = 0; i < tpl.quasis.size(); ++i) {
            uint32_t keyIdx = getKeyConstantIndex(tpl.quasis[i]);
            il::ValueId strId = ilFn.valueCount++;
            il::Instruction sInst;
            sInst.op = il::Op::Box;
            sInst.type = il::Type::Dynamic;
            sInst.boxType = il::Type::Str;
            sInst.result = strId;
            sInst.keyIndex = keyIdx;
            emitInst(ilFn, sInst);

            il::Instruction setInst;
            setInst.op = il::Op::PropSet;
            setInst.type = il::Type::Void;
            setInst.result = il::kNoValue;
            setInst.operands = {cookedArr, strId};
            setInst.keyIndex = getKeyConstantIndex(std::to_string(i));
            setInst.icIndex = icSiteCounter_++;
            emitInst(ilFn, setInst);
        }

        il::ValueId rawArr = ilFn.valueCount++;
        il::Instruction createRaw;
        createRaw.op = il::Op::CreateArray;
        createRaw.type = il::Type::Dynamic;
        createRaw.result = rawArr;
        createRaw.immI32 = static_cast<int32_t>(tpl.rawQuasis.size());
        emitInst(ilFn, createRaw);

        for (size_t i = 0; i < tpl.rawQuasis.size(); ++i) {
            uint32_t keyIdx = getKeyConstantIndex(tpl.rawQuasis[i]);
            il::ValueId strId = ilFn.valueCount++;
            il::Instruction sInst;
            sInst.op = il::Op::Box;
            sInst.type = il::Type::Dynamic;
            sInst.boxType = il::Type::Str;
            sInst.result = strId;
            sInst.keyIndex = keyIdx;
            emitInst(ilFn, sInst);

            il::Instruction setInst;
            setInst.op = il::Op::PropSet;
            setInst.type = il::Type::Void;
            setInst.result = il::kNoValue;
            setInst.operands = {rawArr, strId};
            setInst.keyIndex = getKeyConstantIndex(std::to_string(i));
            setInst.icIndex = icSiteCounter_++;
            emitInst(ilFn, setInst);
        }

        il::ValueId templateObj = ilFn.valueCount++;
        il::Instruction tplInst;
        tplInst.op = il::Op::TemplateObject;
        tplInst.type = il::Type::Dynamic;
        tplInst.result = templateObj;
        tplInst.operands = {cookedArr, rawArr};
        emitInst(ilFn, tplInst);

        auto tagVal = lowerExpr(*tagged->tag, ilFn);
        if (!tagVal) return std::nullopt;
        auto tagBoxed = boxValueIfNeeded(*tagVal, ilFn);

        std::vector<il::ValueId> callOperands;
        callOperands.push_back(tagBoxed.id);
        callOperands.push_back(emitConstUndefined(ilFn));
        callOperands.push_back(templateObj);

        for (const auto& subExpr : tpl.exprs) {
            auto subVal = lowerExpr(*subExpr, ilFn);
            if (!subVal) return std::nullopt;
            callOperands.push_back(boxValueIfNeeded(*subVal, ilFn).id);
        }

        il::ValueId callRes = ilFn.valueCount++;
        il::Instruction callInst;
        callInst.op = il::Op::DynamicCall;
        callInst.type = il::Type::Dynamic;
        callInst.result = callRes;
        callInst.operands = std::move(callOperands);
        emitInst(ilFn, callInst);
        return Value{callRes, il::Type::Dynamic};
    }

    if (dynamic_cast<const ast::NewTargetExpr*>(&expr)) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::GetNewTarget;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }

    if (const auto* objLit = dynamic_cast<const ast::ObjectLit*>(&expr)) {
        return lowerObjectLit(objLit, ilFn);
    }

    if (const auto* destr = dynamic_cast<const ast::DestructuringAssign*>(&expr)) {
        return lowerDestructuringAssign(destr, ilFn);
    }

    // A spread reaching the dispatcher is a spread somewhere no container is
    // being built: every legal position handles it before lowering the
    // element, so this is the "'...' here means nothing" case.
    if (const auto* spread = dynamic_cast<const ast::SpreadElement*>(&expr)) {
        diags_.error(spread->span,
                     "unsupported construct: '...' outside an argument list, an array "
                     "literal or an object literal");
        return std::nullopt;
    }

    if (const auto* arrLit = dynamic_cast<const ast::ArrayLit*>(&expr)) {
        return lowerArrayLit(arrLit, ilFn);
    }

    if (const auto* fnExpr = dynamic_cast<const ast::FunctionExpr*>(&expr)) {
        // Reached only where NO surrounding syntax named it — every
        // NamedEvaluation position goes through `lowerNamedEvaluation` instead —
        // so an anonymous function here really has `name === ""` (10.2.9 step 4
        // via OrdinaryFunctionCreate's default).
        return lowerClosure(*fnExpr, fnExpr->name, fnExpr->name, fnExpr->params,
                            fnExpr->returnType, fnExpr->body, fnExpr->span, ilFn,
                            fnExpr->isArrow);
    }

    if (const auto* clsExpr = dynamic_cast<const ast::ClassExpr*>(&expr)) {
        return lowerClassExpr(clsExpr, ilFn);
    }

    if (const auto* yield = dynamic_cast<const ast::YieldExpr*>(&expr)) {
        if (yield->isAwait) return lowerAwait(*yield, ilFn);
        return yield->delegate ? lowerYieldStar(*yield, ilFn) : lowerYield(*yield, ilFn);
    }
    if (const auto* di = dynamic_cast<const ast::DynamicImportExpr*>(&expr)) {
        auto specOpt = lowerExpr(*di->specifier, ilFn);
        if (!specOpt) return std::nullopt;
        Value specVal = boxValueIfNeeded(*specOpt, ilFn);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::DynamicImport;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = {specVal.id};
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }
    if (const auto* ident = dynamic_cast<const ast::Ident*>(&expr)) {
        auto it = activeVarMap_.find(ident->name);
        if (it == activeVarMap_.end()) {
            // Global value properties (shadowable by local declarations,
            // hence checked only after the variable lookup misses).
            if (ident->name == "NaN" || ident->name == "Infinity") {
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::ConstF64;
                inst.type = il::Type::F64;
                inst.result = res;
                inst.immF64 = ident->name == "NaN"
                                  ? std::numeric_limits<double>::quiet_NaN()
                                  : std::numeric_limits<double>::infinity();
                emitInst(ilFn, inst);
                return Value{res, il::Type::F64};
            }
            if (ident->name == "undefined") {
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::ConstUndefined;
                inst.type = il::Type::Dynamic;
                inst.result = res;
                emitInst(ilFn, inst);
                return Value{res, il::Type::Dynamic};
            }
            // A free variable of a nested function: resolved against the
            // environments of the enclosing scopes.
            uint32_t depth = 0;
            uint32_t index = 0;
            if (currentEnvValue_ != il::kNoValue &&
                findEnclosingEnvVar(ident->name, depth, index)) {
                return emitEnvGet(depth, index, ilFn);
            }
            // A top-level function declaration used as a value rather than
            // called: `new Point(...)`, `Point.prototype`, passing it around.
            // One object per declaration, so decorating it in one place is
            // visible in every other.
            auto fnIt = functionIndices_.find(ident->name);
            if (fnIt != functionIndices_.end()) {
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::FunctionRef;
                inst.type = il::Type::Dynamic;
                inst.result = res;
                inst.calleeIndex = fnIt->second;
                emitInst(ilFn, inst);
                return Value{res, il::Type::Dynamic};
            }
            // A global bronze provides. The set is closed and checked HERE, at
            // compile time, so an unknown free identifier is a compile error
            // rather than a runtime miss a program could feature-test its way
            // around.
            if (isProvidedGlobal(ident->name)) {
                il::ValueId res = ilFn.valueCount++;
                il::Instruction inst;
                inst.op = il::Op::GlobalGet;
                inst.type = il::Type::Dynamic;
                inst.result = res;
                inst.keyIndex = getKeyConstantIndex(ident->name);
                emitInst(ilFn, inst);
                return Value{res, il::Type::Dynamic};
            }
            // Nothing in the ladder claims this name, and nothing here can know
            // whether the running environment does. 6.2.5.5 GetValue step 2.
            return emitReferenceError(ident->name, ident->span, ilFn);
        }
        const auto& b = varBindings_[it->second];
        // An uninitialized binding whose slot carries the marker is the
        // language's dead zone, and 9.1.1.1.6 answers it at the moment of the
        // READ — so it is lowered, not refused. What is left here is bronze's
        // own gap: a binding with no value and no slot to hold a marker in,
        // which no run can turn into an answer.
        const bool tdzChecked =
            b.inEnv && envSlotIsLexical(envDepthOf(b.envScopeIndex), b.envSlot);
        if (!b.isInitialized && !tdzChecked) {
            if (b.isConst) {
                diags_.error(ident->span, "use of 'const' binding before initialization");
            } else {
                diags_.error(ident->span, "use of 'let' binding before initialization");
            }
            return std::nullopt;
        }
        return readBinding(b, ilFn);
    }

    if (const auto* un = dynamic_cast<const ast::Unary*>(&expr)) {
        if (un->op == ast::UnaryOp::Not) {
            Value condVal = lowerCondition(*un->operand, ilFn);
            il::ValueId falseVal = ilFn.valueCount++;
            il::Instruction falseInst;
            falseInst.op = il::Op::ConstBool;
            falseInst.type = il::Type::Bool;
            falseInst.result = falseVal;
            falseInst.immI32 = 0;
            emitInst(ilFn, falseInst);

            il::ValueId res = ilFn.valueCount++;
            il::Instruction cmpInst;
            cmpInst.op = il::Op::CmpEq;
            cmpInst.type = il::Type::Bool;
            cmpInst.result = res;
            cmpInst.operands = {condVal.id, falseVal};
            emitInst(ilFn, cmpInst);
            return Value{res, il::Type::Bool};
        }
        if (un->op == ast::UnaryOp::Negate) {
            auto valOpt = lowerExpr(*un->operand, ilFn);
            if (!valOpt) return std::nullopt;
            // A boxed operand keeps its box: 13.5.5 is ToNumeric, not
            // ToNumber, so `-(1n)` is a BigInt and unboxing to f64 would have
            // thrown on the way to producing the wrong type.
            if (valOpt->type == il::Type::Dynamic) {
                il::ValueId dres = ilFn.valueCount++;
                il::Instruction dinst;
                dinst.op = il::Op::Neg;
                dinst.type = il::Type::Dynamic;
                dinst.result = dres;
                dinst.operands = {valOpt->id};
                emitInst(ilFn, dinst);
                return Value{dres, il::Type::Dynamic};
            }
            Value val = unboxValueIfNeeded(*valOpt, il::Type::F64, ilFn);
            // Negation, not `0 - x`: IEEE-754 makes 0 - 0 positive zero, so the
            // subtraction spelling printed `-0` as `0` — the sign of zero is
            // observable, and `Object.is(-0, 0)` is false in the language.
            il::ValueId res = ilFn.valueCount++;
            il::Instruction negInst;
            negInst.op = il::Op::Neg;
            negInst.type = il::Type::F64;
            negInst.result = res;
            negInst.operands = {val.id};
            emitInst(ilFn, negInst);
            return Value{res, il::Type::F64};
        }
        if (un->op == ast::UnaryOp::Posate) {
            auto valOpt = lowerExpr(*un->operand, ilFn);
            if (!valOpt) return std::nullopt;
            return unboxValueIfNeeded(*valOpt, il::Type::F64, ilFn);
        }
        if (un->op == ast::UnaryOp::BitNot) {
            auto valOpt = lowerExpr(*un->operand, ilFn);
            if (!valOpt) return std::nullopt;
            // A boxed operand takes the one op that exists for it. The xor
            // spelling below is a MIXING TypeError on a BigInt — -1 is a
            // Number — so `~x` for an untyped `x` cannot be written as one.
            if (valOpt->type == il::Type::Dynamic) {
                il::ValueId dres = ilFn.valueCount++;
                il::Instruction dinst;
                dinst.op = il::Op::BitNot;
                dinst.type = il::Type::Dynamic;
                dinst.result = dres;
                dinst.operands = {valOpt->id};
                emitInst(ilFn, dinst);
                return Value{dres, il::Type::Dynamic};
            }
            // `~x` is -ToInt32(x) - 1, which is the one's complement, which
            // is `x ^ -1`. Spelled as the xor rather than as its own op:
            // the language defines it over the same ToInt32'd int32 the
            // binary operators use, so it is that family and not a new one.
            il::ValueId allOnes = ilFn.valueCount++;
            il::Instruction onesInst;
            onesInst.op = il::Op::ConstI32;
            onesInst.type = il::Type::I32;
            onesInst.result = allOnes;
            onesInst.immI32 = -1;
            emitInst(ilFn, onesInst);
            return emitBitwise(il::Op::BitXor, *valOpt, Value{allOnes, il::Type::I32}, ilFn);
        }
        if (un->op == ast::UnaryOp::TypeOf) {
            // 13.5.3 step 1: `typeof` of an UNRESOLVABLE reference is the
            // string "undefined", not an error and not a throw. This is the
            // universal feature-detection idiom, and it is the one position in
            // the language where a free name is not a question about the
            // environment — so it gets no warning either. Bare form only:
            // `typeof x.y` still evaluates `x` and throws.
            if (const auto* operandIdent = dynamic_cast<const ast::Ident*>(un->operand.get());
                operandIdent && !resolvesName(operandIdent->name)) {
                il::ValueId res = ilFn.valueCount++;
                il::Instruction strInst;
                strInst.op = il::Op::Box;
                strInst.type = il::Type::Dynamic;
                strInst.boxType = il::Type::Str;
                strInst.result = res;
                strInst.keyIndex = getKeyConstantIndex("undefined");
                emitInst(ilFn, strInst);
                return Value{res, il::Type::Dynamic};
            }
            auto valOpt = lowerExpr(*un->operand, ilFn);
            if (!valOpt) return std::nullopt;
            Value boxed = boxValueIfNeeded(*valOpt, ilFn);
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::TypeOf;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            inst.operands = {boxed.id};
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        }
        if (un->op == ast::UnaryOp::Void) {
            // The operand is evaluated and its value dropped, which in SSA
            // is simply not using it. What `void` contributes is the
            // undefined, and it contributes nothing else.
            if (!lowerExpr(*un->operand, ilFn)) return std::nullopt;
            il::ValueId res = ilFn.valueCount++;
            il::Instruction inst;
            inst.op = il::Op::ConstUndefined;
            inst.type = il::Type::Dynamic;
            inst.result = res;
            emitInst(ilFn, inst);
            return Value{res, il::Type::Dynamic};
        }
        if (un->op == ast::UnaryOp::Delete) return lowerDelete(*un, ilFn);
        if (un->op == ast::UnaryOp::PreInc || un->op == ast::UnaryOp::PreDec ||
            un->op == ast::UnaryOp::PostInc || un->op == ast::UnaryOp::PostDec) {
            return lowerUpdate(*un, ilFn);
        }
        // Every `UnaryOp` above is handled, so this is unreachable today —
        // and that is exactly why it is here. Falling out of this `if` drops
        // into the dispatcher's generic "unsupported AST expression" at the
        // bottom, which names nothing; the house rule is that an unimplemented
        // construct is diagnosed BY NAME, and the next operator added to the
        // enum must not silently inherit the anonymous error.
        diags_.error(un->span, "unsupported unary operator: " +
                                   std::string(ast::unaryOpName(un->op)));
        return std::nullopt;
    }

    if (const auto* tern = dynamic_cast<const ast::Ternary*>(&expr)) {
        return lowerTernary(tern, ilFn);
    }

    if (const auto* newExpr = dynamic_cast<const ast::NewExpr*>(&expr)) {
        return lowerNewExpr(newExpr, ilFn);
    }

    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(&expr)) {
        return lowerMemberAccess(mem, ilFn, onSpine);
    }

    if (const auto* idxAccess = dynamic_cast<const ast::IndexAccess*>(&expr)) {
        return lowerIndexAccess(idxAccess, ilFn, onSpine);
    }

    if (const auto* bin = dynamic_cast<const ast::Binary*>(&expr)) {
        return lowerBinary(bin, ilFn);
    }

    if (const auto* call = dynamic_cast<const ast::Call*>(&expr)) {
        return lowerCall(call, ilFn, onSpine);
    }

    diags_.error(expr.span, "unsupported AST expression");
    return std::nullopt;
}

std::optional<Lowerer::Value> Lowerer::lowerThisValue(Span span, il::Function& ilFn) {
    if (currentFunctionIsArrow_) {
        uint32_t depth = 0;
        uint32_t index = 0;
        if (currentEnvValue_ != il::kNoValue && findEnclosingEnvVar("this", depth, index)) {
            return emitEnvGet(depth, index, ilFn);
        }
        diags_.error(span, "`this` outside a function is unsupported");
        return std::nullopt;
    }
    if (currentThisValue_ == il::kNoValue) {
        // usesThis() decided the parameter, so reaching here means `this` at
        // module top level, where its value is a module system question bronze
        // has not answered yet.
        diags_.error(span, "`this` outside a function is unsupported");
        return std::nullopt;
    }
    return Value{currentThisValue_, il::Type::Dynamic};
}

}  // namespace bronze::lower
