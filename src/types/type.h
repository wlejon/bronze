#pragma once

#include <cstdint>
#include <string>

namespace bronze::types {

// A compile-time object identity. Interned by ShapeClassTable; `kNoShapeClass`
// means "an object, but which one is not proven" — the rung the join drops to
// when two classes meet.
using ShapeClassId = uint32_t;
inline constexpr ShapeClassId kNoShapeClass = 0xFFFFFFFFu;

// A module-level function's index, which is its position among the
// top-level `FunctionDecl`s — the same numbering lowering assigns.
inline constexpr uint32_t kNoFunctionIndex = 0xFFFFFFFFu;

enum class TypeKind : uint8_t {
    Never,      // bottom: no value reaches here yet
    Number,
    Bool,
    String,
    Undefined,
    Null,
    Object,     // optional shape class
    Function,   // optional module function index
    TypedArray, // optional element kind (Float64Array / Float32Array views)
    Dynamic,    // top: anything, the designed fallback
};

const char* typeKindName(TypeKind kind);

// The element kind a `TypeKind::TypedArray` proof carries. Only the two float
// views are named here because they are the two whose native element path
// exists; a typed array of any other element kind stays `object`/`dynamic`
// and keeps today's inline-cached dynamic path. Widening this enum is how the
// integer views join the native path later.
enum class TypedArrayElem : uint32_t {
    Float64 = 0,
    Float32 = 1,
};
inline constexpr uint32_t kNoTypedArrayElem = 0xFFFFFFFFu;

// The inference lattice. Deliberately flat between Never and Dynamic: there are
// no union types, so `number | undefined` is `Dynamic`. Modelling unions would
// grow a case analysis in every consumer for an unmeasured win; the narrow case
// it would buy (a `let` assigned once before any use) is handled by flow
// sensitivity instead.
class Type {
public:
    constexpr Type() = default;  // Never

    static constexpr Type never() { return Type(TypeKind::Never); }
    static constexpr Type number() { return Type(TypeKind::Number); }
    static constexpr Type boolean() { return Type(TypeKind::Bool); }
    static constexpr Type string() { return Type(TypeKind::String); }
    static constexpr Type undefined() { return Type(TypeKind::Undefined); }
    static constexpr Type null() { return Type(TypeKind::Null); }
    static constexpr Type dynamic() { return Type(TypeKind::Dynamic); }
    static constexpr Type object(ShapeClassId cls = kNoShapeClass) {
        return Type(TypeKind::Object, cls);
    }
    // The same identity, marked as one the interprocedural pass GUESSED rather
    // than watched being made.
    //
    // `Type::object(C)` from `new C()` or an object literal is a fact: that
    // expression built that object. `objectIdentityOnly(C)` is a join over the
    // call sites of a method — "every caller this compilation could see passes a
    // C here" — and a caller it could not see (a `.call`, a computed dispatch, a
    // callback the host invokes) makes it wrong. It is still worth having,
    // because an object identity licenses exactly one thing: the property-site
    // form, whose guard is a shape compare the runtime performs. A wrong guess
    // there costs a miss.
    //
    // What it must never do is become a claim about a VALUE, and there is one
    // road from an identity to a value type — `ClassLayoutTable::fieldTypeOf`,
    // which answers `number` for a field the class body only ever assigns
    // numbers to. `Number` licenses unboxed f64 and IS a soundness obligation,
    // so this bit is what stops the identity being spent on one: a field read
    // through an identity-only base keeps a (still identity-only) object
    // identity and drops every primitive answer to `Dynamic`.
    static constexpr Type objectIdentityOnly(ShapeClassId cls) {
        return cls == kNoShapeClass ? Type(TypeKind::Object, cls)
                                    : Type(TypeKind::Object, cls, true);
    }
    static constexpr Type function(uint32_t index = kNoFunctionIndex) {
        return Type(TypeKind::Function, index);
    }
    static constexpr Type typedArray(TypedArrayElem elem) {
        return Type(TypeKind::TypedArray, static_cast<uint32_t>(elem));
    }
    // A typed array whose element kind is not proven — the rung the join drops
    // to when two element kinds meet. Truthiness and `typeof` still know it is
    // an object; the native element path needs the kind and stays dynamic.
    static constexpr Type typedArray() { return Type(TypeKind::TypedArray); }

    constexpr TypeKind kind() const { return kind_; }
    constexpr bool is(TypeKind k) const { return kind_ == k; }

    // Meaningful only for the matching kind; the sentinel otherwise, so a
    // caller that forgets to check the kind gets "unproven", never a wrong
    // identity.
    constexpr ShapeClassId shapeClass() const {
        return kind_ == TypeKind::Object ? payload_ : kNoShapeClass;
    }
    // See `objectIdentityOnly`. False for every non-object, and false for an
    // object with no identity — there is nothing there to be only.
    constexpr bool identityOnly() const { return identityOnly_; }
    constexpr uint32_t functionIndex() const {
        return kind_ == TypeKind::Function ? payload_ : kNoFunctionIndex;
    }
    // The raw payload, `kNoTypedArrayElem` unless this is a TypedArray whose
    // element kind is proven. Raw rather than the enum so a caller that
    // forgets the sentinel check cannot conjure an enum value from it.
    constexpr uint32_t typedArrayElemRaw() const {
        return kind_ == TypeKind::TypedArray ? payload_ : kNoTypedArrayElem;
    }

    friend constexpr bool operator==(Type a, Type b) {
        return a.kind_ == b.kind_ && a.payload_ == b.payload_ &&
               a.identityOnly_ == b.identityOnly_;
    }
    friend constexpr bool operator!=(Type a, Type b) { return !(a == b); }

    // Canonical text: "number", "object", "object#3", "function#1", ...
    std::string str() const;

private:
    static constexpr uint32_t kNoPayload = 0xFFFFFFFFu;
    constexpr explicit Type(TypeKind k, uint32_t payload = kNoPayload,
                            bool identityOnly = false)
        : kind_(k), payload_(payload), identityOnly_(identityOnly) {}

    TypeKind kind_ = TypeKind::Never;
    uint32_t payload_ = kNoPayload;
    bool identityOnly_ = false;
};

// `Never ⊔ t = t`; `t ⊔ t = t`; anything else is `Dynamic` — except that two
// values of the same kind with different identities keep the kind and lose the
// identity, which is what "Object with no class" means.
Type join(Type a, Type b);

}  // namespace bronze::types
