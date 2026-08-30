// `Object.defineProperties(target, { k: { value: v, ... }, ... })` where every
// descriptor is an object LITERAL, lowered without building any of them.
//
// WHY THE DESCRIPTORS ARE THE COST. ECMA-262 20.1.2.3.1 is a loop over the
// descriptor map's own keys, and each turn hands one descriptor object to
// 6.2.6.5 ToPropertyDescriptor, which asks HasProperty and then Get for six
// field names. So a call with six keys allocates seven objects — the map and
// six descriptors — and then reads thirty-six names back out of them. three.js
// gives EVERY `Object3D` exactly that, in the constructor, so a scene of two
// thousand meshes runs it two thousand times to install twelve thousand fields
// whose attributes the source spelled out in full.
//
// None of that work is a question about the program. A descriptor written as
// `{ configurable: true, enumerable: true, value: position }` says which four
// fields it has and what three of them are at COMPILE time; the only thing the
// runtime contributes is the fourth. This lowering evaluates the target and
// then each `value` expression, in source order, and emits one
// `define.own.attr` per key carrying the other fields as a mask.
//
// WHY IT IS STILL 20.1.2.3 IN ORDER. The call's arguments are evaluated before
// the member runs, so the whole descriptor map — every `value` expression in
// it — is evaluated before step 1 asks whether the target is an object. A
// non-object target therefore throws AFTER those side effects, which is why
// the target check rides on the first `define.own.attr` rather than on the
// target expression. Then 20.1.2.3.1 decodes every descriptor (step 4) and
// only afterwards defines them (step 5) — a split that is invisible here,
// because a decode that cannot run user code and cannot fail has nothing to
// observe. It is what the predicate below is FOR: every field the literal
// names is either an already-evaluated operand or a boolean literal.
//
// WHAT IT REFUSES. Anything whose answer would have to come from the runtime:
// a computed or spread or accessor key, a descriptor that is not a literal, a
// `writable`/`enumerable`/`configurable` that is not a boolean literal, a key
// name that is an array index (an integer-index key is not enumerated in
// source order — 10.1.11.1 puts it first, in ascending numeric order — so a
// run of defines in source order would be a different program), a duplicate
// key (the map holds one property and the LAST descriptor wins, where a run of
// defines would apply both), and `__proto__` in either position (13.2.5.5
// makes it a prototype assignment in the outer literal, and an inherited field
// in the inner one). Each refusal returns nullopt and the caller emits the
// call it always emitted.
//
// ACCESSOR DESCRIPTORS ARE REFUSED. `bronze_define_own_attr`'s mask carries
// the four data fields, and giving it `get` and `set` would mean two more
// operands, the callability check of 6.2.6.5 steps 7.c and 8.c, and the
// mutual-exclusion check of step 9 — the whole decode, at which point nothing
// has been saved. An accessor descriptor is also not what a constructor writes
// in a loop: three.js's `Object3D` block is six data descriptors.
//
// WHAT IT DOES NOT PROVE, and inherits from the `Object.keys` recognition it
// sits beside (lower_call.cpp): that `Object.defineProperties` still holds the
// builtin. The receiver is checked against every way a program can BIND the
// name `Object` to something else, but a program that ASSIGNS over the member —
// `Object.defineProperties = f` — keeps the builtin's behaviour here. Proving
// otherwise is the whole-module taint scan `Math` has (types/infer.cpp), and
// that scan's answer is only available with inference on, where this lowering
// has to emit the same code either way.
//
// THE SEAM is `BRONZE_NO_DEFPROPS_LITERAL=1`, read by the COMPILER once per
// process, because what it isolates is the emitted code.

#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

namespace {

// The seam, read once per compiler process.
bool definePropsSeamDisabled() {
    static const bool off = std::getenv("BRONZE_NO_DEFPROPS_LITERAL") != nullptr;
    return off;
}

// Is this key one that 10.1.11.1 OwnPropertyKeys puts before the string keys,
// in ascending numeric order rather than in insertion order? Those are the
// array indices — a canonical decimal numeric string below 2^32-1 — and a
// descriptor map holding one is enumerated in an order the source does not
// show, so this lowering cannot claim to define its keys in source order.
bool isArrayIndexKey(const std::string& key) {
    if (key.empty() || key.size() > 10) return false;
    if (key.size() > 1 && key[0] == '0') return false;  // "01" is not canonical
    uint64_t n = 0;
    for (const char c : key) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<uint64_t>(c - '0');
    }
    return n < 4294967295ull;
}

// The six field names 6.2.6.5 reads. A descriptor literal naming anything else
// is naming a property the decode never looks at — harmless, but it is also a
// property this lowering would silently drop from an object it never builds,
// so it is refused rather than reasoned about.
const ast::ObjectLit* asDescriptorLiteral(const ast::Expr* value) {
    return dynamic_cast<const ast::ObjectLit*>(value);
}

// A descriptor literal read into the mask the helper takes, plus the `value`
// expression if it has one. False is "this literal is not one I take".
bool readDescriptor(const ast::ObjectLit& lit, uint32_t& mask, const ast::Expr*& valueExpr) {
    mask = 0;
    valueExpr = nullptr;
    std::unordered_set<std::string> seen;
    for (const ast::ObjectProp& prop : lit.props) {
        if (prop.computed() || prop.isMethod || prop.coverInitialized) return false;
        if (prop.accessor != ast::AccessorKind::None) return false;
        if (prop.value == nullptr) return false;
        if (dynamic_cast<const ast::SpreadElement*>(prop.value.get()) != nullptr) return false;
        // 6.2.6.5 reads each field once; a literal that writes one twice holds
        // the LAST, which is a fold this lowering does not perform.
        if (!seen.insert(prop.key).second) return false;

        if (prop.key == "value") {
            mask |= BRONZE_ABI_DESC_HAS_VALUE;
            valueExpr = prop.value.get();
            continue;
        }
        // `get` and `set` are refused with every other name: an accessor
        // descriptor is not what this lowering carries.
        uint32_t hasBit = 0;
        uint32_t valueBit = 0;
        if (prop.key == "writable") {
            hasBit = BRONZE_ABI_DESC_HAS_WRITABLE;
            valueBit = BRONZE_ABI_DESC_WRITABLE;
        } else if (prop.key == "enumerable") {
            hasBit = BRONZE_ABI_DESC_HAS_ENUMERABLE;
            valueBit = BRONZE_ABI_DESC_ENUMERABLE;
        } else if (prop.key == "configurable") {
            hasBit = BRONZE_ABI_DESC_HAS_CONFIGURABLE;
            valueBit = BRONZE_ABI_DESC_CONFIGURABLE;
        } else {
            return false;
        }
        // ToBoolean of anything else is a conversion, and a conversion is a
        // question for the runtime. A boolean LITERAL is the answer already.
        const auto* boolLit = dynamic_cast<const ast::BoolLit*>(prop.value.get());
        if (boolLit == nullptr) return false;
        mask |= hasBit;
        if (boolLit->value) mask |= valueBit;
    }
    return true;
}

}  // namespace

std::optional<Lowerer::Value> Lowerer::lowerDefinePropertiesLiteral(
    const ast::Call* call, const ast::MemberAccess& callee, il::Function& ilFn) {
    if (definePropsSeamDisabled()) return std::nullopt;
    if (callee.optional || call->optional) return std::nullopt;
    if (callee.property != "defineProperties") return std::nullopt;

    // The RECEIVER must be the `Object` bronze provides, and every way a
    // program can mean something else by that name is a refusal: a binding in
    // scope, a function declaration, a captured binding in an enclosing
    // environment record, or a host manifest that promised to supply the name
    // itself. `isProvidedGlobal` is the last question and not the first,
    // because it answers true for a host global too.
    const auto* base = dynamic_cast<const ast::Ident*>(callee.object.get());
    if (base == nullptr || base->name != "Object") return std::nullopt;
    if (activeVarMap_.contains("Object") || functionIndices_.contains("Object")) {
        return std::nullopt;
    }
    if (hostGlobals_.contains("Object")) return std::nullopt;
    uint32_t depth = 0;
    uint32_t index = 0;
    if (currentEnvValue_ != il::kNoValue && findEnclosingEnvVar("Object", depth, index)) {
        return std::nullopt;
    }
    if (!isProvidedGlobal("Object")) return std::nullopt;

    if (call->args.size() != 2) return std::nullopt;
    for (const auto& arg : call->args) {
        if (dynamic_cast<const ast::SpreadElement*>(arg.get()) != nullptr) return std::nullopt;
    }
    const auto* map = dynamic_cast<const ast::ObjectLit*>(call->args[1].get());
    if (map == nullptr || map->isModuleNamespace) return std::nullopt;
    // An empty map defines nothing, so there would be no `define.own.attr` for
    // 20.1.2.3's step-1 check to ride on — and that check is observable.
    if (map->props.empty()) return std::nullopt;

    // The whole map is read before anything is emitted, so a refusal costs no
    // half-lowered call.
    std::vector<std::string> keys;
    std::vector<uint32_t> masks;
    std::vector<const ast::Expr*> valueExprs;
    std::unordered_set<std::string> seenKeys;
    for (const ast::ObjectProp& prop : map->props) {
        if (prop.computed() || prop.isMethod || prop.coverInitialized) return std::nullopt;
        if (prop.accessor != ast::AccessorKind::None) return std::nullopt;
        if (prop.value == nullptr) return std::nullopt;
        if (dynamic_cast<const ast::SpreadElement*>(prop.value.get()) != nullptr) {
            return std::nullopt;
        }
        // 13.2.5.5: `__proto__: v` in a literal sets the prototype instead of
        // creating a key, so the map would not have the property at all.
        if (prop.key == "__proto__") return std::nullopt;
        if (isArrayIndexKey(prop.key)) return std::nullopt;
        if (!seenKeys.insert(prop.key).second) return std::nullopt;

        const ast::ObjectLit* desc = asDescriptorLiteral(prop.value.get());
        if (desc == nullptr || desc->isModuleNamespace) return std::nullopt;
        uint32_t mask = 0;
        const ast::Expr* valueExpr = nullptr;
        if (!readDescriptor(*desc, mask, valueExpr)) return std::nullopt;
        keys.push_back(prop.key);
        masks.push_back(mask);
        valueExprs.push_back(valueExpr);
    }

    // 13.3.6.1: the callee's reference is evaluated before the arguments, and
    // the target is the first argument — so the target expression runs first
    // and every `value` expression after it, in the order the literal wrote
    // them. Both are ordinary evaluation and may run anything.
    auto targetOpt = lowerExpr(*call->args[0], ilFn);
    if (!targetOpt) return std::nullopt;
    Value target = boxValueIfNeeded(*targetOpt, ilFn);

    std::vector<Value> values;
    values.reserve(valueExprs.size());
    for (const ast::Expr* valueExpr : valueExprs) {
        if (valueExpr == nullptr) {
            // A descriptor with no `value` field. 10.1.6.3 never reads the
            // operand for one, and the mask says so, but the instruction still
            // takes an operand.
            values.push_back(Value{emitConstUndefined(ilFn), il::Type::Dynamic});
            continue;
        }
        auto v = lowerExpr(*valueExpr, ilFn);
        if (!v) return std::nullopt;
        values.push_back(boxValueIfNeeded(*v, ilFn));
    }

    for (size_t i = 0; i < keys.size(); ++i) {
        il::Instruction inst;
        inst.op = il::Op::DefineOwnAttr;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = {target.id, values[i].id};
        inst.keyIndex = getKeyConstantIndex(keys[i]);
        inst.immI32 = static_cast<int32_t>(masks[i]);
        emitInst(ilFn, inst);
    }

    // 20.1.2.3 step 3: the answer is the target.
    return target;
}

}  // namespace bronze::lower
