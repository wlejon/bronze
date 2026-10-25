// Public instance fields: how the statement `buildFieldInitStatements`
// synthesizes for one is stored.
//
// A field is DEFINED (15.7.10 DefineField, 7.3.6 CreateDataPropertyOrThrow),
// not assigned. The two agree on an ordinary object whose prototype chain has
// nothing of that name but a writable data property or nothing at all, and
// then the write keeps its inline cache — which is every field of every class
// whose chain this file can see to the root. Where it cannot, the definition
// is spelled as one: an accessor of that name on the chain would otherwise
// run its setter and create no own property, which is the observable
// difference (`class D extends Base { a; }` with a `set a` on Base).

#include <algorithm>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "lower/lowerer.h"
#include "types/walk.h"

namespace bronze::lower {

namespace {

bool declaresAccessor(const std::vector<ast::ClassMethod>& methods, const std::string& name) {
    for (const auto& m : methods) {
        if (m.accessor != ast::AccessorKind::None && !m.isStatic && m.name == name) return true;
    }
    return false;
}

// The ways a program puts something on a prototype that a class body did not
// declare and that a write would not have made: a descriptor (which can be an
// accessor or read-only), a legacy accessor definition, or a different
// prototype altogether. Spelled by the member's name, whatever it is called
// on, because the receiver is not something a syntactic pass can bound.
class PrototypeReshape final : public types::Walker {
public:
    bool found = false;
    void visit(const ast::Call& n) override {
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.callee.get())) {
            const std::string& p = m->property;
            if (p == "defineProperty" || p == "defineProperties" || p == "setPrototypeOf" ||
                p == "__defineSetter__" || p == "__defineGetter__") {
                found = true;
            }
        }
        types::Walker::visit(n);
    }
    void visit(const ast::MemberAccess& n) override {
        if (n.property == "__proto__") found = true;
        types::Walker::visit(n);
    }
};

}  // namespace

// Whether a [[Set]] of `field` on a fresh instance is the definition the
// field asks for. True when nothing on the prototype chain can intercept the
// write: no accessor of the name in this class body, and either no `extends`
// or an `extends` chain of classes inference modelled to the root, none of
// which declares one either. `__proto__` is the one name an ordinary
// object's own chain intercepts (Object.prototype's accessor), so it is
// always a definition.
//
// The class bodies are not the whole chain when the program can reach into a
// prototype by hand — `Object.defineProperty(Base.prototype, 'a', {set})`, a
// read-only property defined the same way, or `Object.prototype` given a
// setter — so a program that spells any of the ways to (`PrototypeReshape`)
// gets definitions throughout, and pays a helper call per field for it.
bool Lowerer::fieldSetIsDefinition(const std::vector<ast::ClassMethod>& methods,
                                   const std::string& superName, bool hasSuper,
                                   const std::string& field) {
    if (field == "__proto__") return false;
    if (declaresAccessor(methods, field)) return false;
    if (!reshapesPrototypes_) {
        PrototypeReshape scan;
        for (const auto& s : astModule_.body) {
            if (s) s->accept(scan);
        }
        reshapesPrototypes_ = scan.found;
    }
    if (*reshapesPrototypes_) return false;
    if (!hasSuper) return true;
    // An `extends` this pass cannot name, or a program compiled without
    // inference, has a chain nothing here can read.
    if (superName.empty() || inference_ == nullptr) return false;
    const types::ClassLayoutTable& table = inference_->classLayouts;
    std::vector<const types::ClassLayout*> seen;
    for (const types::ClassLayout* cl = table.byName(superName); ;) {
        if (cl == nullptr) return false;
        if (std::find(seen.begin(), seen.end(), cl) != seen.end()) return false;
        seen.push_back(cl);
        if (std::find(cl->accessorNames.begin(), cl->accessorNames.end(), field) !=
            cl->accessorNames.end()) {
            return false;
        }
        // A base whose own heritage is an expression has an empty name and a
        // refusal saying so; one with no heritage is the root.
        if (cl->superName.empty()) {
            return cl->refusal != "base class is a computed expression";
        }
        cl = table.byName(cl->superName);
    }
}

// The definition itself: [[DefineOwnProperty]] with a descriptor that is
// writable, enumerable and configurable, which is what 7.3.6 builds. A refusal
// is a TypeError, as CreateDataPropertyOrThrow's is.
std::optional<Lowerer::Value> Lowerer::lowerFieldDefinition(const ast::MemberAccess& mem,
                                                            const ast::Expr& rhs,
                                                            il::Function& ilFn) {
    auto objVal = lowerExpr(*mem.object, ilFn);
    if (!objVal) return std::nullopt;
    const Value objBoxed = boxValueIfNeeded(*objVal, ilFn);
    auto rhsVal = lowerExpr(rhs, ilFn);
    if (!rhsVal) return std::nullopt;
    const Value valBoxed = boxValueIfNeeded(*rhsVal, ilFn);
    il::Instruction inst;
    inst.op = il::Op::DefineOwnAttr;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {objBoxed.id, valBoxed.id};
    inst.keyIndex = getKeyConstantIndex(mem.property);
    inst.immI32 = static_cast<int32_t>(
        BRONZE_ABI_DESC_HAS_VALUE | BRONZE_ABI_DESC_HAS_WRITABLE | BRONZE_ABI_DESC_WRITABLE |
        BRONZE_ABI_DESC_HAS_ENUMERABLE | BRONZE_ABI_DESC_ENUMERABLE |
        BRONZE_ABI_DESC_HAS_CONFIGURABLE | BRONZE_ABI_DESC_CONFIGURABLE);
    inst.span = mem.span;
    emitInst(ilFn, inst);
    return valBoxed;
}

}  // namespace bronze::lower
