// Private class elements, desugared (ECMA-262 6.2.12, 15.7).
//
// A private name is NOT sugar over a property, which is the one thing that
// makes this file necessary at all: `#x` is an entry in the object's
// [[PrivateElements]] list, invisible to every property operation, and its
// identity is per class EVALUATION. `class C { #x }` written once and
// evaluated twice mints two different names, and an instance of the first must
// fail the second's brand check — so nothing keyed by source position can
// stand in for it.
//
// What it is here: one runtime TABLE per private name per class evaluation
// (runtime/rt_private.cpp), keyed by the object that carries the element, held
// in a slot of the ENVIRONMENT RECORD the class evaluation creates. That
// record already has exactly the right lifetime — one per evaluation — and the
// chain that reaches it from a method body is the same chain a captured
// variable takes, so `#x` inside a nested arrow inside a method costs the same
// walk `let` does and shadowing between nested classes falls out for free.
//
// The KIND of a private name — field, method, or accessor pair — is fixed by
// its declaration, so the kind dispatch 6.2.12.2 and 6.2.12.3 write as runtime
// steps is resolved here instead. That is why the runtime's helpers answer
// only the storage question, and why the three TypeErrors a *branded* access
// can still be (writing a method, reading a set-only accessor, writing a
// get-only one) are emitted as an unconditional raise after the brand check
// rather than tested for.

#include <string>
#include <utility>
#include <vector>

#include "ast/clone.h"
#include "lower/lowerer.h"

namespace bronze::lower {

namespace {
// `\x01` cannot appear in an IdentifierName or in a private name, so a slot
// named with one cannot collide with a source binding or with another private
// name's slot — including one an enclosing class declares under the same `#x`.
constexpr char kSlotSep = '\x01';
}  // namespace

std::string Lowerer::privateTableSlot(const std::string& name) { return name; }
std::string Lowerer::privateSetterTableSlot(const std::string& name) {
    return name + kSlotSep + "set";
}
std::string Lowerer::privateFnSlot(const std::string& name) { return name + kSlotSep + "fn"; }
std::string Lowerer::privateSetterFnSlot(const std::string& name) {
    return name + kSlotSep + "setfn";
}

const Lowerer::PrivateElement* Lowerer::findPrivateElement(const std::string& name) const {
    for (size_t i = privateScopes_.size(); i-- > 0;) {
        for (const auto& el : privateScopes_[i]) {
            if (el.name == name) return &el;
        }
    }
    return nullptr;
}

std::vector<Lowerer::PrivateElement> Lowerer::collectPrivateElements(
    const std::vector<ast::ClassMethod>& methods) {
    std::vector<PrivateElement> elements;
    for (const auto& m : methods) {
        if (!m.isPrivate()) continue;
        PrivateElement* found = nullptr;
        for (auto& el : elements) {
            if (el.name == m.name) found = &el;
        }
        if (!found) {
            elements.push_back(PrivateElement{m.name, PrivateKind::Field, m.isStatic, false, false});
            found = &elements.back();
        }
        found->isStatic = m.isStatic;
        if (m.isField) {
            found->kind = PrivateKind::Field;
        } else if (m.accessor != ast::AccessorKind::None) {
            // The two halves of one name are ONE element: 15.7.1 admits the
            // repetition only for a getter/setter pair, and 6.2.12 gives the
            // pair one PrivateElement with two fields.
            found->kind = PrivateKind::Accessor;
            if (m.accessor == ast::AccessorKind::Getter) found->hasGetter = true;
            if (m.accessor == ast::AccessorKind::Setter) found->hasSetter = true;
        } else {
            found->kind = PrivateKind::Method;
        }
    }
    return elements;
}

bool Lowerer::openClassScope(const std::string& className,
                            const std::vector<PrivateElement>& elements, il::Function& ilFn) {
    std::vector<std::string> slots;
    if (!className.empty()) slots.push_back(className);
    for (const auto& el : elements) {
        slots.push_back(privateTableSlot(el.name));
        if (el.kind == PrivateKind::Accessor) slots.push_back(privateSetterTableSlot(el.name));
        if (el.kind == PrivateKind::Method || (el.kind == PrivateKind::Accessor && el.hasGetter)) {
            slots.push_back(privateFnSlot(el.name));
        }
        if (el.kind == PrivateKind::Accessor && el.hasSetter) {
            slots.push_back(privateSetterFnSlot(el.name));
        }
    }
    pushSyntheticEnv(slots, ilFn);
    privateScopes_.push_back(elements);

    // The tables themselves, minted HERE — once per evaluation of this class,
    // which is the whole of private-name identity.
    const size_t scopeIndex = envScopes_.size() - 1;
    const uint32_t depth = envDepthOf(scopeIndex);
    if (!className.empty()) {
        // The class binding starts UNINITIALIZED, which is the whole of
        // `class C extends C {}` being a ReferenceError: the heritage is
        // evaluated with this record already in scope.
        const uint32_t slot = envScopes_[scopeIndex].slotOf.at(className);
        envScopes_[scopeIndex].slotIsLexical[slot] = true;
        il::Instruction inst;
        inst.op = il::Op::EnvInitTdz;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = {currentEnv(ilFn)};
        inst.envDepth = depth;
        inst.envIndex = slot;
        emitInst(ilFn, inst);
        // BOUND, not merely stored: the class evaluation is itself code that
        // can mention the name — a static field initializer is lowered right
        // here — and a mention resolves through `activeVarMap_` before it ever
        // reaches the environment chain. Without the binding, `static a =
        // C.b` read the OUTER `class C` binding, which 15.7.14 leaves in its
        // dead zone for the whole of the evaluation.
        if (declareVariable(className, il::Type::Dynamic, /*isConst=*/false, /*isLet=*/true,
                            /*isVar=*/false, /*isInitialized=*/false, il::kNoValue, Span{})) {
            varBindings_[activeVarMap_.at(className)].isTdzHoisted = true;
        }
    }
    auto mint = [&](const std::string& slotName) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::PrivateNew;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        emitInst(ilFn, inst);
        emitEnvSet(depth, envScopes_[scopeIndex].slotOf.at(slotName),
                   Value{res, il::Type::Dynamic}, ilFn);
    };
    for (const auto& el : elements) {
        mint(privateTableSlot(el.name));
        if (el.kind == PrivateKind::Accessor) mint(privateSetterTableSlot(el.name));
    }
    return true;
}

std::optional<Lowerer::Value> Lowerer::emitPrivateSlotRead(const std::string& slotName, Span span,
                                                           il::Function& ilFn) {
    uint32_t depth = 0;
    uint32_t index = 0;
    if (currentEnvValue_ == il::kNoValue || !findEnclosingEnvVar(slotName, depth, index)) {
        // The parser refuses every undeclared private name, so a miss here is
        // lowering and the class evaluation disagreeing about the record.
        diags_.error(span, "internal: no environment slot for private name '" + slotName + "'");
        return std::nullopt;
    }
    return emitEnvGet(depth, index, ilFn);
}

bool Lowerer::initClassNameBinding(const std::string& className, Value ctorVal, Span span,
                                   il::Function& ilFn) {
    auto it = activeVarMap_.find(className);
    if (it == activeVarMap_.end()) {
        diags_.error(span, "internal: no binding for class name '" + className + "'");
        return false;
    }
    VarBinding& binding = varBindings_[it->second];
    if (!binding.inEnv) {
        diags_.error(span, "internal: class binding '" + className + "' has no record slot");
        return false;
    }
    // Not `writeBinding`: this is InitializeBinding (9.1.1.1.4), which is what
    // ENDS the dead zone, and the checked store an assignment makes would read
    // the marker it is about to replace.
    emitEnvSet(envDepthOf(binding.envScopeIndex), binding.envSlot, ctorVal, ilFn);
    binding.isTdzHoisted = false;
    binding.isInitialized = true;
    return true;
}

Lowerer::Value Lowerer::emitPrivateMisuse(const std::string& name, int32_t code,
                                          il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::PrivateMisuse;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.keyIndex = getKeyConstantIndex(name);
    inst.immI32 = code;
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

Lowerer::Value Lowerer::emitPrivateCall(Value fnVal, Value thisVal,
                                        const std::vector<il::ValueId>& args,
                                        il::Function& ilFn) {
    std::vector<il::ValueId> operands;
    operands.push_back(fnVal.id);
    operands.push_back(thisVal.id);
    for (il::ValueId a : args) operands.push_back(a);
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::DynamicCall;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = std::move(operands);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

// The read half of 6.2.12.2. The brand check is the `private.get` itself: a
// receiver with no element is the TypeError, whatever the name's kind is, and
// it happens BEFORE the kind is consulted — which is why a set-only accessor
// read on a foreign object reports the brand failure rather than the missing
// getter.
std::optional<Lowerer::Value> Lowerer::lowerPrivateRead(const ast::MemberAccess& mem,
                                                        Value objBoxed, il::Function& ilFn) {
    const PrivateElement* el = findPrivateElement(mem.property);
    if (!el) {
        diags_.error(mem.span, "internal: private name '" + mem.property +
                                   "' reached lowering with no declaration");
        return std::nullopt;
    }
    auto table = emitPrivateSlotRead(privateTableSlot(el->name), mem.span, ilFn);
    if (!table) return std::nullopt;

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::PrivateGet;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.operands = {table->id, objBoxed.id};
    inst.keyIndex = getKeyConstantIndex(el->name);
    emitInst(ilFn, inst);
    Value stored{res, il::Type::Dynamic};

    if (el->kind != PrivateKind::Accessor) return stored;
    // 6.2.12.2 step 3.b: an accessor with no getter is a TypeError once the
    // brand has been established, which the get above did.
    if (!el->hasGetter) return emitPrivateMisuse(el->name, /*code=*/1, ilFn);
    return emitPrivateCall(stored, objBoxed, {}, ilFn);
}

// The write half: 6.2.12.4 PrivateFieldAdd for a definition, 6.2.12.3
// PrivateSet otherwise. `isPrivateDefine` is set only on the nodes the class
// desugar synthesizes, and it names the TABLE SLOT directly rather than a
// private name — the two halves of an accessor are two slots of one name, and
// a definition has to be able to say which.
bool Lowerer::lowerPrivateWrite(const ast::MemberAccess& mem, Value objBoxed, Value valBoxed,
                                il::Function& ilFn) {
    if (mem.isPrivateDefine) {
        auto table = emitPrivateSlotRead(mem.property, mem.span, ilFn);
        if (!table) return false;
        il::Instruction inst;
        inst.op = il::Op::PrivateAdd;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = {table->id, objBoxed.id, valBoxed.id};
        emitInst(ilFn, inst);
        return true;
    }

    const PrivateElement* el = findPrivateElement(mem.property);
    if (!el) {
        diags_.error(mem.span, "internal: private name '" + mem.property +
                                   "' reached lowering with no declaration");
        return false;
    }
    if (el->kind == PrivateKind::Field) {
        auto table = emitPrivateSlotRead(privateTableSlot(el->name), mem.span, ilFn);
        if (!table) return false;
        il::Instruction inst;
        inst.op = il::Op::PrivateSet;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = {table->id, objBoxed.id, valBoxed.id};
        inst.keyIndex = getKeyConstantIndex(el->name);
        emitInst(ilFn, inst);
        return true;
    }
    if (el->kind == PrivateKind::Method) {
        // 6.2.12.3 step 2: a method is not writable. The brand is still
        // checked first — `other.#m = 1` from outside the class reports the
        // brand failure, not the method one — so the get is emitted and its
        // result dropped.
        auto table = emitPrivateSlotRead(privateTableSlot(el->name), mem.span, ilFn);
        if (!table) return false;
        il::ValueId probe = ilFn.valueCount++;
        il::Instruction get;
        get.op = il::Op::PrivateGet;
        get.type = il::Type::Dynamic;
        get.result = probe;
        get.operands = {table->id, objBoxed.id};
        get.keyIndex = getKeyConstantIndex(el->name);
        emitInst(ilFn, get);
        emitPrivateMisuse(el->name, /*code=*/0, ilFn);
        return true;
    }
    // An accessor: the brand check is a read of the SETTER table, which every
    // branded object has an entry in whether or not a setter was written.
    auto setterTable = emitPrivateSlotRead(privateSetterTableSlot(el->name), mem.span, ilFn);
    if (!setterTable) return false;
    il::ValueId setterId = ilFn.valueCount++;
    il::Instruction get;
    get.op = il::Op::PrivateGet;
    get.type = il::Type::Dynamic;
    get.result = setterId;
    get.operands = {setterTable->id, objBoxed.id};
    get.keyIndex = getKeyConstantIndex(el->name);
    emitInst(ilFn, get);
    if (!el->hasSetter) {
        emitPrivateMisuse(el->name, /*code=*/2, ilFn);
        return true;
    }
    emitPrivateCall(Value{setterId, il::Type::Dynamic}, objBoxed, {valBoxed.id}, ilFn);
    return true;
}

// `o.#x = v`, `o.#x += v`, and the definitions the class desugar synthesizes.
// The value of the expression is the value STORED (13.15.2), which for an
// accessor is the right-hand side and not whatever the setter returns.
std::optional<Lowerer::Value> Lowerer::lowerPrivateAssignment(const ast::Binary* bin,
                                                              il::Function& ilFn) {
    const auto* mem = static_cast<const ast::MemberAccess*>(bin->lhs.get());
    const bool isLogical = bin->op == ast::BinaryOp::LogicalAndAssign ||
                           bin->op == ast::BinaryOp::LogicalOrAssign ||
                           bin->op == ast::BinaryOp::NullishAssign;
    if (isLogical) {
        // Refused by name rather than compiled wrong: the short-circuit form
        // must not write when it does not fire, which needs the branch-and-join
        // the property path builds, and a private target has not been given one.
        diags_.error(bin->span,
                     "unsupported construct: logical assignment (&&=, ||=, ??=) to the private "
                     "member '" + mem->property + "'");
        return std::nullopt;
    }

    auto objVal = lowerExpr(*mem->object, ilFn);
    if (!objVal) return std::nullopt;
    Value objBoxed = boxValueIfNeeded(*objVal, ilFn);

    std::optional<Value> curVal;
    if (bin->op != ast::BinaryOp::Assign) {
        curVal = lowerPrivateRead(*mem, objBoxed, ilFn);
        if (!curVal) return std::nullopt;
    }
    auto rhsVal = lowerExpr(*bin->rhs, ilFn);
    if (!rhsVal) return std::nullopt;
    Value stored = curVal ? emitCompoundCombine(*curVal, *rhsVal, bin->op, provenNumber(*bin), ilFn)
                          : *rhsVal;
    Value storedBoxed = boxValueIfNeeded(stored, ilFn);
    if (!lowerPrivateWrite(*mem, objBoxed, storedBoxed, ilFn)) return std::nullopt;
    return storedBoxed;
}

// `o.#x++`. The reference is evaluated once, exactly as `o.k++` is: the
// receiver is lowered one time and both the read and the write name it.
std::optional<Lowerer::Value> Lowerer::lowerPrivateUpdate(const ast::MemberAccess& mem,
                                                          ast::UnaryOp op, il::Function& ilFn) {
    auto objVal = lowerExpr(*mem.object, ilFn);
    if (!objVal) return std::nullopt;
    Value objBoxed = boxValueIfNeeded(*objVal, ilFn);
    auto cur = lowerPrivateRead(mem, objBoxed, ilFn);
    if (!cur) return std::nullopt;
    Value numOld = emitUpdateOld(*cur, ilFn);
    Value newVal = emitUpdateStep(numOld, op, ilFn);
    Value storedBoxed = boxValueIfNeeded(newVal, ilFn);
    if (!lowerPrivateWrite(mem, objBoxed, storedBoxed, ilFn)) return std::nullopt;
    const bool prefix = op == ast::UnaryOp::PreInc || op == ast::UnaryOp::PreDec;
    return prefix ? newVal : numOld;
}

// `#x in o` (13.10.1). Not `bronze_has_property`: the question is whether the
// object carries the ELEMENT, which no property lookup can answer, and the
// answer is a boolean even for a receiver of the wrong shape entirely — this
// operator is how a program asks a brand question without a `try`.
std::optional<Lowerer::Value> Lowerer::lowerPrivateIn(const ast::Expr& nameExpr,
                                                      const ast::Expr& objExpr,
                                                      il::Function& ilFn) {
    const auto& ident = static_cast<const ast::Ident&>(nameExpr);
    const PrivateElement* el = findPrivateElement(ident.name);
    if (!el) {
        diags_.error(ident.span, "internal: private name '" + ident.name +
                                     "' reached lowering with no declaration");
        return std::nullopt;
    }
    auto table = emitPrivateSlotRead(privateTableSlot(el->name), ident.span, ilFn);
    if (!table) return std::nullopt;
    auto objVal = lowerExpr(objExpr, ilFn);
    if (!objVal) return std::nullopt;
    Value objBoxed = boxValueIfNeeded(*objVal, ilFn);

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::PrivateHas;
    inst.type = il::Type::Bool;
    inst.result = res;
    inst.operands = {table->id, objBoxed.id};
    inst.keyIndex = getKeyConstantIndex(el->name);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Bool};
}

// The constructor's prologue, in the order 15.7.14 InitializeInstanceElements
// runs it: every private method and accessor gets its brand FIRST, then each
// field initializer in definition order. The order is observable twice over —
// `#a = this.#m()` works because the method is already installed, and
// `#a = this.#b` with `#b` written below is a TypeError because it is not yet —
// so it is the order and not an implementation detail.
//
// Public fields are built here too, because 15.7.14 interleaves them with the
// private ones by position and there is only one order to keep.
std::vector<ast::StmtPtr> Lowerer::buildFieldInitStatements(
    const std::vector<ast::ClassMethod>& methods, const std::vector<PrivateElement>& elements,
    Span span) {
    std::vector<ast::StmtPtr> out;

    // `this.<slot> = <value>` as a DEFINITION: `isPrivateDefine` names the
    // table slot rather than the private name, which is what lets an accessor's
    // two halves be installed as two statements of one name.
    auto define = [&](const std::string& slotName, ast::ExprPtr value) {
        auto thisExpr = std::make_unique<ast::ThisExpr>();
        thisExpr->span = span;
        auto target = std::make_unique<ast::MemberAccess>();
        target->span = span;
        target->object = std::move(thisExpr);
        target->property = slotName;
        target->isPrivate = true;
        target->isPrivateDefine = true;
        auto bin = std::make_unique<ast::Binary>();
        bin->span = span;
        bin->op = ast::BinaryOp::Assign;
        bin->lhs = std::move(target);
        bin->rhs = std::move(value);
        auto stmt = std::make_unique<ast::ExprStmt>();
        stmt->span = span;
        stmt->expr = std::move(bin);
        out.push_back(std::move(stmt));
    };
    auto slotRead = [&](const std::string& slotName) -> ast::ExprPtr {
        auto ident = std::make_unique<ast::Ident>();
        ident->span = span;
        ident->name = slotName;
        return ident;
    };
    auto undefined = [&]() -> ast::ExprPtr {
        auto lit = std::make_unique<ast::UndefinedLit>();
        lit->span = span;
        return lit;
    };

    for (const auto& el : elements) {
        if (el.isStatic || el.kind == PrivateKind::Field) continue;
        if (el.kind == PrivateKind::Method) {
            define(privateTableSlot(el.name), slotRead(privateFnSlot(el.name)));
            continue;
        }
        // Both halves, always, so that the brand check a write makes against
        // the setter table agrees with the one a read makes against the getter
        // table. A half the source did not write stores `undefined`, and
        // lowering never calls it: the missing-half TypeError is emitted at
        // the access, where the kind is known.
        define(privateTableSlot(el.name),
               el.hasGetter ? slotRead(privateFnSlot(el.name)) : undefined());
        define(privateSetterTableSlot(el.name),
               el.hasSetter ? slotRead(privateSetterFnSlot(el.name)) : undefined());
    }

    for (const auto& m : methods) {
        if (!m.isField || m.isStatic) continue;
        ast::ExprPtr value = m.init ? ast::cloneExpr(*m.init) : undefined();
        if (m.isPrivate()) {
            define(privateTableSlot(m.name), std::move(value));
            continue;
        }
        auto thisExpr = std::make_unique<ast::ThisExpr>();
        thisExpr->span = m.fn ? m.fn->span : span;
        ast::ExprPtr target;
        if (m.keyExpr) {
            auto idx = std::make_unique<ast::IndexAccess>();
            idx->span = thisExpr->span;
            idx->object = std::move(thisExpr);
            idx->index = ast::cloneExpr(*m.keyExpr);
            target = std::move(idx);
        } else {
            auto memAccess = std::make_unique<ast::MemberAccess>();
            memAccess->span = thisExpr->span;
            memAccess->object = std::move(thisExpr);
            memAccess->property = m.name;
            target = std::move(memAccess);
        }
        auto bin = std::make_unique<ast::Binary>();
        bin->span = target->span;
        bin->op = ast::BinaryOp::Assign;
        bin->lhs = std::move(target);
        bin->rhs = std::move(value);
        auto stmt = std::make_unique<ast::ExprStmt>();
        stmt->span = bin->span;
        stmt->expr = std::move(bin);
        out.push_back(std::move(stmt));
    }
    return out;
}

// A static private method or accessor is carried by the CONSTRUCTOR, so its
// brand is added at class evaluation rather than in a constructor body — there
// is no instance to add it to, and `X.#m` must work before `new X` ever runs.
bool Lowerer::emitStaticPrivateBrands(const std::vector<PrivateElement>& elements, Value ctorVal,
                                      Span span, il::Function& ilFn) {
    auto add = [&](const std::string& tableSlot, std::optional<Value> value) {
        auto table = emitPrivateSlotRead(tableSlot, span, ilFn);
        if (!table || !value) return false;
        il::Instruction inst;
        inst.op = il::Op::PrivateAdd;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = {table->id, ctorVal.id, boxValueIfNeeded(*value, ilFn).id};
        emitInst(ilFn, inst);
        return true;
    };
    for (const auto& el : elements) {
        if (!el.isStatic || el.kind == PrivateKind::Field) continue;
        if (el.kind == PrivateKind::Method) {
            if (!add(privateTableSlot(el.name), emitPrivateSlotRead(privateFnSlot(el.name), span, ilFn))) {
                return false;
            }
            continue;
        }
        std::optional<Value> getter =
            el.hasGetter ? emitPrivateSlotRead(privateFnSlot(el.name), span, ilFn)
                         : std::optional<Value>{Value{emitConstUndefined(ilFn), il::Type::Dynamic}};
        if (!add(privateTableSlot(el.name), getter)) return false;
        std::optional<Value> setter =
            el.hasSetter ? emitPrivateSlotRead(privateSetterFnSlot(el.name), span, ilFn)
                         : std::optional<Value>{Value{emitConstUndefined(ilFn), il::Type::Dynamic}};
        if (!add(privateSetterTableSlot(el.name), setter)) return false;
    }
    return true;
}

}  // namespace bronze::lower
