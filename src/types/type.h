#pragma once

#include <cstdint>
#include <string>

namespace bronze::types {

// A compile-time object identity (docs/0010 decision 4). Interned by
// ShapeClassTable; `kNoShapeClass` means "an object, but which one is not
// proven" — the rung the join drops to when two classes meet.
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
    Dynamic,    // top: anything, the designed fallback
};

const char* typeKindName(TypeKind kind);

// The lattice of docs/0010 decision 2. Deliberately flat between Never and
// Dynamic: there are no union types, so `number | undefined` is `Dynamic`.
// Modelling unions would grow a case analysis in every consumer for an
// unmeasured win; the narrow case it would buy (a `let` assigned once before
// any use) is handled by flow sensitivity instead.
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
    static constexpr Type function(uint32_t index = kNoFunctionIndex) {
        return Type(TypeKind::Function, index);
    }

    constexpr TypeKind kind() const { return kind_; }
    constexpr bool is(TypeKind k) const { return kind_ == k; }

    // Meaningful only for the matching kind; the sentinel otherwise, so a
    // caller that forgets to check the kind gets "unproven", never a wrong
    // identity.
    constexpr ShapeClassId shapeClass() const {
        return kind_ == TypeKind::Object ? payload_ : kNoShapeClass;
    }
    constexpr uint32_t functionIndex() const {
        return kind_ == TypeKind::Function ? payload_ : kNoFunctionIndex;
    }

    friend constexpr bool operator==(Type a, Type b) {
        return a.kind_ == b.kind_ && a.payload_ == b.payload_;
    }
    friend constexpr bool operator!=(Type a, Type b) { return !(a == b); }

    // Canonical text: "number", "object", "object#3", "function#1", ...
    std::string str() const;

private:
    static constexpr uint32_t kNoPayload = 0xFFFFFFFFu;
    constexpr explicit Type(TypeKind k, uint32_t payload = kNoPayload)
        : kind_(k), payload_(payload) {}

    TypeKind kind_ = TypeKind::Never;
    uint32_t payload_ = kNoPayload;
};

// `Never ⊔ t = t`; `t ⊔ t = t`; anything else is `Dynamic` — except that two
// values of the same kind with different identities keep the kind and lose
// the identity, which is what "Object with no class" means in decision 4.
Type join(Type a, Type b);

}  // namespace bronze::types
