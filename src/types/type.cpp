#include "types/type.h"

namespace bronze::types {

const char* typeKindName(TypeKind kind) {
    switch (kind) {
        case TypeKind::Never: return "never";
        case TypeKind::Number: return "number";
        case TypeKind::Bool: return "bool";
        case TypeKind::String: return "string";
        case TypeKind::Undefined: return "undefined";
        case TypeKind::Null: return "null";
        case TypeKind::Object: return "object";
        case TypeKind::Function: return "function";
        case TypeKind::TypedArray: return "typedarray";
        case TypeKind::Array: return "array";
        case TypeKind::Dynamic: return "dynamic";
    }
    return "?";
}

std::string Type::str() const {
    std::string out = typeKindName(kind_);
    if (kind_ == TypeKind::Object && payload_ != kNoPayload) {
        out += '#' + std::to_string(payload_);
    } else if (kind_ == TypeKind::Function && payload_ != kNoPayload) {
        out += '#' + std::to_string(payload_);
    } else if (kind_ == TypeKind::TypedArray && payload_ != kNoPayload) {
        switch (static_cast<TypedArrayElem>(payload_)) {
            case TypedArrayElem::Float64: out += ":f64"; break;
            case TypedArrayElem::Float32: out += ":f32"; break;
            case TypedArrayElem::Int32: out += ":i32"; break;
            case TypedArrayElem::Uint32: out += ":u32"; break;
            case TypedArrayElem::Int16: out += ":i16"; break;
            case TypedArrayElem::Uint16: out += ":u16"; break;
            case TypedArrayElem::Int8: out += ":i8"; break;
            case TypedArrayElem::Uint8: out += ":u8"; break;
            case TypedArrayElem::Uint8Clamped: out += ":u8c"; break;
            default: break;
        }
    }
    return out;
}

Type join(Type a, Type b) {
    if (a == b) return a;
    if (a.is(TypeKind::Never)) return b;
    if (b.is(TypeKind::Never)) return a;
    if (a.kind() != b.kind()) return Type::dynamic();

    // Same kind, different identity. Keeping the kind matters: an unproven
    // object is still known not to be a double, which is what lets a later
    // consumer skip a type check even when it cannot skip the shape check.
    switch (a.kind()) {
        case TypeKind::Object:
            // Same class, the two sides disagreeing about how well it is known:
            // the weaker claim on each axis is the join. Dropping to `object()`
            // here instead would throw away an identity both sides agree on.
            //
            // The two axes are independent, so both are joined: a value one
            // edge watched being made and another read out of a field is not
            // built here, and one edge's joined-over-call-sites guess makes the
            // merged identity one too.
            if (a.shapeClass() == b.shapeClass()) {
                if (a.identityOnly() || b.identityOnly()) {
                    return Type::objectIdentityOnly(a.shapeClass());
                }
                return Type::objectNotBuiltHere(a.shapeClass());
            }
            return Type::object();
        case TypeKind::Function: return Type::function();
        case TypeKind::TypedArray: return Type::typedArray();
        case TypeKind::Array: return Type::array();
        default: break;
    }
    // Unreachable: every identity-free kind compares equal above.
    return Type::dynamic();
}

}  // namespace bronze::types
