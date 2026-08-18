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
        out += payload_ == static_cast<uint32_t>(TypedArrayElem::Float64) ? ":f64" : ":f32";
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
        case TypeKind::Object: return Type::object();
        case TypeKind::Function: return Type::function();
        case TypeKind::TypedArray: return Type::typedArray();
        default: break;
    }
    // Unreachable: every identity-free kind compares equal above.
    return Type::dynamic();
}

}  // namespace bronze::types
